#pragma once

/**
 * @file board_config.h
 * @brief Cấu hình phần cứng cố định cho S.W.Ni-Oh
 *        Bo mạch: ESP32-S3 SuperMini (4MB Flash, Dynamic PSRAM detection)
 *
 * CẢNH BÁO: Chân GPIO được hàn cố định trên bo mạch, tuyệt đối không tự ý thay đổi.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_psram.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 1. MÀN HÌNH ST7789 1.54" (240x240, SPI3_HOST, KHÔNG CÓ CHÂN CS)
// ============================================================================
#define LCD_SPI_HOST            SPI3_HOST
#define LCD_MOSI_GPIO           GPIO_NUM_9
#define LCD_SCLK_GPIO           GPIO_NUM_10
#define LCD_DC_GPIO             GPIO_NUM_7
#define LCD_RST_GPIO            GPIO_NUM_8
#define LCD_BLK_GPIO            GPIO_NUM_6
#define LCD_CS_GPIO             GPIO_NUM_NC     // (-1) Màn hình 7-pin nối cứng CS xuống GND

// RÀNG BUỘC BẮT BUỘC: Vì CS = -1 nên bắt buộc SPI Mode 3 (CPOL=1, CPHA=1)
#define LCD_SPI_MODE            3
#define LCD_WIDTH               240
#define LCD_HEIGHT              240
#define LCD_PCLK_HZ             (40 * 1000 * 1000)
#define LCD_DMA_LINES           40              // 240 * 40 * 2B = 19.2KB thân thiện cả khi thiếu PSRAM

// Cấu hình LEDC PWM điều khiển độ sáng đèn nền (GPIO 6)
#define LCD_BLK_LEDC_TIMER      LEDC_TIMER_0
#define LCD_BLK_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LCD_BLK_LEDC_CHANNEL    LEDC_CHANNEL_0
#define LCD_BLK_LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define LCD_BLK_LEDC_MAX_DUTY   ((1U << 10) - 1U)
#define LCD_BLK_LEDC_FREQ_HZ    5000

// ============================================================================
// 2. ÂM THANH I2S ĐỘC LẬP (ESP-IDF v5.x driver/i2s_std.h)
// ============================================================================
// Microphone INMP441 (I2S_NUM_0 - RX Master)
#define MIC_I2S_PORT            I2S_NUM_0
#define MIC_BCLK_GPIO           GPIO_NUM_11
#define MIC_WS_GPIO             GPIO_NUM_12
#define MIC_DIN_GPIO            GPIO_NUM_13
// Nếu chân L/R của INMP441 nối GND -> LEFT; nối 3.3V -> đổi sang I2S_STD_SLOT_RIGHT
#define MIC_I2S_SLOT_MASK       I2S_STD_SLOT_LEFT

// Amplifier MAX98357A (I2S_NUM_1 - TX Master)
#define SPK_I2S_PORT            I2S_NUM_1
#define SPK_BCLK_GPIO           GPIO_NUM_1
#define SPK_WS_GPIO             GPIO_NUM_2
#define SPK_DOUT_GPIO           GPIO_NUM_44     // Trùng U0RXD mặc định -> cần board_release_gpio44()

// ============================================================================
// 3. HỆ THỐNG 3 NÚT NHẤN NGOẠI VI (Debounce 30ms, Long Press 1500-2000ms)
// ============================================================================
// 0 = Active LOW (dùng GPIO_PULLUP_ONLY), 1 = Active HIGH (dùng GPIO_PULLDOWN_ONLY)
#ifndef BUTTON_ACTIVE_LEVEL
#define BUTTON_ACTIVE_LEVEL     0
#endif

#define BTN_MAIN_GPIO           GPIO_NUM_3      // Strapping pin: CHỈ khởi tạo sau khi boot xong!
#define BTN_VOL_UP_GPIO         GPIO_NUM_4
#define BTN_VOL_DOWN_GPIO       GPIO_NUM_5
#define BTN_DEBOUNCE_MS         30
#define BTN_LONG_PRESS_MS       1600

// ============================================================================
// 4. ĐỊNH DANH PHÂN VÙNG & NVS NAMESPACE
// ============================================================================
#define BOARD_LITTLEFS_LABEL    "littlefs"
#define BOARD_LITTLEFS_MOUNT    "/lfs"
#define BOARD_NVS_NS_DEVICE     "dev_cfg"
#define BOARD_NVS_NS_AUDIO      "audio_cfg"
#define BOARD_NVS_NS_ZCLAW      "zclaw_cfg"

// ============================================================================
// 5. QUẢN LÝ BỘ NHỚ ĐỘNG (Tự động ưu tiên PSRAM nếu có, Fallback về SRAM)
// ============================================================================

/**
 * @brief Kiểm tra động xem ESP32-S3 SuperMini có PSRAM hoạt động hay không.
 */
static inline bool board_has_psram(void)
{
    return esp_psram_is_initialized();
}

/**
 * @brief Cấp phát bộ nhớ ưu tiên PSRAM (MALLOC_CAP_SPIRAM), tự động lùi về SRAM nếu không có PSRAM.
 */
static inline void* board_malloc_prefer_spiram(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    if (esp_psram_is_initialized()) {
        void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr != NULL) {
            return ptr;
        }
    }
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief Cấp phát và xóa 0 bộ nhớ ưu tiên PSRAM.
 */
static inline void* board_calloc_prefer_spiram(size_t num, size_t size)
{
    if (num == 0 || size == 0) {
        return NULL;
    }
    if (esp_psram_is_initialized()) {
        void* ptr = heap_caps_calloc(num, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr != NULL) {
            return ptr;
        }
    }
    return heap_caps_calloc(num, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief Thay đổi kích thước vùng nhớ đã cấp phát, giữ ưu tiên PSRAM nếu khả dụng.
 */
static inline void* board_realloc_prefer_spiram(void* ptr, size_t new_size)
{
    if (new_size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }
    if (esp_psram_is_initialized()) {
        void* new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_ptr != NULL) {
            return new_ptr;
        }
    }
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief Cấp phát buffer DMA trong Internal SRAM cho I2S/SPI.
 */
static inline void* board_malloc_dma(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

/**
 * @brief Giải phóng vùng nhớ cấp phát bởi board_malloc_* / heap_caps_*.
 */
static inline void board_free(void* ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

// ============================================================================
// 6. XỬ LÝ XUNG ĐỘT GPIO 44 (U0RXD -> SPK_DOUT)
// ============================================================================

/**
 * @brief Giải phóng GPIO 44 khỏi UART0 RX (U0RXD) trước khi khởi tạo I2S_NUM_1.
 *        Bắt buộc gọi ở đầu app_main() và trước khi mở I2S TX cho MAX98357A.
 */
static inline esp_err_t board_release_gpio44(void)
{
    if (uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_delete(UART_NUM_0);
    }
    // Ngắt hoàn toàn tín hiệu U0RXD khỏi chân vật lý ở tầng GPIO Matrix
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U0RXD_IN_IDX, false);

    // Đưa GPIO44 về cấu hình GPIO thuần túy (xóa mọi IOMUX/pull mặc định của UART0)
    esp_err_t err = gpio_reset_pin(SPK_DOUT_GPIO);
    if (err != ESP_OK) {
        return err;
    }
    gpio_pullup_dis(SPK_DOUT_GPIO);
    gpio_pulldown_dis(SPK_DOUT_GPIO);
    return ESP_OK;
}

// ============================================================================
// 7. CẤU HÌNH MÀN HÌNH ST7789 (SPI MODE 3 + XOAY 0/90/180/270)
// ============================================================================

typedef struct {
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    int gap_x;
    int gap_y;
} board_lcd_rotation_t;

/**
 * @brief Trả về cấu hình SPI Bus cho ST7789 trên SPI3_HOST (không dùng MISO).
 */
static inline spi_bus_config_t board_get_lcd_spi_bus_config(void)
{
    spi_bus_config_t bus_cfg;
    memset(&bus_cfg, 0, sizeof(bus_cfg));
    bus_cfg.mosi_io_num = LCD_MOSI_GPIO;
    bus_cfg.miso_io_num = GPIO_NUM_NC;
    bus_cfg.sclk_io_num = LCD_SCLK_GPIO;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.data4_io_num = GPIO_NUM_NC;
    bus_cfg.data5_io_num = GPIO_NUM_NC;
    bus_cfg.data6_io_num = GPIO_NUM_NC;
    bus_cfg.data7_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = LCD_WIDTH * LCD_DMA_LINES * sizeof(uint16_t);
    return bus_cfg;
}

/**
 * @brief Trả về cấu hình Panel IO SPI cho ST7789 không có chân CS (spi_mode = 3).
 */
static inline esp_lcd_panel_io_spi_config_t board_get_lcd_panel_io_config(void)
{
    esp_lcd_panel_io_spi_config_t io_cfg;
    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.cs_gpio_num = LCD_CS_GPIO;         // -1 (GPIO_NUM_NC)
    io_cfg.dc_gpio_num = LCD_DC_GPIO;
    io_cfg.spi_mode = LCD_SPI_MODE;           // BẮT BUỘC = 3 khi CS = -1
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    return io_cfg;
}

/**
 * @brief Quy đổi góc xoay 0/90/180/270 thành tham số ST7789 240x240 (GRAM nội 240x320).
 */
static inline board_lcd_rotation_t board_get_st7789_rotation_params(uint16_t degree)
{
    board_lcd_rotation_t r = {false, false, false, 0, 0};
    switch (degree % 360U) {
        case 90:
            r.swap_xy = true;
            r.mirror_x = true;
            r.mirror_y = false;
            r.gap_x = 0;
            r.gap_y = 0;
            break;
        case 180:
            r.swap_xy = false;
            r.mirror_x = true;
            r.mirror_y = true;
            r.gap_x = 0;
            r.gap_y = 80;   // ST7789 GRAM 240x320 cần dịch 80px khi lật 180 độ
            break;
        case 270:
            r.swap_xy = true;
            r.mirror_x = false;
            r.mirror_y = true;
            r.gap_x = 80;   // Dịch 80px theo trục X sau khi swap_xy
            r.gap_y = 0;
            break;
        case 0:
        default:
            r.swap_xy = false;
            r.mirror_x = false;
            r.mirror_y = false;
            r.gap_x = 0;
            r.gap_y = 0;
            break;
    }
    return r;
}

/**
 * @brief Áp dụng góc xoay màn hình (0, 90, 180, 270) lên esp_lcd_panel_handle_t.
 */
static inline esp_err_t board_apply_lcd_rotation(esp_lcd_panel_handle_t panel, uint16_t degree)
{
    if (panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const board_lcd_rotation_t rot = board_get_st7789_rotation_params(degree);
    esp_err_t err = esp_lcd_panel_swap_xy(panel, rot.swap_xy);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_mirror(panel, rot.mirror_x, rot.mirror_y);
    if (err != ESP_OK) {
        return err;
    }
    return esp_lcd_panel_set_gap(panel, rot.gap_x, rot.gap_y);
}

// ============================================================================
// 8. KHỞI TẠO 3 NÚT NHẤN NGOẠI VI (SAU KHI BOOT)
// ============================================================================

/**
 * @brief Khởi tạo 3 nút nhấn BTN_MAIN (GPIO3), BTN_VOL_UP (GPIO4), BTN_VOL_DOWN (GPIO5).
 * @note  GPIO3 là Strapping Pin trên ESP32-S3, chỉ gọi hàm này sau khi hệ thống đã boot xong.
 */
static inline esp_err_t board_init_buttons(void)
{
    const gpio_num_t pins[3] = {BTN_MAIN_GPIO, BTN_VOL_UP_GPIO, BTN_VOL_DOWN_GPIO};
    const gpio_pull_mode_t pull_mode =
        (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY;

    for (size_t i = 0; i < 3; ++i) {
        esp_err_t err = gpio_reset_pin(pins[i]);
        if (err != ESP_OK) {
            return err;
        }
        err = gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        if (err != ESP_OK) {
            return err;
        }
        err = gpio_set_pull_mode(pins[i], pull_mode);
        if (err != ESP_OK) {
            return err;
        }
        err = gpio_set_intr_type(pins[i], GPIO_INTR_DISABLE);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/**
 * @brief Đọc trạng thái vật lý tức thời của nút nhấn (true = đang nhấn).
 */
static inline bool board_read_button_raw(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == BUTTON_ACTIVE_LEVEL;
}

#ifdef __cplusplus
}
#endif

