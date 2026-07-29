#ifndef KAI_CAR_DRIVE_H
#define KAI_CAR_DRIVE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DRIVE_STOP = 0,
    DRIVE_FORWARD,
    DRIVE_REVERSE,
    DRIVE_TURN_LEFT,
    DRIVE_TURN_RIGHT
} DriveCommand;

void initialize_drive_controls(void);
void apply_drive_command(DriveCommand command, unsigned int speed_percent);

/* Drive a single motor pin (0-3) in isolation. All others held at 0. */
void test_motor_pin(int pin_index, unsigned int speed_percent);

#ifdef __cplusplus
}
#endif

#endif
