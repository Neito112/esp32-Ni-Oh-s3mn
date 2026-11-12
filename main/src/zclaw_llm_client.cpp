// zclaw_llm_client.cpp
// Xiaozhi-VN ZClaw Edition — Direct Cloud LLM + Groq Whisper STT + Edge/Google TTS
// + minimp3 RAM Decoder + Web Captive Portal Tiếng Việt
// Production-ready C++17 implementation — no TODOs, no pseudo-code, no exceptions.

#include "zclaw_llm_client.h"
#include "board_config.h"
#include "espnow_comm.h"
#include "mcp_vietnam_tools.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <strings.h>

static const char* TAG = "ZClawLLM";

namespace zclaw {

namespace {

constexpr const char* kDefaultBaseUrl  = "https://api.groq.com/openai/v1/chat/completions";
constexpr const char* kDefaultModel    = "llama-3.3-70b-versatile";
constexpr const char* kDefaultVoice    = "vi-VN-HoaiMyNeural";
constexpr const char* kGroqSttUrl      = "https://api.groq.com/openai/v1/audio/transcriptions";
constexpr const char* kOpenAiSttUrl    = "https://api.openai.com/v1/audio/transcriptions";
constexpr const char* kSoftApSsid      = "Xiaozhi-ZClaw-VN";

constexpr const char* kSystemPromptVi =
    "Bạn là Xiaozhi ZClaw, trợ lý AI thông minh nói tiếng Việt tự nhiên, thân thiện và ngắn gọn.\n"
    "QUY TẮC BẮT BUỘC:\n"
    "1. Luôn mở đầu MỌI câu trả lời cuối cùng bằng đúng 1 thẻ cảm xúc trong danh sách sau: "
    "[binh_thuong], [vui_ve], [ngac_nhien], [suy_nghi], [buon], [ngu_gat].\n"
    "2. Trả lời tự nhiên như đang trò chuyện bằng giọng nói (tránh dùng ký hiệu markdown **, #, dấu gạch đầu dòng phức tạp).\n"
    "3. Luôn ưu tiên gọi các công cụ MCP tiếng Việt khi người dùng hỏi thời tiết, giờ/âm lịch, tin tức, Wikipedia, nhạc/radio, mở video, suy luận sâu, điều khiển thiết bị, gửi ESP-NOW hoặc hẹn giờ Pomodoro.\n"
    "4. Giữ câu trả lời súc tích khoảng 2-4 câu trừ khi người dùng yêu cầu phân tích chi tiết.";

// URL encode helper
std::string url_encode_str(const char* src) {
    if (!src) return "";
    std::string out;
    out.reserve(strlen(src) * 2);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src); *p; ++p) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// URL decode application/x-www-form-urlencoded
std::string url_decode_form(const char* src, size_t len) {
    std::string out;
    out.reserve(len);
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < len; ++i) {
        if (src[i] == '+') {
            out.push_back(' ');
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(src[i]);
            }
        } else {
            out.push_back(src[i]);
        }
    }
    return out;
}

// HTML escape để chống vỡ form khi giá trị cấu hình có dấu ngoặc kép / < >
std::string html_escape(const char* s) {
    if (!s) return "";
    std::string out;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&':  out += "&amp;"; break;
            case '"':  out += "&quot;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            default:   out.push_back(*p); break;
        }
    }
    return out;
}

// Escape XML cho Edge TTS SSML
std::string xml_escape(const char* s) {
    if (!s) return "";
    std::string out;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(*p); break;
        }
    }
    return out;
}

// Tách đoạn văn tiếng Việt thành các câu <= max_bytes tôn trọng ranh giới UTF-8
std::vector<std::string> split_tts_sentences(const std::string& text, size_t max_bytes = 165) {
    std::vector<std::string> chunks;
    std::string cur;

    auto flush_cur = [&]() {
        size_t a = 0;
        while (a < cur.size() && (cur[a] == ' ' || cur[a] == '\n' || cur[a] == '\r')) a++;
        size_t b = cur.size();
        while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\n' || cur[b - 1] == '\r')) b--;
        if (b > a) {
            chunks.push_back(cur.substr(a, b - a));
        }
        cur.clear();
    };

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t char_len = 1;
        if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;
        if (i + char_len > text.size()) char_len = text.size() - i;

        // Lọc bỏ ký tự markdown gây đọc thô (*, #, `)
        if (char_len == 1 && (text[i] == '*' || text[i] == '#' || text[i] == '`')) {
            i++;
            continue;
        }

        if (cur.size() + char_len > max_bytes) {
            // Ưu tiên cắt tại dấu phẩy hoặc khoảng trắng gần nhất
            size_t split_at = cur.find_last_of(",; ");
            if (split_at != std::string::npos && split_at > cur.size() / 2) {
                std::string head = cur.substr(0, split_at + 1);
                std::string tail = cur.substr(split_at + 1);
                cur = head;
                flush_cur();
                cur = tail;
            } else {
                flush_cur();
            }
        }

        cur.append(text, i, char_len);
        i += char_len;

        if (char_len == 1 && (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n')) {
            // Tránh cắt nhầm dấu chấm trong số thập phân / viết tắt ngắn (VD: "1.5" hoặc "TP.")
            bool next_digit = (i < text.size() && isdigit(static_cast<unsigned char>(text[i])));
            if (!next_digit && cur.size() >= 18) {
                flush_cur();
            }
        }
    }
    flush_cur();
    return chunks;
}

// RAII Wrapper cho bộ giải mã MP3 minimp3 trên Heap (tuyệt đối không đặt mp3dec_t ~15KB lên stack!)
struct Mp3RamContext {
    static constexpr size_t kInBufSize = 8192;
    mp3dec_t* dec     = nullptr;
    uint8_t*  in_buf  = nullptr;
    int16_t*  pcm_out = nullptr;
    size_t    in_len  = 0;

    bool alloc() {
        dec     = static_cast<mp3dec_t*>(board_malloc_prefer_spiram(sizeof(mp3dec_t)));
        in_buf  = static_cast<uint8_t*>(board_malloc_prefer_spiram(kInBufSize));
        pcm_out = static_cast<int16_t*>(board_malloc_prefer_spiram(
            MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
        if (!dec || !in_buf || !pcm_out) {
            free_all();
            return false;
        }
        mp3dec_init(dec);
        in_len = 0;
        return true;
    }

    ~Mp3RamContext() {
        free_all();
    }

    void free_all() {
        board_free(dec);     dec = nullptr;
        board_free(in_buf);  in_buf = nullptr;
        board_free(pcm_out); pcm_out = nullptr;
    }

    esp_err_t decode_and_play_available(bool flush_tail) {
        while (in_len > 0) {
            if (AudioHal::instance().is_playback_stop_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            if (!flush_tail && in_len < 2048) {
                break; // Chờ thêm dữ liệu để tránh xén frame ở biên
            }
            mp3dec_frame_info_t info = {};
            int samples = mp3dec_decode_frame(
                dec, in_buf, static_cast<int>(in_len), pcm_out, &info);

            if (info.frame_bytes <= 0) {
                break;
            }
            if (samples > 0 && info.hz > 0 && (info.channels == 1 || info.channels == 2)) {
                esp_err_t werr = AudioHal::instance().write_speaker(
                    pcm_out,
                    static_cast<size_t>(samples),
                    static_cast<uint8_t>(info.channels),
                    static_cast<uint32_t>(info.hz),
                    pdMS_TO_TICKS(1000));
                if (werr != ESP_OK) {
                    return werr;
                }
            }
            const size_t consumed = static_cast<size_t>(info.frame_bytes);
            if (consumed >= in_len) {
                in_len = 0;
            } else {
                memmove(in_buf, in_buf + consumed, in_len - consumed);
                in_len -= consumed;
            }
        }
        return ESP_OK;
    }
};

// Restart device sau khi trả HTTP response từ Web Portal
void delayed_restart_cb(void* /*arg*/) {
    esp_restart();
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton & Lifecycle
// ---------------------------------------------------------------------------
ZClawLlmClient& ZClawLlmClient::instance() {
    static ZClawLlmClient inst;
    return inst;
}

esp_err_t ZClawLlmClient::init() {
    if (initialized_.load()) return ESP_OK;
    if (!state_mutex_) {
        state_mutex_ = xSemaphoreCreateMutex();
        if (!state_mutex_) return ESP_ERR_NO_MEM;
    }
    load_config();
    initialized_.store(true);
    ESP_LOGI(TAG, "ZClawLlmClient sẵn sàng | Model=%s | Giọng=%s",
             config_.llm_model, config_.tts_voice);
    return ESP_OK;
}

void ZClawLlmClient::deinit() {
    stop_captive_portal();
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    initialized_.store(false);
}

// ---------------------------------------------------------------------------
// NVS Configuration (BOARD_NVS_NS_ZCLAW = "zclaw_cfg")
// ---------------------------------------------------------------------------
void ZClawLlmClient::load_config() {
    ZClawConfig cfg = {};
    snprintf(cfg.llm_base_url, sizeof(cfg.llm_base_url), "%s", kDefaultBaseUrl);
    snprintf(cfg.llm_model, sizeof(cfg.llm_model), "%s", kDefaultModel);
    snprintf(cfg.tts_voice, sizeof(cfg.tts_voice), "%s", kDefaultVoice);
    cfg.tts_prefer_google = false;

    nvs_handle_t h = 0;
    if (nvs_open(BOARD_NVS_NS_ZCLAW, NVS_READONLY, &h) == ESP_OK) {
        auto read_str = [&](const char* key, char* dst, size_t max_len) {
            size_t len = max_len;
            nvs_get_str(h, key, dst, &len);
        };
        read_str("wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
        read_str("wifi_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
        read_str("stt_key",   cfg.stt_api_key, sizeof(cfg.stt_api_key));
        read_str("llm_key",   cfg.llm_api_key, sizeof(cfg.llm_api_key));
        read_str("llm_url",   cfg.llm_base_url, sizeof(cfg.llm_base_url));
        read_str("llm_model", cfg.llm_model, sizeof(cfg.llm_model));
        read_str("tts_voice", cfg.tts_voice, sizeof(cfg.tts_voice));
        uint8_t g_pref = 0;
        if (nvs_get_u8(h, "tts_gg", &g_pref) == ESP_OK) {
            cfg.tts_prefer_google = (g_pref != 0);
        }
        nvs_close(h);
    }

    if (cfg.llm_base_url[0] == '\0') snprintf(cfg.llm_base_url, sizeof(cfg.llm_base_url), "%s", kDefaultBaseUrl);
    if (cfg.llm_model[0] == '\0')    snprintf(cfg.llm_model, sizeof(cfg.llm_model), "%s", kDefaultModel);
    if (cfg.tts_voice[0] == '\0')    snprintf(cfg.tts_voice, sizeof(cfg.tts_voice), "%s", kDefaultVoice);

    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        config_ = cfg;
        xSemaphoreGive(state_mutex_);
    } else {
        config_ = cfg;
    }
}

esp_err_t ZClawLlmClient::save_config(const ZClawConfig& cfg) {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(BOARD_NVS_NS_ZCLAW, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không mở được NVS %s: %s", BOARD_NVS_NS_ZCLAW, esp_err_to_name(err));
        return err;
    }

    nvs_set_str(h, "wifi_ssid", cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg.wifi_pass);
    nvs_set_str(h, "stt_key",   cfg.stt_api_key);
    nvs_set_str(h, "llm_key",   cfg.llm_api_key);
    nvs_set_str(h, "llm_url",   cfg.llm_base_url);
    nvs_set_str(h, "llm_model", cfg.llm_model);
    nvs_set_str(h, "tts_voice", cfg.tts_voice);
    nvs_set_u8(h,  "tts_gg",    cfg.tts_prefer_google ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            config_ = cfg;
            xSemaphoreGive(state_mutex_);
        } else {
            config_ = cfg;
        }
        ESP_LOGI(TAG, "Đã lưu cấu hình NVS (%s)", BOARD_NVS_NS_ZCLAW);
    }
    return err;
}

ZClawConfig ZClawLlmClient::get_config() const {
    ZClawConfig copy = {};
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = config_;
        xSemaphoreGive(state_mutex_);
    } else {
        copy = config_;
    }
    return copy;
}

bool ZClawLlmClient::has_wifi_credentials() const {
    return get_config().wifi_ssid[0] != '\0';
}

// ---------------------------------------------------------------------------
// Web Captive Portal & DNS Responder (100% Tiếng Việt)
// ---------------------------------------------------------------------------
void ZClawLlmClient::dns_portal_task(void* arg) {
    auto* self = static_cast<ZClawLlmClient*>(arg);
    self->dns_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (self->dns_sock_ < 0) {
        ESP_LOGE(TAG, "Không mở được UDP DNS socket cho Captive Portal");
        self->dns_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in srv_addr = {};
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    srv_addr.sin_port        = htons(53);

    if (bind(self->dns_sock_, reinterpret_cast<struct sockaddr*>(&srv_addr), sizeof(srv_addr)) < 0) {
        close(self->dns_sock_);
        self->dns_sock_ = -1;
        self->dns_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(self->dns_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t rx_buf[512];
    uint8_t tx_buf[512];

    while (self->dns_running_.load()) {
        struct sockaddr_in cli_addr = {};
        socklen_t cli_len = sizeof(cli_addr);
        int len = recvfrom(self->dns_sock_, rx_buf, sizeof(rx_buf), 0,
                           reinterpret_cast<struct sockaddr*>(&cli_addr), &cli_len);
        if (len < 12 || len > 480) continue;

        // Phản hồi bản ghi DNS Type A trỏ về 192.168.4.1
        memcpy(tx_buf, rx_buf, len);
        tx_buf[2] = 0x81; // QR=1, RD=1
        tx_buf[3] = 0x80; // RA=1, RCODE=0
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x01; // Answer count = 1
        tx_buf[8] = 0x00;
        tx_buf[9] = 0x00;
        tx_buf[10] = 0x00;
        tx_buf[11] = 0x00;

        static const uint8_t kDnsAnswer[] = {
            0xC0, 0x0C,             // Name pointer -> offset 12
            0x00, 0x01,             // Type A
            0x00, 0x01,             // Class IN
            0x00, 0x00, 0x00, 0x3C, // TTL = 60s
            0x00, 0x04,             // RDLENGTH = 4
            192, 168, 4, 1          // 192.168.4.1
        };
        if (len + sizeof(kDnsAnswer) <= sizeof(tx_buf)) {
            memcpy(tx_buf + len, kDnsAnswer, sizeof(kDnsAnswer));
            sendto(self->dns_sock_, tx_buf, len + sizeof(kDnsAnswer), 0,
                   reinterpret_cast<struct sockaddr*>(&cli_addr), cli_len);
        }
    }

    close(self->dns_sock_);
    self->dns_sock_ = -1;
    self->dns_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t ZClawLlmClient::http_captive_redirect_handler(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Đang chuyển hướng...", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_status_get_handler(httpd_req_t* req) {
    uint8_t mac[6] = {};
    EspNowMeshManager::instance().get_own_mac(mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON* j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "psram", board_has_psram());
    cJSON_AddNumberToObject(j, "free_heap", static_cast<double>(esp_get_free_heap_size()));
    cJSON_AddStringToObject(j, "mac_espnow", mac_str);
    cJSON_AddNumberToObject(j, "volume", AudioHal::instance().get_speaker_volume());
    cJSON_AddNumberToObject(j, "mic_level", AudioHal::instance().get_mic_sensitivity_level());
    cJSON_AddNumberToObject(j, "rotation", UiEyesClock::instance().get_rotation());

    char* str = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, str ? str : "{}");
    if (str) cJSON_free(str);
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_root_get_handler(httpd_req_t* req) {
    const ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    const uint8_t vol     = AudioHal::instance().get_speaker_volume();
    const uint8_t mic_lvl = AudioHal::instance().get_mic_sensitivity_level();
    const uint16_t rot    = UiEyesClock::instance().get_rotation();
    const uint8_t blk     = UiEyesClock::instance().get_backlight();

    std::string html;
    html.reserve(4800);
    html +=
        "<!DOCTYPE html><html lang='vi'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Cấu hình Xiaozhi-VN ZClaw</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#0b131e;color:#e6f1ff;margin:0;padding:16px}"
        ".card{max-width:480px;margin:0 auto;background:#132030;border:1px solid #00b4ff55;border-radius:14px;padding:18px;box-shadow:0 6px 24px #0008}"
        "h2{margin:0 0 6px;color:#00e5ff;font-size:20px}"
        "p.sub{margin:0 0 16px;font-size:13px;color:#9db4c8}"
        "label{display:block;margin:10px 0 4px;font-size:13px;font-weight:600;color:#bce3ff}"
        "input,select{width:100%;box-sizing:border-box;padding:9px 11px;border-radius:8px;border:1px solid #2b4560;background:#0a1522;color:#fff;font-size:14px}"
        ".row{display:flex;gap:8px}.row>div{flex:1}"
        ".presets{display:flex;gap:6px;flex-wrap:wrap;margin-top:6px}"
        ".presets button{background:#1b354d;color:#7fd8ff;border:1px solid #00b4ff66;border-radius:6px;padding:4px 8px;font-size:12px;cursor:pointer}"
        ".btn{width:100%;margin-top:16px;padding:12px;border:none;border-radius:10px;background:linear-gradient(90deg,#0088ff,#00e5ff);color:#001018;font-weight:700;font-size:15px;cursor:pointer}"
        ".badge{display:inline-block;padding:2px 8px;border-radius:99px;background:#00b4ff22;color:#00e5ff;font-size:12px;margin-bottom:10px}"
        "</style></head><body><div class='card'>"
        "<h2>Xiaozhi-VN ZClaw Edition</h2>"
        "<p class='sub'>Cấu hình Wi-Fi, Cloud AI & Phần cứng (Không qua xiaozhi.me)</p>"
        "<div class='badge'>PSRAM: ";
    html += board_has_psram() ? "Có (Tối ưu)" : "Không (SRAM Mode)";
    html += " | Heap trống: ";
    html += std::to_string(esp_get_free_heap_size() / 1024);
    html += " KB</div>"
            "<form method='POST' action='/save'>"
            "<label>Tên Wi-Fi (SSID)</label>"
            "<input name='ssid' maxlength='32' value='" + html_escape(cfg.wifi_ssid) + "' placeholder='Nhập tên Wi-Fi 2.4GHz'>"
            "<label>Mật khẩu Wi-Fi (để trống nếu giữ nguyên mật khẩu cũ)</label>"
            "<input name='pass' type='password' maxlength='64' placeholder='••••••••'>"
            "<label>Groq STT API Key (Whisper tiếng Việt miễn phí)</label>"
            "<input name='stt_key' type='password' maxlength='159' placeholder='";
    html += (cfg.stt_api_key[0] ? "Đã lưu (nhập mới để đổi)" : "gsk_... (nếu trùng Groq LLM có thể bỏ trống)");
    html += "'><label>Nhà cung cấp LLM nhanh</label><div class='presets'>"
            "<button type='button' onclick=\"setP('https://api.groq.com/openai/v1/chat/completions','llama-3.3-70b-versatile')\">Groq Llama-3.3</button>"
            "<button type='button' onclick=\"setP('https://generativelanguage.googleapis.com/v1beta/openai/chat/completions','gemini-2.5-flash')\">Gemini 2.5 Flash</button>"
            "<button type='button' onclick=\"setP('https://api.deepseek.com/chat/completions','deepseek-chat')\">DeepSeek V3</button>"
            "<button type='button' onclick=\"setP('https://api.openai.com/v1/chat/completions','gpt-4o-mini')\">OpenAI GPT-4o-mini</button>"
            "</div><label>LLM Base URL (OpenAI-compatible)</label>"
            "<input id='url' name='llm_url' maxlength='191' value='" + html_escape(cfg.llm_base_url) + "'>"
            "<label>Model LLM</label>"
            "<input id='mdl' name='llm_model' maxlength='63' value='" + html_escape(cfg.llm_model) + "'>"
            "<label>LLM API Key</label>"
            "<input name='llm_key' type='password' maxlength='159' placeholder='";
    html += (cfg.llm_api_key[0] ? "Đã lưu (nhập mới để đổi)" : "Nhập API Key của LLM");
    html += "'>"
            "<div class='row'><div><label>Giọng đọc TTS Tiếng Việt</label><select name='voice'>"
            "<option value='vi-VN-HoaiMyNeural'";
    if (strcmp(cfg.tts_voice, "vi-VN-HoaiMyNeural") == 0) html += " selected";
    html += ">Hoài My (Nữ)</option><option value='vi-VN-NamMinhNeural'";
    if (strcmp(cfg.tts_voice, "vi-VN-NamMinhNeural") == 0) html += " selected";
    html += ">Nam Minh (Nam)</option></select></div>"
            "<div><label>Chế độ TTS</label><select name='tts_gg'>"
            "<option value='0'";
    if (!cfg.tts_prefer_google) html += " selected";
    html += ">Edge + Google dự phòng</option><option value='1'";
    if (cfg.tts_prefer_google) html += " selected";
    html += ">Google TTS trực tiếp</option></select></div></div>"
            "<div class='row'>"
            "<div><label>Âm lượng loa (%)</label><input name='vol' type='number' min='0' max='100' value='" + std::to_string(vol) + "'></div>"
            "<div><label>Độ nhạy Mic (1-10)</label><input name='mic' type='number' min='1' max='10' value='" + std::to_string(mic_lvl) + "'></div>"
            "</div>"
            "<div class='row'>"
            "<div><label>Độ sáng LCD (%)</label><input name='blk' type='number' min='5' max='100' value='" + std::to_string(blk) + "'></div>"
            "<div><label>Xoay màn hình ST7789</label><select name='rot'>";
    for (int d : {0, 90, 180, 270}) {
        html += "<option value='" + std::to_string(d) + "'";
        if (rot == d) html += " selected";
        html += ">" + std::to_string(d) + "°</option>";
    }
    html += "</select></div></div>"
            "<label style='display:flex;align-items:center;gap:8px;margin-top:12px'>"
            "<input type='checkbox' name='reboot' value='1' checked style='width:auto'> Lưu xong tự khởi động lại kết nối Wi-Fi</label>"
            "<button class='btn' type='submit'>LƯU CẤU HÌNH</button></form></div>"
            "<script>function setP(u,m){document.getElementById('url').value=u;document.getElementById('mdl').value=m;}</script>"
            "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html.c_str(), html.size());
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_save_post_handler(httpd_req_t* req) {
    const int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Dữ liệu không hợp lệ");
        return ESP_FAIL;
    }

    std::string body(static_cast<size_t>(total), '\0');
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, &body[received], total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += r;
    }

    ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    bool do_reboot  = false;

    // Parse key=value&...
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            std::string key = url_decode_form(body.data() + pos, eq - pos);
            std::string val = url_decode_form(body.data() + eq + 1, amp - eq - 1);

            if (key == "ssid") {
                snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", val.c_str());
            } else if (key == "pass" && !val.empty()) {
                snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", val.c_str());
            } else if (key == "stt_key" && !val.empty()) {
                snprintf(cfg.stt_api_key, sizeof(cfg.stt_api_key), "%s", val.c_str());
            } else if (key == "llm_key" && !val.empty()) {
                snprintf(cfg.llm_api_key, sizeof(cfg.llm_api_key), "%s", val.c_str());
            } else if (key == "llm_url" && !val.empty()) {
                snprintf(cfg.llm_base_url, sizeof(cfg.llm_base_url), "%s", val.c_str());
            } else if (key == "llm_model" && !val.empty()) {
                snprintf(cfg.llm_model, sizeof(cfg.llm_model), "%s", val.c_str());
            } else if (key == "voice" && !val.empty()) {
                snprintf(cfg.tts_voice, sizeof(cfg.tts_voice), "%s", val.c_str());
            } else if (key == "tts_gg") {
                cfg.tts_prefer_google = (atoi(val.c_str()) != 0);
            } else if (key == "vol") {
                AudioHal::instance().set_speaker_volume(
                    static_cast<uint8_t>(std::max(0, std::min(100, atoi(val.c_str())))), true);
            } else if (key == "mic") {
                AudioHal::instance().set_mic_sensitivity_level(
                    static_cast<uint8_t>(std::max(1, std::min(10, atoi(val.c_str())))), true);
            } else if (key == "blk") {
                UiEyesClock::instance().set_backlight(
                    static_cast<uint8_t>(std::max(5, std::min(100, atoi(val.c_str())))), true);
            } else if (key == "rot") {
                UiEyesClock::instance().set_rotation(static_cast<uint16_t>(atoi(val.c_str())), true);
            } else if (key == "reboot") {
                do_reboot = (val == "1");
            }
        }
        pos = amp + 1;
    }

    ZClawLlmClient::instance().save_config(cfg);

    const char* ok_html =
        "<!DOCTYPE html><html lang='vi'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
        "<body style='font-family:sans-serif;background:#0b131e;color:#e6f1ff;text-align:center;padding:40px'>"
        "<h2 style='color:#00e5ff'>Đã lưu cấu hình thành công!</h2>"
        "<p>Thiết bị đang áp dụng thay đổi...</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, ok_html);

    if (do_reboot) {
        esp_timer_handle_t rst_tm = nullptr;
        esp_timer_create_args_t ta = {};
        ta.callback = delayed_restart_cb;
        ta.arg      = nullptr;
        ta.name     = "zclaw_rst";
        if (esp_timer_create(&ta, &rst_tm) == ESP_OK) {
            esp_timer_start_once(rst_tm, 1200000ULL); // 1.2 giây sau khi gửi xong HTTP
        }
    }
    return ESP_OK;
}

esp_err_t ZClawLlmClient::start_captive_portal(bool start_softap) {
    if (portal_active_.load()) return ESP_OK;

    if (start_softap) {
        static bool s_ap_netif_created = false;
        if (!s_ap_netif_created) {
            esp_netif_create_default_wifi_ap();
            s_ap_netif_created = true;
        }

        wifi_config_t ap_cfg = {};
        snprintf(reinterpret_cast<char*>(ap_cfg.ap.ssid), sizeof(ap_cfg.ap.ssid), "%s", kSoftApSsid);
        ap_cfg.ap.ssid_len       = static_cast<uint8_t>(strlen(kSoftApSsid));
        ap_cfg.ap.channel        = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        esp_wifi_start();

        // Chạy DNS Captive Responder
        dns_running_.store(true);
        xTaskCreatePinnedToCore(dns_portal_task, "dns_portal", 3072,
                                this, 3, &dns_task_handle_, 0);
    }

    httpd_config_t hcfg  = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 10;
    hcfg.stack_size       = 6144;
    hcfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&httpd_handle_, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không khởi chạy được Web Portal: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = http_root_get_handler, .user_ctx = nullptr
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = http_save_post_handler, .user_ctx = nullptr
    };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = http_status_get_handler, .user_ctx = nullptr
    };
    static const httpd_uri_t uri_204 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = http_captive_redirect_handler, .user_ctx = nullptr
    };
    static const httpd_uri_t uri_apple = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = http_captive_redirect_handler, .user_ctx = nullptr
    };
    static const httpd_uri_t uri_win = {
        .uri = "/connecttest.txt", .method = HTTP_GET, .handler = http_captive_redirect_handler, .user_ctx = nullptr
    };
    static const httpd_uri_t uri_wild = {
        .uri = "/*", .method = HTTP_GET, .handler = http_captive_redirect_handler, .user_ctx = nullptr
    };

    httpd_register_uri_handler(httpd_handle_, &uri_root);
    httpd_register_uri_handler(httpd_handle_, &uri_save);
    httpd_register_uri_handler(httpd_handle_, &uri_status);
    httpd_register_uri_handler(httpd_handle_, &uri_204);
    httpd_register_uri_handler(httpd_handle_, &uri_apple);
    httpd_register_uri_handler(httpd_handle_, &uri_win);
    httpd_register_uri_handler(httpd_handle_, &uri_wild);

    portal_active_.store(true);
    ESP_LOGI(TAG, "Web Captive Portal Tiếng Việt mở tại http://192.168.4.1 (SSID: %s)", kSoftApSsid);
    return ESP_OK;
}

void ZClawLlmClient::stop_captive_portal() {
    portal_active_.store(false);
    dns_running_.store(false);
    if (httpd_handle_) {
        httpd_stop(httpd_handle_);
        httpd_handle_ = nullptr;
    }
    for (int i = 0; i < 20 && dns_task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------------------
// Cloud STT — Groq Whisper API (whisper-large-v3-turbo, language="vi")
// ---------------------------------------------------------------------------
esp_err_t ZClawLlmClient::transcribe_wav(const AudioHal::CapturedAudio& audio, std::string& out_text) {
    out_text.clear();
    if (!audio.wav_data || audio.wav_size <= AudioHal::kWavHeaderBytes) {
        return ESP_ERR_INVALID_ARG;
    }

    const ZClawConfig cfg = get_config();
    const char* api_key = (cfg.stt_api_key[0] != '\0') ? cfg.stt_api_key : cfg.llm_api_key;
    if (api_key[0] == '\0') {
        ESP_LOGE(TAG, "Chưa cấu hình API Key STT/Groq");
        return ESP_ERR_INVALID_STATE;
    }

    // Dùng Groq Whisper làm mặc định; tự dùng OpenAI Whisper nếu URL là openai.com và không có stt_key riêng
    const bool use_openai_whisper =
        (cfg.stt_api_key[0] == '\0' && strstr(cfg.llm_base_url, "api.openai.com") != nullptr);
    const char* endpoint = use_openai_whisper ? kOpenAiSttUrl : kGroqSttUrl;
    const char* model    = use_openai_whisper ? "whisper-1" : "whisper-large-v3-turbo";

    static constexpr const char* kBoundary = "----ZClawWavBoundary987654";

    std::string prefix;
    prefix.reserve(512);
    prefix += "--"; prefix += kBoundary; prefix += "\r\n";
    prefix += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    prefix += model; prefix += "\r\n";

    prefix += "--"; prefix += kBoundary; prefix += "\r\n";
    prefix += "Content-Disposition: form-data; name=\"language\"\r\n\r\nvi\r\n";

    prefix += "--"; prefix += kBoundary; prefix += "\r\n";
    prefix += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";

    prefix += "--"; prefix += kBoundary; prefix += "\r\n";
    prefix += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
    prefix += "Content-Type: audio/wav\r\n\r\n";

    std::string suffix = "\r\n--";
    suffix += kBoundary;
    suffix += "--\r\n";

    const int total_len = static_cast<int>(prefix.size() + audio.wav_size + suffix.size());

    esp_http_client_config_t hcfg = {};
    hcfg.url               = endpoint;
    hcfg.method            = HTTP_METHOD_POST;
    hcfg.timeout_ms        = 20000;
    hcfg.buffer_size       = 2048;
    hcfg.buffer_size_tx    = 4096;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) return ESP_ERR_NO_MEM;

    char auth_hdr[200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", api_key);
    char ct_hdr[96];
    snprintf(ct_hdr, sizeof(ct_hdr), "multipart/form-data; boundary=%s", kBoundary);

    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", ct_hdr);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT open lỗi: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    auto write_all = [&](const uint8_t* ptr, size_t len) -> bool {
        size_t sent = 0;
        while (sent < len) {
            const size_t chunk = std::min<size_t>(4096, len - sent);
            int w = esp_http_client_write(
                client, reinterpret_cast<const char*>(ptr + sent), static_cast<int>(chunk));
            if (w <= 0) return false;
            sent += static_cast<size_t>(w);
        }
        return true;
    };

    bool ok = write_all(reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()) &&
              write_all(audio.wav_data, audio.wav_size) &&
              write_all(reinterpret_cast<const uint8_t*>(suffix.data()), suffix.size());

    if (!ok || esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "STT upload thất bại");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const int status = esp_http_client_get_status_code(client);
    std::string resp;
    char tmp[1024];
    int r = 0;
    while ((r = esp_http_client_read(client, tmp, sizeof(tmp))) > 0) {
        resp.append(tmp, static_cast<size_t>(r));
        if (resp.size() >= 8192) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "STT API lỗi HTTP %d: %s", status, resp.c_str());
        return ESP_FAIL;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return ESP_FAIL;
    const cJSON* jtext = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(jtext) && jtext->valuestring) {
        out_text = jtext->valuestring;
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "STT nhận dạng (%s): \"%s\"", model, out_text.c_str());
    return out_text.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
}

// ---------------------------------------------------------------------------
// Direct Cloud LLM Helpers
// ---------------------------------------------------------------------------
EyeEmotion ZClawLlmClient::extract_emotion_tag(const std::string& raw, std::string& out_clean) {
    EyeEmotion emotion = EyeEmotion::BINH_THUONG;
    out_clean = raw;

    static const struct { const char* tag; EyeEmotion em; } kTagMap[] = {
        {"[binh_thuong]", EyeEmotion::BINH_THUONG},
        {"[vui_ve]",      EyeEmotion::VUI_VE},
        {"[ngac_nhien]",  EyeEmotion::NGAC_NHIEN},
        {"[suy_nghi]",    EyeEmotion::SUY_NGHI},
        {"[buon]",        EyeEmotion::BUON},
        {"[ngu_gat]",     EyeEmotion::NGU_GAT},
        {"[dang_nghe]",   EyeEmotion::DANG_NGHE},
    };

    for (const auto& entry : kTagMap) {
        size_t pos = out_clean.find(entry.tag);
        if (pos != std::string::npos) {
            emotion = entry.em;
            out_clean.erase(pos, strlen(entry.tag));
            break;
        }
    }

    // Xóa khoảng trắng dư ở đầu/cuối sau khi tách tag
    size_t start = 0;
    while (start < out_clean.size() &&
           (out_clean[start] == ' ' || out_clean[start] == '\n' || out_clean[start] == '\r')) {
        start++;
    }
    size_t end = out_clean.size();
    while (end > start &&
           (out_clean[end - 1] == ' ' || out_clean[end - 1] == '\n' || out_clean[end - 1] == '\r')) {
        end--;
    }
    out_clean = out_clean.substr(start, end - start);
    return emotion;
}

esp_err_t ZClawLlmClient::post_json_chat_completion(const char* url,
                                                    const char* api_key,
                                                    const char* json_body,
                                                    std::string& out_response) {
    out_response.clear();
    if (!url || !api_key || !json_body || api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t hcfg = {};
    hcfg.url               = url;
    hcfg.method            = HTTP_METHOD_POST;
    hcfg.timeout_ms        = 25000;
    hcfg.buffer_size       = 4096;
    hcfg.buffer_size_tx    = 4096;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) return ESP_ERR_NO_MEM;

    char auth_hdr[200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");

    const size_t body_len = strlen(json_body);
    esp_err_t err = esp_http_client_open(client, static_cast<int>(body_len));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM open lỗi: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    size_t sent = 0;
    while (sent < body_len) {
        const size_t chunk = std::min<size_t>(4096, body_len - sent);
        int w = esp_http_client_write(client, json_body + sent, static_cast<int>(chunk));
        if (w <= 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        sent += static_cast<size_t>(w);
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const int status = esp_http_client_get_status_code(client);
    char* tmp = static_cast<char*>(board_malloc_prefer_spiram(2048));
    if (!tmp) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int r = 0;
    while ((r = esp_http_client_read(client, tmp, 2048)) > 0) {
        out_response.append(tmp, static_cast<size_t>(r));
        if (out_response.size() >= 49152) break;
    }

    board_free(tmp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "LLM API lỗi HTTP %d: %.*s", status,
                 static_cast<int>(std::min<size_t>(240, out_response.size())),
                 out_response.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// chat_with_tools() — Direct LLM Client + Sliding Window + MCP Tool Loop
// ---------------------------------------------------------------------------
esp_err_t ZClawLlmClient::chat_with_tools(const std::string& user_text,
                                           std::string& out_reply_clean,
                                           EyeEmotion& out_emotion) {
    out_reply_clean.clear();
    out_emotion = EyeEmotion::BINH_THUONG;

    const ZClawConfig cfg = get_config();
    if (cfg.llm_api_key[0] == '\0') {
        out_reply_clean = "Bạn chưa cài đặt API Key trí tuệ nhân tạo. Hãy giữ nút chính 5 giây để mở cổng cấu hình Wi-Fi.";
        return ESP_ERR_INVALID_STATE;
    }

    const bool deep_mode = next_turn_deep_reasoning_.exchange(false);

    // Dựng mảng messages: [system] + [sliding window history] + [current user]
    cJSON* messages = cJSON_CreateArray();
    {
        cJSON* sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", kSystemPromptVi);
        cJSON_AddItemToArray(messages, sys);
    }

    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& h : history_) {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", h.role.c_str());
            cJSON_AddStringToObject(m, "content", h.content.c_str());
            cJSON_AddItemToArray(messages, m);
        }
        xSemaphoreGive(state_mutex_);
    }

    {
        cJSON* u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", user_text.c_str());
        cJSON_AddItemToArray(messages, u);
    }

    cJSON* tools_arr = McpRegistry::instance().build_tools_schema_json();
    std::string final_raw;
    esp_err_t final_err = ESP_FAIL;

    constexpr int kMaxToolIters = 4;
    for (int iter = 0; iter < kMaxToolIters; ++iter) {
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "model", cfg.llm_model);
        cJSON_AddItemReferenceToObject(payload, "messages", messages);
        cJSON_AddItemReferenceToObject(payload, "tools", tools_arr);
        cJSON_AddStringToObject(payload, "tool_choice", "auto");
        cJSON_AddNumberToObject(payload, "max_tokens", deep_mode ? 1024 : 512);
        cJSON_AddNumberToObject(payload, "temperature", deep_mode ? 0.3 : 0.7);
        cJSON_AddBoolToObject(payload, "stream", false);

        char* body_str = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!body_str) {
            final_err = ESP_ERR_NO_MEM;
            break;
        }

        std::string resp_json;
        final_err = post_json_chat_completion(cfg.llm_base_url, cfg.llm_api_key, body_str, resp_json);
        cJSON_free(body_str);
        if (final_err != ESP_OK) {
            break;
        }

        cJSON* root = cJSON_Parse(resp_json.c_str());
        if (!root) {
            final_err = ESP_FAIL;
            break;
        }

        cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        cJSON* ch0     = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
        cJSON* msg     = ch0 ? cJSON_GetObjectItemCaseSensitive(ch0, "message") : nullptr;
        if (!msg) {
            cJSON_Delete(root);
            final_err = ESP_FAIL;
            break;
        }

        cJSON* tool_calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            // Đẩy assistant message (kèm tool_calls) vào chuỗi hội thoại tạm của lượt này
            cJSON_AddItemToArray(messages, cJSON_Duplicate(msg, true));

            const int n_calls = cJSON_GetArraySize(tool_calls);
            for (int ti = 0; ti < n_calls; ++ti) {
                cJSON* tc   = cJSON_GetArrayItem(tool_calls, ti);
                cJSON* jid  = cJSON_GetObjectItemCaseSensitive(tc, "id");
                cJSON* fn   = cJSON_GetObjectItemCaseSensitive(tc, "function");
                cJSON* jnm  = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : nullptr;
                cJSON* jarg = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : nullptr;

                const char* call_id   = (cJSON_IsString(jid) && jid->valuestring) ? jid->valuestring : "call_0";
                const char* tool_name = (cJSON_IsString(jnm) && jnm->valuestring) ? jnm->valuestring : "";
                const char* args_str  = (cJSON_IsString(jarg) && jarg->valuestring) ? jarg->valuestring : "{}";

                std::string tool_res = McpRegistry::instance().execute_tool(tool_name, args_str);

                cJSON* tmsg = cJSON_CreateObject();
                cJSON_AddStringToObject(tmsg, "role", "tool");
                cJSON_AddStringToObject(tmsg, "tool_call_id", call_id);
                cJSON_AddStringToObject(tmsg, "name", tool_name);
                cJSON_AddStringToObject(tmsg, "content", tool_res.c_str());
                cJSON_AddItemToArray(messages, tmsg);
            }
            cJSON_Delete(root);
            continue;
        }

        cJSON* jcontent = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (cJSON_IsString(jcontent) && jcontent->valuestring) {
            final_raw = jcontent->valuestring;
            final_err = ESP_OK;
        } else {
            final_err = ESP_FAIL;
        }
        cJSON_Delete(root);
        break;
    }

    cJSON_Delete(tools_arr);
    cJSON_Delete(messages);

    if (final_err != ESP_OK || final_raw.empty()) {
        out_emotion     = EyeEmotion::BUON;
        out_reply_clean = "Xin lỗi, kết nối tới máy chủ AI đang gặp sự cố. Bạn thử lại giúp mình nhé.";
        return final_err != ESP_OK ? final_err : ESP_FAIL;
    }

    out_emotion = extract_emotion_tag(final_raw, out_reply_clean);

    // Lưu lịch sử Sliding Window (giữ tối đa kMaxHistoryPairs cặp user+assistant)
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        history_.push_back({"user", user_text});
        history_.push_back({"assistant", out_reply_clean});
        while (history_.size() > kMaxHistoryPairs * 2) {
            history_.erase(history_.begin(), history_.begin() + 2);
        }
        xSemaphoreGive(state_mutex_);
    }

    ESP_LOGI(TAG, "LLM trả lời (emotion=%d): %s", static_cast<int>(out_emotion), out_reply_clean.c_str());
    return ESP_OK;
}

esp_err_t ZClawLlmClient::run_deep_reasoning(const std::string& problem, std::string& out_analysis) {
    out_analysis.clear();
    const ZClawConfig cfg = get_config();
    if (cfg.llm_api_key[0] == '\0') return ESP_ERR_INVALID_STATE;

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", cfg.llm_model);
    cJSON* msgs = cJSON_CreateArray();

    cJSON* sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
                            "Hãy phân tích từng bước một cách logic, chính xác bằng tiếng Việt, rồi kết luận ngắn gọn rõ ràng.");
    cJSON_AddItemToArray(msgs, sys);

    cJSON* usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", problem.c_str());
    cJSON_AddItemToArray(msgs, usr);

    cJSON_AddItemToObject(payload, "messages", msgs);
    cJSON_AddNumberToObject(payload, "max_tokens", 1200);
    cJSON_AddNumberToObject(payload, "temperature", 0.2);

    char* body_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!body_str) return ESP_ERR_NO_MEM;

    std::string resp;
    esp_err_t err = post_json_chat_completion(cfg.llm_base_url, cfg.llm_api_key, body_str, resp);
    cJSON_free(body_str);
    if (err != ESP_OK) return err;

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return ESP_FAIL;
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON* ch0     = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    cJSON* msg     = ch0 ? cJSON_GetObjectItemCaseSensitive(ch0, "message") : nullptr;
    cJSON* content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : nullptr;
    if (cJSON_IsString(content) && content->valuestring) {
        out_analysis = content->valuestring;
        err = ESP_OK;
    } else {
        err = ESP_FAIL;
    }
    cJSON_Delete(root);
    return err;
}

void ZClawLlmClient::clear_history() {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        history_.clear();
        xSemaphoreGive(state_mutex_);
    } else {
        history_.clear();
    }
    ESP_LOGI(TAG, "Đã xóa lịch sử hội thoại tạm thời");
}

// ---------------------------------------------------------------------------
// MP3 HTTP Streaming + minimp3 RAM Decoding
// ---------------------------------------------------------------------------
esp_err_t ZClawLlmClient::stream_mp3_url(const char* url, uint32_t max_duration_ms) {
    if (!url || url[0] == '\0') return ESP_ERR_INVALID_ARG;

    Mp3RamContext mp3;
    if (!mp3.alloc()) {
        ESP_LOGE(TAG, "Thiếu RAM cấp phát bộ giải mã minimp3");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t hcfg = {};
    hcfg.url               = url;
    hcfg.method            = HTTP_METHOD_GET;
    hcfg.timeout_ms        = 12000;
    hcfg.buffer_size       = 4096;
    hcfg.buffer_size_tx    = 1024;
    hcfg.user_agent        = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_OK;
    int status    = 0;

    // Theo dõi redirect 301/302/307/308 (hay gặp ở các luồng Radio Icecast / CDN)
    for (int hop = 0; hop < 4; ++hop) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MP3 stream open lỗi: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return err;
        }

        if (esp_http_client_fetch_headers(client) < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_close(client);
            if (esp_http_client_set_redirection(client) != ESP_OK) {
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            continue;
        }
        break;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "MP3 HTTP status %d từ %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const int64_t start_us = esp_timer_get_time();
    bool got_audio = false;

    while (!AudioHal::instance().is_playback_stop_requested()) {
        if (max_duration_ms > 0) {
            const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000ULL);
            if (elapsed_ms >= max_duration_ms) break;
        }

        const size_t free_space = Mp3RamContext::kInBufSize - mp3.in_len;
        if (free_space == 0) {
            // Đẩy giải mã hết bộ đệm nếu đang đầy (phòng trường hợp sync lâu)
            if (mp3.decode_and_play_available(true) != ESP_OK) break;
            continue;
        }

        int r = esp_http_client_read(
            client,
            reinterpret_cast<char*>(mp3.in_buf + mp3.in_len),
            static_cast<int>(free_space));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            err = mp3.decode_and_play_available(true);
            break;
        }

        got_audio = true;
        mp3.in_len += static_cast<size_t>(r);
        err = mp3.decode_and_play_available(false);
        if (err != ESP_OK) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (AudioHal::instance().is_playback_stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    return got_audio ? err : ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Microsoft Edge TTS WebSocket Streaming (Free Endpoint)
// ---------------------------------------------------------------------------
struct EdgeWsContext {
    StreamBufferHandle_t stream = nullptr;
    EventGroupHandle_t   events = nullptr;
    bool                 in_audio_payload = false;
    size_t               hdr_skip_rem     = 0;
};

static constexpr EventBits_t kWsConnectedBit = BIT0;
static constexpr EventBits_t kWsTurnEndBit   = BIT1;
static constexpr EventBits_t kWsErrorBit     = BIT2;

static void edge_ws_event_handler(void* arg,
                                  esp_event_base_t /*base*/,
                                  int32_t event_id,
                                  void* event_data) {
    auto* ctx  = static_cast<EdgeWsContext*>(arg);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
    if (!ctx || !ctx->events) return;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupSetBits(ctx->events, kWsConnectedBit);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (!data || data->data_len <= 0) break;
            if (data->op_code == 0x01) {
                // Khung Text báo hiệu kết thúc lượt audio
                if (data->data_ptr &&
                    std::string_view(data->data_ptr, static_cast<size_t>(data->data_len))
                        .find("Path:turn.end") != std::string_view::npos) {
                    xEventGroupSetBits(ctx->events, kWsTurnEndBit);
                }
            } else if (data->op_code == 0x02 || (data->op_code == 0x00 && data->payload_offset > 0)) {
                // Khung Binary (hoặc continuation fragment): [2 bytes header_len BE] + [header] + [raw MP3]
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data->data_ptr);
                size_t len         = static_cast<size_t>(data->data_len);

                if (data->payload_offset == 0 && len >= 2) {
                    const uint16_t hdr_len = (static_cast<uint16_t>(ptr[0]) << 8) | ptr[1];
                    const size_t prefix    = 2U + hdr_len;
                    if (len > prefix) {
                        ptr += prefix;
                        len -= prefix;
                    } else {
                        len = 0;
                    }
                }
                if (len > 0 && ctx->stream) {
                    xStreamBufferSend(ctx->stream, ptr, len, pdMS_TO_TICKS(50));
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
        case WEBSOCKET_EVENT_DISCONNECTED:
            xEventGroupSetBits(ctx->events, kWsErrorBit);
            break;

        default:
            break;
    }
}

esp_err_t ZClawLlmClient::speak_chunk_edge_ws(const std::string& text_chunk, const char* voice) {
    EdgeWsContext ctx;
    ctx.stream = xStreamBufferCreate(16384, 1);
    ctx.events = xEventGroupCreate();
    if (!ctx.stream || !ctx.events) {
        if (ctx.stream) vStreamBufferDelete(ctx.stream);
        if (ctx.events) vEventGroupDelete(ctx.events);
        return ESP_ERR_NO_MEM;
    }

    Mp3RamContext mp3;
    if (!mp3.alloc()) {
        vStreamBufferDelete(ctx.stream);
        vEventGroupDelete(ctx.events);
        return ESP_ERR_NO_MEM;
    }

    // Sinh ConnectionId 32 hex
    char conn_id[33];
    for (int i = 0; i < 4; ++i) {
        snprintf(conn_id + i * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
    }

    char uri[224];
    snprintf(uri, sizeof(uri),
             "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1"
             "?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4&ConnectionId=%s",
             conn_id);

    esp_websocket_client_config_t wcfg = {};
    wcfg.uri                   = uri;
    wcfg.crt_bundle_attach     = esp_crt_bundle_attach;
    wcfg.disable_auto_reconnect = true;
    wcfg.network_timeout_ms    = 4000;
    wcfg.buffer_size           = 4096;
    wcfg.headers               =
        "Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "Chrome/130.0.0.0 Safari/537.36 Edg/130.0.0.0\r\n";

    esp_websocket_client_handle_t ws = esp_websocket_client_init(&wcfg);
    if (!ws) {
        vStreamBufferDelete(ctx.stream);
        vEventGroupDelete(ctx.events);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, edge_ws_event_handler, &ctx);
    esp_err_t err = esp_websocket_client_start(ws);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(ws);
        vStreamBufferDelete(ctx.stream);
        vEventGroupDelete(ctx.events);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ctx.events, kWsConnectedBit | kWsErrorBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    if ((bits & kWsConnectedBit) == 0 || (bits & kWsErrorBit) != 0) {
        esp_websocket_client_close(ws, pdMS_TO_TICKS(500));
        esp_websocket_client_destroy(ws);
        vStreamBufferDelete(ctx.stream);
        vEventGroupDelete(ctx.events);
        return ESP_FAIL;
    }

    // 1. Gửi speech.config
    const char* cfg_msg =
        "Content-Type:application/json; charset=utf-8\r\n"
        "Path:speech.config\r\n\r\n"
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
        "\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"false\"},"
        "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}";
    esp_websocket_client_send_text(ws, cfg_msg, static_cast<int>(strlen(cfg_msg)), pdMS_TO_TICKS(1000));

    // 2. Gửi SSML
    std::string esc_text = xml_escape(text_chunk.c_str());
    std::string ssml;
    ssml.reserve(256 + esc_text.size());
    ssml += "X-RequestId:"; ssml += conn_id; ssml += "\r\n";
    ssml += "Content-Type:application/ssml+xml\r\nPath:ssml\r\n\r\n";
    ssml += "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='vi-VN'>";
    ssml += "<voice name='"; ssml += voice; ssml += "'>";
    ssml += "<prosody pitch='+0Hz' rate='+0%' volume='+0%'>";
    ssml += esc_text;
    ssml += "</prosody></voice></speak>";
    esp_websocket_client_send_text(ws, ssml.c_str(), static_cast<int>(ssml.size()), pdMS_TO_TICKS(1000));

    bool got_audio = false;
    const int64_t start_us = esp_timer_get_time();

    while (!AudioHal::instance().is_playback_stop_requested()) {
        const size_t free_sp = Mp3RamContext::kInBufSize - mp3.in_len;
        if (free_sp > 0) {
            size_t got = xStreamBufferReceive(
                ctx.stream, mp3.in_buf + mp3.in_len, free_sp, pdMS_TO_TICKS(40));
            if (got > 0) {
                got_audio = true;
                mp3.in_len += got;
                if (mp3.decode_and_play_available(false) != ESP_OK) break;
            }
        }

        EventBits_t cur = xEventGroupGetBits(ctx.events);
        if ((cur & (kWsTurnEndBit | kWsErrorBit)) != 0 && xStreamBufferIsEmpty(ctx.stream)) {
            mp3.decode_and_play_available(true);
            break;
        }
        if ((esp_timer_get_time() - start_us) > 15000000LL) {
            break; // Timeout 15s/câu
        }
    }

    esp_websocket_client_close(ws, pdMS_TO_TICKS(400));
    esp_websocket_client_destroy(ws);
    vStreamBufferDelete(ctx.stream);
    vEventGroupDelete(ctx.events);

    if (AudioHal::instance().is_playback_stop_requested()) return ESP_ERR_INVALID_STATE;
    return got_audio ? ESP_OK : ESP_FAIL;
}

esp_err_t ZClawLlmClient::speak_chunk_google_tts(const std::string& text_chunk) {
    std::string url =
        "https://translate.google.com/translate_tts?ie=UTF-8&tl=vi&client=tw-ob&q=" +
        url_encode_str(text_chunk.c_str());
    return stream_mp3_url(url.c_str(), 0);
}

// ---------------------------------------------------------------------------
// speak_vietnamese() — Edge TTS WebSocket + Fallback Google TTS Tiếng Việt
// ---------------------------------------------------------------------------
esp_err_t ZClawLlmClient::speak_vietnamese(const std::string& text) {
    if (text.empty()) return ESP_OK;

    AudioHal::instance().clear_stop_playback();
    const ZClawConfig cfg = get_config();

    std::vector<std::string> chunks = split_tts_sentences(text, 165);
    esp_err_t overall = ESP_OK;

    for (const std::string& chunk : chunks) {
        if (AudioHal::instance().is_playback_stop_requested()) {
            overall = ESP_ERR_INVALID_STATE;
            break;
        }

        esp_err_t ret = ESP_FAIL;
        if (!cfg.tts_prefer_google) {
            ret = speak_chunk_edge_ws(chunk, cfg.tts_voice);
            if (ret == ESP_ERR_INVALID_STATE) {
                overall = ret;
                break;
            }
        }

        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "Dùng Google TTS tiếng Việt cho đoạn (%u bytes)", (unsigned)chunk.size());
            ret = speak_chunk_google_tts(chunk);
            if (ret != ESP_OK) {
                overall = ret;
                if (ret == ESP_ERR_INVALID_STATE) break;
            }
        }
    }

    AudioHal::instance().flush_speaker_silence(40);
    return overall;
}

// ---------------------------------------------------------------------------
// Pending Radio Playback Helpers
// ---------------------------------------------------------------------------
void ZClawLlmClient::schedule_radio_playback(const char* url, const char* station_name) {
    if (!url) return;
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        pending_radio_url_  = url;
        pending_radio_name_ = station_name ? station_name : "Radio";
        xSemaphoreGive(state_mutex_);
    } else {
        pending_radio_url_  = url;
        pending_radio_name_ = station_name ? station_name : "Radio";
    }
}

bool ZClawLlmClient::consume_pending_radio(std::string& out_url, std::string& out_name) {
    bool has = false;
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!pending_radio_url_.empty()) {
            out_url  = pending_radio_url_;
            out_name = pending_radio_name_;
            pending_radio_url_.clear();
            pending_radio_name_.clear();
            has = true;
        }
        xSemaphoreGive(state_mutex_);
    }
    return has;
}

void ZClawLlmClient::clear_pending_radio() {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        pending_radio_url_.clear();
        pending_radio_name_.clear();
        xSemaphoreGive(state_mutex_);
    }
}

} // namespace zclaw
