#pragma once

/**
 * @file zclaw_llm_client.h
 * @brief Direct LLM / Free Cloud STT / Free Cloud TTS / Media Streamer (MP3 & MJPEG)
 *        + Web Captive Portal Tiếng Việt. Loại bỏ hoàn toàn server xiaozhi.me.
 */

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_hal.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ui_eyes_clock.h"

namespace zclaw {

enum class MediaKind : uint8_t {
    NONE = 0,
    AUDIO_MP3,
    VIDEO_MJPEG,
};

struct PendingMedia {
    MediaKind   kind = MediaKind::NONE;
    std::string url;
    std::string source_tag; // "[YouTube]", "[SoundCloud]", "[Radio]", "[Video]"
    std::string title;
};

struct ZClawConfig {
    char wifi_ssid[33];
    char wifi_pass[65];
    char stt_api_key[160];       // Groq Whisper / OpenAI STT key (nếu trống dùng llm_api_key)
    char llm_api_key[160];
    char llm_base_url[192];      // VD: https://api.groq.com/openai/v1/chat/completions
    char llm_model[64];          // VD: llama-3.3-70b-versatile
    char tts_voice[48];          // vi-VN-HoaiMyNeural hoặc vi-VN-NamMinhNeural
    bool tts_prefer_google;      // false = thử Edge TTS trước rồi fallback Google TTS
    char media_proxy_url[192];   // Custom Media Gateway (yt-dlp/ffmpeg) cho YouTube/SoundCloud/MJPEG
    char soundcloud_client_id[64]; // SoundCloud client_id (tùy chọn)
};

class ZClawLlmClient {
public:
    static ZClawLlmClient& instance();

    esp_err_t init();
    void deinit();
    bool is_initialized() const { return initialized_.load(); }

    // --- Cấu hình NVS (zclaw_cfg) ---
    void load_config();
    esp_err_t save_config(const ZClawConfig& cfg);
    ZClawConfig get_config() const;
    bool has_wifi_credentials() const;

    // --- Web Captive Portal Tiếng Việt ---
    esp_err_t start_captive_portal(bool start_softap = true);
    void stop_captive_portal();
    bool is_portal_active() const { return portal_active_.load(); }

    // --- Speech-to-Text (Groq Whisper / OpenAI Audio) ---
    esp_err_t transcribe_wav(const AudioHal::CapturedAudio& audio, std::string& out_text);

    // --- Direct Cloud LLM + MCP Function Calling ---
    esp_err_t chat_with_tools(const std::string& user_text,
                              std::string& out_reply_clean,
                              EyeEmotion& out_emotion);
    esp_err_t run_deep_reasoning(const std::string& problem, std::string& out_analysis);
    void clear_history();
    void set_reasoning_mode(bool enabled) { next_turn_deep_reasoning_.store(enabled); }

    // --- Free Cloud TTS & Media Streaming (minimp3 + esp_jpeg trên RAM) ---
    esp_err_t speak_vietnamese(const std::string& text);
    esp_err_t stream_mp3_url(const char* url, uint32_t max_duration_ms = 0);
    esp_err_t stream_mjpeg_video_url(const std::string& url,
                                     const std::string& title,
                                     const std::string& source_tag = "[Video]");

    // Hàng đợi phát Nhạc / Radio / Video ngay sau khi Bot nói xong lời xác nhận TTS
    void schedule_media_playback(MediaKind kind,
                                 const char* url,
                                 const char* source_tag,
                                 const char* title);
    bool consume_pending_media(PendingMedia& out_media);
    void clear_pending_media();

    // Alias tương thích cho Radio
    void schedule_radio_playback(const char* url, const char* station_name) {
        schedule_media_playback(MediaKind::AUDIO_MP3, url, "[Radio]", station_name);
    }
    bool consume_pending_radio(std::string& out_url, std::string& out_name) {
        PendingMedia m;
        if (!consume_pending_media(m) || m.kind != MediaKind::AUDIO_MP3) return false;
        out_url  = m.url;
        out_name = m.title;
        return true;
    }
    void clear_pending_radio() { clear_pending_media(); }

private:
    ZClawLlmClient() = default;
    ~ZClawLlmClient() = default;
    ZClawLlmClient(const ZClawLlmClient&) = delete;
    ZClawLlmClient& operator=(const ZClawLlmClient&) = delete;

    struct ChatMsg {
        std::string role;
        std::string content;
    };

    static constexpr size_t kMaxHistoryPairs = 8;

    ZClawConfig           config_ = {};
    std::vector<ChatMsg>  history_;
    PendingMedia          pending_media_ = {};

    mutable SemaphoreHandle_t state_mutex_ = nullptr;
    std::atomic<bool>         initialized_{false};
    std::atomic<bool>         portal_active_{false};
    std::atomic<bool>         next_turn_deep_reasoning_{false};

    // Web Captive Portal & DNS Responder
    httpd_handle_t    httpd_handle_    = nullptr;
    TaskHandle_t      dns_task_handle_ = nullptr;
    int               dns_sock_        = -1;
    std::atomic<bool> dns_running_{false};

    static void dns_portal_task(void* arg);
    static esp_err_t http_root_get_handler(httpd_req_t* req);
    static esp_err_t http_save_post_handler(httpd_req_t* req);
    static esp_err_t http_status_get_handler(httpd_req_t* req);
    static esp_err_t http_captive_redirect_handler(httpd_req_t* req);

    // Helpers
    static EyeEmotion extract_emotion_tag(const std::string& raw, std::string& out_clean);
    esp_err_t post_json_chat_completion(const char* url,
                                        const char* api_key,
                                        const char* json_body,
                                        std::string& out_response);
    esp_err_t speak_chunk_edge_ws(const std::string& text_chunk, const char* voice);
    esp_err_t speak_chunk_google_tts(const std::string& text_chunk);
};

} // namespace zclaw
