#include "ble.h"

#include "audio.h"
#include "command_translation.h"
#include "display.h"
#include "drive.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "nvs_flash.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kDeviceName = "KaiCar";
constexpr uint16_t kAppId = 0;
constexpr uint16_t kServiceUuid = 0x00FF;
constexpr uint16_t kCharUuid = 0xFF01;

std::atomic<bool> g_drive_enabled{false};
DriveCommand g_last_ble_command = DRIVE_STOP;

uint16_t g_gatts_if = ESP_GATT_IF_NONE;
uint16_t g_service_handle = 0;
uint16_t g_char_handle = 0;

esp_ble_adv_params_t g_adv_params = {};

void start_advertising()
{
    esp_ble_gap_start_advertising(&g_adv_params);
}

void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            std::printf("BLE advertising started.\n");
        }
        break;
    default:
        break;
    }
}

void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                         esp_ble_gatts_cb_param_t* param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        g_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(kDeviceName);

        esp_ble_adv_data_t adv_data = {};
        adv_data.set_scan_rsp = false;
        adv_data.include_name = true;
        adv_data.include_txpower = false;
        adv_data.min_interval = 0x0006;
        adv_data.max_interval = 0x0010;
        adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
        esp_ble_gap_config_adv_data(&adv_data);

        esp_gatt_srvc_id_t service_id = {};
        service_id.is_primary = true;
        service_id.id.inst_id = 0;
        service_id.id.uuid.len = ESP_UUID_LEN_16;
        service_id.id.uuid.uuid.uuid16 = kServiceUuid;
        esp_ble_gatts_create_service(gatts_if, &service_id, 4);
        break;
    }
    case ESP_GATTS_CREATE_EVT: {
        g_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(g_service_handle);

        esp_bt_uuid_t char_uuid = {};
        char_uuid.len = ESP_UUID_LEN_16;
        char_uuid.uuid.uuid16 = kCharUuid;

        esp_gatt_char_prop_t property = ESP_GATT_CHAR_PROP_BIT_WRITE
                                      | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

        esp_ble_gatts_add_char(g_service_handle, &char_uuid,
                               ESP_GATT_PERM_WRITE, property,
                               nullptr, nullptr);
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT:
        g_char_handle = param->add_char.attr_handle;
        std::printf("BLE characteristic ready (handle %u).\n", g_char_handle);
        break;

    case ESP_GATTS_CONNECT_EVT:
        std::printf("BLE client connected.\n");
        g_last_ble_command = DRIVE_STOP;
        show_face(FACE_CONNECTED);
        play_sound(SOUND_CONNECTED);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        std::printf("BLE client disconnected.\n");
        g_last_ble_command = DRIVE_STOP;
        if (g_drive_enabled.load(std::memory_order_relaxed)) {
            apply_drive_command(DRIVE_STOP, 0);
        }
        show_face(FACE_SAD);
        stop_sound();
        start_advertising();
        break;

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.handle == g_char_handle && param->write.len > 0) {
            char cmd_buf[32] = {};
            const size_t len = param->write.len < sizeof(cmd_buf) - 1
                             ? param->write.len : sizeof(cmd_buf) - 1;
            std::memcpy(cmd_buf, param->write.value, len);

            // Parse "command:speed" format (e.g. "forward:70"). Default 80.
            unsigned int speed = 80;
            char* colon = std::strchr(cmd_buf, ':');
            if (colon != nullptr) {
                *colon = '\0';
                speed = static_cast<unsigned int>(std::atoi(colon + 1));
                if (speed > 100) speed = 100;
            }

            std::printf("BLE command: \"%s\" speed=%u%%\n", cmd_buf, speed);

            // Honk is a one-shot sound, not a drive command
            if (std::strcmp(cmd_buf, "honk") == 0) {
                play_sound(SOUND_HONK);
            } else if (g_drive_enabled.load(std::memory_order_relaxed)) {
                const DriveCommand cmd = translate_command_string(cmd_buf);
                apply_drive_command(cmd, speed);

                if (cmd != g_last_ble_command) {
                    g_last_ble_command = cmd;
                    switch (cmd) {
                    case DRIVE_FORWARD:
                        show_face(FACE_DRIVING);
                        set_background_sound(SOUND_DRIVE_HUM);
                        play_sound(SOUND_DRIVE_HUM);
                        break;
                    case DRIVE_REVERSE:
                        show_face(FACE_DRIVING);
                        set_background_sound(SOUND_REVERSE_BEEP);
                        play_sound(SOUND_REVERSE_BEEP);
                        break;
                    case DRIVE_TURN_LEFT:
                    case DRIVE_TURN_RIGHT:
                        show_face(FACE_DRIVING);
                        set_background_sound(SOUND_TURN_TICK);
                        play_sound(SOUND_TURN_TICK);
                        break;
                    case DRIVE_STOP:
                        show_face(FACE_HAPPY);
                        stop_sound();
                        break;
                    }
                }
            }
        }
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id,
                                        ESP_GATT_OK, nullptr);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

void initialize_ble(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    g_adv_params.adv_int_min = 0x20;
    g_adv_params.adv_int_max = 0x40;
    g_adv_params.adv_type = ADV_TYPE_IND;
    g_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    g_adv_params.channel_map = ADV_CHNL_ALL;
    g_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    ESP_ERROR_CHECK(esp_ble_gatts_app_register(kAppId));

    std::printf("BLE initialized as \"%s\".\n", kDeviceName);
}

void ble_enable_drive(bool enable)
{
    g_drive_enabled.store(enable, std::memory_order_relaxed);
    std::printf("BLE drive forwarding %s.\n", enable ? "ENABLED" : "DISABLED");
}
