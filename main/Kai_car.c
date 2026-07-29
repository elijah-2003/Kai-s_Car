#include <stdio.h>

#include "initialize.h"

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf("   KAI CAR — BLE CONTROL MODE\n");
    printf("========================================\n");
    printf("  Connect via BLE (device: KaiCar)\n");
    printf("  Use pc_control.py to drive\n");
    printf("========================================\n\n");

    initialize_system();
}
