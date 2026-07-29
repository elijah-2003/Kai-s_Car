#include "initialize.h"

#include "audio.h"
#include "ble.h"
#include "display.h"
#include "drive.h"

#include "esp_system.h"
#include <cstdio>

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

    initialize_display();
    initialize_audio();
    initialize_drive_controls();
    initialize_ble();
    ble_enable_drive(true);

    play_sound(SOUND_STARTUP);
    show_face(FACE_HAPPY);

    std::printf("Kai Car ready — waiting for BLE connection.\n");
}
