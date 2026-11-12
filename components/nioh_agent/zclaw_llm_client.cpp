// zclaw_llm_client.cpp
// S.W.Ni-Oh — Direct Cloud LLM + Groq Whisper STT + Edge/Google TTS
// + minimp3 RAM Decoder + MJPEG ROM TJpgDec + Web Captive Portal Tiếng Việt
// Production-ready C++17 implementation — no TODOs, no pseudo-code, no exceptions.

#include "zclaw_llm_client.h"
#include "board_config.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#if __has_include("esp_rom_tjpgd.h")
#include "esp_rom_tjpgd.h"
#else
#include "esp32s3/rom/tjpgd.h"
#endif
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
#include <type_traits>

static const char* TAG = "NiOhAgent";

namespace zclaw {

namespace {

constexpr const char* kDefaultBaseUrl        = "https://api.groq.com/openai/v1";
constexpr const char* kDefaultModel          = "llama-3.3-70b-versatile";
constexpr const char* kDefaultVoice          = "vi-VN-HoaiMyNeural";
constexpr const char* kGroqSttUrl            = "https://api.groq.com/openai/v1/audio/transcriptions";
constexpr const char* kOpenAiSttUrl          = "https://api.openai.com/v1/audio/transcriptions";
constexpr const char* kSoftApSsid            = "S.W.Ni-Oh-Setup";
constexpr const char* kDefaultSearchEngine   = "ddg_wiki";
constexpr const char* kDefaultPipedInstances = "https://pipedapi.kavin.rocks,https://pipedapi.adminforge.de,https://pipedapi.tokhmi.xyz";

constexpr const char* kSystemPromptVi =
    "Bạn là S.W.Ni-Oh, trợ lý AI thông minh nói tiếng Việt tự nhiên, thân thiện và ngắn gọn.\n"
    "QUY TẮC BẮT BUỘC:\n"
    "1. Luôn mở đầu MỌI câu trả lời cuối cùng bằng đúng 1 thẻ cảm xúc trong danh sách sau: "
    "[binh_thuong], [vui_ve], [ngac_nhien], [suy_nghi], [buon], [ngu_gat].\n"
    "2. Trả lời tự nhiên như đang trò chuyện bằng giọng nói (tránh dùng ký hiệu markdown **, #, dấu gạch đầu dòng phức tạp).\n"
    "3. Luôn ưu tiên gọi các công cụ MCP tiếng Việt khi người dùng hỏi thời tiết, giờ/âm lịch, tin tức, tra cứu thông tin, phát nhạc YouTube/SoundCloud (phat_nhac_truc_tuyen — luôn mặc định nen_tang='youtube' hoặc 'soundcloud', CHỈ chọn 'radio' khi người dùng nói rõ đài Radio/VOV), xem video trực tiếp lên màn hình (xem_video_truc_tuyen), suy luận sâu, điều khiển thiết bị, gửi ESP-NOW hoặc hẹn giờ Pomodoro.\n"
    "4. Giữ câu trả lời súc tích khoảng 2-4 câu trừ khi người dùng yêu cầu phân tích chi tiết.";

// URL encode helper (RFC 3986 %20 cho khoảng trắng để tương thích tuyệt đối Google TTS & Proxy)
std::string url_encode_str(const char* src) {
    if (!src) return "";
    std::string out;
    out.reserve(strlen(src) * 3);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src); *p; ++p) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
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
// Multi-Provider Helpers & Endpoint Builders (ESP-Claw style)
// ---------------------------------------------------------------------------
namespace {

void init_default_profile(LlmProfile& p, size_t idx) {
    memset(&p, 0, sizeof(p));
    if (idx == 0) {
        snprintf(p.name, sizeof(p.name), "Groq Primary");
        snprintf(p.provider, sizeof(p.provider), "groq");
        snprintf(p.base_url, sizeof(p.base_url), "%s", kDefaultBaseUrl);
        snprintf(p.model, sizeof(p.model), "%s", kDefaultModel);
        snprintf(p.auth_scheme, sizeof(p.auth_scheme), "bearer");
        p.enabled = true;
    } else {
        snprintf(p.name, sizeof(p.name), "Fallback #%u", static_cast<unsigned>(idx + 1));
        snprintf(p.provider, sizeof(p.provider), "openrouter");
        snprintf(p.base_url, sizeof(p.base_url), "https://openrouter.ai/api/v1");
        snprintf(p.model, sizeof(p.model), "openai/gpt-4o-mini");
        snprintf(p.auth_scheme, sizeof(p.auth_scheme), "bearer");
        p.enabled = false;
    }
    p.raw_endpoint = false;
}

void sync_legacy_mirrors_from_active(ZClawConfig& cfg) {
    if (cfg.active_profile_idx >= ZClawConfig::kMaxLlmProfiles) {
        cfg.active_profile_idx = 0;
    }
    const LlmProfile& act = cfg.llm_profiles[cfg.active_profile_idx];
    snprintf(cfg.llm_api_key, sizeof(cfg.llm_api_key), "%s", act.api_key);
    snprintf(cfg.llm_base_url, sizeof(cfg.llm_base_url), "%s", act.base_url);
    snprintf(cfg.llm_model, sizeof(cfg.llm_model), "%s", act.model);
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t sl = strlen(suffix);
    if (s.size() < sl) return false;
    return strncasecmp(s.c_str() + (s.size() - sl), suffix, sl) == 0;
}

void append_query_kv(std::string& url, const char* key, const char* val) {
    url.push_back(url.find('?') == std::string::npos ? '?' : '&');
    url += key;
    url.push_back('=');
    url += url_encode_str(val);
}

std::string resolve_chat_url(const LlmProfile& prof) {
    std::string u = prof.base_url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.empty()) return "";

    if (!prof.raw_endpoint) {
        if (strcmp(prof.provider, "anthropic") == 0) {
            if (!ends_with_ci(u, "/messages")) {
                if (ends_with_ci(u, "/v1")) u += "/messages";
                else u += "/v1/messages";
            }
        } else if (strcmp(prof.provider, "gemini_native") == 0) {
            if (u.find(":generateContent") == std::string::npos) {
                if (u.find("/models") == std::string::npos) u += "/models/";
                else if (u.back() != '/') u.push_back('/');
                u += prof.model;
                u += ":generateContent";
            }
        } else {
            if (!ends_with_ci(u, "/chat/completions")) {
                if (ends_with_ci(u, "/v1") || ends_with_ci(u, "/openai") ||
                    ends_with_ci(u, "/v1beta/openai") || ends_with_ci(u, "/ai/v1") ||
                    ends_with_ci(u, "/api/v1") || ends_with_ci(u, "/v2")) {
                    u += "/chat/completions";
                } else {
                    u += "/v1/chat/completions";
                }
            }
        }
    }

    if (strcmp(prof.auth_scheme, "query_param") == 0 && prof.api_key[0] != '\0') {
        const char* qname = prof.custom_auth_name[0] ? prof.custom_auth_name : "key";
        append_query_kv(u, qname, prof.api_key);
    }
    return u;
}

std::string resolve_models_url(const LlmProfile& prof) {
    std::string u = prof.base_url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.empty()) return "";

    // Nếu người dùng đã nhập endpoint /chat/completions -> lùi về /models
    const char* suffix = "/chat/completions";
    if (ends_with_ci(u, suffix)) {
        u.erase(u.size() - strlen(suffix));
    } else if (ends_with_ci(u, "/messages")) {
        u.erase(u.size() - strlen("/messages"));
    }

    if (strcmp(prof.provider, "gemini_native") == 0) {
        if (!ends_with_ci(u, "/models")) u += "/models";
    } else {
        if (!ends_with_ci(u, "/models")) {
            if (ends_with_ci(u, "/v1") || ends_with_ci(u, "/openai") ||
                ends_with_ci(u, "/v1beta/openai") || ends_with_ci(u, "/api/v1") ||
                ends_with_ci(u, "/ai/v1") || ends_with_ci(u, "/v2")) {
                u += "/models";
            } else {
                u += "/v1/models";
            }
        }
    }

    if (strcmp(prof.auth_scheme, "query_param") == 0 && prof.api_key[0] != '\0') {
        const char* qname = prof.custom_auth_name[0] ? prof.custom_auth_name : "key";
        append_query_kv(u, qname, prof.api_key);
    }
    return u;
}

void apply_profile_auth_headers(esp_http_client_handle_t client, const LlmProfile& prof) {
    if (prof.api_key[0] == '\0') return;
    if (strcmp(prof.auth_scheme, "query_param") == 0) return;

    if (strcmp(prof.auth_scheme, "x-api-key") == 0) {
        esp_http_client_set_header(client, "x-api-key", prof.api_key);
        if (strcmp(prof.provider, "anthropic") == 0) {
            esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
        }
    } else if (strcmp(prof.auth_scheme, "x-goog-api-key") == 0) {
        esp_http_client_set_header(client, "x-goog-api-key", prof.api_key);
    } else if (strcmp(prof.auth_scheme, "custom_header") == 0 && prof.custom_auth_name[0] != '\0') {
        esp_http_client_set_header(client, prof.custom_auth_name, prof.api_key);
    } else {
        char auth_hdr[200];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", prof.api_key);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }
}

cJSON* serialize_profiles_json(const ZClawConfig& cfg) {
    cJSON* arr = cJSON_CreateArray();
    for (size_t i = 0; i < ZClawConfig::kMaxLlmProfiles; ++i) {
        const LlmProfile& p = cfg.llm_profiles[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name);
        cJSON_AddStringToObject(o, "provider", p.provider);
        cJSON_AddStringToObject(o, "base_url", p.base_url);
        cJSON_AddStringToObject(o, "model", p.model);
        cJSON_AddStringToObject(o, "api_key", p.api_key);
        cJSON_AddStringToObject(o, "auth_scheme", p.auth_scheme);
        cJSON_AddStringToObject(o, "custom_auth_name", p.custom_auth_name);
        cJSON_AddBoolToObject(o, "raw_endpoint", p.raw_endpoint);
        cJSON_AddBoolToObject(o, "enabled", p.enabled);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

void deserialize_profiles_json(const char* json_str, ZClawConfig& cfg, bool mask_keep_existing = false) {
    if (!json_str || !json_str[0]) return;
    cJSON* arr = cJSON_Parse(json_str);
    if (!cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return;
    }
    const int n = std::min<int>(cJSON_GetArraySize(arr), static_cast<int>(ZClawConfig::kMaxLlmProfiles));
    for (int i = 0; i < n; ++i) {
        cJSON* o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) continue;
        LlmProfile& p = cfg.llm_profiles[i];
        auto copy_s = [&](const char* k, char* dst, size_t max_len) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(o, k);
            if (cJSON_IsString(j) && j->valuestring) {
                if (mask_keep_existing && strcmp(k, "api_key") == 0 && strcmp(j->valuestring, "__KEEP__") == 0) {
                    return;
                }
                snprintf(dst, max_len, "%s", j->valuestring);
            }
        };
        copy_s("name", p.name, sizeof(p.name));
        copy_s("provider", p.provider, sizeof(p.provider));
        copy_s("base_url", p.base_url, sizeof(p.base_url));
        copy_s("model", p.model, sizeof(p.model));
        copy_s("api_key", p.api_key, sizeof(p.api_key));
        copy_s("auth_scheme", p.auth_scheme, sizeof(p.auth_scheme));
        copy_s("custom_auth_name", p.custom_auth_name, sizeof(p.custom_auth_name));
        cJSON* jr = cJSON_GetObjectItemCaseSensitive(o, "raw_endpoint");
        if (cJSON_IsBool(jr)) p.raw_endpoint = cJSON_IsTrue(jr);
        cJSON* je = cJSON_GetObjectItemCaseSensitive(o, "enabled");
        if (cJSON_IsBool(je)) p.enabled = cJSON_IsTrue(je);
    }
    cJSON_Delete(arr);
}

} // namespace

// ---------------------------------------------------------------------------
// NVS Configuration (BOARD_NVS_NS_ZCLAW = "zclaw_cfg")
// ---------------------------------------------------------------------------
void ZClawLlmClient::load_config() {
    ZClawConfig cfg = {};
    for (size_t i = 0; i < ZClawConfig::kMaxLlmProfiles; ++i) {
        init_default_profile(cfg.llm_profiles[i], i);
    }
    cfg.active_profile_idx = 0;
    cfg.auto_fallback      = true;
    snprintf(cfg.tts_voice, sizeof(cfg.tts_voice), "%s", kDefaultVoice);
    cfg.tts_prefer_google = false;
    snprintf(cfg.search_engine, sizeof(cfg.search_engine), "%s", kDefaultSearchEngine);
    snprintf(cfg.piped_instances, sizeof(cfg.piped_instances), "%s", kDefaultPipedInstances);
    snprintf(cfg.video_res, sizeof(cfg.video_res), "240x240");
    cfg.video_fps          = 12;
    cfg.video_jpeg_quality = 75;

    nvs_handle_t h = 0;
    if (nvs_open(BOARD_NVS_NS_ZCLAW, NVS_READONLY, &h) == ESP_OK) {
        auto read_str = [&](const char* key, char* dst, size_t max_len) {
            size_t len = max_len;
            nvs_get_str(h, key, dst, &len);
        };
        read_str("wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
        read_str("wifi_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
        read_str("stt_key",   cfg.stt_api_key, sizeof(cfg.stt_api_key));
        read_str("llm_key",   cfg.llm_profiles[0].api_key, sizeof(cfg.llm_profiles[0].api_key));
        read_str("llm_url",   cfg.llm_profiles[0].base_url, sizeof(cfg.llm_profiles[0].base_url));
        read_str("llm_model", cfg.llm_profiles[0].model, sizeof(cfg.llm_profiles[0].model));
        read_str("tts_voice", cfg.tts_voice, sizeof(cfg.tts_voice));
        read_str("media_proxy", cfg.media_proxy_url, sizeof(cfg.media_proxy_url));
        read_str("sc_cid",      cfg.soundcloud_client_id, sizeof(cfg.soundcloud_client_id));
        read_str("srch_eng",    cfg.search_engine, sizeof(cfg.search_engine));
        read_str("srch_key",    cfg.search_api_key, sizeof(cfg.search_api_key));
        read_str("searxng",     cfg.searxng_url, sizeof(cfg.searxng_url));
        read_str("piped_inst",  cfg.piped_instances, sizeof(cfg.piped_instances));
        read_str("vid_res",     cfg.video_res, sizeof(cfg.video_res));

        size_t prof_len = 0;
        if (nvs_get_str(h, "llm_profs", nullptr, &prof_len) == ESP_OK && prof_len > 2 && prof_len < 3600) {
            std::string prof_json(prof_len, '\0');
            if (nvs_get_str(h, "llm_profs", &prof_json[0], &prof_len) == ESP_OK) {
                deserialize_profiles_json(prof_json.c_str(), cfg, false);
            }
        }

        uint8_t u8v = 0;
        if (nvs_get_u8(h, "tts_gg", &u8v) == ESP_OK)  cfg.tts_prefer_google  = (u8v != 0);
        if (nvs_get_u8(h, "llm_act", &u8v) == ESP_OK) cfg.active_profile_idx = std::min<uint8_t>(u8v, ZClawConfig::kMaxLlmProfiles - 1);
        if (nvs_get_u8(h, "llm_fb", &u8v) == ESP_OK)  cfg.auto_fallback      = (u8v != 0);
        if (nvs_get_u8(h, "vid_fps", &u8v) == ESP_OK && u8v >= 5 && u8v <= 30) cfg.video_fps = u8v;
        if (nvs_get_u8(h, "vid_qlt", &u8v) == ESP_OK && u8v >= 30 && u8v <= 95) cfg.video_jpeg_quality = u8v;

        nvs_close(h);
    }

    if (cfg.llm_profiles[0].base_url[0] == '\0') snprintf(cfg.llm_profiles[0].base_url, sizeof(cfg.llm_profiles[0].base_url), "%s", kDefaultBaseUrl);
    if (cfg.llm_profiles[0].model[0] == '\0')    snprintf(cfg.llm_profiles[0].model, sizeof(cfg.llm_profiles[0].model), "%s", kDefaultModel);
    if (cfg.tts_voice[0] == '\0')                snprintf(cfg.tts_voice, sizeof(cfg.tts_voice), "%s", kDefaultVoice);
    if (cfg.search_engine[0] == '\0')            snprintf(cfg.search_engine, sizeof(cfg.search_engine), "%s", kDefaultSearchEngine);
    if (cfg.piped_instances[0] == '\0')          snprintf(cfg.piped_instances, sizeof(cfg.piped_instances), "%s", kDefaultPipedInstances);
    if (cfg.video_res[0] == '\0')                snprintf(cfg.video_res, sizeof(cfg.video_res), "240x240");
    sync_legacy_mirrors_from_active(cfg);

    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        config_ = cfg;
        xSemaphoreGive(state_mutex_);
    } else {
        config_ = cfg;
    }
}

esp_err_t ZClawLlmClient::save_config(const ZClawConfig& in_cfg) {
    ZClawConfig cfg = in_cfg;
    sync_legacy_mirrors_from_active(cfg);

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(BOARD_NVS_NS_ZCLAW, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không mở được NVS %s: %s", BOARD_NVS_NS_ZCLAW, esp_err_to_name(err));
        return err;
    }

    nvs_set_str(h, "wifi_ssid",  cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass",  cfg.wifi_pass);
    nvs_set_str(h, "stt_key",    cfg.stt_api_key);
    nvs_set_str(h, "llm_key",    cfg.llm_api_key);
    nvs_set_str(h, "llm_url",    cfg.llm_base_url);
    nvs_set_str(h, "llm_model",  cfg.llm_model);
    nvs_set_str(h, "tts_voice",  cfg.tts_voice);
    nvs_set_str(h, "media_proxy",cfg.media_proxy_url);
    nvs_set_str(h, "sc_cid",     cfg.soundcloud_client_id);
    nvs_set_str(h, "srch_eng",   cfg.search_engine);
    nvs_set_str(h, "srch_key",   cfg.search_api_key);
    nvs_set_str(h, "searxng",    cfg.searxng_url);
    nvs_set_str(h, "piped_inst", cfg.piped_instances);
    nvs_set_str(h, "vid_res",    cfg.video_res);

    cJSON* prof_arr = serialize_profiles_json(cfg);
    char* prof_str  = cJSON_PrintUnformatted(prof_arr);
    cJSON_Delete(prof_arr);
    if (prof_str) {
        nvs_set_str(h, "llm_profs", prof_str);
        cJSON_free(prof_str);
    }

    nvs_set_u8(h, "tts_gg",  cfg.tts_prefer_google ? 1 : 0);
    nvs_set_u8(h, "llm_act", cfg.active_profile_idx);
    nvs_set_u8(h, "llm_fb",  cfg.auto_fallback ? 1 : 0);
    nvs_set_u8(h, "vid_fps", cfg.video_fps);
    nvs_set_u8(h, "vid_qlt", cfg.video_jpeg_quality);

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            config_ = cfg;
            xSemaphoreGive(state_mutex_);
        } else {
            config_ = cfg;
        }
        ESP_LOGI(TAG, "Đã lưu cấu hình Multi-Provider NVS (%s)", BOARD_NVS_NS_ZCLAW);
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

namespace {

// Static Flash HTML Dashboard (ported 1:1 from esp-claw theme & LlmPage/SearchFields) — gửi từng chunk để không tốn RAM Heap
constexpr const char kDashboardHtmlPart1[] = R"HTML(<!DOCTYPE html><html lang='vi'><head><meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>S.W.Ni-Oh — ESP-Claw WebUI</title>
<style>
:root{--bg:#0a0b0e;--sf:#111318;--cd:#161920;--ch:#1c2028;--in:#0e1015;--bd:rgba(255,255,255,0.1);--bs:rgba(255,255,255,0.2);--tx:#e8eaf0;--t2:#9aa0b0;--tm:#565b6b;--ac:#e8362d;--ok:#68d391;--wn:#f6ad55;--er:#f87171}
*{box-sizing:border-box}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--tx);min-height:100vh;display:flex;flex-direction:column}
.top{height:48px;background:var(--sf);border-bottom:1px solid var(--bd);display:flex;align-items:center;justify-content:space-between;padding:0 16px}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:14px}.dot{width:8px;height:8px;border-radius:50%;background:var(--ok)}
.shell{display:flex;flex:1;max-width:1100px;width:100%;margin:0 auto}@media(max-width:720px){.shell{flex-direction:column}}
.side{width:220px;background:var(--sf);border-right:1px solid var(--bd);padding:12px 8px;display:flex;flex-direction:column;gap:4px}@media(max-width:720px){.side{width:100%;flex-direction:row;overflow-x:auto;border-right:none;border-bottom:1px solid var(--bd)}}
.nav{padding:9px 12px;border-radius:6px;border:none;background:transparent;color:var(--t2);text-align:left;cursor:pointer;font-size:13px;font-weight:500;white-space:nowrap}
.nav:hover{background:var(--ch);color:var(--tx)}.nav.act{background:rgba(232,54,45,.12);color:#ff6b63;border-left:3px solid var(--ac)}
.main{flex:1;padding:20px;min-width:0}
.blk{background:var(--cd);border:1px solid var(--bd);border-radius:10px;padding:16px;margin-bottom:14px}
.blkh{font-size:14px;font-weight:600;margin:0 0 12px;display:flex;justify-content:space-between;align-items:center}
.pn{display:none}.pn.act{display:block}
.g{display:grid;grid-template-columns:1fr 1fr;gap:12px}@media(max-width:600px){.g{grid-template-columns:1fr}}
.f{grid-column:1/-1}label{display:block;font-size:12px;font-weight:500;color:var(--t2);margin-bottom:5px}
input,select,textarea{width:100%;padding:8px 10px;border-radius:6px;border:1px solid var(--bd);background:var(--in);color:var(--tx);font-size:13px;outline:none}
input:focus,select:focus{border-color:var(--ac)}
.btn{padding:9px 16px;border:none;border-radius:6px;background:var(--ac);color:#fff;font-weight:600;cursor:pointer;font-size:13px}
.btn2{padding:8px 12px;border:1px solid var(--bd);border-radius:6px;background:var(--ch);color:var(--tx);cursor:pointer;font-size:12px;font-weight:500}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.st{font-size:12px;padding:10px;border-radius:6px;background:var(--in);border:1px solid var(--bd);margin-top:10px;white-space:pre-wrap;font-family:monospace}
.pill{display:inline-flex;align-items:center;gap:6px;padding:3px 9px;border-radius:99px;background:var(--cd);border:1px solid var(--bd);color:var(--t2);font-size:11px;margin-left:6px}
.pgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:6px;margin-bottom:10px}
.pbtn{text-align:left;padding:6px 8px;border-radius:6px;border:1px solid var(--bd);background:var(--sf);color:var(--t2);cursor:pointer;font-size:11px}
.pbtn.act{border-color:var(--ac);background:rgba(232,54,45,.15);color:var(--tx)}
details.adv{margin-top:10px;border:1px solid var(--bd);border-radius:8px;padding:10px;background:var(--sf)}
details.adv summary{cursor:pointer;font-size:12px;font-weight:600;color:var(--t2)}
</style></head><body>
<div class='top'>
<div class='brand'><span class='dot'></span><span>S.W.Ni-Oh &middot; ESP-Claw WebUI</span></div>
<div id='hwBar'><span class='pill'>Đang tải...</span></div>
</div>
<div class='shell'>
<aside class='side'>
<button class='nav act' onclick='sw(0)'>LLM Multi-Provider</button>
<button class='nav' onclick='sw(1)'>Media &amp; Tools (S3Mini I2S1)</button>
<button class='nav' onclick='sw(2)'>BLE Chronos (Đồng hồ)</button>
<button class='nav' onclick='sw(3)'>Basic &amp; Hardware</button>
</aside>
<main class='main'>
<div id='p0' class='pn act'>
<div class='blk'><div class='blkh'><span>LLM Configuration (ESP-Claw Standard + Fallback 429)</span>
<label style='display:flex;align-items:center;gap:6px;margin:0;font-size:12px'><input type='checkbox' id='autoFb' style='width:auto'> Auto-Fallback 429</label></div>
<div class='row' style='margin-bottom:10px;justify-content:space-between'>
<select id='pIdx' style='width:220px' onchange='selProf(this.value)'></select>
<span style='font-size:11px;color:var(--t2)'>#1 = Primary, #2..#4 = Fallback 429</span></div>
<div style='font-size:11px;color:var(--t2);margin-bottom:6px'>Fill Default Config (chọn nhanh hoặc dán thẳng API Key bên dưới):</div>
<div id='pGrid' class='row' style='margin-bottom:12px'></div>
<div class='g'>
<div><label>API Key (tự nhận diện Provider &amp; Model khi dán)</label><input id='pKey' type='password' placeholder='Dán API Key (gsk_..., AIza..., sk-or-..., sk-...)' oninput='autoFromKey(this.value)'></div>
<div><label>Model (tự động chọn từ Key hoặc nhập/chọn)</label><input id='pMdl' list='mList' placeholder='Tự động điền từ Key hoặc Preset'><datalist id='mList'></datalist></div>
</div>
<div class='row' style='margin-top:10px'>
<button class='btn2' type='button' onclick='fetchModels(true)'>Tự chọn Model từ Key (/models)</button>
<button class='btn2' type='button' onclick='testLlm()'>Test Kết Nối (Ping LLM)</button></div>
<details id='advBox' class='adv'><summary>LLM Advanced Options (Base URL, Auth Type, Custom Header)</summary>
<div class='g' style='margin-top:10px'>
<div><label>Profile Name</label><input id='pName' placeholder='Primary / Fallback'></div>
<div><label>Connection Provider</label><select id='pProv' onchange='applyPreset(this.value)'><option value='openai'>OpenAI</option><option value='bailian'>Bailian</option><option value='deepseek'>DeepSeek</option><option value='anthropic'>Anthropic</option><option value='kimi_global'>Kimi Global</option><option value='kimi_cn'>Kimi CN</option><option value='minimax_global'>MiniMax Global</option><option value='groq'>Groq</option><option value='openrouter'>OpenRouter</option><option value='gemini'>Google Gemini</option><option value='gemini_native'>Gemini Native</option><option value='ollama'>Ollama/vLLM</option><option value='custom'>Custom</option></select></div>
<div class='f'><label>Base URL</label><div class='row'><input id='pUrl' style='flex:1'><label style='display:flex;align-items:center;gap:5px;white-space:nowrap;margin:0'><input type='checkbox' id='pRaw' style='width:auto'> Raw Full URL</label></div></div>
<div><label>Auth Type</label><select id='pAuth'><option value='bearer'>bearer</option><option value='x-api-key'>x-api-key</option><option value='x-goog-api-key'>x-goog-api-key</option><option value='custom_header'>custom_header</option><option value='query_param'>query_param</option></select></div>
<div><label>Custom Header / Param</label><input id='pCAuth' placeholder='X-Proxy-Key hoặc api_key'></div>
<div class='f row'><label style='display:flex;align-items:center;gap:6px;margin:0'><input type='checkbox' id='pEn' style='width:auto'> Cho phép dùng trong chuỗi Fallback 429</label></div>
</div></details>
<div id='llmTestOut' class='st' style='display:none'></div></div></div>)HTML";

constexpr const char kDashboardHtmlPart2[] = R"HTML(<div id='p1' class='pn'>
<div class='blk'><div class='blkh'><span>Web Search Engine (tra_cuu_thong_tin)</span></div><div class='g'>
<div><label>Search Provider</label><select id='sEng'>
<option value='ddg_wiki'>DuckDuckGo + Wikipedia Tiếng Việt (Miễn phí)</option>
<option value='tavily'>Tavily AI Search</option>
<option value='brave'>Brave Search API</option>
<option value='serper'>Serper.dev Google Search</option>
<option value='bocha'>Bocha Search (ESP-Claw)</option>
<option value='searxng'>SearXNG Custom Instance</option>
</select></div>
<div><label>Search API Key</label><input id='sKey' type='password' placeholder='Để trống nếu dùng DuckDuckGo'></div>
<div class='f'><label>SearXNG Custom Instance URL</label><input id='sSearx' placeholder='https://searx.example.com'></div>
</div></div>
<div class='blk'><div class='blkh'><span>Nhạc YouTube/SoundCloud &amp; Video MJPEG 240x240 (Chạy trực tiếp trên S3Mini I2S1 GPIO44 + minimp3)</span></div><div class='g'>
<div class='f'><label>Piped / Invidious Instances (mặc định phát YouTube/SoundCloud trực tiếp, Radio chỉ mở khi yêu cầu)</label>
<input id='mPiped' placeholder='Piped API instances (cách nhau dấu phẩy)'></div>
<div><label>YouTube / SoundCloud Media Stream Proxy URL</label><input id='mProxy' placeholder='http://192.168.1.50:8080'></div>
<div><label>SoundCloud Client ID (Trống = tự dò)</label><input id='mSc' placeholder='Tự động lấy nếu để trống'></div>
<div><label>ST7789 Video Resolution</label><select id='vRes'><option value='240x240'>240x240 Square Crop</option><option value='240x180'>240x180 Letterbox</option></select></div>
<div><label>Video FPS &amp; JPEG Quality</label><div class='row'><select id='vFps'><option value='12'>12 FPS</option><option value='15'>15 FPS</option><option value='20'>20 FPS</option></select><input id='vQlt' type='number' min='40' max='90' value='75'></div></div>
</div></div></div>
<div id='p2' class='pn'><div class='blk'>
<div class='blkh'><span>BLE Chronos Đồng bộ Đồng hồ, Thông báo &amp; AVRCP</span><button class='btn2' type='button' onclick='loadChronos()'>Làm mới</button></div>
<p style='margin:0 0 10px;color:var(--t2);font-size:12px'>Tách bạch hoàn toàn khỏi Media S3Mini: BLE Chronos chỉ nhận Giờ <b>0x93</b>, Thời tiết <b>0x88</b>, Thông báo <b>0x72</b>, Google Maps <b>0xEF</b>, Pin <b>0x91</b> và điều khiển phụ AVRCP.</p>
<div id='chrBox' class='st'>Đang tải dữ liệu BLE...</div>
</div></div>
<div id='p3' class='pn'><div class='blk'><div class='blkh'><span>Basic System, Wi-Fi, TTS &amp; Hardware</span></div><div class='g'>
<div><label>Wi-Fi SSID</label><input id='wSsid'></div>
<div><label>Wi-Fi Password</label><input id='wPass' type='password' placeholder='Để trống nếu giữ nguyên'></div>
<div class='f'><label>STT API Key (Groq Whisper / OpenAI, trống = dùng Profile Key)</label><input id='sttKey' type='password' placeholder='gsk_...'></div>
<div><label>Giọng đọc TTS Tiếng Việt</label><select id='tVoice'><option value='vi-VN-HoaiMyNeural'>Hoài My (Nữ)</option><option value='vi-VN-NamMinhNeural'>Nam Minh (Nam)</option></select></div>
<div><label>Chế độ TTS</label><select id='tGg'><option value='false'>Edge TTS + Google dự phòng</option><option value='true'>Google TTS trực tiếp</option></select></div>
<div><label>Âm lượng I2S1 MAX98357A (%)</label><input id='hwVol' type='number' min='0' max='100'></div>
<div><label>Độ nhạy Mic (1-10)</label><input id='hwMic' type='number' min='1' max='10'></div>
<div><label>Độ sáng LCD (%)</label><input id='hwBlk' type='number' min='5' max='100'></div>
<div><label>Xoay ST7789</label><select id='hwRot'><option value='0'>0°</option><option value='90'>90°</option><option value='180'>180°</option><option value='270'>270°</option></select></div>
</div></div></div>
<div class='row' style='margin-top:12px;justify-content:space-between'>
<label style='display:flex;align-items:center;gap:6px;margin:0'><input type='checkbox' id='doReboot' style='width:auto'> Reboot ESP32-S3 after save</label>
<button class='btn' type='button' onclick='saveAll()'>Save Configuration</button>
</div><div id='saveOut' class='st' style='display:none'></div>
</main></div>
<script>
const PR={
openrouter:{n:'OpenRouter',u:'https://openrouter.ai/api/v1',m:'openai/gpt-4o-mini',a:'bearer'},
groq:{n:'Groq',u:'https://api.groq.com/openai/v1',m:'llama-3.3-70b-versatile',a:'bearer'},
gemini:{n:'Gemini (OpenAI)',u:'https://generativelanguage.googleapis.com/v1beta/openai',m:'gemini-2.5-flash',a:'bearer'},
gemini_native:{n:'Gemini (Native)',u:'https://generativelanguage.googleapis.com/v1beta',m:'gemini-2.5-flash',a:'x-goog-api-key'},
anthropic:{n:'Claude',u:'https://api.anthropic.com/v1',m:'claude-sonnet-4-20250514',a:'x-api-key'},
deepseek:{n:'DeepSeek',u:'https://api.deepseek.com',m:'deepseek-chat',a:'bearer'},
together:{n:'TogetherAI',u:'https://api.together.xyz/v1',m:'meta-llama/Llama-3.3-70B-Instruct-Turbo',a:'bearer'},
siliconflow:{n:'SiliconFlow',u:'https://api.siliconflow.cn/v1',m:'Qwen/Qwen2.5-72B-Instruct',a:'bearer'},
mistral:{n:'Mistral',u:'https://api.mistral.ai/v1',m:'mistral-small-latest',a:'bearer'},
perplexity:{n:'Perplexity',u:'https://api.perplexity.ai',m:'sonar',a:'bearer'},
ollama:{n:'Ollama/vLLM',u:'http://192.168.1.10:11434/v1',m:'qwen2.5:7b',a:'bearer'},
openai:{n:'OpenAI',u:'https://api.openai.com/v1',m:'gpt-4o-mini',a:'bearer'},
bailian:{n:'Bailian',u:'https://dashscope.aliyuncs.com/compatible-mode/v1',m:'qwen3.5-plus',a:'bearer'},
kimi_global:{n:'Kimi',u:'https://api.moonshot.ai/v1',m:'kimi-k2.5',a:'bearer'},
minimax_global:{n:'MiniMax',u:'https://api.minimax.io/anthropic/v1',m:'MiniMax-M2.7',a:'x-api-key'},
custom:{n:'Custom',u:'',m:'',a:'bearer'}};
let C=null,cur=0;
function sw(i){document.querySelectorAll('.nav').forEach((b,k)=>b.classList.toggle('act',k===i));document.querySelectorAll('.pn').forEach((p,k)=>p.classList.toggle('act',k===i));if(i===2)loadChronos();}
function drawGrid(){const g=document.getElementById('pGrid'),cp=C.llm_profiles[cur].provider;g.innerHTML=['openai','bailian','deepseek','anthropic','kimi_global','kimi_cn','minimax_global','groq','openrouter','gemini','ollama','custom'].map(k=>`<button type='button' class='${cp===k?'btn':'btn2'}' style='padding:5px 9px;font-size:11px' onclick="pickProv('${k}')">${PR[k].n}</button>`).join('');}
function pickProv(k){readUIToProf();document.getElementById('pProv').value=k;applyPreset(k);readUIToProf();drawGrid();}
let kt=null;
function autoFromKey(v){const k=(v||'').trim();if(k.length<16)return;if(k.startsWith('gsk_'))applyPreset('groq',true);else if(k.startsWith('AIza'))applyPreset('gemini',true);else if(k.startsWith('sk-or-'))applyPreset('openrouter',true);else if(k.startsWith('sk-ant-'))applyPreset('anthropic',true);clearTimeout(kt);kt=setTimeout(()=>fetchModels(true),500);}
function syncProfToUI(){const p=C.llm_profiles[cur];document.getElementById('pName').value=p.name||'';document.getElementById('pProv').value=PR[p.provider]?p.provider:'custom';document.getElementById('pUrl').value=p.base_url||'';document.getElementById('pMdl').value=p.model||'';document.getElementById('pKey').value='';document.getElementById('pKey').placeholder=p.has_key?'Đã lưu API Key (dán key mới để đổi)':'Dán API Key';document.getElementById('pAuth').value=p.auth_scheme||'bearer';document.getElementById('pCAuth').value=p.custom_auth_name||'';document.getElementById('pRaw').checked=!!p.raw_endpoint;document.getElementById('pEn').checked=!!p.enabled;}
function readUIToProf(){const p=C.llm_profiles[cur];p.name=document.getElementById('pName').value;p.provider=document.getElementById('pProv').value;p.base_url=document.getElementById('pUrl').value;p.model=document.getElementById('pMdl').value;const k=document.getElementById('pKey').value.trim();if(k)p.api_key=k;p.auth_scheme=document.getElementById('pAuth').value;p.custom_auth_name=document.getElementById('pCAuth').value;p.raw_endpoint=document.getElementById('pRaw').checked;p.enabled=document.getElementById('pEn').checked;}
function selProf(v){readUIToProf();cur=parseInt(v);syncProfToUI();drawGrid();}
function applyPreset(v,silent){const x=PR[v];if(!x)return;document.getElementById('pProv').value=v;if(x.u)document.getElementById('pUrl').value=x.u;if(x.m)document.getElementById('pMdl').value=x.m;document.getElementById('pAuth').value=x.a;document.getElementById('pRaw').checked=false;if(!silent){const ab=document.getElementById('advBox');if(ab)ab.open=(v==='custom');}drawGrid();}
async function init(){const s=await(await fetch('/api/status')).json();document.getElementById('hwBar').innerHTML=`<span class='pill'>MAC: ${s.mac_espnow}</span><span class='pill'>PSRAM: ${s.psram?'Yes':'No'}</span><span class='pill'>Heap: ${Math.round(s.free_heap/1024)} KB</span><span class='pill'>IP: ${s.ip||'192.168.4.1'}</span>`;
C=await(await fetch('/api/config')).json();cur=C.active_profile_idx||0;document.getElementById('autoFb').checked=!!C.auto_fallback;
const sel=document.getElementById('pIdx');sel.innerHTML=C.llm_profiles.map((p,i)=>`<option value='${i}'>#${i+1}: ${p.name||p.provider}${i===C.active_profile_idx?' (Primary)':' (Fallback)'}</option>`).join('');sel.value=cur;syncProfToUI();drawGrid();
document.getElementById('sEng').value=C.search_engine||'ddg_wiki';document.getElementById('sSearx').value=C.searxng_url||'';document.getElementById('mPiped').value=C.piped_instances||'';document.getElementById('mProxy').value=C.media_proxy_url||'';document.getElementById('mSc').value=C.soundcloud_client_id||'';
document.getElementById('vRes').value=C.video_res||'240x240';document.getElementById('vFps').value=String(C.video_fps||12);document.getElementById('vQlt').value=C.video_jpeg_quality||75;
document.getElementById('wSsid').value=C.wifi_ssid||'';document.getElementById('tVoice').value=C.tts_voice||'vi-VN-HoaiMyNeural';document.getElementById('tGg').value=String(!!C.tts_prefer_google);
document.getElementById('hwVol').value=s.volume;document.getElementById('hwMic').value=s.mic_level;document.getElementById('hwRot').value=String(s.rotation);document.getElementById('hwBlk').value=s.backlight||85;}
async function fetchModels(autoPick){readUIToProf();const o=document.getElementById('llmTestOut');o.style.display='block';o.textContent='Đang tự lấy danh sách model từ /models...';const r=await(await fetch('/api/llm_models',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({profile_idx:cur,profile:C.llm_profiles[cur]})})).json();if(r.ok&&r.models&&r.models.length){const d=document.getElementById('mList');d.innerHTML=r.models.map(m=>`<option value='${m}'></option>`).join('');const chatM=r.models.find(m=>!/embed|whisper|tts|dall-e|imagen|moderation/i.test(m))||r.models[0];if(autoPick&&chatM)document.getElementById('pMdl').value=chatM;o.textContent=`Tìm thấy ${r.models.length} model — đã chọn: ${document.getElementById('pMdl').value}`;}else{o.textContent='Lỗi /models: '+(r.error||'Không lấy được');}}
async function testLlm(){readUIToProf();const o=document.getElementById('llmTestOut');o.style.display='block';o.textContent='Đang Ping LLM...';const r=await(await fetch('/api/llm_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({profile_idx:cur,profile:C.llm_profiles[cur]})})).json();o.textContent=r.ok?`Thành công (${r.latency_ms} ms): ${r.reply}`:`Thất bại (${r.latency_ms||0} ms): ${r.error}`;}
async function loadChronos(){const c=await(await fetch('/api/chronos_status')).json();document.getElementById('chrBox').textContent=`Trạng thái BLE: ${c.connected?'Đã kết nối App Chronos':'Đang quảng bá ("S.W.Ni-Oh")'}\nPin điện thoại (0x91): ${c.phone_battery_pct}%\nGiờ đồng bộ (0x93): ${c.last_time_sync||'Chưa có'}\nThời tiết (0x88): ${c.weather_valid?`${c.temp_c}°C - ${c.condition}`:'Chưa nhận'}\nThông báo mới nhất (0x72): ${c.notif_title||'Không có'}\nChỉ đường Maps (0xEF): ${c.nav_active?`${c.nav_dir} (${c.nav_dist}m) - ${c.nav_street}`:'Không hoạt động'}\nTổng gói BLE đã nhận: ${c.rx_packets||0}`;}
async function saveAll(){readUIToProf();C.active_profile_idx=cur;C.auto_fallback=document.getElementById('autoFb').checked;C.search_engine=document.getElementById('sEng').value;const sk=document.getElementById('sKey').value.trim();if(sk)C.search_api_key=sk;C.searxng_url=document.getElementById('sSearx').value.trim();C.piped_instances=document.getElementById('mPiped').value.trim();C.media_proxy_url=document.getElementById('mProxy').value.trim();C.soundcloud_client_id=document.getElementById('mSc').value.trim();C.video_res=document.getElementById('vRes').value;C.video_fps=parseInt(document.getElementById('vFps').value)||12;C.video_jpeg_quality=parseInt(document.getElementById('vQlt').value)||75;
C.wifi_ssid=document.getElementById('wSsid').value.trim();const wp=document.getElementById('wPass').value;if(wp)C.wifi_pass=wp;const st=document.getElementById('sttKey').value.trim();if(st)C.stt_api_key=st;C.tts_voice=document.getElementById('tVoice').value;C.tts_prefer_google=(document.getElementById('tGg').value==='true');
C.volume=parseInt(document.getElementById('hwVol').value);C.mic_level=parseInt(document.getElementById('hwMic').value);C.backlight=parseInt(document.getElementById('hwBlk').value);C.rotation=parseInt(document.getElementById('hwRot').value);C.reboot=document.getElementById('doReboot').checked;
const r=await(await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(C)})).json();const o=document.getElementById('saveOut');o.style.display='block';o.textContent=r.ok?'Đã lưu toàn bộ cấu hình thành công!':'Lỗi lưu cấu hình.';}
init();setInterval(()=>{if(document.getElementById('p2').classList.contains('act'))loadChronos();},2500);
</script></body></html>)HTML";

std::string recv_http_body(httpd_req_t* req, size_t max_len = 8192) {
    const int total = req->content_len;
    if (total <= 0 || static_cast<size_t>(total) > max_len) return "";
    std::string body(static_cast<size_t>(total), '\0');
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, &body[received], total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return "";
        }
        received += r;
    }
    return body;
}

void parse_profile_patch(cJSON* o, LlmProfile& p) {
    if (!cJSON_IsObject(o)) return;
    auto copy_s = [&](const char* k, char* dst, size_t max_len) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(o, k);
        if (cJSON_IsString(j) && j->valuestring) {
            if (strcmp(k, "api_key") == 0 && (strcmp(j->valuestring, "__KEEP__") == 0 || j->valuestring[0] == '\0')) {
                return;
            }
            snprintf(dst, max_len, "%s", j->valuestring);
        }
    };
    copy_s("name", p.name, sizeof(p.name));
    copy_s("provider", p.provider, sizeof(p.provider));
    copy_s("base_url", p.base_url, sizeof(p.base_url));
    copy_s("model", p.model, sizeof(p.model));
    copy_s("api_key", p.api_key, sizeof(p.api_key));
    copy_s("auth_scheme", p.auth_scheme, sizeof(p.auth_scheme));
    copy_s("custom_auth_name", p.custom_auth_name, sizeof(p.custom_auth_name));
    cJSON* jr = cJSON_GetObjectItemCaseSensitive(o, "raw_endpoint");
    if (cJSON_IsBool(jr)) p.raw_endpoint = cJSON_IsTrue(jr);
    cJSON* je = cJSON_GetObjectItemCaseSensitive(o, "enabled");
    if (cJSON_IsBool(je)) p.enabled = cJSON_IsTrue(je);
}

} // namespace

esp_err_t ZClawLlmClient::http_status_get_handler(httpd_req_t* req) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char ip_str[24] = "192.168.4.1";
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "brand", "S.W.Ni-Oh");
    cJSON_AddBoolToObject(j, "psram", board_has_psram());
    cJSON_AddNumberToObject(j, "free_heap", static_cast<double>(esp_get_free_heap_size()));
    cJSON_AddStringToObject(j, "mac_espnow", mac_str);
    cJSON_AddStringToObject(j, "ip", ip_str);
    cJSON_AddNumberToObject(j, "volume", AudioHal::instance().get_speaker_volume());
    cJSON_AddNumberToObject(j, "mic_level", AudioHal::instance().get_mic_sensitivity_level());
    cJSON_AddNumberToObject(j, "rotation", UiEyesClock::instance().get_rotation());
    cJSON_AddNumberToObject(j, "backlight", UiEyesClock::instance().get_backlight());

    char* str = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, str ? str : "{}");
    if (str) cJSON_free(str);
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_chronos_status_get_handler(httpd_req_t* req) {
    ZClawLlmClient& self = ZClawLlmClient::instance();
    std::string json = self.chronos_status_cb_
                           ? self.chronos_status_cb_()
                           : "{\"connected\":false,\"phone_battery_pct\":100,\"rx_packets\":0}";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_config_get_handler(httpd_req_t* req) {
    const ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddBoolToObject(j, "has_stt_key", cfg.stt_api_key[0] != '\0');
    cJSON_AddNumberToObject(j, "active_profile_idx", cfg.active_profile_idx);
    cJSON_AddBoolToObject(j, "auto_fallback", cfg.auto_fallback);

    cJSON* profs = cJSON_CreateArray();
    for (size_t i = 0; i < ZClawConfig::kMaxLlmProfiles; ++i) {
        const LlmProfile& p = cfg.llm_profiles[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name);
        cJSON_AddStringToObject(o, "provider", p.provider);
        cJSON_AddStringToObject(o, "base_url", p.base_url);
        cJSON_AddStringToObject(o, "model", p.model);
        cJSON_AddStringToObject(o, "api_key", p.api_key[0] ? "__KEEP__" : "");
        cJSON_AddBoolToObject(o, "has_key", p.api_key[0] != '\0');
        cJSON_AddStringToObject(o, "auth_scheme", p.auth_scheme);
        cJSON_AddStringToObject(o, "custom_auth_name", p.custom_auth_name);
        cJSON_AddBoolToObject(o, "raw_endpoint", p.raw_endpoint);
        cJSON_AddBoolToObject(o, "enabled", p.enabled);
        cJSON_AddItemToArray(profs, o);
    }
    cJSON_AddItemToObject(j, "llm_profiles", profs);

    cJSON_AddStringToObject(j, "tts_voice", cfg.tts_voice);
    cJSON_AddBoolToObject(j, "tts_prefer_google", cfg.tts_prefer_google);
    cJSON_AddStringToObject(j, "search_engine", cfg.search_engine);
    cJSON_AddBoolToObject(j, "has_search_key", cfg.search_api_key[0] != '\0');
    cJSON_AddStringToObject(j, "searxng_url", cfg.searxng_url);
    cJSON_AddStringToObject(j, "piped_instances", cfg.piped_instances);
    cJSON_AddStringToObject(j, "media_proxy_url", cfg.media_proxy_url);
    cJSON_AddStringToObject(j, "soundcloud_client_id", cfg.soundcloud_client_id);
    cJSON_AddStringToObject(j, "video_res", cfg.video_res);
    cJSON_AddNumberToObject(j, "video_fps", cfg.video_fps);
    cJSON_AddNumberToObject(j, "video_jpeg_quality", cfg.video_jpeg_quality);

    char* str = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, str ? str : "{}");
    if (str) cJSON_free(str);
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_config_post_handler(httpd_req_t* req) {
    std::string body = recv_http_body(req, 8192);
    cJSON* root = body.empty() ? nullptr : cJSON_Parse(body.c_str());
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON không hợp lệ");
        return ESP_FAIL;
    }

    ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    auto set_s = [&](const char* k, char* dst, size_t max_len, bool keep_if_empty = false) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(root, k);
        if (cJSON_IsString(j) && j->valuestring) {
            if (keep_if_empty && (j->valuestring[0] == '\0' || strcmp(j->valuestring, "__KEEP__") == 0)) return;
            snprintf(dst, max_len, "%s", j->valuestring);
        }
    };

    set_s("wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    set_s("wifi_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass), true);
    set_s("stt_api_key", cfg.stt_api_key, sizeof(cfg.stt_api_key), true);
    set_s("tts_voice", cfg.tts_voice, sizeof(cfg.tts_voice));
    set_s("search_engine", cfg.search_engine, sizeof(cfg.search_engine));
    set_s("search_api_key", cfg.search_api_key, sizeof(cfg.search_api_key), true);
    set_s("searxng_url", cfg.searxng_url, sizeof(cfg.searxng_url));
    set_s("piped_instances", cfg.piped_instances, sizeof(cfg.piped_instances));
    set_s("media_proxy_url", cfg.media_proxy_url, sizeof(cfg.media_proxy_url));
    set_s("soundcloud_client_id", cfg.soundcloud_client_id, sizeof(cfg.soundcloud_client_id));
    set_s("video_res", cfg.video_res, sizeof(cfg.video_res));

    cJSON* jfb = cJSON_GetObjectItemCaseSensitive(root, "auto_fallback");
    if (cJSON_IsBool(jfb)) cfg.auto_fallback = cJSON_IsTrue(jfb);
    cJSON* jact = cJSON_GetObjectItemCaseSensitive(root, "active_profile_idx");
    if (cJSON_IsNumber(jact)) {
        cfg.active_profile_idx = static_cast<uint8_t>(std::max(0, std::min<int>(jact->valueint, ZClawConfig::kMaxLlmProfiles - 1)));
    }
    cJSON* jtgg = cJSON_GetObjectItemCaseSensitive(root, "tts_prefer_google");
    if (cJSON_IsBool(jtgg)) cfg.tts_prefer_google = cJSON_IsTrue(jtgg);
    cJSON* jfps = cJSON_GetObjectItemCaseSensitive(root, "video_fps");
    if (cJSON_IsNumber(jfps)) cfg.video_fps = static_cast<uint8_t>(std::max(5, std::min(30, jfps->valueint)));
    cJSON* jqlt = cJSON_GetObjectItemCaseSensitive(root, "video_jpeg_quality");
    if (cJSON_IsNumber(jqlt)) cfg.video_jpeg_quality = static_cast<uint8_t>(std::max(30, std::min(95, jqlt->valueint)));

    cJSON* jprofs = cJSON_GetObjectItemCaseSensitive(root, "llm_profiles");
    if (cJSON_IsArray(jprofs)) {
        const int n = std::min<int>(cJSON_GetArraySize(jprofs), static_cast<int>(ZClawConfig::kMaxLlmProfiles));
        for (int i = 0; i < n; ++i) {
            parse_profile_patch(cJSON_GetArrayItem(jprofs, i), cfg.llm_profiles[i]);
        }
    }

    cJSON* jvol = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if (cJSON_IsNumber(jvol)) AudioHal::instance().set_speaker_volume(static_cast<uint8_t>(std::max(0, std::min(100, jvol->valueint))), true);
    cJSON* jmic = cJSON_GetObjectItemCaseSensitive(root, "mic_level");
    if (cJSON_IsNumber(jmic)) AudioHal::instance().set_mic_sensitivity_level(static_cast<uint8_t>(std::max(1, std::min(10, jmic->valueint))), true);
    cJSON* jblk = cJSON_GetObjectItemCaseSensitive(root, "backlight");
    if (cJSON_IsNumber(jblk)) UiEyesClock::instance().set_backlight(static_cast<uint8_t>(std::max(5, std::min(100, jblk->valueint))), true);
    cJSON* jrot = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    if (cJSON_IsNumber(jrot)) UiEyesClock::instance().set_rotation(static_cast<uint16_t>(jrot->valueint), true);

    const bool do_reboot = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reboot"));
    cJSON_Delete(root);

    ZClawLlmClient::instance().save_config(cfg);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    if (do_reboot) {
        esp_timer_handle_t rst_tm = nullptr;
        esp_timer_create_args_t ta = {};
        ta.callback = delayed_restart_cb;
        ta.name     = "nioh_rst";
        if (esp_timer_create(&ta, &rst_tm) == ESP_OK) {
            esp_timer_start_once(rst_tm, 1200000ULL);
        }
    }
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_llm_models_post_handler(httpd_req_t* req) {
    std::string body = recv_http_body(req, 2048);
    cJSON* root = body.empty() ? nullptr : cJSON_Parse(body.c_str());
    ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    int idx = cfg.active_profile_idx;
    if (root) {
        cJSON* jidx = cJSON_GetObjectItemCaseSensitive(root, "profile_idx");
        if (cJSON_IsNumber(jidx)) idx = std::max(0, std::min<int>(jidx->valueint, ZClawConfig::kMaxLlmProfiles - 1));
    }
    LlmProfile prof = cfg.llm_profiles[idx];
    if (root) {
        parse_profile_patch(cJSON_GetObjectItemCaseSensitive(root, "profile"), prof);
        cJSON_Delete(root);
    }

    std::vector<std::string> models;
    std::string err_msg;
    esp_err_t err = ZClawLlmClient::instance().fetch_models_for_profile(prof, models, err_msg);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON* arr = cJSON_CreateArray();
        for (const auto& m : models) cJSON_AddItemToArray(arr, cJSON_CreateString(m.c_str()));
        cJSON_AddItemToObject(out, "models", arr);
    } else {
        cJSON_AddStringToObject(out, "error", err_msg.c_str());
    }
    char* str = cJSON_PrintUnformatted(out);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, str ? str : "{}");
    if (str) cJSON_free(str);
    cJSON_Delete(out);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_llm_test_post_handler(httpd_req_t* req) {
    std::string body = recv_http_body(req, 2048);
    cJSON* root = body.empty() ? nullptr : cJSON_Parse(body.c_str());
    ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    int idx = cfg.active_profile_idx;
    if (root) {
        cJSON* jidx = cJSON_GetObjectItemCaseSensitive(root, "profile_idx");
        if (cJSON_IsNumber(jidx)) idx = std::max(0, std::min<int>(jidx->valueint, ZClawConfig::kMaxLlmProfiles - 1));
    }
    LlmProfile prof = cfg.llm_profiles[idx];
    if (root) {
        parse_profile_patch(cJSON_GetObjectItemCaseSensitive(root, "profile"), prof);
        cJSON_Delete(root);
    }

    uint32_t latency_ms = 0;
    std::string reply, err_msg;
    esp_err_t err = ZClawLlmClient::instance().test_profile_connection(prof, latency_ms, reply, err_msg);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(out, "latency_ms", latency_ms);
    if (err == ESP_OK) cJSON_AddStringToObject(out, "reply", reply.c_str());
    else cJSON_AddStringToObject(out, "error", err_msg.c_str());

    char* str = cJSON_PrintUnformatted(out);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, str ? str : "{}");
    if (str) cJSON_free(str);
    cJSON_Delete(out);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_root_get_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, kDashboardHtmlPart1);
    httpd_resp_sendstr_chunk(req, kDashboardHtmlPart2);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_save_post_handler(httpd_req_t* req) {
    const std::string body = recv_http_body(req, 4096);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Dữ liệu không hợp lệ");
        return ESP_FAIL;
    }

    ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    LlmProfile& act = cfg.llm_profiles[cfg.active_profile_idx < ZClawConfig::kMaxLlmProfiles ? cfg.active_profile_idx : 0];
    bool do_reboot  = false;

    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            std::string key = url_decode_form(body.data() + pos, eq - pos);
            std::string val = url_decode_form(body.data() + eq + 1, amp - eq - 1);

            if (key == "ssid")             snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", val.c_str());
            else if (key == "pass" && !val.empty()) snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", val.c_str());
            else if (key == "stt_key" && !val.empty()) snprintf(cfg.stt_api_key, sizeof(cfg.stt_api_key), "%s", val.c_str());
            else if (key == "llm_key" && !val.empty()) snprintf(act.api_key, sizeof(act.api_key), "%s", val.c_str());
            else if (key == "llm_url" && !val.empty()) snprintf(act.base_url, sizeof(act.base_url), "%s", val.c_str());
            else if (key == "llm_model" && !val.empty()) snprintf(act.model, sizeof(act.model), "%s", val.c_str());
            else if (key == "voice" && !val.empty()) snprintf(cfg.tts_voice, sizeof(cfg.tts_voice), "%s", val.c_str());
            else if (key == "media_proxy") snprintf(cfg.media_proxy_url, sizeof(cfg.media_proxy_url), "%s", val.c_str());
            else if (key == "sc_cid")      snprintf(cfg.soundcloud_client_id, sizeof(cfg.soundcloud_client_id), "%s", val.c_str());
            else if (key == "tts_gg")      cfg.tts_prefer_google = (atoi(val.c_str()) != 0);
            else if (key == "vol")         AudioHal::instance().set_speaker_volume(static_cast<uint8_t>(std::max(0, std::min(100, atoi(val.c_str())))), true);
            else if (key == "mic")         AudioHal::instance().set_mic_sensitivity_level(static_cast<uint8_t>(std::max(1, std::min(10, atoi(val.c_str())))), true);
            else if (key == "blk")         UiEyesClock::instance().set_backlight(static_cast<uint8_t>(std::max(5, std::min(100, atoi(val.c_str())))), true);
            else if (key == "rot")         UiEyesClock::instance().set_rotation(static_cast<uint16_t>(atoi(val.c_str())), true);
            else if (key == "reboot")      do_reboot = (val == "1");
        }
        pos = amp + 1;
    }

    ZClawLlmClient::instance().save_config(cfg);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<h2>Đã lưu cấu hình S.W.Ni-Oh!</h2>");

    if (do_reboot) {
        esp_timer_handle_t rst_tm = nullptr;
        esp_timer_create_args_t ta = {};
        ta.callback = delayed_restart_cb;
        ta.name     = "nioh_rst";
        if (esp_timer_create(&ta, &rst_tm) == ESP_OK) {
            esp_timer_start_once(rst_tm, 1200000ULL);
        }
    }
    return ESP_OK;
}

esp_err_t ZClawLlmClient::start_captive_portal(bool start_softap) {
    // Nếu HTTP Dashboard đang chạy ở chế độ STA-only và giờ user giữ 5s mở SoftAP -> dừng nhẹ rồi khởi chạy lại cùng SoftAP
    if (portal_active_.load()) {
        if (!start_softap || softap_started_.load()) {
            return ESP_OK;
        }
        stop_captive_portal();
    }

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

        dns_running_.store(true);
        xTaskCreatePinnedToCore(dns_portal_task, "dns_portal", 3072,
                                this, 3, &dns_task_handle_, 0);
        softap_started_.store(true);
    }

    httpd_config_t hcfg  = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 16;
    hcfg.stack_size       = 8192;
    hcfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&httpd_handle_, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không khởi chạy được Web Dashboard: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uri_root       = { "/", HTTP_GET, http_root_get_handler, nullptr };
    static const httpd_uri_t uri_save       = { "/save", HTTP_POST, http_save_post_handler, nullptr };
    static const httpd_uri_t uri_cfg_get    = { "/api/config", HTTP_GET, http_config_get_handler, nullptr };
    static const httpd_uri_t uri_cfg_post   = { "/api/config", HTTP_POST, http_config_post_handler, nullptr };
    static const httpd_uri_t uri_llm_models = { "/api/llm_models", HTTP_POST, http_llm_models_post_handler, nullptr };
    static const httpd_uri_t uri_llm_test   = { "/api/llm_test", HTTP_POST, http_llm_test_post_handler, nullptr };
    static const httpd_uri_t uri_chronos    = { "/api/chronos_status", HTTP_GET, http_chronos_status_get_handler, nullptr };
    static const httpd_uri_t uri_status     = { "/api/status", HTTP_GET, http_status_get_handler, nullptr };
    static const httpd_uri_t uri_204        = { "/generate_204", HTTP_GET, http_captive_redirect_handler, nullptr };
    static const httpd_uri_t uri_apple      = { "/hotspot-detect.html", HTTP_GET, http_captive_redirect_handler, nullptr };
    static const httpd_uri_t uri_win        = { "/connecttest.txt", HTTP_GET, http_captive_redirect_handler, nullptr };
    static const httpd_uri_t uri_wild       = { "/*", HTTP_GET, http_captive_redirect_handler, nullptr };

    httpd_register_uri_handler(httpd_handle_, &uri_root);
    httpd_register_uri_handler(httpd_handle_, &uri_save);
    httpd_register_uri_handler(httpd_handle_, &uri_cfg_get);
    httpd_register_uri_handler(httpd_handle_, &uri_cfg_post);
    httpd_register_uri_handler(httpd_handle_, &uri_llm_models);
    httpd_register_uri_handler(httpd_handle_, &uri_llm_test);
    httpd_register_uri_handler(httpd_handle_, &uri_chronos);
    httpd_register_uri_handler(httpd_handle_, &uri_status);
    if (start_softap) {
        httpd_register_uri_handler(httpd_handle_, &uri_204);
        httpd_register_uri_handler(httpd_handle_, &uri_apple);
        httpd_register_uri_handler(httpd_handle_, &uri_win);
        httpd_register_uri_handler(httpd_handle_, &uri_wild);
    }

    portal_active_.store(true);
    ESP_LOGI(TAG, "S.W.Ni-Oh Web Dashboard mở (%s)", start_softap ? "Captive Portal 192.168.4.1" : "LAN / sw-nioh.local");
    return ESP_OK;
}

void ZClawLlmClient::stop_captive_portal() {
    portal_active_.store(false);
    softap_started_.store(false);
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

esp_err_t ZClawLlmClient::post_json_with_profile(const LlmProfile& prof,
                                                 const char* json_body,
                                                 std::string& out_response,
                                                 int* out_http_status) {
    out_response.clear();
    if (out_http_status) *out_http_status = 0;

    std::string url = resolve_chat_url(prof);
    if (url.empty() || !json_body) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t hcfg = {};
    hcfg.url               = url.c_str();
    hcfg.method            = HTTP_METHOD_POST;
    hcfg.timeout_ms        = 25000;
    hcfg.buffer_size       = 4096;
    hcfg.buffer_size_tx    = 4096;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) return ESP_ERR_NO_MEM;

    apply_profile_auth_headers(client, prof);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");

    const size_t body_len = strlen(json_body);
    esp_err_t err = esp_http_client_open(client, static_cast<int>(body_len));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM open lỗi (%s): %s", prof.name, esp_err_to_name(err));
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
    if (out_http_status) *out_http_status = status;

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
        ESP_LOGE(TAG, "LLM [%s] HTTP %d: %.*s", prof.name, status,
                 static_cast<int>(std::min<size_t>(200, out_response.size())),
                 out_response.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ZClawLlmClient::http_get_with_profile(const LlmProfile& prof,
                                               const char* url,
                                               std::string& out_response,
                                               int* out_http_status) {
    out_response.clear();
    if (out_http_status) *out_http_status = 0;
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t hcfg = {};
    hcfg.url               = url;
    hcfg.method            = HTTP_METHOD_GET;
    hcfg.timeout_ms        = 15000;
    hcfg.buffer_size       = 4096;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) return ESP_ERR_NO_MEM;
    apply_profile_auth_headers(client, prof);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    const int status = esp_http_client_get_status_code(client);
    if (out_http_status) *out_http_status = status;

    char* tmp = static_cast<char*>(board_malloc_prefer_spiram(2048));
    if (tmp) {
        int r = 0;
        while ((r = esp_http_client_read(client, tmp, 2048)) > 0) {
            out_response.append(tmp, static_cast<size_t>(r));
            if (out_response.size() >= 65536) break;
        }
        board_free(tmp);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

esp_err_t ZClawLlmClient::fetch_models_for_profile(const LlmProfile& prof,
                                                   std::vector<std::string>& out_models,
                                                   std::string& out_err) {
    out_models.clear();
    out_err.clear();
    std::string url = resolve_models_url(prof);
    if (url.empty()) {
        out_err = "Thiếu Base URL";
        return ESP_ERR_INVALID_ARG;
    }

    std::string resp;
    int status = 0;
    esp_err_t err = http_get_with_profile(prof, url.c_str(), resp, &status);
    if (err != ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "HTTP %d khi tải /models", status);
        out_err = buf;
        return err;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) {
        out_err = "JSON /models không hợp lệ";
        return ESP_FAIL;
    }

    // OpenAI / Groq / OpenRouter: { data: [ { id: "..." } ] } hoặc Gemini: { models: [ { name: "models/..." } ] }
    cJSON* data_arr = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data_arr)) data_arr = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!cJSON_IsArray(data_arr) && cJSON_IsArray(root)) data_arr = root;

    if (cJSON_IsArray(data_arr)) {
        const int n = std::min<int>(cJSON_GetArraySize(data_arr), 40);
        for (int i = 0; i < n; ++i) {
            cJSON* item = cJSON_GetArrayItem(data_arr, i);
            cJSON* jid  = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "id") : nullptr;
            if (!jid && cJSON_IsObject(item)) jid = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (cJSON_IsString(jid) && jid->valuestring) {
                std::string mid = jid->valuestring;
                if (mid.rfind("models/", 0) == 0) mid = mid.substr(7);
                out_models.push_back(mid);
            }
        }
    }
    cJSON_Delete(root);
    if (out_models.empty()) {
        out_err = "API không trả về danh sách model";
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t ZClawLlmClient::test_profile_connection(const LlmProfile& prof,
                                                  uint32_t& out_latency_ms,
                                                  std::string& out_reply,
                                                  std::string& out_err) {
    out_latency_ms = 0;
    out_reply.clear();
    out_err.clear();

    cJSON* msgs = cJSON_CreateArray();
    cJSON* usr  = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", "Trả lời đúng 1 câu ngắn tiếng Việt để xác nhận kết nối S.W.Ni-Oh.");
    cJSON_AddItemToArray(msgs, usr);

    const int64_t t0 = esp_timer_get_time();
    int http_st = 0;
    esp_err_t err = execute_single_profile_turn(prof, msgs, nullptr, false, out_reply, &http_st);
    out_latency_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000ULL);
    cJSON_Delete(msgs);

    if (err != ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Lỗi kết nối LLM (HTTP %d)", http_st);
        out_err = buf;
    }
    return err;
}

esp_err_t ZClawLlmClient::execute_single_profile_turn(const LlmProfile& prof,
                                                      cJSON* base_messages,
                                                      cJSON* tools_arr,
                                                      bool deep_mode,
                                                      std::string& out_raw_reply,
                                                      int* out_http_status) {
    out_raw_reply.clear();
    if (out_http_status) *out_http_status = 0;
    if (prof.base_url[0] == '\0' || (prof.api_key[0] == '\0' && strcmp(prof.provider, "ollama") != 0)) {
        return ESP_ERR_INVALID_STATE;
    }

    // Nhánh đặc biệt cho Anthropic Native (/v1/messages) hoặc Gemini Native (:generateContent)
    if (strcmp(prof.provider, "anthropic") == 0) {
        cJSON* msgs = cJSON_CreateArray();
        const int n = cJSON_GetArraySize(base_messages);
        std::string sys_text = kSystemPromptVi;
        for (int i = 0; i < n; ++i) {
            cJSON* m = cJSON_GetArrayItem(base_messages, i);
            cJSON* r = cJSON_GetObjectItemCaseSensitive(m, "role");
            cJSON* c = cJSON_GetObjectItemCaseSensitive(m, "content");
            if (!cJSON_IsString(r) || !cJSON_IsString(c)) continue;
            if (strcmp(r->valuestring, "system") == 0) sys_text = c->valuestring;
            else {
                cJSON* am = cJSON_CreateObject();
                cJSON_AddStringToObject(am, "role", strcmp(r->valuestring, "assistant") == 0 ? "assistant" : "user");
                cJSON_AddStringToObject(am, "content", c->valuestring);
                cJSON_AddItemToArray(msgs, am);
            }
        }
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "model", prof.model);
        cJSON_AddStringToObject(payload, "system", sys_text.c_str());
        cJSON_AddItemToObject(payload, "messages", msgs);
        cJSON_AddNumberToObject(payload, "max_tokens", deep_mode ? 1024 : 512);
        char* body = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!body) return ESP_ERR_NO_MEM;

        std::string resp;
        esp_err_t err = post_json_with_profile(prof, body, resp, out_http_status);
        cJSON_free(body);
        if (err != ESP_OK) return err;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) return ESP_FAIL;
        cJSON* content_arr = cJSON_GetObjectItemCaseSensitive(root, "content");
        cJSON* c0 = cJSON_IsArray(content_arr) ? cJSON_GetArrayItem(content_arr, 0) : nullptr;
        cJSON* jtxt = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "text") : nullptr;
        if (cJSON_IsString(jtxt) && jtxt->valuestring) {
            out_raw_reply = jtxt->valuestring;
            err = ESP_OK;
        } else {
            err = ESP_FAIL;
        }
        cJSON_Delete(root);
        return err;
    }

    if (strcmp(prof.provider, "gemini_native") == 0) {
        std::string merged;
        const int n = cJSON_GetArraySize(base_messages);
        for (int i = 0; i < n; ++i) {
            cJSON* m = cJSON_GetArrayItem(base_messages, i);
            cJSON* r = cJSON_GetObjectItemCaseSensitive(m, "role");
            cJSON* c = cJSON_GetObjectItemCaseSensitive(m, "content");
            if (cJSON_IsString(r) && cJSON_IsString(c)) {
                merged += r->valuestring; merged += ": "; merged += c->valuestring; merged += "\n";
            }
        }
        cJSON* part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", merged.c_str());
        cJSON* parts = cJSON_CreateArray(); cJSON_AddItemToArray(parts, part);
        cJSON* item = cJSON_CreateObject(); cJSON_AddItemToObject(item, "parts", parts);
        cJSON* contents = cJSON_CreateArray(); cJSON_AddItemToArray(contents, item);
        cJSON* payload = cJSON_CreateObject(); cJSON_AddItemToObject(payload, "contents", contents);
        char* body = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!body) return ESP_ERR_NO_MEM;

        std::string resp;
        esp_err_t err = post_json_with_profile(prof, body, resp, out_http_status);
        cJSON_free(body);
        if (err != ESP_OK) return err;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) return ESP_FAIL;
        cJSON* cands = cJSON_GetObjectItemCaseSensitive(root, "candidates");
        cJSON* c0    = cJSON_IsArray(cands) ? cJSON_GetArrayItem(cands, 0) : nullptr;
        cJSON* cont  = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "content") : nullptr;
        cJSON* p_arr = cont ? cJSON_GetObjectItemCaseSensitive(cont, "parts") : nullptr;
        cJSON* p0    = cJSON_IsArray(p_arr) ? cJSON_GetArrayItem(p_arr, 0) : nullptr;
        cJSON* jtxt  = p0 ? cJSON_GetObjectItemCaseSensitive(p0, "text") : nullptr;
        if (cJSON_IsString(jtxt) && jtxt->valuestring) {
            out_raw_reply = jtxt->valuestring;
            err = ESP_OK;
        } else {
            err = ESP_FAIL;
        }
        cJSON_Delete(root);
        return err;
    }

    // Standard OpenAI-compatible loop (+ MCP Tool Calling)
    cJSON* messages  = cJSON_Duplicate(base_messages, true);
    const bool has_tools = (tools_arr != nullptr && cJSON_GetArraySize(tools_arr) > 0);
    esp_err_t final_err  = ESP_FAIL;

    constexpr int kMaxToolIters = 4;
    for (int iter = 0; iter < kMaxToolIters; ++iter) {
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "model", prof.model);
        cJSON_AddItemReferenceToObject(payload, "messages", messages);
        if (has_tools) {
            cJSON_AddItemReferenceToObject(payload, "tools", tools_arr);
            cJSON_AddStringToObject(payload, "tool_choice", "auto");
        }
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
        final_err = post_json_with_profile(prof, body_str, resp_json, out_http_status);
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

                std::string tool_res = mcp_exec_cb_
                                           ? mcp_exec_cb_(tool_name, args_str)
                                           : std::string("{\"ok\":false,\"loi\":\"MCP chưa được đăng ký\"}");

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
            out_raw_reply = jcontent->valuestring;
            final_err     = ESP_OK;
        } else {
            final_err = ESP_FAIL;
        }
        cJSON_Delete(root);
        break;
    }

    cJSON_Delete(messages);
    return final_err;
}

// ---------------------------------------------------------------------------
// chat_with_tools() — Direct Multi-Provider LLM Client + Fallback 429 + MCP Loop
// ---------------------------------------------------------------------------
esp_err_t ZClawLlmClient::chat_with_tools(const std::string& user_text,
                                           std::string& out_reply_clean,
                                           EyeEmotion& out_emotion) {
    out_reply_clean.clear();
    out_emotion = EyeEmotion::BINH_THUONG;

    const ZClawConfig cfg = get_config();
    const uint8_t primary = (cfg.active_profile_idx < ZClawConfig::kMaxLlmProfiles) ? cfg.active_profile_idx : 0;

    std::vector<size_t> order;
    order.push_back(primary);
    if (cfg.auto_fallback) {
        for (size_t i = 0; i < ZClawConfig::kMaxLlmProfiles; ++i) {
            if (i != primary && cfg.llm_profiles[i].enabled && cfg.llm_profiles[i].base_url[0] != '\0') {
                order.push_back(i);
            }
        }
    }

    bool any_key = false;
    for (size_t idx : order) {
        if (cfg.llm_profiles[idx].api_key[0] != '\0' || strcmp(cfg.llm_profiles[idx].provider, "ollama") == 0) {
            any_key = true;
            break;
        }
    }
    if (!any_key) {
        out_reply_clean = "Bạn chưa cài đặt API Key trí tuệ nhân tạo. Hãy mở trang Dashboard S.W.Ni-Oh để thêm Profile LLM.";
        return ESP_ERR_INVALID_STATE;
    }

    const bool deep_mode = next_turn_deep_reasoning_.exchange(false);

    cJSON* base_messages = cJSON_CreateArray();
    {
        cJSON* sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", kSystemPromptVi);
        cJSON_AddItemToArray(base_messages, sys);
    }
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& h : history_) {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", h.role.c_str());
            cJSON_AddStringToObject(m, "content", h.content.c_str());
            cJSON_AddItemToArray(base_messages, m);
        }
        xSemaphoreGive(state_mutex_);
    }
    {
        cJSON* u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", user_text.c_str());
        cJSON_AddItemToArray(base_messages, u);
    }

    cJSON* tools_arr = mcp_schema_cb_ ? mcp_schema_cb_() : nullptr;
    std::string final_raw;
    esp_err_t final_err = ESP_FAIL;

    for (size_t attempt_idx = 0; attempt_idx < order.size(); ++attempt_idx) {
        const size_t pidx = order[attempt_idx];
        const LlmProfile& prof = cfg.llm_profiles[pidx];
        int http_status = 0;
        final_err = execute_single_profile_turn(prof, base_messages, tools_arr, deep_mode, final_raw, &http_status);
        if (final_err == ESP_OK && !final_raw.empty()) {
            break;
        }
        if (attempt_idx + 1 < order.size()) {
            ESP_LOGW(TAG, "Profile #%u (%s) thất bại (HTTP %d) -> Chuyển Fallback Profile #%u!",
                     static_cast<unsigned>(pidx + 1), prof.name, http_status,
                     static_cast<unsigned>(order[attempt_idx + 1] + 1));
        }
    }

    if (tools_arr) cJSON_Delete(tools_arr);
    cJSON_Delete(base_messages);

    if (final_err != ESP_OK || final_raw.empty()) {
        out_emotion     = EyeEmotion::BUON;
        out_reply_clean = "Xin lỗi, kết nối tới máy chủ AI đang gặp sự cố. Bạn kiểm tra API Key trên Dashboard giúp mình nhé.";
        return final_err != ESP_OK ? final_err : ESP_FAIL;
    }

    out_emotion = extract_emotion_tag(final_raw, out_reply_clean);

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
    const uint8_t primary = (cfg.active_profile_idx < ZClawConfig::kMaxLlmProfiles) ? cfg.active_profile_idx : 0;
    const LlmProfile& prof = cfg.llm_profiles[primary];

    cJSON* msgs = cJSON_CreateArray();
    cJSON* sys  = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
                            "Hãy phân tích từng bước một cách logic, chính xác bằng tiếng Việt, rồi kết luận ngắn gọn rõ ràng.");
    cJSON_AddItemToArray(msgs, sys);
    cJSON* usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", problem.c_str());
    cJSON_AddItemToArray(msgs, usr);

    int http_st = 0;
    esp_err_t err = execute_single_profile_turn(prof, msgs, nullptr, true, out_analysis, &http_st);
    cJSON_Delete(msgs);
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
    AudioHal::instance().set_muted(false);
    if (AudioHal::instance().get_speaker_volume() == 0) {
        AudioHal::instance().set_speaker_volume(85, false);
    }

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
    AudioHal::instance().set_muted(false);
    if (AudioHal::instance().get_speaker_volume() == 0) {
        AudioHal::instance().set_speaker_volume(85, false);
    }
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
// ROM TJpgDec (esp_rom_tjpgd.h — 0 byte Flash) Helpers cho Video MJPEG ST7789
// ---------------------------------------------------------------------------
namespace {

struct TjpgdDecodeCtx {
    const uint8_t* jpeg_data;
    size_t         jpeg_size;
    size_t         read_pos;
    uint16_t*      out_rgb565_be;
    uint16_t       out_w;
    uint16_t       out_h;
};

UINT tjpgd_in_cb(JDEC* jd, BYTE* buff, UINT nbyte) {
    auto* ctx = static_cast<TjpgdDecodeCtx*>(jd->device);
    if (!ctx || ctx->read_pos >= ctx->jpeg_size) return 0;
    const size_t avail = ctx->jpeg_size - ctx->read_pos;
    const size_t n     = std::min<size_t>(avail, static_cast<size_t>(nbyte));
    if (buff && n > 0) {
        memcpy(buff, ctx->jpeg_data + ctx->read_pos, n);
    }
    ctx->read_pos += n;
    return static_cast<UINT>(n);
}

inline int tjpgd_out_common(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* ctx = static_cast<TjpgdDecodeCtx*>(jd->device);
    if (!ctx || !ctx->out_rgb565_be || !bitmap || !rect) return 0;

    const int block_w = static_cast<int>(rect->right) - static_cast<int>(rect->left) + 1;
#if defined(JD_FORMAT) && (JD_FORMAT == 1)
    const uint16_t* src = static_cast<const uint16_t*>(bitmap);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        if (y < 0 || y >= ctx->out_h) continue;
        const int row_off = (y - rect->top) * block_w;
        for (int x = rect->left; x <= rect->right; ++x) {
            if (x < 0 || x >= ctx->out_w) continue;
            const uint16_t px = src[row_off + (x - rect->left)];
            ctx->out_rgb565_be[y * ctx->out_w + x] = static_cast<uint16_t>((px >> 8) | (px << 8));
        }
    }
#else
    const uint8_t* src = static_cast<const uint8_t*>(bitmap);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        if (y < 0 || y >= ctx->out_h) continue;
        const int row_off = (y - rect->top) * block_w;
        for (int x = rect->left; x <= rect->right; ++x) {
            if (x < 0 || x >= ctx->out_w) continue;
            const uint8_t* p = src + (row_off + (x - rect->left)) * 3;
            const uint16_t px =
                (static_cast<uint16_t>(p[0] & 0xF8) << 8) |
                (static_cast<uint16_t>(p[1] & 0xFC) << 3) |
                (static_cast<uint16_t>(p[2]) >> 3);
            ctx->out_rgb565_be[y * ctx->out_w + x] = static_cast<uint16_t>((px >> 8) | (px << 8));
        }
    }
#endif
    return 1;
}

UINT tjpgd_out_cb_uint(JDEC* jd, void* bitmap, JRECT* rect) {
    return static_cast<UINT>(tjpgd_out_common(jd, bitmap, rect));
}

int tjpgd_out_cb_int(JDEC* jd, void* bitmap, JRECT* rect) {
    return tjpgd_out_common(jd, bitmap, rect);
}

template <typename FnU, typename FnI>
inline JRESULT dispatch_jd_decomp(JDEC* jd, FnU fn_u, FnI fn_i, BYTE scale) {
    if constexpr (std::is_convertible_v<FnU, UINT (*)(JDEC*, void*, JRECT*)>) {
        (void)fn_i;
        return jd_decomp(jd, fn_u, scale);
    } else {
        (void)fn_u;
        return jd_decomp(jd, fn_i, scale);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Real-time MJPEG (+ Audio) Video Streamer -> ST7789 240x240 qua ROM esp_rom_tjpgd.h
// ---------------------------------------------------------------------------
esp_err_t ZClawLlmClient::stream_mjpeg_video_url(const std::string& url,
                                                 const std::string& title,
                                                 const std::string& source_tag) {
    if (url.empty()) return ESP_ERR_INVALID_ARG;

    constexpr size_t kStreamBufSz = 32768; // 32KB đủ chứa 1 frame JPEG 240x240 + part header
    constexpr size_t kWorkBufSz   = 4096;  // Workspace nội bộ cho ROM TJpgDec
    uint8_t* rx_buf   = static_cast<uint8_t*>(board_malloc_prefer_spiram(kStreamBufSz));
    void*    jd_work  = board_malloc_prefer_spiram(kWorkBufSz);
    if (!rx_buf || !jd_work) {
        board_free(rx_buf);
        board_free(jd_work);
        ESP_LOGE(TAG, "Thiếu RAM cấp phát buffer nhận/giải mã MJPEG");
        return ESP_ERR_NO_MEM;
    }

    uint16_t max_fb_w = LCD_WIDTH;
    uint16_t max_fb_h = LCD_HEIGHT;
    size_t rgb_buf_sz = static_cast<size_t>(max_fb_w) * max_fb_h * sizeof(uint16_t);
    uint8_t* rgb_buf  = static_cast<uint8_t*>(board_malloc_prefer_spiram(rgb_buf_sz));
    if (!rgb_buf) {
        // Fallback 120x120 cho bo không có PSRAM nếu internal SRAM bị chật
        max_fb_w   = LCD_WIDTH / 2;
        max_fb_h   = LCD_HEIGHT / 2;
        rgb_buf_sz = static_cast<size_t>(max_fb_w) * max_fb_h * sizeof(uint16_t);
        rgb_buf    = static_cast<uint8_t*>(board_malloc_prefer_spiram(rgb_buf_sz));
    }
    if (!rgb_buf) {
        board_free(jd_work);
        board_free(rx_buf);
        ESP_LOGE(TAG, "Thiếu RAM cấp phát RGB565 framebuffer video");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t hcfg = {};
    hcfg.url               = url.c_str();
    hcfg.method            = HTTP_METHOD_GET;
    hcfg.timeout_ms        = 12000;
    hcfg.buffer_size       = 4096;
    hcfg.buffer_size_tx    = 1024;
    hcfg.user_agent        = "Mozilla/5.0 (ESP32-S3 S.W.Ni-Oh MJPEG/1.0)";
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        board_free(rgb_buf);
        board_free(jd_work);
        board_free(rx_buf);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    int status    = 0;
    for (int hop = 0; hop < 4; ++hop) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MJPEG open lỗi: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            board_free(rgb_buf);
            board_free(jd_work);
            board_free(rx_buf);
            return err;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            board_free(rgb_buf);
            board_free(jd_work);
            board_free(rx_buf);
            return ESP_FAIL;
        }
        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_close(client);
            if (esp_http_client_set_redirection(client) != ESP_OK) {
                esp_http_client_cleanup(client);
                board_free(rgb_buf);
                board_free(jd_work);
                board_free(rx_buf);
                return ESP_FAIL;
            }
            continue;
        }
        break;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "MJPEG HTTP status %d từ %s", status, url.c_str());
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        board_free(rgb_buf);
        board_free(jd_work);
        board_free(rx_buf);
        return ESP_FAIL;
    }

    AudioHal::instance().clear_stop_playback();
    if (UiEyesClock::instance().enter_video_mode(title.c_str(), source_tag.c_str()) != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        board_free(rgb_buf);
        board_free(jd_work);
        board_free(rx_buf);
        return ESP_FAIL;
    }

    // Cấp phát bộ giải mã âm thanh MP3 lười (chỉ alloc nếu luồng có part audio/mpeg đi kèm)
    Mp3RamContext mp3_opt;
    bool mp3_ready = false;

    size_t rx_len         = 0;
    bool   banner_hidden  = false;
    bool   eof            = false;
    const int64_t start_us = esp_timer_get_time();

    auto find_seq = [](const uint8_t* hay, size_t hlen, const uint8_t* needle, size_t nlen, size_t from) -> int {
        if (hlen < nlen || from > hlen - nlen) return -1;
        for (size_t i = from; i <= hlen - nlen; ++i) {
            if (memcmp(hay + i, needle, nlen) == 0) return static_cast<int>(i);
        }
        return -1;
    };

    auto decode_and_draw_jpeg = [&](const uint8_t* jdata, size_t jsize) {
        TjpgdDecodeCtx dctx = {
            .jpeg_data     = jdata,
            .jpeg_size     = jsize,
            .read_pos      = 0,
            .out_rgb565_be = reinterpret_cast<uint16_t*>(rgb_buf),
            .out_w         = 0,
            .out_h         = 0,
        };
        JDEC jdec = {};
        if (jd_prepare(&jdec, tjpgd_in_cb, jd_work, kWorkBufSz, &dctx) != JDR_OK) {
            return;
        }

        BYTE scale = 0;
        while (scale < 3 &&
               ((static_cast<uint16_t>(jdec.width)  >> scale) > max_fb_w ||
                (static_cast<uint16_t>(jdec.height) >> scale) > max_fb_h)) {
            scale++;
        }

        dctx.out_w = static_cast<uint16_t>(jdec.width >> scale);
        dctx.out_h = static_cast<uint16_t>(jdec.height >> scale);
        if (dctx.out_w == 0 || dctx.out_h == 0 || dctx.out_w > max_fb_w || dctx.out_h > max_fb_h) {
            return;
        }

        if (dispatch_jd_decomp(&jdec, tjpgd_out_cb_uint, tjpgd_out_cb_int, scale) == JDR_OK) {
            const uint16_t dst_x = (dctx.out_w < LCD_WIDTH)
                                       ? static_cast<uint16_t>((LCD_WIDTH - dctx.out_w) / 2)
                                       : 0;
            const uint16_t dst_y = (dctx.out_h < LCD_HEIGHT)
                                       ? static_cast<uint16_t>((LCD_HEIGHT - dctx.out_h) / 2)
                                       : 0;
            UiEyesClock::instance().push_video_frame_rgb565(
                dst_x, dst_y, dctx.out_w, dctx.out_h, dctx.out_rgb565_be);
        }
    };

    enum class PartType { UNKNOWN, JPEG, AUDIO_MP3 };
    PartType cur_part_type = PartType::UNKNOWN;
    size_t   cur_part_clen = 0;

    while (!AudioHal::instance().is_playback_stop_requested()) {
        // Ẩn thanh tiêu đề video sau 3 giây đầu tiên
        if (!banner_hidden && (esp_timer_get_time() - start_us) >= 3000000LL) {
            banner_hidden = true;
            UiEyesClock::instance().hide_video_title_banner();
        }

        if (!eof && rx_len < kStreamBufSz) {
            int r = esp_http_client_read(
                client,
                reinterpret_cast<char*>(rx_buf + rx_len),
                static_cast<int>(kStreamBufSz - rx_len));
            if (r < 0) {
                err = ESP_FAIL;
                break;
            }
            if (r == 0) {
                eof = true;
            } else {
                rx_len += static_cast<size_t>(r);
            }
        }

        // Xử lý part multipart header nếu gặp "--" đứng trước "\r\n\r\n"
        if (rx_len >= 4 && rx_buf[0] == '-' && rx_buf[1] == '-') {
            static const uint8_t kHdrEnd[4] = {'\r', '\n', '\r', '\n'};
            int hdr_end = find_seq(rx_buf, std::min<size_t>(rx_len, 512), kHdrEnd, 4, 2);
            if (hdr_end > 0) {
                std::string hdr(reinterpret_cast<const char*>(rx_buf), static_cast<size_t>(hdr_end));
                cur_part_type = PartType::JPEG;
                cur_part_clen = 0;
                if (hdr.find("audio/mpeg") != std::string::npos ||
                    hdr.find("audio/mp3") != std::string::npos) {
                    cur_part_type = PartType::AUDIO_MP3;
                }
                size_t cl_pos = hdr.find("Content-Length:");
                if (cl_pos == std::string::npos) cl_pos = hdr.find("content-length:");
                if (cl_pos != std::string::npos) {
                    cur_part_clen = static_cast<size_t>(strtoul(hdr.c_str() + cl_pos + 15, nullptr, 10));
                }

                const size_t drop = static_cast<size_t>(hdr_end) + 4U;
                memmove(rx_buf, rx_buf + drop, rx_len - drop);
                rx_len -= drop;

                // Giải mã phần audio/mpeg đi kèm qua minimp3 ra loa MAX98357A
                if (cur_part_type == PartType::AUDIO_MP3 && cur_part_clen > 0 && cur_part_clen <= kStreamBufSz) {
                    while (rx_len < cur_part_clen && !eof && !AudioHal::instance().is_playback_stop_requested()) {
                        int r = esp_http_client_read(
                            client,
                            reinterpret_cast<char*>(rx_buf + rx_len),
                            static_cast<int>(kStreamBufSz - rx_len));
                        if (r <= 0) { eof = true; break; }
                        rx_len += static_cast<size_t>(r);
                    }
                    if (rx_len >= cur_part_clen) {
                        if (!mp3_ready) mp3_ready = mp3_opt.alloc();
                        if (mp3_ready && mp3_opt.in_len + cur_part_clen <= Mp3RamContext::kInBufSize) {
                            memcpy(mp3_opt.in_buf + mp3_opt.in_len, rx_buf, cur_part_clen);
                            mp3_opt.in_len += cur_part_clen;
                            mp3_opt.decode_and_play_available(false);
                        }
                        memmove(rx_buf, rx_buf + cur_part_clen, rx_len - cur_part_clen);
                        rx_len -= cur_part_clen;
                        cur_part_clen = 0;
                        continue;
                    }
                }
            }
        }

        // Tìm marker JPEG SOI (0xFF 0xD8) .. EOI (0xFF 0xD9)
        static const uint8_t kSoi[2] = {0xFF, 0xD8};
        static const uint8_t kEoi[2] = {0xFF, 0xD9};
        int soi = find_seq(rx_buf, rx_len, kSoi, 2, 0);
        if (soi >= 0) {
            if (soi > 0) {
                memmove(rx_buf, rx_buf + soi, rx_len - static_cast<size_t>(soi));
                rx_len -= static_cast<size_t>(soi);
            }
            if (cur_part_clen >= 2 && rx_len >= cur_part_clen) {
                decode_and_draw_jpeg(rx_buf, cur_part_clen);
                memmove(rx_buf, rx_buf + cur_part_clen, rx_len - cur_part_clen);
                rx_len -= cur_part_clen;
                cur_part_clen = 0;
                continue;
            }
            int eoi = find_seq(rx_buf, rx_len, kEoi, 2, 2);
            if (eoi > 0) {
                const size_t frame_len = static_cast<size_t>(eoi) + 2U;
                decode_and_draw_jpeg(rx_buf, frame_len);
                memmove(rx_buf, rx_buf + frame_len, rx_len - frame_len);
                rx_len -= frame_len;
                cur_part_clen = 0;
                continue;
            }
        } else if (rx_len > 1024 && cur_part_type != PartType::AUDIO_MP3) {
            memmove(rx_buf, rx_buf + (rx_len - 2), 2);
            rx_len = 2;
        }

        if (rx_len >= kStreamBufSz) {
            rx_len = 0;
        }
        if (eof && rx_len < 4) break;
    }

    if (mp3_ready) {
        AudioHal::instance().flush_speaker_silence(30);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    board_free(rgb_buf);
    board_free(jd_work);
    board_free(rx_buf);
    UiEyesClock::instance().exit_video_mode();

    return AudioHal::instance().is_playback_stop_requested() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void ZClawLlmClient::register_mcp_tools(McpSchemaProviderFn schema_cb, McpExecuteFn exec_cb) {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        mcp_schema_cb_ = schema_cb;
        mcp_exec_cb_   = exec_cb;
        xSemaphoreGive(state_mutex_);
    } else {
        mcp_schema_cb_ = schema_cb;
        mcp_exec_cb_   = exec_cb;
    }
}

void ZClawLlmClient::register_chronos_status_provider(ChronosStatusJsonFn cb) {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        chronos_status_cb_ = cb;
        xSemaphoreGive(state_mutex_);
    } else {
        chronos_status_cb_ = cb;
    }
}

// ---------------------------------------------------------------------------
// Pending Media Playback Helpers (Music / Radio / Video)
// ---------------------------------------------------------------------------
void ZClawLlmClient::schedule_media_playback(MediaKind kind,
                                             const char* url,
                                             const char* source_tag,
                                             const char* title) {
    if (!url || kind == MediaKind::NONE) return;
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        pending_media_.kind       = kind;
        pending_media_.url        = url;
        pending_media_.source_tag = source_tag ? source_tag : "";
        pending_media_.title      = title ? title : "";
        xSemaphoreGive(state_mutex_);
    } else {
        pending_media_.kind       = kind;
        pending_media_.url        = url;
        pending_media_.source_tag = source_tag ? source_tag : "";
        pending_media_.title      = title ? title : "";
    }
}

bool ZClawLlmClient::consume_pending_media(PendingMedia& out_media) {
    bool has = false;
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (pending_media_.kind != MediaKind::NONE && !pending_media_.url.empty()) {
            out_media = pending_media_;
            pending_media_ = {};
            has = true;
        }
        xSemaphoreGive(state_mutex_);
    }
    return has;
}

void ZClawLlmClient::clear_pending_media() {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        pending_media_ = {};
        xSemaphoreGive(state_mutex_);
    }
}

} // namespace zclaw
