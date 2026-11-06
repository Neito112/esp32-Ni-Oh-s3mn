#include "s3mini_display.h"

#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>

#include "application.h"
#include "display/lvgl_display/lvgl_theme.h"

#define TAG "S3MiniDisplay"

namespace {

// Danh sách các hướng liếc mắt tự nhiên ngẫu nhiên
constexpr const char* kWeightedGazeEyes[] = {
    "nhin tren phai",
    "nhin tren trai",
    "nhin tren phai",
    "nhin tren trai",
    "nhin tren phai",
    "nhin tren trai",
    "nhin duoi phai",
    "nhin duoi trai",
};

// Danh sách biểu cảm ngẫu nhiên thỉnh thoảng xuất hiện khi đứng chờ (Idle)
constexpr const char* kWeightedIdleExpressions[] = {
    "happy_eyes",
    "happy_eyes",
    "suspicious_eyes",
    "suspicious_eyes",
    "eyes_shake",
    "sad_eyes",
};

bool MatchesAny(const char* value, std::initializer_list<const char*> choices) {
    if (value == nullptr) {
        return false;
    }
    for (const char* choice : choices) {
        if (std::strcmp(value, choice) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void S3MiniDisplay::VinylAnimCallback(void* var, int32_t v) {
    auto* obj = static_cast<lv_obj_t*>(var);
    if (obj != nullptr) {
        // Xoay góc đĩa than trong LVGL (0 - 3600 tương đương 0 - 360 độ)
        lv_image_set_rotation(obj, (int16_t)v);
    }
}

void S3MiniDisplay::NotificationTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<S3MiniDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr) {
        display->HideNotification();
    }
}

S3MiniDisplay::S3MiniDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    esp_timer_create_args_t idle_timer_args = {
        .callback = IdleEyeTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "s3mini_idle_eye",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&idle_timer_args, &idle_eye_timer_));
}

S3MiniDisplay::~S3MiniDisplay() {
    if (idle_eye_timer_ != nullptr) {
        esp_timer_stop(idle_eye_timer_);
        esp_timer_delete(idle_eye_timer_);
        idle_eye_timer_ = nullptr;
    }

    if (typewriter_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_delete(typewriter_timer_);
        typewriter_timer_ = nullptr;
    }
    if (cursor_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_delete(cursor_timer_);
        cursor_timer_ = nullptr;
    }
    if (notif_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_delete(notif_timer_);
        notif_timer_ = nullptr;
    }
}

void S3MiniDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI called more than once");
        return;
    }

    SpiLcdDisplay::SetupUI();

    {
        DisplayLockGuard lock(this);
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        auto screen = lv_screen_active();
        const lv_color_t black = lv_color_hex(0x000000);
        const lv_color_t text_color = lv_color_hex(0xEAFBFF);

        lv_obj_set_style_bg_color(screen, black, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(container_, black, 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);

        // Đôi mắt 240x100 đặt ngay dưới thanh trạng thái
        lv_obj_set_size(emoji_box_, width_, kEyeHeight);
        lv_obj_align(emoji_box_, LV_ALIGN_TOP_MID, 0, kStatusBarHeight);
        lv_obj_set_style_bg_color(emoji_box_, black, 0);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(emoji_box_, 0, 0);
        lv_obj_set_style_border_width(emoji_box_, 0, 0);
        lv_obj_set_scrollbar_mode(emoji_box_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_center(emoji_image_);

        // Khung chữ phụ đề bên dưới đôi mắt
        lv_obj_set_size(bottom_bar_, width_, height_ - kStatusBarHeight - kEyeHeight - 10);
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x03090C), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bottom_bar_, 1, 0);
        lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(0x16414A), 0);
        lv_obj_set_style_border_opa(bottom_bar_, LV_OPA_60, 0);
        lv_obj_set_style_radius(bottom_bar_, 4, 0);
        lv_obj_set_style_pad_top(bottom_bar_, 10, 0);
        lv_obj_set_style_pad_bottom(bottom_bar_, 6, 0);
        lv_obj_set_style_pad_left(bottom_bar_, 8, 0);
        lv_obj_set_style_pad_right(bottom_bar_, 8, 0);
        lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_width(chat_message_label_, width_ - 16);
        lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(chat_message_label_, text_color, 0);
        if (theme != nullptr && theme->text_font() != nullptr) {
            lv_obj_set_style_text_font(chat_message_label_, theme->text_font()->font(), 0);
        }
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_LEFT, 0, 0);

        // Thanh trạng thái trên đỉnh
        lv_obj_set_size(top_bar_, width_, kStatusBarHeight);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(top_bar_, black, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_top(top_bar_, 0, 0);
        lv_obj_set_style_pad_bottom(top_bar_, 0, 0);
        lv_obj_set_style_pad_left(top_bar_, 6, 0);
        lv_obj_set_style_pad_right(top_bar_, 6, 0);
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_text_color(network_label_, lv_color_hex(0xEAFBFF), 0);
        lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* right_icons = lv_obj_get_parent(mute_label_);
        state_dot_ = lv_obj_create(right_icons);
        lv_obj_set_size(state_dot_, 7, 7);
        lv_obj_set_style_radius(state_dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(state_dot_, 0, 0);
        lv_obj_set_style_pad_all(state_dot_, 0, 0);
        lv_obj_set_style_bg_opa(state_dot_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(state_dot_, lv_color_hex(0xF5B942), 0);
        lv_obj_clear_flag(state_dot_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

        // =====================================================================
        // Giao diện Đĩa than xoay tròn khi phát nhạc (Vinyl Record Player)
        // =====================================================================
        music_container_ = lv_obj_create(container_);
        lv_obj_set_size(music_container_, width_, height_ - kStatusBarHeight);
        lv_obj_align(music_container_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(music_container_, black, 0);
        lv_obj_set_style_bg_opa(music_container_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(music_container_, 0, 0);
        lv_obj_set_style_pad_all(music_container_, 0, 0);
        lv_obj_clear_flag(music_container_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(music_container_, LV_OBJ_FLAG_HIDDEN);

        // Thân đĩa than màu đen đường kính 136px
        vinyl_disc_ = lv_obj_create(music_container_);
        lv_obj_set_size(vinyl_disc_, 136, 136);
        lv_obj_align(vinyl_disc_, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_radius(vinyl_disc_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(vinyl_disc_, lv_color_hex(0x111114), 0);
        lv_obj_set_style_bg_opa(vinyl_disc_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(vinyl_disc_, 2, 0);
        lv_obj_set_style_border_color(vinyl_disc_, lv_color_hex(0x282830), 0);
        lv_obj_clear_flag(vinyl_disc_, LV_OBJ_FLAG_SCROLLABLE);

        // Rãnh đĩa 1 (Vòng rãnh ngoài)
        vinyl_groove1_ = lv_obj_create(vinyl_disc_);
        lv_obj_set_size(vinyl_groove1_, 106, 106);
        lv_obj_center(vinyl_groove1_);
        lv_obj_set_style_radius(vinyl_groove1_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(vinyl_groove1_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(vinyl_groove1_, 1, 0);
        lv_obj_set_style_border_color(vinyl_groove1_, lv_color_hex(0x202026), 0);
        lv_obj_clear_flag(vinyl_groove1_, LV_OBJ_FLAG_SCROLLABLE);

        // Rãnh đĩa 2 (Vòng rãnh trong)
        vinyl_groove2_ = lv_obj_create(vinyl_disc_);
        lv_obj_set_size(vinyl_groove2_, 76, 76);
        lv_obj_center(vinyl_groove2_);
        lv_obj_set_style_radius(vinyl_groove2_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(vinyl_groove2_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(vinyl_groove2_, 1, 0);
        lv_obj_set_style_border_color(vinyl_groove2_, lv_color_hex(0x2A2A35), 0);
        lv_obj_clear_flag(vinyl_groove2_, LV_OBJ_FLAG_SCROLLABLE);

        // Tâm đĩa than (Label tròn màu đỏ cam nổi bật)
        vinyl_label_ = lv_obj_create(vinyl_disc_);
        lv_obj_set_size(vinyl_label_, 42, 42);
        lv_obj_center(vinyl_label_);
        lv_obj_set_style_radius(vinyl_label_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(vinyl_label_, lv_color_hex(0xE74C3C), 0);
        lv_obj_set_style_bg_opa(vinyl_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(vinyl_label_, 2, 0);
        lv_obj_set_style_border_color(vinyl_label_, lv_color_hex(0xC0392B), 0);
        lv_obj_clear_flag(vinyl_label_, LV_OBJ_FLAG_SCROLLABLE);

        // Lỗ trục quay trung tâm
        lv_obj_t* vinyl_spindle = lv_obj_create(vinyl_label_);
        lv_obj_set_size(vinyl_spindle, 8, 8);
        lv_obj_center(vinyl_spindle);
        lv_obj_set_style_radius(vinyl_spindle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(vinyl_spindle, black, 0);
        lv_obj_set_style_bg_opa(vinyl_spindle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(vinyl_spindle, 0, 0);
        lv_obj_clear_flag(vinyl_spindle, LV_OBJ_FLAG_SCROLLABLE);

        // Tên bài hát chạy chữ cuộn ngang
        song_title_label_ = lv_label_create(music_container_);
        lv_obj_set_width(song_title_label_, width_ - 24);
        lv_label_set_long_mode(song_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(song_title_label_, LV_ALIGN_BOTTOM_MID, 0, -34);
        lv_obj_set_style_text_align(song_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(song_title_label_, lv_color_hex(0xFFFFFF), 0);
        if (theme != nullptr && theme->text_font() != nullptr) {
            lv_obj_set_style_text_font(song_title_label_, theme->text_font()->font(), 0);
        }
        lv_label_set_text(song_title_label_, "Đang phát nhạc...");

        // Ca sĩ / nghệ sĩ
        song_artist_label_ = lv_label_create(music_container_);
        lv_obj_set_width(song_artist_label_, width_ - 24);
        lv_obj_align(song_artist_label_, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_text_align(song_artist_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(song_artist_label_, lv_color_hex(0x27D7E8), 0);
        if (theme != nullptr && theme->text_font() != nullptr) {
            lv_obj_set_style_text_font(song_artist_label_, theme->text_font()->font(), 0);
        }
        lv_label_set_text(song_artist_label_, "Âm nhạc trực tuyến");

        // Hoạt ảnh xoay đĩa than
        lv_anim_init(&vinyl_anim_);
        lv_anim_set_var(&vinyl_anim_, vinyl_disc_);
        lv_anim_set_exec_cb(&vinyl_anim_, VinylAnimCallback);
        lv_anim_set_values(&vinyl_anim_, 0, 3600);
        lv_anim_set_duration(&vinyl_anim_, 2400); // 1 vòng quay trong 2.4s (~25 rpm)
        lv_anim_set_repeat_count(&vinyl_anim_, LV_ANIM_REPEAT_INFINITE);

        // =====================================================================
        // Hộp thông báo Chronos BLE (Notification Pop-up)
        // =====================================================================
        notif_box_ = lv_obj_create(screen);
        lv_obj_set_size(notif_box_, width_ - 16, 68);
        lv_obj_align(notif_box_, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_bg_color(notif_box_, lv_color_hex(0x0A1724), 0);
        lv_obj_set_style_bg_opa(notif_box_, LV_OPA_90, 0);
        lv_obj_set_style_radius(notif_box_, 8, 0);
        lv_obj_set_style_border_width(notif_box_, 1, 0);
        lv_obj_set_style_border_color(notif_box_, lv_color_hex(0x258DFF), 0);
        lv_obj_set_style_pad_all(notif_box_, 6, 0);
        lv_obj_clear_flag(notif_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(notif_box_, LV_OBJ_FLAG_HIDDEN);

        notif_app_label_ = lv_label_create(notif_box_);
        lv_obj_align(notif_app_label_, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_color(notif_app_label_, lv_color_hex(0x35D05B), 0);
        lv_label_set_text(notif_app_label_, "Tin nhắn");

        notif_title_label_ = lv_label_create(notif_box_);
        lv_obj_align(notif_title_label_, LV_ALIGN_TOP_LEFT, 0, 16);
        lv_obj_set_style_text_color(notif_title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(notif_title_label_, "Người gửi");

        notif_msg_label_ = lv_label_create(notif_box_);
        lv_obj_set_width(notif_msg_label_, width_ - 32);
        lv_obj_align(notif_msg_label_, LV_ALIGN_TOP_LEFT, 0, 32);
        lv_label_set_long_mode(notif_msg_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(notif_msg_label_, lv_color_hex(0xCCD8E0), 0);
        lv_label_set_text(notif_msg_label_, "Nội dung...");

        notif_timer_ = lv_timer_create(NotificationTimerCallback, 5000, this);
        lv_timer_pause(notif_timer_);

        typewriter_timer_ = lv_timer_create(TypewriterTimerCallback, kTypewriterPeriodMs, this);
        lv_timer_pause(typewriter_timer_);

        cursor_timer_ = lv_timer_create(CursorTimerCallback, kCursorBlinkPeriodMs, this);
        lv_timer_pause(cursor_timer_);
    }

    ScheduleIdleEye(2000);
}

void S3MiniDisplay::ShowMusicPlayer(const char* song_name, const char* artist_name) {
    if (!setup_ui_called_ || music_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    music_active_ = true;

    if (song_name != nullptr && song_name[0] != '\0') {
        lv_label_set_text(song_title_label_, song_name);
    } else {
        lv_label_set_text(song_title_label_, "Đang phát nhạc...");
    }

    if (artist_name != nullptr && artist_name[0] != '\0') {
        lv_label_set_text(song_artist_label_, artist_name);
    } else {
        lv_label_set_text(song_artist_label_, "Âm nhạc trực tuyến");
    }

    // Ẩn mắt và khung chat, hiện đĩa than
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(music_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(music_container_);

    // Bắt đầu quay đĩa than
    lv_anim_start(&vinyl_anim_);
    ESP_LOGI(TAG, "Music player started: %s - %s", song_name ? song_name : "", artist_name ? artist_name : "");
}

void S3MiniDisplay::HideMusicPlayer() {
    if (!setup_ui_called_ || music_container_ == nullptr || !music_active_) {
        return;
    }
    DisplayLockGuard lock(this);
    music_active_ = false;

    // Dừng quay đĩa than
    lv_anim_delete(vinyl_disc_, VinylAnimCallback);
    lv_obj_add_flag(music_container_, LV_OBJ_FLAG_HIDDEN);

    // Hiện lại đôi mắt
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Music player closed, restored eyes");
}

void S3MiniDisplay::ShowNotification(const char* app_name, const char* title, const char* message) {
    if (!setup_ui_called_ || notif_box_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (app_name != nullptr) lv_label_set_text(notif_app_label_, app_name);
    if (title != nullptr) lv_label_set_text(notif_title_label_, title);
    if (message != nullptr) lv_label_set_text(notif_msg_label_, message);

    lv_obj_remove_flag(notif_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(notif_box_);

    // Tự động đóng sau 5 giây
    lv_timer_reset(notif_timer_);
    lv_timer_resume(notif_timer_);
}

void S3MiniDisplay::HideNotification() {
    if (!setup_ui_called_ || notif_box_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_timer_pause(notif_timer_);
    lv_obj_add_flag(notif_box_, LV_OBJ_FLAG_HIDDEN);
}

void S3MiniDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);
    if (state_dot_ == nullptr) {
        return;
    }

    lv_color_t color = lv_color_hex(0xF5B942);
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateIdle:
            color = lv_color_hex(0x35D05B);
            break;
        case kDeviceStateListening:
            color = lv_color_hex(0x258DFF);
            break;
        case kDeviceStateSpeaking:
            color = lv_color_hex(0x27D7E8);
            break;
        case kDeviceStateWifiConfiguring:
            color = lv_color_hex(0xFF5757);
            break;
        default:
            break;
    }

    DisplayLockGuard lock(this);
    lv_obj_set_style_bg_color(state_dot_, color, 0);
}

const char* S3MiniDisplay::MapEmotionToEye(const char* emotion) const {
    if (MatchesAny(emotion, {"happy", "laughing", "funny", "loving", "kissy", "confident",
                             "delicious", "cool", "winking"})) {
        return "happy_eyes";
    }
    if (MatchesAny(emotion, {"sad", "crying", "sleepy"})) {
        return "sad_eyes";
    }
    if (MatchesAny(emotion, {"angry", "disgusted"})) {
        return "angry_eyes";
    }
    if (MatchesAny(emotion, {"suspicious", "thinking", "confused", "embarrassed"})) {
        return "suspicious_eyes";
    }
    if (MatchesAny(emotion, {"eyes_shake", "shocked", "surprised", "silly"})) {
        return "eyes_shake";
    }
    if (MatchesAny(emotion, {"look_up_right", "nhin_tren_phai"})) {
        return "nhin tren phai";
    }
    if (MatchesAny(emotion, {"look_up_left", "nhin_tren_trai"})) {
        return "nhin tren trai";
    }
    if (MatchesAny(emotion, {"look_down_right", "nhin_duoi_phai"})) {
        return "nhin duoi phai";
    }
    if (MatchesAny(emotion, {"look_down_left", "nhin_duoi_trai"})) {
        return "nhin duoi trai";
    }
    return "eyes_nomal";
}

void S3MiniDisplay::SetEyeIfChanged(const char* eye, int loop_count, bool restart_if_same) {
    if (eye == nullptr || eye[0] == '\0') {
        return;
    }
    if (music_active_) {
        return; // Đang phát nhạc đĩa than thì không đè mắt lên
    }

    std::lock_guard<std::mutex> lock(eye_mutex_);
    if (!restart_if_same && current_eye_ == eye) {
        return;
    }

    current_eye_ = eye;
    if (loop_count == 0) {
        LvglDisplay::SetEmotion(eye);
    } else {
        LvglDisplay::SetEmotion(eye);
    }
}

void S3MiniDisplay::SetEmotion(const char* emotion) {
    if (music_active_) {
        HideMusicPlayer(); // Khi AI nói chuyện thì tắt đĩa than, hiện mắt
    }
    const char* mapped_eye = MapEmotionToEye(emotion);
    SetEyeIfChanged(mapped_eye, 0, false);

    std::lock_guard<std::mutex> lock(idle_timer_mutex_);
    if (idle_eye_timer_ != nullptr) {
        esp_timer_stop(idle_eye_timer_);
    }

    if (std::strcmp(mapped_eye, "eyes_nomal") == 0) {
        ScheduleIdleEye(2500 + (esp_random() % 2500));
    }
}

void S3MiniDisplay::IdleEyeTimerCallback(void* arg) {
    auto* self = static_cast<S3MiniDisplay*>(arg);
    if (self != nullptr) {
        self->ShowRandomIdleEye();
    }
}

void S3MiniDisplay::ShowRandomIdleEye() {
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateStarting) {
        ScheduleIdleEye(3000);
        return;
    }

    if (current_eye_ != "eyes_nomal" && !current_eye_.empty()) {
        SetEyeIfChanged("eyes_nomal", 0, true);
        ScheduleIdleEye(3500 + (esp_random() % 3500));
        return;
    }

    bool pick_accent = idle_accent_next_.exchange(!idle_accent_next_.load());
    const char* chosen_eye = "eyes_nomal";

    if (pick_accent) {
        constexpr size_t count = sizeof(kWeightedIdleExpressions) / sizeof(kWeightedIdleExpressions[0]);
        chosen_eye = kWeightedIdleExpressions[esp_random() % count];
    } else {
        constexpr size_t count = sizeof(kWeightedGazeEyes) / sizeof(kWeightedGazeEyes[0]);
        chosen_eye = kWeightedGazeEyes[esp_random() % count];
    }

    SetEyeIfChanged(chosen_eye, 1, true);
    ScheduleIdleEye(1800 + (esp_random() % 1200));
}

void S3MiniDisplay::ScheduleIdleEye(uint32_t delay_ms) {
    std::lock_guard<std::mutex> lock(idle_timer_mutex_);
    if (idle_eye_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(idle_eye_timer_);
    esp_timer_start_once(idle_eye_timer_, static_cast<uint64_t>(delay_ms) * 1000ULL);
}

void S3MiniDisplay::TypewriterTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<S3MiniDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->AdvanceTypewriter();
    }
}

void S3MiniDisplay::CursorTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<S3MiniDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->BlinkTerminalCursor();
    }
}

void S3MiniDisplay::AdvanceTypewriter() {
    DisplayLockGuard lock(this);
    if (typewriter_position_ >= typewriter_target_.size()) {
        lv_timer_pause(typewriter_timer_);
        terminal_cursor_enabled_ = true;
        terminal_cursor_visible_ = true;
        lv_timer_resume(cursor_timer_);
        RenderTerminalTextLocked();
        return;
    }

    size_t next_pos = typewriter_position_;
    unsigned char first_byte = static_cast<unsigned char>(typewriter_target_[next_pos]);
    size_t char_len = 1;
    if ((first_byte & 0x80) == 0) {
        char_len = 1;
    } else if ((first_byte & 0xE0) == 0xC0) {
        char_len = 2;
    } else if ((first_byte & 0xF0) == 0xE0) {
        char_len = 3;
    } else if ((first_byte & 0xF8) == 0xF0) {
        char_len = 4;
    }

    next_pos = std::min(typewriter_target_.size(), next_pos + char_len);
    typewriter_visible_ = typewriter_target_.substr(0, next_pos);
    typewriter_position_ = next_pos;

    RenderTerminalTextLocked();

    if (typewriter_position_ >= typewriter_target_.size()) {
        lv_timer_pause(typewriter_timer_);
        terminal_cursor_enabled_ = true;
        terminal_cursor_visible_ = true;
        lv_timer_resume(cursor_timer_);
        RenderTerminalTextLocked();
    }
}

void S3MiniDisplay::BlinkTerminalCursor() {
    DisplayLockGuard lock(this);
    if (!terminal_cursor_enabled_) {
        return;
    }
    terminal_cursor_visible_ = !terminal_cursor_visible_;
    RenderTerminalTextLocked();
}

void S3MiniDisplay::RenderTerminalTextLocked() {
    std::string text = typewriter_visible_;
    if (terminal_cursor_enabled_ && terminal_cursor_visible_) {
        text += "_";
    }
    ShowTextLocked(text.c_str());
}

void S3MiniDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_ || chat_message_label_ == nullptr) {
        return;
    }
    if (content == nullptr || content[0] == '\0') {
        return;
    }

    if (music_active_) {
        HideMusicPlayer(); // AI phản hồi thì ẩn đĩa than
    }

    DisplayLockGuard lock(this);
    typewriter_target_ = content;
    typewriter_visible_.clear();
    typewriter_position_ = 0;
    terminal_prompt_active_ = (role != nullptr && std::strcmp(role, "user") == 0);
    terminal_cursor_enabled_ = false;
    terminal_cursor_visible_ = true;
    terminal_has_content_ = true;

    ShowTerminalPopupLocked();
    lv_timer_pause(cursor_timer_);
    lv_timer_reset(typewriter_timer_);
    lv_timer_resume(typewriter_timer_);
    AdvanceTypewriter();
}

void S3MiniDisplay::ShowTerminalPopupLocked() {
    if (bottom_bar_ == nullptr) {
        return;
    }
    lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bottom_bar_);
}

void S3MiniDisplay::HideTerminalLocked() {
    if (bottom_bar_ == nullptr) {
        return;
    }
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
}

void S3MiniDisplay::ShowTextLocked(const char* content) {
    lv_label_set_text(chat_message_label_, content != nullptr ? content : "");
    ScrollTextToBottomLocked();
}

void S3MiniDisplay::ScrollTextToBottomLocked() {
    lv_obj_update_layout(chat_message_label_);
    lv_obj_scroll_to_y(bottom_bar_, LV_COORD_MAX, LV_ANIM_OFF);
}

void S3MiniDisplay::ClearChatMessages() {
    if (!setup_ui_called_ || chat_message_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    typewriter_target_.clear();
    typewriter_visible_.clear();
    typewriter_position_ = 0;
    terminal_prompt_active_ = false;
    terminal_cursor_enabled_ = false;
    terminal_cursor_visible_ = false;
    terminal_has_content_ = false;
    lv_timer_pause(typewriter_timer_);
    lv_timer_pause(cursor_timer_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_scroll_to_y(bottom_bar_, 0, LV_ANIM_OFF);
    HideTerminalLocked();
}

void S3MiniDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    if (!setup_ui_called_) {
        return;
    }

    DisplayLockGuard lock(this);
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    const lv_color_t black = lv_color_hex(0x000000);
    lv_obj_set_style_bg_color(lv_screen_active(), black, 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(container_, black, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(container_, nullptr, 0);
    lv_obj_set_style_bg_color(emoji_box_, black, 0);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x03090C), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bottom_bar_, 1, 0);
    lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(0x16414A), 0);
    lv_obj_set_style_border_opa(bottom_bar_, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(top_bar_, black, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_hex(0xEAFBFF), 0);
    if (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(chat_message_label_, lvgl_theme->text_font()->font(), 0);
    }
    lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xEAFBFF), 0);
}
