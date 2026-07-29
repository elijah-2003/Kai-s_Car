#include "drive.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace {

// Physical pin connections to DRV8833
constexpr gpio_num_t kLeftPinA  = GPIO_NUM_27; // DRV8833 IN1
constexpr gpio_num_t kLeftPinB  = GPIO_NUM_26; // DRV8833 IN2
constexpr gpio_num_t kRightPinA = GPIO_NUM_25; // DRV8833 IN3
constexpr gpio_num_t kRightPinB = GPIO_NUM_33; // DRV8833 IN4

// Direction mapping — toggle via idf.py menuconfig → "Kai Car Motor Configuration"
#ifdef CONFIG_KAICAR_LEFT_MOTOR_REVERSED
constexpr gpio_num_t kLeftForwardPin  = kLeftPinB;
constexpr gpio_num_t kLeftReversePin  = kLeftPinA;
#else
constexpr gpio_num_t kLeftForwardPin  = kLeftPinA;
constexpr gpio_num_t kLeftReversePin  = kLeftPinB;
#endif

#ifdef CONFIG_KAICAR_RIGHT_MOTOR_REVERSED
constexpr gpio_num_t kRightForwardPin = kRightPinB;
constexpr gpio_num_t kRightReversePin = kRightPinA;
#else
constexpr gpio_num_t kRightForwardPin = kRightPinA;
constexpr gpio_num_t kRightReversePin = kRightPinB;
#endif

constexpr int kPwmResolutionBits = 10;
constexpr int kPwmFrequencyHz = 5000;
constexpr int kMaxDuty = (1 << kPwmResolutionBits) - 1;
constexpr int kAccelSteps = 20;
constexpr int kAccelDelayMs = 15;
constexpr int kMinDutyPercent = 65; // 4×AA ≈ 5V, motor needs 3V min, covers sag to ~4.2V

constexpr uint32_t kDeadmanTimeoutMs = 800;
constexpr uint32_t kDeadmanCheckMs = 100;
constexpr uint32_t kCoastDelayMs = 80; // coast time between direction changes

std::atomic<TickType_t> g_last_active_tick{0};
std::atomic<bool> g_active{false};
std::atomic<DriveCommand> g_current_command{DRIVE_STOP};
std::atomic<unsigned int> g_current_speed{0};

// Returns true if going from old_cmd to new_cmd involves a motor direction reversal
bool is_direction_reversal(DriveCommand old_cmd, DriveCommand new_cmd)
{
    if (old_cmd == DRIVE_STOP || new_cmd == DRIVE_STOP) return false;
    if (old_cmd == new_cmd) return false;

    // Forward↔Reverse is the worst case
    if ((old_cmd == DRIVE_FORWARD && new_cmd == DRIVE_REVERSE) ||
        (old_cmd == DRIVE_REVERSE && new_cmd == DRIVE_FORWARD))
        return true;

    // Any other direction change also reverses at least one motor
    return old_cmd != new_cmd;
}

void save_crash_log(DriveCommand prev, DriveCommand next, unsigned int speed)
{
    nvs_handle_t h;
    if (nvs_open("drive_log", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "prev_cmd", static_cast<int32_t>(prev));
        nvs_set_i32(h, "next_cmd", static_cast<int32_t>(next));
        nvs_set_i32(h, "speed", static_cast<int32_t>(speed));
        int64_t t = esp_timer_get_time();
        nvs_set_i64(h, "time_us", t);
        // increment crash counter
        int32_t count = 0;
        nvs_get_i32(h, "crash_n", &count);
        nvs_set_i32(h, "crash_n", count + 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

void deadman_task(void* /*arg*/)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kDeadmanCheckMs));
        if (!g_active.load(std::memory_order_relaxed)) {
            continue;
        }
        const TickType_t now = xTaskGetTickCount();
        const TickType_t last = g_last_active_tick.load(std::memory_order_relaxed);
        const uint32_t elapsed_ms = (now - last) * portTICK_PERIOD_MS;
        if (elapsed_ms >= kDeadmanTimeoutMs) {
            std::printf("[DRIVE %lld us] DEADMAN: no command for %ums, stopping.\n",
                        esp_timer_get_time(), static_cast<unsigned>(elapsed_ms));
            apply_drive_command(DRIVE_STOP, 0);
        }
    }
}

void set_channel_duty(ledc_channel_t channel, uint32_t duty)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, channel));
}

void configure_motor_channel(ledc_channel_t channel, gpio_num_t pin)
{
    ledc_channel_config_t channel_config = {};
    channel_config.speed_mode = LEDC_HIGH_SPEED_MODE;
    channel_config.channel = channel;
    channel_config.timer_sel = LEDC_TIMER_0;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.gpio_num = static_cast<int>(pin);
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
}

void apply_motor_state(bool left_forward, bool left_reverse, bool right_forward, bool right_reverse, uint32_t duty)
{
    set_channel_duty(LEDC_CHANNEL_0, left_forward ? duty : 0);
    set_channel_duty(LEDC_CHANNEL_1, left_reverse ? duty : 0);
    set_channel_duty(LEDC_CHANNEL_2, right_forward ? duty : 0);
    set_channel_duty(LEDC_CHANNEL_3, right_reverse ? duty : 0);

    (void)left_forward;
    (void)left_reverse;
    (void)right_forward;
    (void)right_reverse;
}

void ramp_motor_state(bool left_forward, bool left_reverse, bool right_forward, bool right_reverse, uint32_t target_duty)
{
    if (target_duty == 0) {
        apply_motor_state(left_forward, left_reverse, right_forward, right_reverse, 0);
        return;
    }

    const uint32_t min_duty = (kMinDutyPercent * kMaxDuty) / 100;
    const uint32_t floor_duty = target_duty < min_duty ? target_duty : min_duty;

    for (int step = 1; step <= kAccelSteps; ++step) {
        const float progress = static_cast<float>(step) / static_cast<float>(kAccelSteps);
        const float eased = 0.5f * (1.0f - std::cosf(3.14159265f * progress));
        // Ramp from floor_duty to target_duty so the motor never sits in the buzz zone.
        const uint32_t stepped_duty = floor_duty
            + static_cast<uint32_t>((target_duty - floor_duty) * eased);

        // Stagger left and right motors so their inrush doesn't stack.
        set_channel_duty(LEDC_CHANNEL_0, left_forward ? stepped_duty : 0);
        set_channel_duty(LEDC_CHANNEL_1, left_reverse ? stepped_duty : 0);
        vTaskDelay(pdMS_TO_TICKS(kAccelDelayMs));

        set_channel_duty(LEDC_CHANNEL_2, right_forward ? stepped_duty : 0);
        set_channel_duty(LEDC_CHANNEL_3, right_reverse ? stepped_duty : 0);
        vTaskDelay(pdMS_TO_TICKS(kAccelDelayMs));
    }

    apply_motor_state(left_forward, left_reverse, right_forward, right_reverse, target_duty);
}

} // namespace

void initialize_drive_controls(void)
{
    gpio_reset_pin(kLeftForwardPin);
    gpio_reset_pin(kLeftReversePin);
    gpio_reset_pin(kRightForwardPin);
    gpio_reset_pin(kRightReversePin);

    gpio_set_direction(kLeftForwardPin, GPIO_MODE_OUTPUT);
    gpio_set_direction(kLeftReversePin, GPIO_MODE_OUTPUT);
    gpio_set_direction(kRightForwardPin, GPIO_MODE_OUTPUT);
    gpio_set_direction(kRightReversePin, GPIO_MODE_OUTPUT);

    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = LEDC_HIGH_SPEED_MODE;
    timer_config.timer_num = LEDC_TIMER_0;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.freq_hz = kPwmFrequencyHz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    configure_motor_channel(LEDC_CHANNEL_0, kLeftForwardPin);
    configure_motor_channel(LEDC_CHANNEL_1, kLeftReversePin);
    configure_motor_channel(LEDC_CHANNEL_2, kRightForwardPin);
    configure_motor_channel(LEDC_CHANNEL_3, kRightReversePin);

    apply_motor_state(false, false, false, false, 0);

    xTaskCreate(deadman_task, "drive_deadman", 2560, nullptr, 4, nullptr);

    std::printf("Configured four PWM motor outputs for differential steering.\n");
}

void apply_drive_command(DriveCommand command, unsigned int speed_percent)
{
    const DriveCommand prev_command = g_current_command.load(std::memory_order_relaxed);
    const int64_t t_start = esp_timer_get_time(); // microseconds

    if (command == DRIVE_STOP) {
        g_active.store(false, std::memory_order_relaxed);
        g_current_command.store(DRIVE_STOP, std::memory_order_relaxed);
        g_current_speed.store(0, std::memory_order_relaxed);
    } else {
        g_last_active_tick.store(xTaskGetTickCount(), std::memory_order_relaxed);
        g_active.store(true, std::memory_order_relaxed);

        // Same command at same speed — just refresh the deadman timer, no re-ramp.
        if (command == prev_command &&
            speed_percent == g_current_speed.load(std::memory_order_relaxed)) {
            return;
        }

        // Direction reversal: coast first to let motors spin down,
        // prevents back-EMF + inrush current spike from causing brownout.
        if (is_direction_reversal(prev_command, command)) {
            std::printf("[DRIVE %lld us] COAST: %dms pause before direction change\n",
                        t_start, static_cast<int>(kCoastDelayMs));
            apply_motor_state(false, false, false, false, 0);
            vTaskDelay(pdMS_TO_TICKS(kCoastDelayMs));
        }

        g_current_command.store(command, std::memory_order_relaxed);
        g_current_speed.store(speed_percent, std::memory_order_relaxed);
    }

    // Save to NVS before executing — survives power loss
    save_crash_log(prev_command, command, speed_percent);

    const char* command_name = "STOP";
    const unsigned int clamped_speed = std::min<unsigned int>(100, speed_percent);
    const uint32_t target_duty = (clamped_speed * kMaxDuty) / 100;

    switch (command) {
    case DRIVE_FORWARD:
        command_name = "FORWARD";
        ramp_motor_state(true, false, true, false, target_duty);
        break;
    case DRIVE_REVERSE:
        command_name = "REVERSE";
        ramp_motor_state(false, true, false, true, target_duty);
        break;
    case DRIVE_TURN_LEFT:
        command_name = "TURN_LEFT";
        ramp_motor_state(false, false, true, false, kMaxDuty);
        break;
    case DRIVE_TURN_RIGHT:
        command_name = "TURN_RIGHT";
        ramp_motor_state(true, false, false, false, kMaxDuty);
        break;
    case DRIVE_STOP:
    default:
        command_name = "STOP";
        apply_motor_state(false, false, false, false, 0);
        break;
    }

    const int64_t t_end = esp_timer_get_time();
    std::printf("[DRIVE %lld us] %s → %s at %u%% duty=%u/1023 (took %lld us)\n",
                t_end,
                prev_command == DRIVE_STOP ? "STOP" :
                prev_command == DRIVE_FORWARD ? "FWD" :
                prev_command == DRIVE_REVERSE ? "REV" :
                prev_command == DRIVE_TURN_LEFT ? "LEFT" : "RIGHT",
                command_name, clamped_speed,
                static_cast<unsigned>(target_duty),
                t_end - t_start);
}

void test_motor_pin(int pin_index, unsigned int speed_percent)
{
    const unsigned int clamped = std::min<unsigned int>(100, speed_percent);
    const uint32_t duty = (clamped * kMaxDuty) / 100;

    const char* names[] = { "LEFT_FWD", "LEFT_REV", "RIGHT_FWD", "RIGHT_REV" };
    const gpio_num_t pins[] = { kLeftForwardPin, kLeftReversePin, kRightForwardPin, kRightReversePin };

    // All channels off first
    for (int i = 0; i < 4; ++i) {
        set_channel_duty(static_cast<ledc_channel_t>(i), 0);
    }

    if (pin_index < 0 || pin_index > 3) {
        std::printf("test_motor_pin: invalid index %d\n", pin_index);
        return;
    }

    set_channel_duty(static_cast<ledc_channel_t>(pin_index), duty);
    std::printf("PIN TEST: %s (GPIO %d) at %u%% duty=%u/1023\n",
                names[pin_index], static_cast<int>(pins[pin_index]),
                clamped, static_cast<unsigned>(duty));
}
