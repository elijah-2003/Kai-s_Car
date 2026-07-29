#ifndef KAI_CAR_WIFI_H
#define KAI_CAR_WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

void initialize_wifi(void);
void start_wifi_server(void);
void publish_vehicle_status(const char* status);

#ifdef __cplusplus
}
#endif

#endif
