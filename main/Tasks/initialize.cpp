#include "initialize.h"

#include "audio.h"
#include "ble.h"
#include "display.h"
#include "drive.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_system.h"
#include <algorithm>
#include <cstdio>

static adc_oneshot_unit_handle_t g_adc_handle = nullptr;

static void initialize_battery_adc(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {};
    init_cfg.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &g_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    chan_cfg.atten = ADC_ATTEN_DB_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc_handle, ADC_CHANNEL_6, &chan_cfg));

    std::printf("Battery ADC initialized — GPIO 34 (ADC1_CH6), 10K/10K divider.\n");
}

float read_battery_voltage(void)
{
    if (!g_adc_handle) return 0.0f;

    int raw = 0;
    adc_oneshot_read(g_adc_handle, ADC_CHANNEL_6, &raw);

    // ADC reads 0–4095 over 0–3.3V. 10K/10K divider halves battery voltage.
    float adc_voltage = (static_cast<float>(raw) / 4095.0f) * 3.3f;
    return adc_voltage * 2.0f;
}

int read_battery_percent(void)
{
    float voltage = read_battery_voltage();
    // 4×AA: ~6.0V full, ~4.0V empty
    float percent = (voltage - 4.0f) / 2.0f * 100.0f;
    return std::max(0, std::min(100, static_cast<int>(percent)));
}

static void log_reset_reason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    const char* desc;
    switch (reason) {
    case ESP_RST_POWERON:   desc = "POWER-ON";          break;
    case ESP_RST_EXT:       desc = "EXTERNAL RESET";    break;
    case ESP_RST_SW:        desc = "SOFTWARE RESET";    break;
    case ESP_RST_PANIC:     desc = "PANIC (crash)";     break;
    case ESP_RST_INT_WDT:   desc = "INTERRUPT WDT";     break;
    case ESP_RST_TASK_WDT:  desc = "TASK WDT";          break;
    case ESP_RST_WDT:       desc = "OTHER WDT";         break;
    case ESP_RST_DEEPSLEEP: desc = "DEEP SLEEP WAKE";   break;
    case ESP_RST_BROWNOUT:  desc = "*** BROWNOUT ***";   break;
    case ESP_RST_SDIO:      desc = "SDIO";              break;
    default:                desc = "UNKNOWN";            break;
    }
    std::printf("=== RESET REASON: %s (code %d) ===\n", desc, static_cast<int>(reason));
}

void initialize_system(void)
{
    log_reset_reason();
    std::printf("Initializing Kai Car subsystems...\n");

    initialize_battery_adc();
    initialize_display();
    initialize_audio();
    initialize_drive_controls();
    initialize_ble();
    ble_enable_drive(true);

    play_sound(SOUND_HONK);
    show_face(FACE_HAPPY);

    std::printf("Kai Car ready — waiting for BLE connection.\n");
}
