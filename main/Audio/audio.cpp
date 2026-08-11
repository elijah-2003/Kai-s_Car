#include "audio.h"

#include "driver/dac_continuous.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <atomic>
#include <cstdio>
#include <cstring>

static std::atomic<SoundEffect> g_bg_sound{SOUND_NONE};

namespace {

constexpr int kSampleRate = 44100;
constexpr int kBufSize = 1024;
constexpr int kWavHeaderSize = 44;

dac_continuous_handle_t g_dac = nullptr;
QueueHandle_t g_sound_queue = nullptr;

const char* sound_path(SoundEffect effect)
{
    switch (effect) {
    case SOUND_STARTUP:  return "/audio/startup.wav";
    case SOUND_DRIVING:  return "/audio/driving.wav";
    case SOUND_REVERSE:  return "/audio/reverse.wav";
    case SOUND_SCREECH:  return "/audio/screech.wav";
    case SOUND_HONK:     return "/audio/honk.wav";
    default:             return nullptr;
    }
}

bool is_looping(SoundEffect effect)
{
    return effect == SOUND_DRIVING || effect == SOUND_REVERSE;
}

bool check_interrupted(void)
{
    SoundEffect peek;
    return xQueuePeek(g_sound_queue, &peek, 0) == pdTRUE;
}

void play_wav(SoundEffect effect)
{
    const char* path = sound_path(effect);
    if (!path) return;

    FILE* f = fopen(path, "rb");
    if (!f) {
        std::printf("Audio: cannot open %s\n", path);
        return;
    }

    fseek(f, kWavHeaderSize, SEEK_SET);

    uint8_t buf[kBufSize];
    size_t bytes_read;
    size_t bytes_written;
    bool loop = is_looping(effect);

    do {
        fseek(f, kWavHeaderSize, SEEK_SET);

        while ((bytes_read = fread(buf, 1, kBufSize, f)) > 0) {
            if (check_interrupted()) {
                fclose(f);
                return;
            }
            dac_continuous_write(g_dac, buf, bytes_read, &bytes_written, portMAX_DELAY);
        }
    } while (loop && !check_interrupted());

    fclose(f);
}

// Startup: play startup.wav once, then transition to driving loop
void play_startup_sequence(void)
{
    const char* path = sound_path(SOUND_STARTUP);
    if (!path) return;

    FILE* f = fopen(path, "rb");
    if (!f) return;

    fseek(f, kWavHeaderSize, SEEK_SET);

    uint8_t buf[kBufSize];
    size_t bytes_read;
    size_t bytes_written;

    while ((bytes_read = fread(buf, 1, kBufSize, f)) > 0) {
        if (check_interrupted()) {
            fclose(f);
            return;
        }
        dac_continuous_write(g_dac, buf, bytes_read, &bytes_written, portMAX_DELAY);
    }

    fclose(f);

    // Transition straight into driving loop
    if (!check_interrupted()) {
        play_wav(SOUND_DRIVING);
    }
}

void silence(void)
{
    uint8_t silence_buf[256];
    std::memset(silence_buf, 128, sizeof(silence_buf));
    size_t written;
    dac_continuous_write(g_dac, silence_buf, sizeof(silence_buf), &written, portMAX_DELAY);
}

void audio_task(void* /*arg*/)
{
    SoundEffect effect;

    while (true) {
        if (xQueueReceive(g_sound_queue, &effect, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Drain queued duplicates
        SoundEffect drain;
        while (xQueueReceive(g_sound_queue, &drain, 0) == pdTRUE) {
            effect = drain;
        }

        if (effect == SOUND_NONE) {
            silence();
            continue;
        }

        if (effect == SOUND_STARTUP) {
            play_startup_sequence();
        } else {
            play_wav(effect);
        }

        silence();

        // Resume background sound if nothing new queued
        SoundEffect peek;
        if (xQueuePeek(g_sound_queue, &peek, 0) != pdTRUE) {
            SoundEffect bg = g_bg_sound.load(std::memory_order_relaxed);
            if (bg != SOUND_NONE) {
                xQueueSend(g_sound_queue, &bg, 0);
            }
        }
    }
}

} // namespace

void initialize_audio(void)
{
    // Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_cfg = {};
    spiffs_cfg.base_path = "/audio";
    spiffs_cfg.partition_label = "audio";
    spiffs_cfg.max_files = 2;
    spiffs_cfg.format_if_mount_failed = false;
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_cfg));

    size_t total = 0, used = 0;
    esp_spiffs_info("audio", &total, &used);
    std::printf("Audio SPIFFS: %u / %u bytes used\n", (unsigned)used, (unsigned)total);

    // Configure DAC continuous output
    dac_continuous_config_t dac_cfg = {};
    dac_cfg.chan_mask = DAC_CHANNEL_MASK_CH0;
    dac_cfg.desc_num = 4;
    dac_cfg.buf_size = kBufSize;
    dac_cfg.freq_hz = kSampleRate;
    dac_cfg.offset = 0;
    dac_cfg.clk_src = DAC_DIGI_CLK_SRC_APLL;
    dac_cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;
    ESP_ERROR_CHECK(dac_continuous_new_channels(&dac_cfg, &g_dac));
    ESP_ERROR_CHECK(dac_continuous_enable(g_dac));

    g_sound_queue = xQueueCreate(4, sizeof(SoundEffect));
    xTaskCreate(audio_task, "audio", 4096, nullptr, 3, nullptr);

    std::printf("Audio initialized — DAC continuous on GPIO 25, 44100 Hz.\n");
}

void play_sound(SoundEffect effect)
{
    if (g_sound_queue) {
        xQueueSend(g_sound_queue, &effect, 0);
    }
}

void set_background_sound(SoundEffect effect)
{
    g_bg_sound.store(effect, std::memory_order_relaxed);
}

void clear_background_sound(void)
{
    g_bg_sound.store(SOUND_NONE, std::memory_order_relaxed);
}

void stop_sound(void)
{
    g_bg_sound.store(SOUND_NONE, std::memory_order_relaxed);
    play_sound(SOUND_NONE);
}
