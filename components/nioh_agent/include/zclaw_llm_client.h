#pragma once

/**
 * @file zclaw_llm_client.h
 * @brief Direct Multi-Provider LLM (ESP-Claw style) / Free Cloud STT / Free Cloud TTS
 *        / Media Streamer (MP3 & MJPEG) + Web Dashboard Tiếng Việt cho S.W.Ni-Oh.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "audio_hal.h"
#include "cJSON.h"
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

struct LlmProfile {
    char name[32];              // Tên hiển thị Profile (VD: "Groq Chính", "OpenRouter Dự phòng")
    char provider[24];          // groq, openrouter, gemini, gemini_native, anthropic, deepseek, ...
    char base_url[192];         // Base URL hoặc Raw Full Endpoint URL
    char model[64];             // Model ID
    char api_key[160];          // API Key / Token
    char auth_scheme[24];       // bearer, x-api-key, x-goog-api-key, custom_header, query_param
    char custom_auth_name[32];  // Tên Header hoặc Query Param khi chọn custom_header / query_param
    bool raw_endpoint;          // true = giữ nguyên base_url làm endpoint chat, false = tự ghép /chat/completions
    bool enabled;               // true = tham gia chuỗi Fallback khi gặp 429/lỗi
};

struct ZClawConfig {
    static constexpr size_t kMaxLlmProfiles = 4;

    char wifi_ssid[33];
    char wifi_pass[65];
    char stt_api_key[160];       // Groq Whisper / OpenAI STT key (nếu trống dùng key của LLM Profile)

    // Multi-Provider LLM Profiles (ESP-Claw style)
    LlmProfile llm_profiles[kMaxLlmProfiles];
    uint8_t    active_profile_idx;
    bool       auto_fallback;

    // Legacy mirrors của Profile đang kích hoạt (tương thích ngược)
    char llm_api_key[160];
    char llm_base_url[192];
    char llm_model[64];

    // TTS Tiếng Việt
    char tts_voice[48];          // vi-VN-HoaiMyNeural hoặc vi-VN-NamMinhNeural
    bool tts_prefer_google;      // false = thử Edge TTS trước rồi fallback Google TTS

    // Dịch vụ Web Search (tra_cuu_thong_tin)
    char search_engine[24];      // ddg_wiki, tavily, brave, serper, searxng
    char search_api_key[128];
    char searxng_url[160];

    // Dịch vụ Nhạc & Video (phat_nhac_truc_tuyen & xem_video_truc_tuyen)
    char piped_instances[256];      // Danh sách Piped/Invidious/Cobalt cách nhau dấu phẩy
    char media_proxy_url[192];      // Custom Media Gateway (yt-dlp/ffmpeg)
    char soundcloud_client_id[64];  // SoundCloud client_id (để trống = tự cạo)
    char video_res[12];             // "240x240" hoặc "240x180"
    uint8_t video_fps;              // 12, 15 hoặc 20
    uint8_t video_jpeg_quality;     // 40..90
};

using McpSchemaProviderFn      = std::function<cJSON*()>;
using McpExecuteFn             = std::function<std::string(const char* tool_name, const char* args_json)>;
using ChronosStatusJsonFn      = std::function<std::string()>;

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

    // --- Web Captive Portal & LAN Dashboard Tiếng Việt (IP / sw-nioh.local) ---
    esp_err_t start_captive_portal(bool start_softap = true);
    void stop_captive_portal();
    bool is_portal_active() const { return portal_active_.load(); }

    // --- Speech-to-Text (Groq Whisper / OpenAI Audio) ---
    esp_err_t transcribe_wav(const AudioHal::CapturedAudio& audio, std::string& out_text);

    // --- Direct Cloud LLM + Multi-Provider Failover + MCP Function Calling ---
    esp_err_t chat_with_tools(const std::string& user_text,
                              std::string& out_reply_clean,
                              EyeEmotion& out_emotion);
    esp_err_t run_deep_reasoning(const std::string& problem, std::string& out_analysis);
    void clear_history();
    void set_reasoning_mode(bool enabled) { next_turn_deep_reasoning_.store(enabled); }

    // Trợ giúp Dashboard: Fetch /models & Test kết nối LLM
    esp_err_t fetch_models_for_profile(const LlmProfile& prof,
                                       std::vector<std::string>& out_models,
                                       std::string& out_err);
    esp_err_t test_profile_connection(const LlmProfile& prof,
                                      uint32_t& out_latency_ms,
                                      std::string& out_reply,
                                      std::string& out_err);

    // Đăng ký provider MCP Tools & Chronos Status (tách rời dependency vòng)
    void register_mcp_tools(McpSchemaProviderFn schema_cb, McpExecuteFn exec_cb);
    void register_chronos_status_provider(ChronosStatusJsonFn cb);

    // --- Free Cloud TTS & Media Streaming (minimp3 + ROM TJpgDec trên RAM) ---
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
    McpSchemaProviderFn   mcp_schema_cb_;
    McpExecuteFn          mcp_exec_cb_;
    ChronosStatusJsonFn   chronos_status_cb_;

    mutable SemaphoreHandle_t state_mutex_ = nullptr;
    std::atomic<bool>         initialized_{false};
    std::atomic<bool>         portal_active_{false};
    std::atomic<bool>         softap_started_{false};
    std::atomic<bool>         next_turn_deep_reasoning_{false};

    // Web Dashboard & DNS Captive Responder
    httpd_handle_t    httpd_handle_    = nullptr;
    TaskHandle_t      dns_task_handle_ = nullptr;
    int               dns_sock_        = -1;
    std::atomic<bool> dns_running_{false};

    static void dns_portal_task(void* arg);
    static esp_err_t http_root_get_handler(httpd_req_t* req);
    static esp_err_t http_save_post_handler(httpd_req_t* req);
    static esp_err_t http_config_get_handler(httpd_req_t* req);
    static esp_err_t http_config_post_handler(httpd_req_t* req);
    static esp_err_t http_llm_models_post_handler(httpd_req_t* req);
    static esp_err_t http_llm_test_post_handler(httpd_req_t* req);
    static esp_err_t http_chronos_status_get_handler(httpd_req_t* req);
    static esp_err_t http_status_get_handler(httpd_req_t* req);
    static esp_err_t http_captive_redirect_handler(httpd_req_t* req);

    // Helpers
    static EyeEmotion extract_emotion_tag(const std::string& raw, std::string& out_clean);
    esp_err_t post_json_with_profile(const LlmProfile& prof,
                                     const char* json_body,
                                     std::string& out_response,
                                     int* out_http_status = nullptr);
    esp_err_t http_get_with_profile(const LlmProfile& prof,
                                    const char* url,
                                    std::string& out_response,
                                    int* out_http_status = nullptr);
    esp_err_t execute_single_profile_turn(const LlmProfile& prof,
                                          cJSON* messages,
                                          cJSON* tools_arr,
                                          bool deep_mode,
                                          std::string& out_raw_reply,
                                          int* out_http_status);
    esp_err_t speak_chunk_edge_ws(const std::string& text_chunk, const char* voice);
    esp_err_t speak_chunk_google_tts(const std::string& text_chunk);
};

} // namespace zclaw
