#include "wifi.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>

void initialize_wifi(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {};
    const char* ssid = "KaiCar-AP";
    const char* password = "KaiCar123";

    std::memcpy(wifi_config.ap.ssid, ssid, std::strlen(ssid));
    wifi_config.ap.ssid_len = std::strlen(ssid);
    std::memcpy(wifi_config.ap.password, password, std::strlen(password));
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = std::strlen(password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    std::printf("Wi-Fi AP started: SSID=%s password=%s\n", ssid, password);
}

void start_wifi_server(void)
{
    std::printf("Wi-Fi layer ready; server startup is handled separately.\n");
}

void publish_vehicle_status(const char* status)
{
    std::printf("Publishing vehicle status: %s\n", status ? status : "idle");
}
