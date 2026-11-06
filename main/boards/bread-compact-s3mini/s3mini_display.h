#pragma once

#include "display/lcd_display.h"

#include <esp_timer.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

class S3MiniDisplay : public SpiLcdDisplay {
public:
    S3MiniDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
    ~S3MiniDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetTheme(Theme* theme) override;

    // Music Player with Rotating Vinyl Record
    void ShowMusicPlayer(const char* song_name, const char* artist_name);
    void HideMusicPlayer();
    bool IsMusicPlayerActive() const { return music_active_; }

    // Phone Notifications via Chronos BLE
    void ShowNotification(const char* app_name, const char* title, const char* message);
    void HideNotification();

private:
    static constexpr int kStatusBarHeight = 18;
    static constexpr int kEyeHeight = 100;
    static constexpr int kTypewriterPeriodMs = 35;
    static constexpr int kCursorBlinkPeriodMs = 420;

    lv_timer_t* typewriter_timer_ = nullptr;
    lv_timer_t* cursor_timer_ = nullptr;
    lv_obj_t* state_dot_ = nullptr;
    esp_timer_handle_t idle_eye_timer_ = nullptr;

    // Vinyl Record UI elements
    lv_obj_t* music_container_ = nullptr;
    lv_obj_t* vinyl_disc_ = nullptr;
    lv_obj_t* vinyl_label_ = nullptr;
    lv_obj_t* vinyl_groove1_ = nullptr;
    lv_obj_t* vinyl_groove2_ = nullptr;
    lv_obj_t* song_title_label_ = nullptr;
    lv_obj_t* song_artist_label_ = nullptr;
    lv_anim_t vinyl_anim_;
    bool music_active_ = false;

    // Notification Pop-up UI elements
    lv_obj_t* notif_box_ = nullptr;
    lv_obj_t* notif_app_label_ = nullptr;
    lv_obj_t* notif_title_label_ = nullptr;
    lv_obj_t* notif_msg_label_ = nullptr;
    lv_timer_t* notif_timer_ = nullptr;

    std::string typewriter_target_;
    std::string typewriter_visible_;
    std::string current_eye_;
    std::string last_idle_accent_;
    size_t typewriter_position_ = 0;
    std::atomic_bool idle_accent_next_ = true;

    bool terminal_prompt_active_ = false;
    bool terminal_cursor_enabled_ = false;
    bool terminal_cursor_visible_ = true;
    bool terminal_has_content_ = false;

    std::mutex eye_mutex_;
    std::mutex idle_timer_mutex_;

    static void TypewriterTimerCallback(lv_timer_t* timer);
    static void CursorTimerCallback(lv_timer_t* timer);
    static void IdleEyeTimerCallback(void* arg);
    static void NotificationTimerCallback(lv_timer_t* timer);
    static void VinylAnimCallback(void* var, int32_t v);

    void AdvanceTypewriter();
    void BlinkTerminalCursor();
    void RenderTerminalTextLocked();
    void ShowTerminalPopupLocked();
    void HideTerminalLocked();
    void ShowTextLocked(const char* content);
    void ScrollTextToBottomLocked();
    void ScheduleIdleEye(uint32_t delay_ms);
    void ShowRandomIdleEye();
    void SetEyeIfChanged(const char* eye, int loop_count = 0, bool restart_if_same = false);
    const char* MapEmotionToEye(const char* emotion) const;
};
