#include "audio.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <atomic>
#include <cstdio>

static std::atomic<SoundEffect> g_bg_sound{SOUND_NONE};

namespace {

constexpr gpio_num_t kBuzzerPin = GPIO_NUM_23;
constexpr ledc_timer_t kBuzzerTimer = LEDC_TIMER_1;       // timer 0 used by motors
constexpr ledc_channel_t kBuzzerChannel = LEDC_CHANNEL_4;  // channels 0-3 used by motors
constexpr int kBuzzerResolutionBits = 8;
constexpr uint32_t kHalfDuty = (1 << kBuzzerResolutionBits) / 2; // 50% = square wave

QueueHandle_t g_sound_queue = nullptr;

// ── low-level helpers ───────────────────────────────────────────────

void buzzer_tone(uint32_t freq_hz)
{
    if (freq_hz == 0) {
        ledc_set_duty(LEDC_HIGH_SPEED_MODE, kBuzzerChannel, 0);
        ledc_update_duty(LEDC_HIGH_SPEED_MODE, kBuzzerChannel);
        return;
    }
    ledc_set_freq(LEDC_HIGH_SPEED_MODE, kBuzzerTimer, freq_hz);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, kBuzzerChannel, kHalfDuty);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, kBuzzerChannel);
}

void buzzer_off()
{
    buzzer_tone(0);
}

void tone_ms(uint32_t freq_hz, uint32_t duration_ms)
{
    buzzer_tone(freq_hz);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_off();
}

void pause_ms(uint32_t duration_ms)
{
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

// ── note frequencies ────────────────────────────────────────────────

constexpr uint32_t C4  = 262;
constexpr uint32_t D4  = 294;
constexpr uint32_t E4  = 330;
constexpr uint32_t F4  = 349;
constexpr uint32_t G4  = 392;
constexpr uint32_t A4  = 440;
constexpr uint32_t B4  = 494;
constexpr uint32_t C5  = 523;
constexpr uint32_t D5  = 587;
constexpr uint32_t E5  = 659;
constexpr uint32_t G5  = 784;

// ── sound effect implementations ────────────────────────────────────

bool check_interrupted(void)
{
    SoundEffect peek;
    return xQueuePeek(g_sound_queue, &peek, 0) == pdTRUE;
}

void play_startup(void)
{
    // Ascending bright jingle
    tone_ms(C5, 100);
    pause_ms(30);
    tone_ms(E5, 100);
    pause_ms(30);
    tone_ms(G5, 200);
}

void play_honk(void)
{
    // Two-tone car horn
    tone_ms(A4, 150);
    tone_ms(E4, 300);
}

void play_reverse_beep(void)
{
    // Repeating high beep — runs until interrupted
    while (!check_interrupted()) {
        tone_ms(C5, 200);
        pause_ms(400);
    }
}

void play_drive_hum(void)
{
    // Low pulsing hum — runs until interrupted
    while (!check_interrupted()) {
        tone_ms(E4, 80);
        pause_ms(120);
    }
}

void play_turn_tick(void)
{
    // Indicator click — runs until interrupted
    while (!check_interrupted()) {
        tone_ms(G4, 30);
        pause_ms(470);
    }
}

void play_low_battery(void)
{
    // Descending warning
    tone_ms(E5, 150);
    pause_ms(50);
    tone_ms(C5, 150);
    pause_ms(50);
    tone_ms(A4, 300);
}

void play_connected(void)
{
    // Quick double chirp
    tone_ms(D5, 80);
    pause_ms(60);
    tone_ms(G5, 120);
}

// ── audio task ──────────────────────────────────────────────────────

void audio_task(void* /*arg*/)
{
    SoundEffect effect;

    while (true) {
        if (xQueueReceive(g_sound_queue, &effect, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Drain any queued duplicates so we start fresh
        SoundEffect drain;
        while (xQueueReceive(g_sound_queue, &drain, 0) == pdTRUE) {
            effect = drain;
        }

        switch (effect) {
        case SOUND_STARTUP:      play_startup();      break;
        case SOUND_HONK:         play_honk();         break;
        case SOUND_REVERSE_BEEP: play_reverse_beep(); break;
        case SOUND_DRIVE_HUM:    play_drive_hum();    break;
        case SOUND_TURN_TICK:    play_turn_tick();     break;
        case SOUND_LOW_BATTERY:  play_low_battery();   break;
        case SOUND_CONNECTED:    play_connected();     break;
        case SOUND_NONE:
        default:
            buzzer_off();
            break;
        }

        buzzer_off();

        // If nothing new queued, resume the background drive sound
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
    gpio_reset_pin(kBuzzerPin);
    gpio_set_direction(kBuzzerPin, GPIO_MODE_OUTPUT);

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_HIGH_SPEED_MODE;
    timer_cfg.timer_num = kBuzzerTimer;
    timer_cfg.duty_resolution = LEDC_TIMER_8_BIT;
    timer_cfg.freq_hz = 1000; // default, changed per tone
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.speed_mode = LEDC_HIGH_SPEED_MODE;
    ch_cfg.channel = kBuzzerChannel;
    ch_cfg.timer_sel = kBuzzerTimer;
    ch_cfg.intr_type = LEDC_INTR_DISABLE;
    ch_cfg.gpio_num = static_cast<int>(kBuzzerPin);
    ch_cfg.duty = 0;
    ch_cfg.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    g_sound_queue = xQueueCreate(4, sizeof(SoundEffect));
    xTaskCreate(audio_task, "audio", 2560, nullptr, 3, nullptr);

    std::printf("Audio initialized on GPIO %d.\n", static_cast<int>(kBuzzerPin));
}

void play_sound(SoundEffect effect)
{
    if (g_sound_queue != nullptr) {
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
