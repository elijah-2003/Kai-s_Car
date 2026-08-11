#ifndef KAI_CAR_INITIALIZE_H
#define KAI_CAR_INITIALIZE_H

#ifdef __cplusplus
extern "C" {
#endif

void initialize_system(void);

/* Battery ADC on GPIO 34 (10K/10K divider from battery) */
int  read_battery_percent(void);   /* 0–100 */
float read_battery_voltage(void);  /* actual battery voltage */

#ifdef __cplusplus
}
#endif

#endif
