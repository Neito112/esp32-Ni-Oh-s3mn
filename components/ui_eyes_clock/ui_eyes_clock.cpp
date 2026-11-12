// ui_eyes_clock.cpp
// S.W.Ni-Oh — ST7789 240x240 LVGL v8.4 / ESP-IDF v5.x
// Production-ready C++17 implementation — no TODOs, no pseudo-code.

#include "ui_eyes_clock.h"
#include "board_config.h"
#include "audio_hal.h"
#include "vn_fonts.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char* TAG = "UiEyes";

namespace zclaw {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint16_t LCD_W           = LCD_WIDTH;
static constexpr uint16_t LCD_H           = LCD_HEIGHT;
static constexpr uint32_t LVGL_TASK_STACK = 8192;
static constexpr uint32_t LVGL_TASK_PRIO  = 4;
static constexpr uint32_t LVGL_TICK_MS    = 5;

// Hình học đôi mắt (màn hình 240x240)
static constexpr int EYE_W       = 56;
static constexpr int EYE_H       = 56;
static constexpr int EYE_RAD     = 15;
static constexpr int EYE_LEFT_X  = 48;
static constexpr int EYE_RIGHT_X = 136;
static constexpr int EYE_Y       = 62;

// Bảng màu Chatbot Xanh Dương
static constexpr uint32_t COLOR_EYE      = 0x00B4FF;
static constexpr uint32_t COLOR_EYE_GRAD = 0x00E5FF;
static constexpr uint32_t COLOR_SHADOW   = 0x0077FF;
static constexpr uint32_t COLOR_BLACK    = 0x000000;
static constexpr uint32_t COLOR_WHITE    = 0xFFFFFF;
static constexpr uint32_t COLOR_BG       = 0x000000;

// Sóng âm 11 thanh dọc tại Y = 175
static constexpr int WAVE_BAR_W        = 6;
static constexpr int WAVE_BAR_GAP      = 6;
static constexpr int WAVE_BAR_Y        = 175;
static constexpr int WAVE_N_BARS       = 11;
static constexpr int WAVE_X_START      = 57;   // (240 - (11*6 + 10*6)) / 2
static constexpr int WAVE_MAX_H_CENTER = 40;

// Thời lượng hiển thị OSD & Popup
static constexpr uint32_t VOL_OSD_DURATION_MS = 2000;
static constexpr uint32_t NOTIF_DURATION_MS   = 5000;

// NVS keys
static constexpr const char* NVS_KEY_BACKLIGHT = "lcd_blk";
static constexpr const char* NVS_KEY_ROTATION  = "lcd_rot";

// ---------------------------------------------------------------------------
// Lunar calendar helpers (Thuật toán Hồ Ngọc Đức cho Âm lịch Việt Nam)
// ---------------------------------------------------------------------------
struct LunarDate {
    int  lunar_day;
    int  lunar_month;
    int  lunar_year;
    bool is_leap;
};

static int jdn_from_greg(int day, int month, int year) {
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    int jd = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
    if (jd < 2299161) {
        jd = day + (153 * m + 2) / 5 + 365 * y + y / 4 - 32083;
    }
    return jd;
}

static double new_moon_jdn(int k) {
    double T  = static_cast<double>(k) / 1236.85;
    double T2 = T * T;
    double T3 = T2 * T;
    double dr = M_PI / 180.0;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
    Jd1 += 0.00033 * std::sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    double M  = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    double F  = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1 = (0.1734 - 0.000393 * T) * std::sin(M * dr) + 0.0021 * std::sin(2.0 * dr * M);
    C1 = C1 - 0.4068 * std::sin(Mpr * dr) + 0.0161 * std::sin(dr * 2.0 * Mpr);
    C1 = C1 - 0.0004 * std::sin(dr * 3.0 * Mpr);
    C1 = C1 + 0.0104 * std::sin(dr * 2.0 * F) - 0.0051 * std::sin(dr * (M + Mpr));
    C1 = C1 - 0.0074 * std::sin(dr * (M - Mpr)) + 0.0004 * std::sin(dr * (2.0 * F + M));
    C1 = C1 - 0.0004 * std::sin(dr * (2.0 * F - M)) - 0.0006 * std::sin(dr * (2.0 * F + Mpr));
    C1 = C1 + 0.0010 * std::sin(dr * (2.0 * F - Mpr)) + 0.0005 * std::sin(dr * (2.0 * Mpr + M));
    double delta_t;
    if (T < -11.0) {
        delta_t = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
    } else {
        delta_t = -0.000278 + 0.000265 * T + 0.000262 * T2;
    }
    return Jd1 + C1 - delta_t;
}

static int get_new_moon_day_tz(int k, double tz_hours) {
    return static_cast<int>(std::floor(new_moon_jdn(k) + 0.5 + tz_hours / 24.0));
}

static double sun_longitude_deg(double jdn) {
    double T  = (jdn - 2451545.0) / 36525.0;
    double T2 = T * T;
    double dr = M_PI / 180.0;
    double M  = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * std::sin(dr * M);
    DL += (0.019993 - 0.000101 * T) * std::sin(dr * 2.0 * M) + 0.000290 * std::sin(dr * 3.0 * M);
    double L = (L0 + DL) * dr;
    L = L - M_PI * 2.0 * std::floor(L / (M_PI * 2.0));
    return (L / dr);
}

static int get_sun_longitude_sector(int day_number, double tz_hours) {
    double sl = sun_longitude_deg(static_cast<double>(day_number) - 0.5 - tz_hours / 24.0);
    return static_cast<int>(std::floor(sl / 30.0));
}

static int get_lunar_month11(int yy, double tz_hours) {
    int off = jdn_from_greg(31, 12, yy) - 2415021;
    int k   = static_cast<int>(std::floor(static_cast<double>(off) / 29.530588853));
    int nm  = get_new_moon_day_tz(k, tz_hours);
    int sun_long = get_sun_longitude_sector(nm, tz_hours);
    if (sun_long >= 9) {
        nm = get_new_moon_day_tz(k - 1, tz_hours);
    }
    return nm;
}

static int get_leap_month_offset(int a11, double tz_hours) {
    int k = static_cast<int>(std::floor((a11 - 2415021.076998695) / 29.530588853 + 0.5));
    int last = 0;
    int i    = 1;
    int arc  = get_sun_longitude_sector(get_new_moon_day_tz(k + i, tz_hours), tz_hours);
    do {
        last = arc;
        i++;
        arc = get_sun_longitude_sector(get_new_moon_day_tz(k + i, tz_hours), tz_hours);
    } while (arc != last && i < 14);
    return i - 1;
}

static LunarDate gregorian_to_viet_lunar(int dd, int mm, int yy) {
    constexpr double TZ = 7.0; // ICT+7 Việt Nam
    int jdn = jdn_from_greg(dd, mm, yy);
    int k   = static_cast<int>(std::floor((jdn - 2415021.076998695) / 29.530588853));
    int month_start = get_new_moon_day_tz(k + 1, TZ);
    if (month_start > jdn) {
        month_start = get_new_moon_day_tz(k, TZ);
    }

    int a11 = get_lunar_month11(yy, TZ);
    int b11 = a11;
    int lunar_year;
    if (a11 >= month_start) {
        lunar_year = yy;
        a11        = get_lunar_month11(yy - 1, TZ);
    } else {
        lunar_year = yy + 1;
        b11        = get_lunar_month11(yy + 1, TZ);
    }

    int lunar_day   = jdn - month_start + 1;
    int diff        = static_cast<int>(std::floor((month_start - a11) / 29.0));
    bool lunar_leap = false;
    int lunar_month = diff + 11;

    if (b11 - a11 > 365) {
        int leap_month_diff = get_leap_month_offset(a11, TZ);
        if (diff >= leap_month_diff) {
            lunar_month = diff + 10;
            if (diff == leap_month_diff) {
                lunar_leap = true;
            }
        }
    }
    if (lunar_month > 12) {
        lunar_month -= 12;
    }
    if (lunar_month >= 11 && diff < 4) {
        lunar_year -= 1;
    }

    return LunarDate{lunar_day, lunar_month, lunar_year, lunar_leap};
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
UiEyesClock& UiEyesClock::instance() {
    static UiEyesClock inst;
    return inst;
}

// ---------------------------------------------------------------------------
// init()
// ---------------------------------------------------------------------------
esp_err_t UiEyesClock::init() {
    if (initialized_.load()) return ESP_OK;

    // Đọc cấu hình UI đã lưu trong NVS (độ sáng & góc xoay)
    {
        nvs_handle_t h = 0;
        if (nvs_open(BOARD_NVS_NS_DEVICE, NVS_READONLY, &h) == ESP_OK) {
            uint8_t blk = 80;
            if (nvs_get_u8(h, NVS_KEY_BACKLIGHT, &blk) == ESP_OK) {
                backlight_pct_.store(std::min<uint8_t>(blk, 100));
            }
            uint16_t rot = 0;
            if (nvs_get_u16(h, NVS_KEY_ROTATION, &rot) == ESP_OK) {
                rotation_deg_.store(rot % 360U);
            }
            nvs_close(h);
        }
    }

    esp_err_t ret = init_spi_lcd();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo SPI ST7789 thất bại: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = init_ledc_backlight();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo LEDC Backlight thất bại: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = init_lvgl();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo LVGL thất bại: %s", esp_err_to_name(ret));
        return ret;
    }

    // Dựng màn hình trong lock để đồng bộ hoàn toàn trước khi lvgl_task chạy vẽ
    if (lvgl_lock(200)) {
        build_chat_screen();
        build_clock_screen();
        build_video_screen();
        build_overlays();

        lv_disp_load_scr(scr_chat_);
        clock_screen_active_.store(false);
        lvgl_unlock();
    }

    apply_backlight_hardware(backlight_pct_.load());

    initialized_.store(true);
    ESP_LOGI(TAG, "UiEyesClock sẵn sàng (240x240, SPI Mode 3, rot=%u°)", rotation_deg_.load());
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// deinit()
// ---------------------------------------------------------------------------
void UiEyesClock::deinit() {
    if (!initialized_.load()) return;
    exit_video_mode();
    initialized_.store(false);
    task_running_.store(false);

    // Chờ lvgl_task thoát an toàn khỏi vòng lặp
    for (int i = 0; i < 30 && lvgl_task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (lvgl_task_handle_) {
        vTaskDelete(lvgl_task_handle_);
        lvgl_task_handle_ = nullptr;
    }
    if (lvgl_mutex_) {
        vSemaphoreDelete(lvgl_mutex_);
        lvgl_mutex_ = nullptr;
    }
    board_free(draw_buf1_);
    draw_buf1_ = nullptr;
    board_free(draw_buf2_);
    draw_buf2_ = nullptr;

    if (panel_handle_) {
        esp_lcd_panel_del(panel_handle_);
        panel_handle_ = nullptr;
    }
    if (io_handle_) {
        esp_lcd_panel_io_del(io_handle_);
        io_handle_ = nullptr;
    }
    spi_bus_free(LCD_SPI_HOST);
}

// ---------------------------------------------------------------------------
// init_spi_lcd()
// ---------------------------------------------------------------------------
esp_err_t UiEyesClock::init_spi_lcd() {
    spi_bus_config_t bus = board_get_lcd_spi_bus_config();
    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }

    // Bắt buộc spi_mode = 3 và cs_gpio_num = -1 theo board_config.h
    esp_lcd_panel_io_spi_config_t io_cfg = board_get_lcd_panel_io_config();
    io_cfg.on_color_trans_done = lvgl_flush_ready_cb;
    io_cfg.user_ctx            = this;

    ret = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(LCD_SPI_HOST), &io_cfg, &io_handle_);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_RST_GPIO;
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;

    ret = esp_lcd_new_panel_st7789(io_handle_, &panel_cfg, &panel_handle_);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle_, true));
    ESP_ERROR_CHECK(board_apply_lcd_rotation(panel_handle_, rotation_deg_.load()));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle_, true));

    ESP_LOGI(TAG, "ST7789 SPI3 khởi tạo xong (Mode %d, CS=%d)", LCD_SPI_MODE, (int)LCD_CS_GPIO);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// init_ledc_backlight()
// ---------------------------------------------------------------------------
esp_err_t UiEyesClock::init_ledc_backlight() {
    ledc_timer_config_t timer = {};
    timer.speed_mode      = LCD_BLK_LEDC_MODE;
    timer.duty_resolution = LCD_BLK_LEDC_DUTY_RES;
    timer.timer_num       = LCD_BLK_LEDC_TIMER;
    timer.freq_hz         = LCD_BLK_LEDC_FREQ_HZ;
    timer.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t ret = ledc_timer_config(&timer);
    if (ret != ESP_OK) return ret;

    ledc_channel_config_t ch = {};
    ch.gpio_num   = LCD_BLK_GPIO;
    ch.speed_mode = LCD_BLK_LEDC_MODE;
    ch.channel    = LCD_BLK_LEDC_CHANNEL;
    ch.intr_type  = LEDC_INTR_DISABLE;
    ch.timer_sel  = LCD_BLK_LEDC_TIMER;
    ch.duty       = 0; // Bật sáng sau khi vẽ xong màn hình đầu tiên
    ch.hpoint     = 0;
    return ledc_channel_config(&ch);
}

// ---------------------------------------------------------------------------
// init_lvgl()
// ---------------------------------------------------------------------------
esp_err_t UiEyesClock::init_lvgl() {
    const size_t pixel_cnt = static_cast<size_t>(LCD_W) * LCD_DMA_LINES;
    const size_t buf_sz    = pixel_cnt * sizeof(lv_color_t);

    // Nếu có PSRAM dùng heap PSRAM, nếu không có PSRAM fallback DMA internal SRAM
    if (board_has_psram()) {
        draw_buf1_ = static_cast<lv_color_t*>(board_malloc_prefer_spiram(buf_sz));
        draw_buf2_ = static_cast<lv_color_t*>(board_malloc_prefer_spiram(buf_sz));
    } else {
        draw_buf1_ = static_cast<lv_color_t*>(board_malloc_dma(buf_sz));
        draw_buf2_ = static_cast<lv_color_t*>(board_malloc_dma(buf_sz));
    }

    if (!draw_buf1_) {
        ESP_LOGE(TAG, "Không đủ RAM cấp phát LVGL buffer (%u bytes)", (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }

    // Cho phép chạy single-buffer nếu SRAM không PSRAM bị chật
    const char* mode_str = board_has_psram()
        ? (draw_buf2_ ? "PSRAM Dual" : "PSRAM Single")
        : (draw_buf2_ ? "DMA-SRAM Dual" : "DMA-SRAM Single");
    ESP_LOGI(TAG, "LVGL framebuffer: %s (%u bytes/buf)", mode_str, (unsigned)buf_sz);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf_, draw_buf1_, draw_buf2_, pixel_cnt);

    lv_disp_drv_init(&disp_drv_);
    disp_drv_.flush_cb  = lvgl_flush_cb;
    disp_drv_.draw_buf  = &draw_buf_;
    disp_drv_.hor_res   = LCD_W;
    disp_drv_.ver_res   = LCD_H;
    disp_drv_.user_data = this;
    lvgl_disp_ = lv_disp_drv_register(&disp_drv_);

    lvgl_mutex_ = xSemaphoreCreateMutex();
    if (!lvgl_mutex_) {
        ESP_LOGE(TAG, "Không tạo được LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    task_running_.store(true);
    BaseType_t rc = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", LVGL_TASK_STACK, this,
        LVGL_TASK_PRIO, &lvgl_task_handle_, 1);
    if (rc != pdPASS) {
        task_running_.store(false);
        ESP_LOGE(TAG, "Không tạo được LVGL task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// build_chat_screen() — Mắt vuông bo tròn xanh dương + 11 thanh sóng âm
// ---------------------------------------------------------------------------
void UiEyesClock::build_chat_screen() {
    scr_chat_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_chat_, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_chat_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr_chat_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_brand_chat = lv_label_create(scr_chat_);
    lv_label_set_text(lbl_brand_chat, "S.W.Ni-Oh");
    lv_obj_set_style_text_font(lbl_brand_chat, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_brand_chat, lv_color_hex(0x2F7CA8), LV_PART_MAIN);
    lv_obj_align(lbl_brand_chat, LV_ALIGN_TOP_MID, 0, 8);

    auto make_eye = [&](int x, int y) -> lv_obj_t* {
        lv_obj_t* e = lv_obj_create(scr_chat_);
        lv_obj_set_size(e, EYE_W, EYE_H);
        lv_obj_set_pos(e, x, y);
        lv_obj_set_style_radius(e, EYE_RAD, LV_PART_MAIN);
        lv_obj_set_style_bg_color(e, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(e, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(e, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(e, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(e, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(e, 18, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(e, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(e, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_x(e, EYE_W / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(e, EYE_H / 2, LV_PART_MAIN);
        lv_obj_clear_flag(e, LV_OBJ_FLAG_SCROLLABLE);
        return e;
    };

    eye_left_  = make_eye(EYE_LEFT_X,  EYE_Y);
    eye_right_ = make_eye(EYE_RIGHT_X, EYE_Y);

    // Cung cười ^ ^ thuần hình học vector (không dùng vùng đen che đè lên mắt)
    auto make_happy_arc = [&](int x, int y) -> lv_obj_t* {
        lv_obj_t* arc = lv_arc_create(scr_chat_);
        lv_obj_set_size(arc, EYE_W + 4, EYE_H + 4);
        lv_obj_set_pos(arc, x - 2, y + 6);
        lv_arc_set_bg_angles(arc, 0, 0);
        lv_arc_set_angles(arc, 200, 340);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 100);
        lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_EYE_GRAD), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 11, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        return arc;
    };

    eye_happy_left_  = make_happy_arc(EYE_LEFT_X,  EYE_Y);
    eye_happy_right_ = make_happy_arc(EYE_RIGHT_X, EYE_Y);

    // 11 thanh sóng âm màu xanh dương đồng bộ với mắt
    for (int i = 0; i < WAVE_N_BARS; i++) {
        int x = WAVE_X_START + i * (WAVE_BAR_W + WAVE_BAR_GAP);
        int y = WAVE_BAR_Y + (WAVE_MAX_H_CENTER - 4) / 2;
        lv_obj_t* b = lv_obj_create(scr_chat_);
        lv_obj_set_size(b, WAVE_BAR_W, 4);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        wave_bars_[i] = b;
    }

    // Banner phát nhạc [YouTube] / [SoundCloud] / [Radio] nằm gọn giữa mắt và sóng âm
    music_banner_ = lv_obj_create(scr_chat_);
    lv_obj_set_size(music_banner_, 224, 26);
    lv_obj_set_pos(music_banner_, 8, 136);
    lv_obj_set_style_bg_color(music_banner_, lv_color_hex(0x0D1E2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(music_banner_, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_radius(music_banner_, 8, LV_PART_MAIN);
    lv_obj_set_style_border_color(music_banner_, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
    lv_obj_set_style_border_width(music_banner_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(music_banner_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(music_banner_, 2, LV_PART_MAIN);
    lv_obj_clear_flag(music_banner_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(music_banner_, LV_OBJ_FLAG_HIDDEN);

    lbl_music_title_ = lv_label_create(music_banner_);
    lv_label_set_text(lbl_music_title_, "");
    lv_label_set_long_mode(lbl_music_title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_music_title_, 210);
    lv_obj_set_style_text_font(lbl_music_title_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_music_title_, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
    lv_obj_align(lbl_music_title_, LV_ALIGN_CENTER, 0, 0);
}

// ---------------------------------------------------------------------------
// build_video_screen() — Nền đen + thanh tiêu đề video 3 giây đầu tại Y=212..240
// ---------------------------------------------------------------------------
void UiEyesClock::build_video_screen() {
    scr_video_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_video_, lv_color_hex(COLOR_BLACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_video_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr_video_, LV_OBJ_FLAG_SCROLLABLE);

    video_title_bar_ = lv_obj_create(scr_video_);
    lv_obj_set_size(video_title_bar_, LCD_W, 28);
    lv_obj_set_pos(video_title_bar_, 0, 212);
    lv_obj_set_style_bg_color(video_title_bar_, lv_color_hex(0x0E1D2B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(video_title_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(video_title_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(video_title_bar_, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(video_title_bar_, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
    lv_obj_set_style_border_width(video_title_bar_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(video_title_bar_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(video_title_bar_, 4, LV_PART_MAIN);
    lv_obj_clear_flag(video_title_bar_, LV_OBJ_FLAG_SCROLLABLE);

    lbl_video_title_ = lv_label_create(video_title_bar_);
    lv_label_set_text(lbl_video_title_, "Đang tải video...");
    lv_label_set_long_mode(lbl_video_title_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_video_title_, LCD_W - 12);
    lv_obj_set_style_text_font(lbl_video_title_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_video_title_, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_align(lbl_video_title_, LV_ALIGN_LEFT_MID, 0, 0);
}

// ---------------------------------------------------------------------------
// build_overlays() — Tạo Volume OSD và Notification Popup trên lv_layer_top()
//                    để hiển thị xuyên suốt cả màn hình Mắt và Đồng hồ
// ---------------------------------------------------------------------------
void UiEyesClock::build_overlays() {
    lv_obj_t* top = lv_layer_top();

    // --- Thanh Volume OSD ---
    volume_osd_ = lv_obj_create(top);
    lv_obj_set_size(volume_osd_, 176, 46);
    lv_obj_align(volume_osd_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(volume_osd_, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_osd_, lv_color_hex(0x16222E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_osd_, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(volume_osd_, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
    lv_obj_set_style_border_width(volume_osd_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(volume_osd_, 4, LV_PART_MAIN);
    lv_obj_clear_flag(volume_osd_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_osd_, LV_OBJ_FLAG_HIDDEN);

    volume_label_ = lv_label_create(volume_osd_);
    lv_label_set_text(volume_label_, "Âm lượng: 75%");
    lv_obj_set_style_text_font(volume_label_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(volume_label_, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_align(volume_label_, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t* bar_track = lv_obj_create(volume_osd_);
    lv_obj_set_size(bar_track, 140, 7);
    lv_obj_align(bar_track, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(bar_track, lv_color_hex(0x334455), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_track, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_track, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_track, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_SCROLLABLE);

    volume_bar_ = lv_obj_create(bar_track);
    lv_obj_set_size(volume_bar_, 105, 7);
    lv_obj_align(volume_bar_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(volume_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(volume_bar_, 4, LV_PART_MAIN);

    // --- Popup Thông báo Chronos / ESP-NOW ---
    notif_popup_ = lv_obj_create(top);
    lv_obj_set_size(notif_popup_, 224, 58);
    lv_obj_set_pos(notif_popup_, 8, 174);
    lv_obj_set_style_bg_color(notif_popup_, lv_color_hex(0x18232E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(notif_popup_, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_radius(notif_popup_, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(notif_popup_, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
    lv_obj_set_style_border_width(notif_popup_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(notif_popup_, 4, LV_PART_MAIN);
    lv_obj_clear_flag(notif_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(notif_popup_, LV_OBJ_FLAG_HIDDEN);

    lbl_notif_app_ = lv_label_create(notif_popup_);
    lv_label_set_text(lbl_notif_app_, LV_SYMBOL_BELL " Thông báo");
    lv_obj_set_style_text_font(lbl_notif_app_, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_notif_app_, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
    lv_obj_align(lbl_notif_app_, LV_ALIGN_TOP_LEFT, 4, 0);

    lbl_notif_title_ = lv_label_create(notif_popup_);
    lv_label_set_text(lbl_notif_title_, "");
    lv_label_set_long_mode(lbl_notif_title_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_notif_title_, 208);
    lv_obj_set_style_text_font(lbl_notif_title_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_notif_title_, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_align(lbl_notif_title_, LV_ALIGN_TOP_LEFT, 4, 15);

    lbl_notif_body_ = lv_label_create(notif_popup_);
    lv_label_set_text(lbl_notif_body_, "");
    lv_label_set_long_mode(lbl_notif_body_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_notif_body_, 208);
    lv_obj_set_style_text_font(lbl_notif_body_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_notif_body_, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_align(lbl_notif_body_, LV_ALIGN_TOP_LEFT, 4, 33);
}

// ---------------------------------------------------------------------------
// build_clock_screen() — Đồng hồ + Âm lịch Việt Nam + Thời tiết + Chỉ đường
// ---------------------------------------------------------------------------
void UiEyesClock::build_clock_screen() {
    scr_clock_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_clock_, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_clock_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr_clock_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_brand_clock = lv_label_create(scr_clock_);
    lv_label_set_text(lbl_brand_clock, "S.W.Ni-Oh");
    lv_obj_set_style_text_font(lbl_brand_clock, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_brand_clock, lv_color_hex(0x2F7CA8), LV_PART_MAIN);
    lv_obj_align(lbl_brand_clock, LV_ALIGN_TOP_MID, 0, 3);

    // Giờ:Phút lớn
    lbl_time_ = lv_label_create(scr_clock_);
    lv_label_set_text(lbl_time_, "--:--");
    lv_obj_set_style_text_font(lbl_time_, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time_, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_align(lbl_time_, LV_ALIGN_TOP_MID, -18, 18);

    // Giây
    lbl_seconds_ = lv_label_create(scr_clock_);
    lv_label_set_text(lbl_seconds_, ":00");
    lv_obj_set_style_text_font(lbl_seconds_, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_seconds_, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
    lv_obj_align_to(lbl_seconds_, lbl_time_, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);

    // Ngày Dương lịch
    lbl_date_ = lv_label_create(scr_clock_);
    lv_label_set_text(lbl_date_, "Đang đồng bộ giờ...");
    lv_obj_set_style_text_font(lbl_date_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_date_, lv_color_hex(0xDDDDDD), LV_PART_MAIN);
    lv_obj_align(lbl_date_, LV_ALIGN_TOP_MID, 0, 74);

    // Ngày Âm lịch Việt Nam
    lbl_lunar_ = lv_label_create(scr_clock_);
    lv_label_set_text(lbl_lunar_, "Âm lịch: --/--");
    lv_obj_set_style_text_font(lbl_lunar_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_lunar_, lv_color_hex(0xFFD700), LV_PART_MAIN);
    lv_obj_align(lbl_lunar_, LV_ALIGN_TOP_MID, 0, 96);

    // Badge Thời tiết (nền bo tròn tinh gọn)
    weather_badge_ = lv_obj_create(scr_clock_);
    lv_obj_set_size(weather_badge_, 220, 40);
    lv_obj_set_pos(weather_badge_, 10, 122);
    lv_obj_set_style_bg_color(weather_badge_, lv_color_hex(0x101C28), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(weather_badge_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(weather_badge_, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(weather_badge_, lv_color_hex(0x1E3A52), LV_PART_MAIN);
    lv_obj_set_style_border_width(weather_badge_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(weather_badge_, 6, LV_PART_MAIN);
    lv_obj_clear_flag(weather_badge_, LV_OBJ_FLAG_SCROLLABLE);

    // Dot chỉ báo trạng thái thời tiết (Nắng vàng / Mây xám / Mưa xanh)
    lbl_weather_icon_ = lv_obj_create(weather_badge_);
    lv_obj_set_size(lbl_weather_icon_, 14, 14);
    lv_obj_set_style_radius(lbl_weather_icon_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lbl_weather_icon_, lv_color_hex(0xFFD700), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lbl_weather_icon_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lbl_weather_icon_, 0, LV_PART_MAIN);
    lv_obj_align(lbl_weather_icon_, LV_ALIGN_LEFT_MID, 2, 0);

    lbl_temp_ = lv_label_create(weather_badge_);
    lv_label_set_text(lbl_temp_, "--°C");
    lv_obj_set_style_text_font(lbl_temp_, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_temp_, lv_color_hex(0x87CEEB), LV_PART_MAIN);
    lv_obj_align(lbl_temp_, LV_ALIGN_LEFT_MID, 24, 0);

    lbl_humidity_ = lv_label_create(weather_badge_);
    lv_label_set_text(lbl_humidity_, "--%");
    lv_obj_set_style_text_font(lbl_humidity_, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_humidity_, lv_color_hex(0xA0C8E0), LV_PART_MAIN);
    lv_obj_align(lbl_humidity_, LV_ALIGN_LEFT_MID, 82, 0);

    lbl_weather_cond_ = lv_label_create(weather_badge_);
    lv_label_set_text(lbl_weather_cond_, "Chờ dữ liệu");
    lv_label_set_long_mode(lbl_weather_cond_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_weather_cond_, 86);
    lv_obj_set_style_text_align(lbl_weather_cond_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weather_cond_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_weather_cond_, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_align(lbl_weather_cond_, LV_ALIGN_RIGHT_MID, -2, 0);

    // Khung chỉ đường Google Maps từ Chronos
    nav_container_ = lv_obj_create(scr_clock_);
    lv_obj_set_size(nav_container_, 220, 56);
    lv_obj_set_pos(nav_container_, 10, 170);
    lv_obj_set_style_bg_color(nav_container_, lv_color_hex(0x133352), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nav_container_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(nav_container_, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(nav_container_, lv_color_hex(COLOR_EYE), LV_PART_MAIN);
    lv_obj_set_style_border_width(nav_container_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nav_container_, 6, LV_PART_MAIN);
    lv_obj_clear_flag(nav_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nav_container_, LV_OBJ_FLAG_HIDDEN);

    lbl_nav_direction_ = lv_label_create(nav_container_);
    lv_label_set_text(lbl_nav_direction_, LV_SYMBOL_UP " Đi thẳng");
    lv_obj_set_style_text_font(lbl_nav_direction_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_nav_direction_, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_align(lbl_nav_direction_, LV_ALIGN_TOP_LEFT, 2, 0);

    lbl_nav_distance_ = lv_label_create(nav_container_);
    lv_label_set_text(lbl_nav_distance_, "0 m");
    lv_obj_set_style_text_font(lbl_nav_distance_, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_nav_distance_, lv_color_hex(COLOR_EYE_GRAD), LV_PART_MAIN);
    lv_obj_align(lbl_nav_distance_, LV_ALIGN_TOP_RIGHT, -2, 0);

    lbl_nav_street_ = lv_label_create(nav_container_);
    lv_label_set_text(lbl_nav_street_, "");
    lv_label_set_long_mode(lbl_nav_street_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_nav_street_, 204);
    lv_obj_set_style_text_font(lbl_nav_street_, VN_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_nav_street_, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_align(lbl_nav_street_, LV_ALIGN_BOTTOM_LEFT, 2, 0);
}

// ---------------------------------------------------------------------------
// LVGL task
// ---------------------------------------------------------------------------
void UiEyesClock::lvgl_task(void* arg) {
    auto* self = static_cast<UiEyesClock*>(arg);
    uint32_t last_tick = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

    while (self->task_running_.load()) {
        uint32_t now     = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        uint32_t elapsed = now - last_tick;
        last_tick        = now;

        if (xSemaphoreTake(self->lvgl_mutex_, pdMS_TO_TICKS(15)) == pdTRUE) {
            lv_tick_inc(elapsed);

            if (!self->video_mode_.load(std::memory_order_acquire)) {
                lv_timer_handler();

                if (!self->clock_screen_active_.load()) {
                    self->tick_blink_and_glance(now);
                    self->tick_waveform();
                } else {
                    self->tick_clock_update(false);
                }
                self->tick_notif_dismiss(now);
                self->tick_vol_osd_dismiss(now);
            }

            xSemaphoreGive(self->lvgl_mutex_);
        }
        vTaskDelay(pdMS_TO_TICKS(self->video_mode_.load(std::memory_order_relaxed) ? 20 : LVGL_TICK_MS));
    }

    self->lvgl_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Flush callbacks
// ---------------------------------------------------------------------------
bool UiEyesClock::lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t /*panel_io*/,
                                       esp_lcd_panel_io_event_data_t* /*edata*/,
                                       void* user_ctx) {
    auto* self = static_cast<UiEyesClock*>(user_ctx);
    if (self->video_mode_.load(std::memory_order_acquire)) {
        if (self->video_tx_done_sem_) {
            BaseType_t hp_woken = pdFALSE;
            xSemaphoreGiveFromISR(self->video_tx_done_sem_, &hp_woken);
            return (hp_woken == pdTRUE);
        }
        return false;
    }
    lv_disp_flush_ready(&self->disp_drv_);
    return false;
}

void UiEyesClock::lvgl_flush_cb(lv_disp_drv_t* drv,
                                  const lv_area_t* area,
                                  lv_color_t* color_map) {
    auto* self = static_cast<UiEyesClock*>(drv->user_data);
    esp_lcd_panel_draw_bitmap(self->panel_handle_,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
}

// ---------------------------------------------------------------------------
// lvgl_lock / unlock
// ---------------------------------------------------------------------------
bool UiEyesClock::lvgl_lock(uint32_t timeout_ms) {
    if (!lvgl_mutex_) return false;
    return xSemaphoreTake(lvgl_mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void UiEyesClock::lvgl_unlock() {
    if (lvgl_mutex_) {
        xSemaphoreGive(lvgl_mutex_);
    }
}

// ---------------------------------------------------------------------------
// Animation callbacks
// ---------------------------------------------------------------------------
void UiEyesClock::anim_set_height_cb(void* obj, int32_t v) {
    lv_obj_set_height(static_cast<lv_obj_t*>(obj), v);
}
void UiEyesClock::anim_set_width_cb(void* obj, int32_t v) {
    lv_obj_set_width(static_cast<lv_obj_t*>(obj), v);
}
void UiEyesClock::anim_set_y_cb(void* obj, int32_t v) {
    lv_obj_set_y(static_cast<lv_obj_t*>(obj), v);
}
void UiEyesClock::anim_set_x_cb(void* obj, int32_t v) {
    lv_obj_set_x(static_cast<lv_obj_t*>(obj), v);
}
void UiEyesClock::anim_set_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj),
                         static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

void UiEyesClock::anim_surprise_cb(void* obj, int32_t t_ms) {
    auto* self = static_cast<UiEyesClock*>(obj);
    if (!self || !self->eye_left_ || !self->eye_right_) return;

    // Quỹ đạo kích thước 0..350ms: 56 -> 42 (0..90ms) -> 66 (90..230ms) -> 56 (230..350ms)
    int sz = EYE_W;
    if (t_ms < 90) {
        sz = EYE_W + ((42 - EYE_W) * t_ms) / 90;
    } else if (t_ms < 230) {
        sz = 42 + ((66 - 42) * (t_ms - 90)) / 140;
    } else {
        sz = 66 + ((EYE_W - 66) * (t_ms - 230)) / 120;
    }

    const int cy  = EYE_Y + EYE_H / 2;
    const int cxl = EYE_LEFT_X + EYE_W / 2;
    const int cxr = EYE_RIGHT_X + EYE_W / 2;

    lv_obj_set_size(self->eye_left_,  sz, sz);
    lv_obj_set_pos(self->eye_left_,   cxl - sz / 2, cy - sz / 2);
    lv_obj_set_size(self->eye_right_, sz, sz);
    lv_obj_set_pos(self->eye_right_,  cxr - sz / 2, cy - sz / 2);
}

// ---------------------------------------------------------------------------
// animate_emotion() — Đầy đủ 7 trạng thái biểu cảm 60 FPS
// ---------------------------------------------------------------------------
void UiEyesClock::animate_emotion(EyeEmotion emotion) {
    auto make_anim = [](void* obj, lv_anim_exec_xcb_t cb,
                        int32_t start, int32_t end,
                        uint32_t dur_ms, uint32_t delay_ms = 0,
                        bool repeat = false) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, cb);
        lv_anim_set_values(&a, start, end);
        lv_anim_set_time(&a, dur_ms);
        lv_anim_set_delay(&a, delay_ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        if (repeat) {
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_playback_time(&a, dur_ms);
        }
        lv_anim_start(&a);
    };

    auto reset_eyes = [&]() {
        lv_anim_del(this,       nullptr);
        lv_anim_del(eye_left_,  nullptr);
        lv_anim_del(eye_right_, nullptr);
        if (eye_happy_left_) {
            lv_anim_del(eye_happy_left_, nullptr);
            lv_obj_add_flag(eye_happy_left_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(eye_happy_left_, EYE_LEFT_X - 2, EYE_Y + 6);
        }
        if (eye_happy_right_) {
            lv_anim_del(eye_happy_right_, nullptr);
            lv_obj_add_flag(eye_happy_right_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(eye_happy_right_, EYE_RIGHT_X - 2, EYE_Y + 6);
        }
        lv_obj_clear_flag(eye_left_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(eye_left_,  EYE_W, EYE_H);
        lv_obj_set_size(eye_right_, EYE_W, EYE_H);
        lv_obj_set_pos(eye_left_,  EYE_LEFT_X,  EYE_Y);
        lv_obj_set_pos(eye_right_, EYE_RIGHT_X, EYE_Y);
        lv_obj_set_style_radius(eye_left_,  EYE_RAD, LV_PART_MAIN);
        lv_obj_set_style_radius(eye_right_, EYE_RAD, LV_PART_MAIN);
        lv_obj_set_style_transform_angle(eye_left_,  0, LV_PART_MAIN);
        lv_obj_set_style_transform_angle(eye_right_, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(eye_left_,  LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_opa(eye_right_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(eye_left_,  18, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(eye_right_, 18, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(eye_left_,  LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(eye_right_, LV_OPA_60, LV_PART_MAIN);
    };

    // Khôi phục độ sáng người dùng nếu thoát khỏi trạng thái ngủ gật
    if (current_emotion_.load() == EyeEmotion::NGU_GAT && emotion != EyeEmotion::NGU_GAT) {
        apply_backlight_hardware(backlight_pct_.load());
    }

    reset_eyes();

    switch (emotion) {
    case EyeEmotion::BINH_THUONG:
        break;

    case EyeEmotion::DANG_NGHE:
        make_anim(eye_left_,  anim_set_height_cb, EYE_H, 62, 180);
        make_anim(eye_right_, anim_set_height_cb, EYE_H, 62, 180);
        make_anim(eye_left_,  anim_set_y_cb, EYE_Y, EYE_Y - 3, 180);
        make_anim(eye_right_, anim_set_y_cb, EYE_Y, EYE_Y - 3, 180);
        lv_obj_set_style_shadow_opa(eye_left_,  LV_OPA_90, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(eye_right_, LV_OPA_90, LV_PART_MAIN);
        make_anim(eye_left_,  anim_set_opa_cb, LV_OPA_COVER, 210, 550, 0, true);
        make_anim(eye_right_, anim_set_opa_cb, LV_OPA_COVER, 210, 550, 0, true);
        break;

    case EyeEmotion::SUY_NGHI:
        // Biến đổi hình học trực tiếp: mắt trái dẹt nheo lại (không che đen), mắt phải liếc lên
        lv_obj_set_style_radius(eye_left_, 12, LV_PART_MAIN);
        make_anim(eye_left_,  anim_set_height_cb, EYE_H, 24, 240);
        make_anim(eye_left_,  anim_set_y_cb, EYE_Y, EYE_Y + 16, 240);
        make_anim(eye_right_, anim_set_x_cb, EYE_RIGHT_X, EYE_RIGHT_X + 8, 260);
        make_anim(eye_right_, anim_set_y_cb, EYE_Y, EYE_Y - 6, 260);
        break;

    case EyeEmotion::VUI_VE:
        // Hiện cặp vòm cung ^ ^ vector thuần túy, ẩn hình chữ nhật mắt để 0% vệt đen
        lv_obj_add_flag(eye_left_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);
        if (eye_happy_left_ && eye_happy_right_) {
            lv_obj_clear_flag(eye_happy_left_,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(eye_happy_right_, LV_OBJ_FLAG_HIDDEN);
            make_anim(eye_happy_left_,  anim_set_y_cb, EYE_Y + 8, EYE_Y + 2, 340, 0, true);
            make_anim(eye_happy_right_, anim_set_y_cb, EYE_Y + 8, EYE_Y + 2, 340, 0, true);
        }
        break;

    case EyeEmotion::NGAC_NHIEN: {
        // Dùng một timeline animation duy nhất 0..350ms tránh xung đột hủy animation cùng (var, exec_cb) của LVGL v8
        lv_anim_del(this, nullptr);
        make_anim(this, anim_surprise_cb, 0, 350, 350);
        lv_obj_set_style_shadow_opa(eye_left_,  LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(eye_right_, LV_OPA_COVER, LV_PART_MAIN);
        break;
    }

    case EyeEmotion::BUON:
        // Xoay nghiêng & thu dẹt chính khối squircle (-14° / +14°), tuyệt đối không đắp mảng đen
        lv_obj_set_style_transform_angle(eye_left_,  -140, LV_PART_MAIN);
        lv_obj_set_style_transform_angle(eye_right_,  140, LV_PART_MAIN);
        make_anim(eye_left_,  anim_set_height_cb, EYE_H, 32, 320);
        make_anim(eye_right_, anim_set_height_cb, EYE_H, 32, 320);
        make_anim(eye_left_,  anim_set_y_cb, EYE_Y, EYE_Y + 12, 320);
        make_anim(eye_right_, anim_set_y_cb, EYE_Y, EYE_Y + 12, 320);
        make_anim(eye_left_,  anim_set_opa_cb, LV_OPA_COVER, LV_OPA_60, 350);
        make_anim(eye_right_, anim_set_opa_cb, LV_OPA_COVER, LV_OPA_60, 350);
        break;

    case EyeEmotion::NGU_GAT:
        // Díp dần xuống 4px giữa mắt và hạ đèn nền
        make_anim(eye_left_,  anim_set_height_cb, EYE_H, 4, 1600);
        make_anim(eye_right_, anim_set_height_cb, EYE_H, 4, 1600);
        make_anim(eye_left_,  anim_set_y_cb, EYE_Y, EYE_Y + EYE_H / 2 - 2, 1600);
        make_anim(eye_right_, anim_set_y_cb, EYE_Y, EYE_Y + EYE_H / 2 - 2, 1600);
        make_anim(eye_left_,  anim_set_opa_cb, LV_OPA_COVER, LV_OPA_40, 1600);
        make_anim(eye_right_, anim_set_opa_cb, LV_OPA_COVER, LV_OPA_40, 1600);
        apply_backlight_hardware(25);
        break;
    }
}

// ---------------------------------------------------------------------------
// tick_blink_and_glance() — Chớp mắt trực tiếp bằng height 56px -> 4px -> 56px
// ---------------------------------------------------------------------------
void UiEyesClock::tick_blink_and_glance(uint32_t now_ms) {
    if (current_emotion_.load() != EyeEmotion::BINH_THUONG) return;

    if (now_ms - last_blink_ms_ >= next_blink_ms_) {
        last_blink_ms_ = now_ms;
        next_blink_ms_ = (esp_random() % 2200U) + 2800U;

        auto blink = [](lv_obj_t* eye) {
            lv_anim_t ah;
            lv_anim_init(&ah);
            lv_anim_set_var(&ah, eye);
            lv_anim_set_exec_cb(&ah, anim_set_height_cb);
            lv_anim_set_values(&ah, EYE_H, 4);
            lv_anim_set_time(&ah, 65);
            lv_anim_set_playback_time(&ah, 75);
            lv_anim_set_path_cb(&ah, lv_anim_path_ease_in_out);
            lv_anim_start(&ah);

            lv_anim_t ay;
            lv_anim_init(&ay);
            lv_anim_set_var(&ay, eye);
            lv_anim_set_exec_cb(&ay, anim_set_y_cb);
            lv_anim_set_values(&ay, EYE_Y, EYE_Y + EYE_H / 2 - 2);
            lv_anim_set_time(&ay, 65);
            lv_anim_set_playback_time(&ay, 75);
            lv_anim_set_path_cb(&ay, lv_anim_path_ease_in_out);
            lv_anim_start(&ay);
        };
        blink(eye_left_);
        blink(eye_right_);
    }

    if (now_ms - last_glance_ms_ >= next_glance_ms_) {
        last_glance_ms_ = now_ms;
        next_glance_ms_ = (esp_random() % 3500U) + 4500U;

        const int dx = static_cast<int>(esp_random() % 13U) - 6; // -6..+6 px
        const int dy = static_cast<int>(esp_random() % 7U) - 3;  // -3..+3 px
        auto glance = [](lv_obj_t* eye, int base_x, int base_y, int off_x, int off_y) {
            lv_anim_t ax;
            lv_anim_init(&ax);
            lv_anim_set_var(&ax, eye);
            lv_anim_set_exec_cb(&ax, anim_set_x_cb);
            lv_anim_set_values(&ax, base_x, base_x + off_x);
            lv_anim_set_time(&ax, 180);
            lv_anim_set_playback_delay(&ax, 650);
            lv_anim_set_playback_time(&ax, 220);
            lv_anim_start(&ax);

            lv_anim_t ay;
            lv_anim_init(&ay);
            lv_anim_set_var(&ay, eye);
            lv_anim_set_exec_cb(&ay, anim_set_y_cb);
            lv_anim_set_values(&ay, base_y, base_y + off_y);
            lv_anim_set_time(&ay, 180);
            lv_anim_set_playback_delay(&ay, 650);
            lv_anim_set_playback_time(&ay, 220);
            lv_anim_start(&ay);
        };
        glance(eye_left_,  EYE_LEFT_X,  EYE_Y, dx, dy);
        glance(eye_right_, EYE_RIGHT_X, EYE_Y, dx, dy);
    }
}

// ---------------------------------------------------------------------------
// tick_waveform() — Đọc trực tiếp snapshot 11 dải từ AudioHal (0..100)
// ---------------------------------------------------------------------------
void UiEyesClock::tick_waveform() {
    const AudioHal::WaveformSnapshot snap = AudioHal::instance().get_waveform_snapshot();
    static constexpr int max_h[WAVE_N_BARS] = {12, 16, 20, 26, 32, 40, 32, 26, 20, 16, 12};

    for (int i = 0; i < WAVE_N_BARS; i++) {
        int val = std::min<int>(snap.bands[i], 100);
        int h   = std::max(4, (val * max_h[i]) / 100);
        int x   = WAVE_X_START + i * (WAVE_BAR_W + WAVE_BAR_GAP);
        int y   = WAVE_BAR_Y + (WAVE_MAX_H_CENTER - h) / 2;
        lv_obj_set_pos(wave_bars_[i], x, y);
        lv_obj_set_height(wave_bars_[i], h);
    }
}

// ---------------------------------------------------------------------------
// tick_clock_update()
// ---------------------------------------------------------------------------
void UiEyesClock::tick_clock_update(bool force) {
    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (!force && (now_ms - last_clock_tick_ms_ < 1000)) return;
    last_clock_tick_ms_ = now_ms;

    time_t t = time(nullptr);
    struct tm tm_info = {};
    localtime_r(&t, &tm_info);

    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
    lv_label_set_text(lbl_time_, time_buf);

    char sec_buf[6];
    snprintf(sec_buf, sizeof(sec_buf), ":%02d", tm_info.tm_sec);
    lv_label_set_text(lbl_seconds_, sec_buf);

    static const char* const thu[] = {
        "Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư",
        "Thứ Năm", "Thứ Sáu", "Thứ Bảy"
    };
    const int wday = (tm_info.tm_wday >= 0 && tm_info.tm_wday < 7) ? tm_info.tm_wday : 0;

    char date_buf[64];
    snprintf(date_buf, sizeof(date_buf), "%s, %02d/%02d/%04d",
             thu[wday],
             tm_info.tm_mday,
             tm_info.tm_mon + 1,
             tm_info.tm_year + 1900);
    lv_label_set_text(lbl_date_, date_buf);

    LunarDate ld = gregorian_to_viet_lunar(
        tm_info.tm_mday, tm_info.tm_mon + 1, tm_info.tm_year + 1900);
    char lunar_buf[72];
    snprintf(lunar_buf, sizeof(lunar_buf), "Âm lịch: %d/%d%s (%d)",
             ld.lunar_day, ld.lunar_month,
             ld.is_leap ? " Nhuận" : "",
             ld.lunar_year);
    lv_label_set_text(lbl_lunar_, lunar_buf);

    if (weather_data_.valid) {
        char temp_buf[16];
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", weather_data_.temp_c);
        lv_label_set_text(lbl_temp_, temp_buf);

        char hum_buf[12];
        snprintf(hum_buf, sizeof(hum_buf), "%u%%", weather_data_.humidity_pct);
        lv_label_set_text(lbl_humidity_, hum_buf);

        lv_label_set_text(lbl_weather_cond_, weather_data_.condition);

        uint32_t dot_col = 0xFFD700; // Nắng vàng
        if (strstr(weather_data_.condition, "Mưa") != nullptr) {
            dot_col = 0x00B4FF;
        } else if (strstr(weather_data_.condition, "Bão") != nullptr) {
            dot_col = 0xFF4500;
        } else if (strstr(weather_data_.condition, "Mây") != nullptr) {
            dot_col = 0xB0C4DE;
        }
        lv_obj_set_style_bg_color(lbl_weather_icon_, lv_color_hex(dot_col), LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------------------
// tick_notif_dismiss() / tick_vol_osd_dismiss()
// ---------------------------------------------------------------------------
void UiEyesClock::tick_notif_dismiss(uint32_t now_ms) {
    if (notif_dismiss_ms_ == 0) return;
    if (static_cast<int32_t>(now_ms - notif_dismiss_ms_) >= 0) {
        notif_dismiss_ms_ = 0;
        if (notif_popup_) lv_obj_add_flag(notif_popup_, LV_OBJ_FLAG_HIDDEN);
    }
}

void UiEyesClock::tick_vol_osd_dismiss(uint32_t now_ms) {
    if (vol_osd_dismiss_ms_ == 0) return;
    if (static_cast<int32_t>(now_ms - vol_osd_dismiss_ms_) >= 0) {
        vol_osd_dismiss_ms_ = 0;
        if (volume_osd_) lv_obj_add_flag(volume_osd_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Public APIs
// ---------------------------------------------------------------------------
void UiEyesClock::set_emotion(EyeEmotion emotion) {
    if (!initialized_.load()) return;
    if (emotion == current_emotion_.load()) return;
    if (lvgl_lock(50)) {
        animate_emotion(emotion);
        current_emotion_.store(emotion);
        lvgl_unlock();
    }
}

EyeEmotion UiEyesClock::get_emotion() const {
    return current_emotion_.load();
}

void UiEyesClock::apply_backlight_hardware(uint8_t percent) {
    if (percent > 100) percent = 100;
    const uint32_t duty = (static_cast<uint32_t>(percent) * percent * LCD_BLK_LEDC_MAX_DUTY) / 10000U;
    ledc_set_duty(LCD_BLK_LEDC_MODE, LCD_BLK_LEDC_CHANNEL, duty);
    ledc_update_duty(LCD_BLK_LEDC_MODE, LCD_BLK_LEDC_CHANNEL);
}

void UiEyesClock::set_backlight(uint8_t percent, bool persist_nvs) {
    if (percent > 100) percent = 100;
    backlight_pct_.store(percent);
    apply_backlight_hardware(percent);

    if (persist_nvs) {
        nvs_handle_t h = 0;
        if (nvs_open(BOARD_NVS_NS_DEVICE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, NVS_KEY_BACKLIGHT, percent);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

uint8_t UiEyesClock::get_backlight() const {
    return backlight_pct_.load();
}

esp_err_t UiEyesClock::set_rotation(uint16_t degree, bool persist_nvs) {
    const uint16_t normalized = (degree / 90U * 90U) % 360U;
    if (!panel_handle_) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    if (lvgl_lock(100)) {
        err = board_apply_lcd_rotation(panel_handle_, normalized);
        if (err == ESP_OK) {
            rotation_deg_.store(normalized);
            lv_obj_invalidate(lv_scr_act());
        }
        lvgl_unlock();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    if (err == ESP_OK && persist_nvs) {
        nvs_handle_t h = 0;
        if (nvs_open(BOARD_NVS_NS_DEVICE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u16(h, NVS_KEY_ROTATION, normalized);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    return err;
}

uint16_t UiEyesClock::get_rotation() const {
    return rotation_deg_.load();
}

void UiEyesClock::update_weather(const WeatherData& data) {
    if (!initialized_.load()) return;
    if (!lvgl_lock(50)) return;
    weather_data_ = data;
    weather_data_.valid = true;
    if (clock_screen_active_.load()) {
        tick_clock_update(true);
    }
    lvgl_unlock();
}

void UiEyesClock::show_volume_osd(uint8_t percent) {
    if (!initialized_.load() || !lvgl_lock(50)) return;
    if (percent > 100) percent = 100;

    char buf[32];
    if (percent == 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_MUTE " Tắt tiếng (0%%)");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX " Âm lượng: %u%%", percent);
    }
    lv_label_set_text(volume_label_, buf);

    const int bar_w = std::max(1, static_cast<int>(percent) * 140 / 100);
    lv_obj_set_width(volume_bar_, bar_w);

    lv_obj_clear_flag(volume_osd_, LV_OBJ_FLAG_HIDDEN);
    vol_osd_dismiss_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL)
                          + VOL_OSD_DURATION_MS;

    lvgl_unlock();
}

void UiEyesClock::show_notification(const ChronosNotification& notif) {
    if (!initialized_.load() || !lvgl_lock(50)) return;

    char app_hdr[48];
    snprintf(app_hdr, sizeof(app_hdr), LV_SYMBOL_BELL " %s", notif.app_name);
    lv_label_set_text(lbl_notif_app_,   app_hdr);
    lv_label_set_text(lbl_notif_title_, notif.title);
    lv_label_set_text(lbl_notif_body_,  notif.body);

    lv_obj_set_style_opa(notif_popup_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(notif_popup_, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, notif_popup_);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_90);
    lv_anim_set_time(&a, 280);
    lv_anim_start(&a);

    notif_dismiss_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL)
                        + NOTIF_DURATION_MS;

    lvgl_unlock();
}

void UiEyesClock::update_nav_info(const ChronosNavInfo& nav) {
    if (!initialized_.load() || !lvgl_lock(50)) return;

    nav_info_ = nav;

    if (nav.active) {
        const char* arrow = LV_SYMBOL_UP;
        if (strstr(nav.direction, "left") != nullptr || strstr(nav.direction, "trái") != nullptr ||
            strstr(nav.direction, "trai") != nullptr) {
            arrow = LV_SYMBOL_LEFT;
        } else if (strstr(nav.direction, "right") != nullptr || strstr(nav.direction, "phải") != nullptr ||
                   strstr(nav.direction, "phai") != nullptr) {
            arrow = LV_SYMBOL_RIGHT;
        } else if (strstr(nav.direction, "Quay") != nullptr || strstr(nav.direction, "quay") != nullptr ||
                   strstr(nav.direction, "u-turn") != nullptr) {
            arrow = LV_SYMBOL_LOOP;
        } else if (strstr(nav.direction, "đến") != nullptr || strstr(nav.direction, "arrive") != nullptr) {
            arrow = LV_SYMBOL_OK;
        }

        char dir_buf[64];
        snprintf(dir_buf, sizeof(dir_buf), "%s %s", arrow, nav.direction);
        lv_label_set_text(lbl_nav_direction_, dir_buf);

        char dist_buf[32];
        if (nav.distance_m >= 1000) {
            snprintf(dist_buf, sizeof(dist_buf), "%.1f km", static_cast<double>(nav.distance_m) / 1000.0);
        } else {
            snprintf(dist_buf, sizeof(dist_buf), "%lu m", static_cast<unsigned long>(nav.distance_m));
        }
        lv_label_set_text(lbl_nav_distance_, dist_buf);
        lv_label_set_text(lbl_nav_street_,   nav.street_name);

        lv_obj_clear_flag(nav_container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(nav_container_, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_unlock();
}

void UiEyesClock::switch_to_clock_screen() {
    if (!initialized_.load() || clock_screen_active_.load()) return;
    if (!lvgl_lock(100)) return;
    tick_clock_update(true);
    lv_scr_load_anim(scr_clock_, LV_SCR_LOAD_ANIM_FADE_ON, 350, 0, false);
    clock_screen_active_.store(true);
    lvgl_unlock();
}

void UiEyesClock::switch_to_chat_screen() {
    if (!initialized_.load() || !clock_screen_active_.load()) return;
    if (!lvgl_lock(100)) return;
    lv_scr_load_anim(scr_chat_, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    clock_screen_active_.store(false);
    lvgl_unlock();
}

bool UiEyesClock::is_clock_screen_active() const {
    return clock_screen_active_.load();
}

void UiEyesClock::refresh_waveform() {
    if (!initialized_.load() || video_mode_.load() || !lvgl_lock(20)) return;
    tick_waveform();
    lvgl_unlock();
}

// ---------------------------------------------------------------------------
// Music Banner trên màn hình Mắt + Sóng âm
// ---------------------------------------------------------------------------
void UiEyesClock::show_music_banner(const char* source_tag, const char* title) {
    if (!initialized_.load() || !music_banner_ || !lbl_music_title_ || !lvgl_lock(50)) return;

    char buf[160];
    if (source_tag && source_tag[0]) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO " %s %s",
                 source_tag, (title && title[0]) ? title : "Đang phát nhạc");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO " %s",
                 (title && title[0]) ? title : "Đang phát nhạc");
    }
    lv_label_set_text(lbl_music_title_, buf);
    lv_obj_clear_flag(music_banner_, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

void UiEyesClock::hide_music_banner() {
    if (!initialized_.load() || !music_banner_ || !lvgl_lock(50)) return;
    lv_obj_add_flag(music_banner_, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

// ---------------------------------------------------------------------------
// Chế độ Video MJPEG trực tiếp ra ST7789 (12-15 FPS qua DMA Strip)
// ---------------------------------------------------------------------------
void UiEyesClock::fill_rect_solid_dma(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color_be) {
    if (!panel_handle_ || !video_dma_strip_ || !video_tx_done_sem_) return;
    if (x >= LCD_W || y >= LCD_H || w == 0 || h == 0) return;

    const uint16_t draw_w = std::min<uint16_t>(w, LCD_W - x);
    const uint16_t draw_h = std::min<uint16_t>(h, LCD_H - y);
    const size_t strip_pixels = static_cast<size_t>(draw_w) * LCD_DMA_LINES;
    for (size_t i = 0; i < strip_pixels; ++i) {
        video_dma_strip_[i] = color_be;
    }

    uint16_t cur_y = y;
    while (cur_y < y + draw_h) {
        const uint16_t chunk_h = std::min<uint16_t>(LCD_DMA_LINES, static_cast<uint16_t>(y + draw_h - cur_y));
        xSemaphoreTake(video_tx_done_sem_, 0);
        if (esp_lcd_panel_draw_bitmap(panel_handle_, x, cur_y, x + draw_w, cur_y + chunk_h, video_dma_strip_) == ESP_OK) {
            xSemaphoreTake(video_tx_done_sem_, pdMS_TO_TICKS(100));
        }
        cur_y += chunk_h;
    }
}

esp_err_t UiEyesClock::enter_video_mode(const char* title, const char* source_tag) {
    if (!initialized_.load() || !panel_handle_) return ESP_ERR_INVALID_STATE;
    if (video_mode_.load()) return ESP_OK;

    if (!video_tx_done_sem_) {
        video_tx_done_sem_ = xSemaphoreCreateBinary();
        if (!video_tx_done_sem_) return ESP_ERR_NO_MEM;
    }
    if (!video_dma_strip_) {
        video_dma_strip_ = static_cast<uint16_t*>(
            board_malloc_dma(static_cast<size_t>(LCD_W) * LCD_DMA_LINES * sizeof(uint16_t)));
        if (!video_dma_strip_) {
            vSemaphoreDelete(video_tx_done_sem_);
            video_tx_done_sem_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (!lvgl_lock(200)) return ESP_ERR_TIMEOUT;

    // Ẩn popup/OSD tạm để khỏi đè lên khung hình video
    if (notif_popup_) lv_obj_add_flag(notif_popup_, LV_OBJ_FLAG_HIDDEN);
    if (volume_osd_)  lv_obj_add_flag(volume_osd_,  LV_OBJ_FLAG_HIDDEN);

    char banner_txt[160];
    snprintf(banner_txt, sizeof(banner_txt), "%s %s",
             (source_tag && source_tag[0]) ? source_tag : "[Video]",
             (title && title[0]) ? title : "Đang phát trực tuyến");
    if (lbl_video_title_) {
        lv_label_set_text(lbl_video_title_, banner_txt);
    }

    lv_disp_load_scr(scr_video_);
    lv_obj_invalidate(scr_video_);
    lv_refr_now(lvgl_disp_); // Vẽ đồng bộ nền đen + thanh tiêu đề 3 giây đầu trước khi khóa LVGL

    video_banner_visible_.store(true);
    video_mode_.store(true);
    lvgl_unlock();

    ESP_LOGI(TAG, "Đã vào chế độ phát Video ST7789: %s", banner_txt);
    return ESP_OK;
}

esp_err_t UiEyesClock::push_video_frame_rgb565(uint16_t x,
                                                uint16_t y,
                                                uint16_t w,
                                                uint16_t h,
                                                const uint16_t* pixels_rgb565_be) {
    if (!video_mode_.load() || !panel_handle_ || !pixels_rgb565_be ||
        !video_dma_strip_ || !video_tx_done_sem_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= LCD_W || y >= LCD_H || w == 0 || h == 0) return ESP_ERR_INVALID_ARG;

    const uint16_t max_y = video_banner_visible_.load() ? 212 : LCD_H;
    if (y >= max_y) return ESP_OK;

    const uint16_t draw_w = std::min<uint16_t>(w, LCD_W - x);
    const uint16_t draw_h = std::min<uint16_t>(h, max_y - y);

    // Chia nhỏ theo từng dải <= LCD_DMA_LINES (40 dòng) để tuân thủ SPI max_transfer_sz
    // và copy qua DMA-capable Internal SRAM nên hỗ trợ an toàn cả khi frame gốc nằm trên PSRAM
    uint16_t row = 0;
    while (row < draw_h) {
        const uint16_t strip_h = std::min<uint16_t>(LCD_DMA_LINES, static_cast<uint16_t>(draw_h - row));
        for (uint16_t r = 0; r < strip_h; ++r) {
            memcpy(video_dma_strip_ + static_cast<size_t>(r) * draw_w,
                   pixels_rgb565_be + static_cast<size_t>(row + r) * w,
                   static_cast<size_t>(draw_w) * sizeof(uint16_t));
        }
        xSemaphoreTake(video_tx_done_sem_, 0);
        esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel_handle_, x, y + row, x + draw_w, y + row + strip_h, video_dma_strip_);
        if (err != ESP_OK) return err;
        xSemaphoreTake(video_tx_done_sem_, pdMS_TO_TICKS(100));
        row += strip_h;
    }
    return ESP_OK;
}

void UiEyesClock::hide_video_title_banner() {
    if (!video_mode_.load()) return;
    if (video_banner_visible_.exchange(false)) {
        fill_rect_solid_dma(0, 212, LCD_W, 28, 0x0000);
    }
}

void UiEyesClock::exit_video_mode() {
    if (!video_mode_.load()) return;

    if (lvgl_lock(200)) {
        video_mode_.store(false);
        video_banner_visible_.store(false);

        if (video_dma_strip_) {
            board_free(video_dma_strip_);
            video_dma_strip_ = nullptr;
        }
        if (video_tx_done_sem_) {
            vSemaphoreDelete(video_tx_done_sem_);
            video_tx_done_sem_ = nullptr;
        }

        clock_screen_active_.store(false);
        lv_disp_load_scr(scr_chat_);
        lv_obj_invalidate(scr_chat_);
        lvgl_unlock();
    } else {
        video_mode_.store(false);
    }
    ESP_LOGI(TAG, "Đã thoát chế độ Video về màn hình Mắt Chatbot");
}

} // namespace zclaw
