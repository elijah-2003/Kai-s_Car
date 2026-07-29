#include "server.h"

#include "command_translation.h"
#include "drive.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

namespace {

httpd_handle_t g_server = nullptr;

void add_cors_headers(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

esp_err_t drive_handler(httpd_req_t* req)
{
    add_cors_headers(req);

    char query[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char command[16] = {};
        if (httpd_query_key_value(query, "cmd", command, sizeof(command)) == ESP_OK) {
            const DriveCommand drive_command = translate_command_string(command);
            // Send the HTTP response BEFORE starting motors so the WiFi TX
            // current spike finishes before motor inrush begins.
            httpd_resp_set_type(req, "text/plain");
            esp_err_t err = httpd_resp_send(req, "ok", 2);
            vTaskDelay(pdMS_TO_TICKS(30));
            apply_drive_command(drive_command, 80);
            return err;
        }
    }

    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "expected /drive?cmd=forward");
}

esp_err_t drive_options_handler(httpd_req_t* req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

} // namespace

void initialize_server(void)
{
    std::printf("Preparing HTTP control server layer...\n");
}

void start_server(void)
{
    if (g_server != nullptr) {
        std::printf("Control server already running.\n");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    if (httpd_start(&g_server, &config) != ESP_OK) {
        std::printf("Failed to start control server.\n");
        return;
    }

    httpd_uri_t drive_uri = {};
    drive_uri.uri = "/drive";
    drive_uri.method = HTTP_GET;
    drive_uri.handler = drive_handler;
    drive_uri.user_ctx = nullptr;

    httpd_uri_t drive_post_uri = {};
    drive_post_uri.uri = "/drive";
    drive_post_uri.method = HTTP_POST;
    drive_post_uri.handler = drive_handler;
    drive_post_uri.user_ctx = nullptr;

    httpd_uri_t drive_options_uri = {};
    drive_options_uri.uri = "/drive";
    drive_options_uri.method = HTTP_OPTIONS;
    drive_options_uri.handler = drive_options_handler;
    drive_options_uri.user_ctx = nullptr;

    httpd_register_uri_handler(g_server, &drive_uri);
    httpd_register_uri_handler(g_server, &drive_post_uri);
    httpd_register_uri_handler(g_server, &drive_options_uri);

    std::printf("HTTP control server ready on http://192.168.4.1/drive\n");
}
