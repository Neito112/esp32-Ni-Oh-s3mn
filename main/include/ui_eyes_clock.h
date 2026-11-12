#pragma once

#include <atomic>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

namespace zclaw {

enum class EyeEmotion : uint8_t {
    BINH_THUONG = 0,
    DANG_NGHE,
    SUY_NGHI,
    VUI_VE,
    NGAC_NHIEN,
    BUON,
    NGU_GAT
};

struct ChronosNavInfo {
    char direction[32];
    char street_name[64];
    uint32_t distance_m;
    uint32_t eta_seconds;
    uint16_t speed_kmh;
    bool active;
};

struct ChronosNotification {
    char app_name[32];
    char title[64];
    char body[128];
    bool unread;
};

struct WeatherData {
    float temp_c;
    uint8_t humidity_pct;
    float wind_kmh;
    uint8_t rain_pct;
    char condition[32]; // "Nắng", "Mây", "Mưa", "Bão"
    bool valid;
};

class UiEyesClock {
public:
    static UiEyesClock& instance();

    esp_err_t init();
    void deinit();
    bool is_initialized() const { return initialized_.load(); }

    void set_emotion(EyeEmotion emotion);
    EyeEmotion get_emotion() const;

    void set_backlight(uint8_t percent, bool persist_nvs = true);
    uint8_t get_backlight() const;

    // Xoay màn hình 0 / 90 / 180 / 270 độ, lưu NVS
    esp_err_t set_rotation(uint16_t degree, bool persist_nvs = true);
    uint16_t get_rotation() const;

    void update_weather(const WeatherData& data);
    void show_volume_osd(uint8_t percent);
    void update_nav_info(const ChronosNavInfo& nav);
    void show_notification(const ChronosNotification& notif);

    void refresh_waveform();

    void switch_to_clock_screen();
    void switch_to_chat_screen();
    bool is_clock_screen_active() const;

    // Lấy mutex LVGL để gọi lv_* từ bên ngoài task LVGL
    bool lvgl_lock(uint32_t timeout_ms = 50);
    void lvgl_unlock();

private:
    UiEyesClock() = default;
    ~UiEyesClock() = default;
    UiEyesClock(const UiEyesClock&) = delete;
    UiEyesClock& operator=(const UiEyesClock&) = delete;

    // --- Màn hình Chat (Mắt + Sóng âm) ---
    lv_obj_t* scr_chat_          = nullptr;
    lv_obj_t* eye_left_          = nullptr;
    lv_obj_t* eye_right_         = nullptr;
    lv_obj_t* eyelid_top_left_   = nullptr;
    lv_obj_t* eyelid_bot_left_   = nullptr;
    lv_obj_t* eyelid_top_right_  = nullptr;
    lv_obj_t* eyelid_bot_right_  = nullptr;
    lv_obj_t* wave_bars_[11]     = {};

    // --- Overlay chung (lv_layer_top) hiển thị trên cả 2 màn hình ---
    lv_obj_t* volume_osd_        = nullptr;
    lv_obj_t* volume_bar_        = nullptr;
    lv_obj_t* volume_label_      = nullptr;
    lv_obj_t* notif_popup_       = nullptr;
    lv_obj_t* lbl_notif_app_     = nullptr;
    lv_obj_t* lbl_notif_title_   = nullptr;
    lv_obj_t* lbl_notif_body_    = nullptr;

    // --- Màn hình Đồng hồ ---
    lv_obj_t* scr_clock_         = nullptr;
    lv_obj_t* lbl_time_          = nullptr;
    lv_obj_t* lbl_seconds_       = nullptr;
    lv_obj_t* lbl_date_          = nullptr;
    lv_obj_t* lbl_lunar_         = nullptr;
    lv_obj_t* weather_badge_     = nullptr;
    lv_obj_t* lbl_weather_icon_  = nullptr;
    lv_obj_t* lbl_temp_          = nullptr;
    lv_obj_t* lbl_humidity_      = nullptr;
    lv_obj_t* lbl_weather_cond_  = nullptr;
    lv_obj_t* nav_container_     = nullptr;
    lv_obj_t* lbl_nav_direction_ = nullptr;
    lv_obj_t* lbl_nav_distance_  = nullptr;
    lv_obj_t* lbl_nav_street_    = nullptr;

    // --- Hardware handles ---
    esp_lcd_panel_handle_t    panel_handle_ = nullptr;
    esp_lcd_panel_io_handle_t io_handle_    = nullptr;
    lv_disp_t*                lvgl_disp_    = nullptr;
    lv_disp_drv_t             disp_drv_     = {};
    lv_disp_draw_buf_t        draw_buf_     = {};
    lv_color_t*               draw_buf1_    = nullptr;
    lv_color_t*               draw_buf2_    = nullptr;

    // --- FreeRTOS ---
    SemaphoreHandle_t lvgl_mutex_       = nullptr;
    TaskHandle_t      lvgl_task_handle_ = nullptr;

    // --- Trạng thái ---
    std::atomic<EyeEmotion> current_emotion_{EyeEmotion::BINH_THUONG};
    std::atomic<bool>       clock_screen_active_{false};
    std::atomic<uint8_t>    backlight_pct_{80};
    std::atomic<uint16_t>   rotation_deg_{0};
    std::atomic<bool>       initialized_{false};
    std::atomic<bool>       task_running_{false};

    WeatherData    weather_data_ = {};
    ChronosNavInfo nav_info_     = {};

    uint32_t last_blink_ms_      = 0;
    uint32_t next_blink_ms_      = 3200;
    uint32_t last_glance_ms_     = 0;
    uint32_t next_glance_ms_     = 5200;
    uint32_t notif_dismiss_ms_   = 0;
    uint32_t vol_osd_dismiss_ms_ = 0;
    uint32_t last_clock_tick_ms_ = 0;

    // --- Static callbacks ---
    static bool lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t* edata,
                                    void* user_ctx);
    static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map);
    static void lvgl_task(void* arg);

    // --- Init helpers ---
    esp_err_t init_spi_lcd();
    esp_err_t init_ledc_backlight();
    esp_err_t init_lvgl();
    void      apply_backlight_hardware(uint8_t percent);
    void      build_chat_screen();
    void      build_clock_screen();
    void      build_overlays();

    // --- Animation helpers ---
    void animate_emotion(EyeEmotion emotion);
    void tick_blink_and_glance(uint32_t now_ms);
    void tick_clock_update(bool force = false);
    void tick_waveform();
    void tick_notif_dismiss(uint32_t now_ms);
    void tick_vol_osd_dismiss(uint32_t now_ms);

    static void anim_set_height_cb(void* obj, int32_t v);
    static void anim_set_width_cb(void* obj, int32_t v);
    static void anim_set_y_cb(void* obj, int32_t v);
    static void anim_set_x_cb(void* obj, int32_t v);
    static void anim_set_opa_cb(void* obj, int32_t v);
    static void anim_surprise_cb(void* obj, int32_t t_ms);
};

} // namespace zclaw
