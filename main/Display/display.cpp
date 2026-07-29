#include "display.h"

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// ── I2C / SSD1306 config ────────────────────────────────────────────

constexpr i2c_port_t kI2CPort = I2C_NUM_0;
constexpr gpio_num_t kSdaPin = GPIO_NUM_21;
constexpr gpio_num_t kSclPin = GPIO_NUM_22;
constexpr uint32_t kI2CFreqHz = 400000;
constexpr uint8_t kOledAddr = 0x3C;

constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr int kPages = kHeight / 8;
constexpr int kBufSize = kWidth * kPages;

uint8_t g_fb[kBufSize];

// ── animation state ─────────────────────────────────────────────────

std::atomic<FaceType> g_current_face{FACE_HAPPY};
std::atomic<bool> g_animate{false}; // animation task started after init

// Wobble tables — different periods for X (16 steps) and Y (12 steps)
// at 200ms per tick: X cycle ~3.2s, Y cycle ~2.4s → gentle Lissajous
constexpr int kWobbleX[] = {0, 0, 1, 1, 2, 2, 1, 1, 0, 0, -1, -1, -2, -2, -1, -1};
constexpr int kWobbleY[] = {0, 0, 1, 1, 1, 0, 0, -1, -1, -1, 0, 0};
constexpr int kWobbleXLen = sizeof(kWobbleX) / sizeof(kWobbleX[0]);
constexpr int kWobbleYLen = sizeof(kWobbleY) / sizeof(kWobbleY[0]);

// ── I2C helpers ─────────────────────────────────────────────────────

void ssd1306_send(uint8_t control, const uint8_t* data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (kOledAddr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, control, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(kI2CPort, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
}

void ssd1306_cmd(uint8_t c)
{
    ssd1306_send(0x00, &c, 1);
}

void ssd1306_init()
{
    vTaskDelay(pdMS_TO_TICKS(100));

    ssd1306_cmd(0xAE);
    ssd1306_cmd(0xD5); ssd1306_cmd(0x80);
    ssd1306_cmd(0xA8); ssd1306_cmd(0x3F);
    ssd1306_cmd(0xD3); ssd1306_cmd(0x00);
    ssd1306_cmd(0x40);
    ssd1306_cmd(0x8D); ssd1306_cmd(0x14);
    ssd1306_cmd(0x20); ssd1306_cmd(0x00);
    ssd1306_cmd(0xA1);
    ssd1306_cmd(0xC8);
    ssd1306_cmd(0xDA); ssd1306_cmd(0x12);
    ssd1306_cmd(0x81); ssd1306_cmd(0xCF);
    ssd1306_cmd(0xD9); ssd1306_cmd(0xF1);
    ssd1306_cmd(0xDB); ssd1306_cmd(0x40);
    ssd1306_cmd(0xA4);
    ssd1306_cmd(0xA6);
    ssd1306_cmd(0xAF);
}

void fb_flush()
{
    ssd1306_cmd(0x21); ssd1306_cmd(0); ssd1306_cmd(127);
    ssd1306_cmd(0x22); ssd1306_cmd(0); ssd1306_cmd(7);
    ssd1306_send(0x40, g_fb, kBufSize);
}

// ── framebuffer drawing ─────────────────────────────────────────────

void fb_clear()
{
    std::memset(g_fb, 0, kBufSize);
}

void fb_set(int x, int y)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    g_fb[x + (y / 8) * kWidth] |= (1 << (y & 7));
}

void fb_hline(int x1, int x2, int y)
{
    for (int x = x1; x <= x2; ++x) fb_set(x, y);
}

void fb_filled_circle(int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; ++dy) {
        int dx = 0;
        while (dx * dx + dy * dy <= r * r) ++dx;
        fb_hline(cx - dx + 1, cx + dx - 1, cy + dy);
    }
}

void fb_circle(int cx, int cy, int r, int thickness)
{
    int ro_sq = r * r;
    int ri = r - thickness;
    int ri_sq = ri * ri;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            int dsq = dx * dx + dy * dy;
            if (dsq <= ro_sq && dsq >= ri_sq) {
                fb_set(cx + dx, cy + dy);
            }
        }
    }
}

void fb_filled_rect(int x, int y, int w, int h)
{
    for (int row = y; row < y + h; ++row)
        fb_hline(x, x + w - 1, row);
}

void fb_arc_bottom(int cx, int cy, int r, int thickness)
{
    int ro_sq = r * r;
    int ri = r - thickness;
    int ri_sq = ri * ri;
    for (int dy = 0; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            int dsq = dx * dx + dy * dy;
            if (dsq <= ro_sq && dsq >= ri_sq)
                fb_set(cx + dx, cy + dy);
        }
    }
}

void fb_arc_top(int cx, int cy, int r, int thickness)
{
    int ro_sq = r * r;
    int ri = r - thickness;
    int ri_sq = ri * ri;
    for (int dy = -r; dy <= 0; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            int dsq = dx * dx + dy * dy;
            if (dsq <= ro_sq && dsq >= ri_sq)
                fb_set(cx + dx, cy + dy);
        }
    }
}

void fb_heart(int cx, int cy, int size)
{
    int r = size / 2;
    fb_filled_circle(cx - r, cy - r / 2, r);
    fb_filled_circle(cx + r, cy - r / 2, r);
    for (int row = 0; row <= size + r; ++row) {
        int half_w = size + r - (row * (size + r)) / (size + r);
        if (half_w < 0) half_w = 0;
        fb_hline(cx - half_w, cx + half_w, cy + row);
    }
}

// ── font for battery % ─────────────────────────────────────────────

constexpr uint8_t kFont[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x00,0x00,0x00,0x00,0x00},
    {0x7F,0x49,0x49,0x49,0x36}, {0x7F,0x09,0x09,0x09,0x06},
};

void fb_char(int x, int y, int idx)
{
    if (idx < 0 || idx > 13) return;
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = kFont[idx][col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1 << row)) fb_set(x + col, y + row);
        }
    }
}

void fb_string_num(int x, int y, unsigned int val)
{
    char buf[8];
    int len = snprintf(buf, sizeof(buf), "%u%%", val);
    for (int i = 0; i < len && buf[i]; ++i) {
        int idx = 11;
        if (buf[i] >= '0' && buf[i] <= '9') idx = buf[i] - '0';
        else if (buf[i] == '%') { fb_char(x, y, 13); x += 6; continue; }
        fb_char(x, y, idx);
        x += 6;
    }
}

// ── face compositions (with offset + blink) ─────────────────────────

constexpr int kLeftEyeX = 40;
constexpr int kRightEyeX = 88;
constexpr int kEyeY = 22;
constexpr int kMouthY = 48;
constexpr int kMouthCX = 64;

void draw_blink_eyes(int dx, int dy)
{
    fb_filled_rect(kLeftEyeX - 8 + dx, kEyeY + dy, 16, 2);
    fb_filled_rect(kRightEyeX - 8 + dx, kEyeY + dy, 16, 2);
}

void compose_happy(int dx, int dy, bool blink)
{
    fb_clear();
    if (blink) {
        draw_blink_eyes(dx, dy);
    } else {
        fb_filled_circle(kLeftEyeX + dx, kEyeY + dy, 7);
        fb_filled_circle(kRightEyeX + dx, kEyeY + dy, 7);
    }
    fb_arc_bottom(kMouthCX + dx, kMouthY - 6 + dy, 22, 3);
}

void compose_love(int dx, int dy, bool blink)
{
    fb_clear();
    if (blink) {
        draw_blink_eyes(dx, dy);
    } else {
        fb_heart(kLeftEyeX + dx, kEyeY + dy, 7);
        fb_heart(kRightEyeX + dx, kEyeY + dy, 7);
    }
    fb_arc_bottom(kMouthCX + dx, kMouthY - 8 + dy, 24, 3);
}

void compose_connected(int dx, int dy, bool blink)
{
    fb_clear();
    if (blink) {
        draw_blink_eyes(dx, dy + 4);
    } else {
        fb_filled_circle(kLeftEyeX + dx, kEyeY + 4 + dy, 5);
        fb_filled_circle(kRightEyeX + dx, kEyeY + 4 + dy, 5);
    }
    fb_arc_bottom(kMouthCX + dx, kMouthY - 2 + dy, 16, 2);
    fb_arc_top(kMouthCX + dx, 8 + dy, 6, 2);
    fb_arc_top(kMouthCX + dx, 8 + dy, 14, 2);
    fb_arc_top(kMouthCX + dx, 8 + dy, 22, 2);
    fb_filled_circle(kMouthCX + dx, 8 + dy, 2);
}

void compose_driving(int dx, int dy, bool blink)
{
    fb_clear();
    if (blink) {
        draw_blink_eyes(dx, dy);
    } else {
        fb_filled_rect(kLeftEyeX - 8 + dx, kEyeY - 2 + dy, 16, 6);
        fb_filled_rect(kRightEyeX - 8 + dx, kEyeY - 2 + dy, 16, 6);
        fb_filled_circle(kLeftEyeX + 2 + dx, kEyeY + 1 + dy, 2);
        fb_filled_circle(kRightEyeX + 2 + dx, kEyeY + 1 + dy, 2);
    }
    fb_filled_rect(kMouthCX - 14 + dx, kMouthY + dy, 28, 3);
}

void compose_sleep(int dx, int dy, bool /*blink*/)
{
    fb_clear();
    // Already has closed eyes — no blink change
    fb_filled_rect(kLeftEyeX - 8 + dx, kEyeY + dy, 16, 2);
    fb_filled_rect(kRightEyeX - 8 + dx, kEyeY + dy, 16, 2);
    fb_circle(kMouthCX + dx, kMouthY + 2 + dy, 5, 2);
    // ZZZ (fixed position, doesn't wobble)
    fb_hline(100, 112, 6);
    for (int i = 0; i < 12; ++i) fb_set(112 - i, 6 + i);
    for (int i = 0; i < 12; ++i) fb_set(112 - i, 7 + i);
    fb_hline(100, 112, 18);
    fb_hline(100, 112, 19);
    fb_hline(108, 118, 22);
    for (int i = 0; i < 8; ++i) fb_set(118 - i, 22 + i);
    fb_hline(108, 118, 30);
}

void compose_sad(int dx, int dy, bool blink)
{
    fb_clear();
    if (blink) {
        draw_blink_eyes(dx, dy);
    } else {
        fb_filled_circle(kLeftEyeX + dx, kEyeY + dy, 7);
        fb_filled_circle(kRightEyeX + dx, kEyeY + dy, 7);
    }
    fb_arc_top(kMouthCX + dx, kMouthY + 12 + dy, 20, 3);
}

void render_face(FaceType face, int dx, int dy, bool blink)
{
    switch (face) {
    case FACE_HAPPY:     compose_happy(dx, dy, blink);     break;
    case FACE_LOVE:      compose_love(dx, dy, blink);      break;
    case FACE_CONNECTED: compose_connected(dx, dy, blink); break;
    case FACE_DRIVING:   compose_driving(dx, dy, blink);   break;
    case FACE_SLEEP:     compose_sleep(dx, dy, blink);     break;
    case FACE_SAD:       compose_sad(dx, dy, blink);       break;
    }
    fb_flush();
}

// ── animation task ──────────────────────────────────────────────────

void display_task(void* /*arg*/)
{
    int tick = 0;
    int next_blink = 15 + static_cast<int>(esp_random() % 15); // 3-6s

    while (true) {
        if (!g_animate.load(std::memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        FaceType face = g_current_face.load(std::memory_order_relaxed);
        int dx = kWobbleX[tick % kWobbleXLen];
        int dy = kWobbleY[tick % kWobbleYLen];

        next_blink--;
        if (next_blink <= 0) {
            // Blink: closed eyes for one frame
            render_face(face, dx, dy, true);
            vTaskDelay(pdMS_TO_TICKS(120));
            // Sometimes double-blink
            if (esp_random() % 4 == 0) {
                render_face(face, dx, dy, false);
                vTaskDelay(pdMS_TO_TICKS(100));
                render_face(face, dx, dy, true);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            next_blink = 15 + static_cast<int>(esp_random() % 20); // 3-7s
        }

        render_face(face, dx, dy, false);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

} // namespace

// ── public API ──────────────────────────────────────────────────────

void initialize_display(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = kSdaPin;
    conf.scl_io_num = kSclPin;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = kI2CFreqHz;
    ESP_ERROR_CHECK(i2c_param_config(kI2CPort, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(kI2CPort, I2C_MODE_MASTER, 0, 0, 0));

    ssd1306_init();
    fb_clear();
    fb_flush();

    xTaskCreate(display_task, "display", 4096, nullptr, 2, nullptr);

    std::printf("OLED display initialized at 0x%02X.\n", kOledAddr);
}

void show_face(FaceType face)
{
    g_current_face.store(face, std::memory_order_relaxed);
    g_animate.store(true, std::memory_order_relaxed);
}

void show_battery_percentage(unsigned int percentage)
{
    // Pause animation, render static battery screen
    g_animate.store(false, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(250)); // let animation task settle

    if (percentage > 100) percentage = 100;
    fb_clear();
    fb_filled_rect(20, 18, 80, 30);
    for (int y = 20; y < 46; ++y)
        for (int x = 22; x < 98; ++x)
            g_fb[x + (y / 8) * kWidth] &= ~(1 << (y & 7));
    fb_filled_rect(100, 26, 6, 12);
    int fill_w = (74 * static_cast<int>(percentage)) / 100;
    if (fill_w > 0) fb_filled_rect(23, 21, fill_w, 24);
    fb_string_num(52, 54, percentage);
    fb_flush();
}

void show_greeting(const char* name)
{
    show_face(FACE_HAPPY);
    std::printf("Display greeting for %s\n", name ? name : "brother");
}
