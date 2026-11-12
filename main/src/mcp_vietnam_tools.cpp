// mcp_vietnam_tools.cpp
// Xiaozhi-VN ZClaw Edition — Vietnamese Native MCP (10 công cụ 100% Tiếng Việt)
// Production-ready C++17 implementation — no TODOs, no pseudo-code, no exceptions.

#include "mcp_vietnam_tools.h"
#include "audio_hal.h"
#include "board_config.h"
#include "chronos_ble.h"
#include "espnow_comm.h"
#include "ui_eyes_clock.h"
#include "zclaw_llm_client.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char* TAG = "McpVN";

namespace zclaw {

namespace {

// ---------------------------------------------------------------------------
// URL Encode & Text Normalization Helpers
// ---------------------------------------------------------------------------
std::string url_encode(const char* src, bool space_as_underscore = false) {
    if (!src) return "";
    std::string out;
    out.reserve(strlen(src) * 2);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src); *p; ++p) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ' && space_as_underscore) {
            out.push_back('_');
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Chuẩn hóa chuỗi tiếng Việt về chữ thường không dấu để khớp tên tỉnh/chủ đề/kênh radio
std::string fold_vn_lower(const char* s) {
    if (!s) return "";
    static const struct { const char* utf8; char ascii; } kMap[] = {
        {"à",'a'},{"á",'a'},{"ả",'a'},{"ã",'a'},{"ạ",'a'},
        {"ă",'a'},{"ằ",'a'},{"ắ",'a'},{"ẳ",'a'},{"ẵ",'a'},{"ặ",'a'},
        {"â",'a'},{"ầ",'a'},{"ấ",'a'},{"ẩ",'a'},{"ẫ",'a'},{"ậ",'a'},
        {"À",'a'},{"Á",'a'},{"Ả",'a'},{"Ã",'a'},{"Ạ",'a'},
        {"Ă",'a'},{"Ằ",'a'},{"Ắ",'a'},{"Ẳ",'a'},{"Ẵ",'a'},{"Ặ",'a'},
        {"Â",'a'},{"Ầ",'a'},{"Ấ",'a'},{"Ẩ",'a'},{"Ẫ",'a'},{"Ậ",'a'},
        {"đ",'d'},{"Đ",'d'},
        {"è",'e'},{"é",'e'},{"ẻ",'e'},{"ẽ",'e'},{"ẹ",'e'},
        {"ê",'e'},{"ề",'e'},{"ế",'e'},{"ể",'e'},{"ễ",'e'},{"ệ",'e'},
        {"È",'e'},{"É",'e'},{"Ẻ",'e'},{"Ẽ",'e'},{"Ẹ",'e'},
        {"Ê",'e'},{"Ề",'e'},{"Ế",'e'},{"Ể",'e'},{"Ễ",'e'},{"Ệ",'e'},
        {"ì",'i'},{"í",'i'},{"ỉ",'i'},{"ĩ",'i'},{"ị",'i'},
        {"Ì",'i'},{"Í",'i'},{"Ỉ",'i'},{"Ĩ",'i'},{"Ị",'i'},
        {"ò",'o'},{"ó",'o'},{"ỏ",'o'},{"õ",'o'},{"ọ",'o'},
        {"ô",'o'},{"ồ",'o'},{"ố",'o'},{"ổ",'o'},{"ỗ",'o'},{"ộ",'o'},
        {"ơ",'o'},{"ờ",'o'},{"ớ",'o'},{"ở",'o'},{"ỡ",'o'},{"ợ",'o'},
        {"Ò",'o'},{"Ó",'o'},{"Ỏ",'o'},{"Õ",'o'},{"Ọ",'o'},
        {"Ô",'o'},{"Ồ",'o'},{"Ố",'o'},{"Ổ",'o'},{"Ỗ",'o'},{"Ộ",'o'},
        {"Ơ",'o'},{"Ờ",'o'},{"Ớ",'o'},{"Ở",'o'},{"Ỡ",'o'},{"Ợ",'o'},
        {"ù",'u'},{"ú",'u'},{"ủ",'u'},{"ũ",'u'},{"ụ",'u'},
        {"ư",'u'},{"ừ",'u'},{"ứ",'u'},{"ử",'u'},{"ữ",'u'},{"ự",'u'},
        {"Ù",'u'},{"Ú",'u'},{"Ủ",'u'},{"Ũ",'u'},{"Ụ",'u'},
        {"Ư",'u'},{"Ừ",'u'},{"Ứ",'u'},{"Ử",'u'},{"Ữ",'u'},{"Ự",'u'},
        {"ỳ",'y'},{"ý",'y'},{"ỷ",'y'},{"ỹ",'y'},{"ỵ",'y'},
        {"Ỳ",'y'},{"Ý",'y'},{"Ỷ",'y'},{"Ỹ",'y'},{"Ỵ",'y'},
    };

    std::string out;
    const size_t len = strlen(s);
    out.reserve(len);
    for (size_t i = 0; i < len; ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(tolower(c)));
            i++;
            continue;
        }
        bool matched = false;
        for (const auto& m : kMap) {
            const size_t mlen = strlen(m.utf8);
            if (i + mlen <= len && memcmp(s + i, m.utf8, mlen) == 0) {
                out.push_back(m.ascii);
                i += mlen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            // Bỏ qua ký tự đa byte không nằm trong bảng hoặc copy an toàn
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i += 1;
        }
    }
    return out;
}

std::string strip_html_and_cdata(const std::string& raw) {
    std::string s = raw;
    // Bỏ CDATA wrapper
    const std::string cdata_start = "<![CDATA[";
    const std::string cdata_end   = "]]>";
    size_t pos = s.find(cdata_start);
    if (pos != std::string::npos) {
        s.erase(pos, cdata_start.size());
    }
    pos = s.find(cdata_end);
    if (pos != std::string::npos) {
        s.erase(pos, cdata_end.size());
    }

    // Lọc bỏ các thẻ HTML <...>
    std::string clean;
    clean.reserve(s.size());
    bool in_tag = false;
    for (char ch : s) {
        if (ch == '<') {
            in_tag = true;
            continue;
        }
        if (ch == '>') {
            in_tag = false;
            clean.push_back(' ');
            continue;
        }
        if (!in_tag) {
            clean.push_back(ch);
        }
    }

    // Giải mã các HTML entities hay gặp
    auto replace_all = [&](const char* from, const char* to) {
        size_t flen = strlen(from);
        size_t tlen = strlen(to);
        size_t p = 0;
        while ((p = clean.find(from, p)) != std::string::npos) {
            clean.replace(p, flen, to);
            p += tlen;
        }
    };
    replace_all("&amp;", "&");
    replace_all("&quot;", "\"");
    replace_all("&#39;", "'");
    replace_all("&apos;", "'");
    replace_all("&lt;", "<");
    replace_all("&gt;", ">");
    replace_all("&nbsp;", " ");

    // Gọn khoảng trắng dư
    std::string final_str;
    final_str.reserve(clean.size());
    bool prev_space = true;
    for (char ch : clean) {
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            if (!prev_space) {
                final_str.push_back(' ');
                prev_space = true;
            }
        } else {
            final_str.push_back(ch);
            prev_space = false;
        }
    }
    if (!final_str.empty() && final_str.back() == ' ') {
        final_str.pop_back();
    }
    return final_str;
}

// ---------------------------------------------------------------------------
// HTTP GET an toàn bộ nhớ (cấp phát ưu tiên PSRAM, giới hạn max_bytes)
// ---------------------------------------------------------------------------
esp_err_t http_get_text(const char* url,
                        std::string& out_body,
                        int* out_status = nullptr,
                        size_t max_bytes = 32768,
                        const char* user_agent = "Xiaozhi-VN-ZClaw/1.0") {
    out_body.clear();
    if (!url) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_GET;
    cfg.timeout_ms        = 10000;
    cfg.buffer_size       = 2048;
    cfg.buffer_size_tx    = 1024;
    cfg.user_agent        = user_agent;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_OK;
    int status    = 0;

    // Vòng lặp theo dõi HTTP Redirect (301/302/307/308) tối đa 4 chặng
    for (int hop = 0; hop < 4; ++hop) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP GET open lỗi (%s): %s", url, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return err;
        }

        int hdr_ret = esp_http_client_fetch_headers(client);
        if (hdr_ret < 0) {
            ESP_LOGE(TAG, "HTTP GET fetch_headers lỗi (%s)", url);
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

    if (out_status) *out_status = status;

    char* chunk_buf = static_cast<char*>(board_malloc_prefer_spiram(2048));
    if (!chunk_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    while (out_body.size() < max_bytes) {
        const size_t to_read = std::min<size_t>(2048, max_bytes - out_body.size());
        int r = esp_http_client_read(client, chunk_buf, static_cast<int>(to_read));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break; // EOF
        }
        out_body.append(chunk_buf, static_cast<size_t>(r));
    }

    board_free(chunk_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

std::string make_error_json(const char* msg) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "loi", msg ? msg : "Lỗi không xác định");
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "{\"ok\":false}";
    if (printed) cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

std::string cjson_to_string_and_free(cJSON* obj) {
    char* printed = cJSON_PrintUnformatted(obj);
    std::string out = printed ? printed : "{}";
    if (printed) cJSON_free(printed);
    cJSON_Delete(obj);
    return out;
}

// ---------------------------------------------------------------------------
// Bảng tọa độ các tỉnh/thành Việt Nam phổ biến (Open-Meteo)
// ---------------------------------------------------------------------------
struct VnCityCoord {
    const char* match_key; // chữ thường không dấu
    const char* display_name;
    double lat;
    double lon;
};

static constexpr VnCityCoord kVnCities[] = {
    {"ha noi",       "Hà Nội",            21.0285, 105.8542},
    {"hanoi",        "Hà Nội",            21.0285, 105.8542},
    {"ho chi minh",  "TP. Hồ Chí Minh",   10.8231, 106.6297},
    {"tp hcm",       "TP. Hồ Chí Minh",   10.8231, 106.6297},
    {"tphcm",        "TP. Hồ Chí Minh",   10.8231, 106.6297},
    {"hcm",          "TP. Hồ Chí Minh",   10.8231, 106.6297},
    {"sai gon",      "TP. Hồ Chí Minh",   10.8231, 106.6297},
    {"da nang",      "Đà Nẵng",           16.0544, 108.2022},
    {"hai phong",    "Hải Phòng",         20.8449, 106.6881},
    {"can tho",      "Cần Thơ",           10.0452, 105.7469},
    {"hue",          "Huế",               16.4637, 107.5909},
    {"nha trang",    "Nha Trang",         12.2388, 109.1967},
    {"da lat",       "Đà Lạt",            11.9404, 108.4583},
    {"vung tau",     "Vũng Tàu",          10.3460, 107.0843},
    {"phu quoc",     "Phú Quốc",          10.2899, 103.9840},
    {"ha long",      "Hạ Long",           20.9599, 107.0425},
    {"quang ninh",   "Quảng Ninh",        20.9599, 107.0425},
    {"vinh",         "TP. Vinh",          18.6796, 105.6813},
    {"thanh hoa",    "Thanh Hóa",         19.8067, 105.7852},
    {"quy nhon",     "Quy Nhơn",          13.7830, 109.2197},
    {"buon ma thuot","Buôn Ma Thuột",     12.6667, 108.0500},
    {"pleiku",       "Pleiku",            13.9833, 108.0000},
    {"sa pa",        "Sa Pa",             22.3364, 103.8438},
    {"lao cai",      "Lào Cai",           22.4809, 103.9755},
    {"binh duong",   "Bình Dương",        11.3254, 106.4770},
    {"bien hoa",     "Biên Hòa",          10.9574, 106.8427},
    {"dong nai",     "Đồng Nai",          10.9574, 106.8427},
    {"bac ninh",     "Bắc Ninh",          21.1861, 106.0763},
    {"thai nguyen",  "Thái Nguyên",       21.5942, 105.8482},
    {"ninh binh",    "Ninh Bình",         20.2506, 105.9745},
    {"phan thiet",   "Phan Thiết",        10.9289, 108.1021},
    {"ca mau",       "Cà Mau",             9.1768, 105.1524},
};

const char* wmo_code_to_vi(int code, const char** out_short_ui) {
    if (code == 0) {
        if (out_short_ui) *out_short_ui = "Nắng";
        return "Trời quang đãng, nắng đẹp";
    }
    if (code <= 3) {
        if (out_short_ui) *out_short_ui = "Mây";
        return (code == 1) ? "Ít mây, nắng nhẹ" : "Trời nhiều mây";
    }
    if (code == 45 || code == 48) {
        if (out_short_ui) *out_short_ui = "Mây";
        return "Có sương mù nhẹ";
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        if (out_short_ui) *out_short_ui = "Mưa";
        return (code >= 63 || code == 82) ? "Mưa rào vừa đến to" : "Có mưa nhẹ / mưa phùn";
    }
    if (code >= 95) {
        if (out_short_ui) *out_short_ui = "Bão";
        return "Có mưa dông và sấm sét";
    }
    if (out_short_ui) *out_short_ui = "Mây";
    return "Thời tiết ôn hòa";
}

// ---------------------------------------------------------------------------
// Thuật toán Hồ Ngọc Đức cho Âm lịch Việt Nam + Can Chi + Giờ Hoàng Đạo
// ---------------------------------------------------------------------------
int jdn_from_greg(int dd, int mm, int yy) {
    int a = (14 - mm) / 12;
    int y = yy + 4800 - a;
    int m = mm + 12 * a - 3;
    int jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
    if (jd < 2299161) {
        jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - 32083;
    }
    return jd;
}

double new_moon_jdn(int k) {
    double T  = static_cast<double>(k) / 1236.85;
    double T2 = T * T;
    double T3 = T2 * T;
    double dr = M_PI / 180.0;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
    Jd1 += 0.00033 * std::sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    double M   = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    double F   = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1  = (0.1734 - 0.000393 * T) * std::sin(M * dr) + 0.0021 * std::sin(2.0 * dr * M);
    C1 = C1 - 0.4068 * std::sin(Mpr * dr) + 0.0161 * std::sin(dr * 2.0 * Mpr);
    C1 = C1 - 0.0004 * std::sin(dr * 3.0 * Mpr);
    C1 = C1 + 0.0104 * std::sin(dr * 2.0 * F) - 0.0051 * std::sin(dr * (M + Mpr));
    C1 = C1 - 0.0074 * std::sin(dr * (M - Mpr)) + 0.0004 * std::sin(dr * (2.0 * F + M));
    C1 = C1 - 0.0004 * std::sin(dr * (2.0 * F - M)) - 0.0006 * std::sin(dr * (2.0 * F + Mpr));
    C1 = C1 + 0.0010 * std::sin(dr * (2.0 * F - Mpr)) + 0.0005 * std::sin(dr * (2.0 * Mpr + M));
    double delta_t = (T < -11.0)
        ? (0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3)
        : (-0.000278 + 0.000265 * T + 0.000262 * T2);
    return Jd1 + C1 - delta_t;
}

int nm_day(int k, double tz) {
    return static_cast<int>(std::floor(new_moon_jdn(k) + 0.5 + tz / 24.0));
}

double sun_long_deg(double jdn) {
    double T  = (jdn - 2451545.0) / 36525.0;
    double T2 = T * T;
    double dr = M_PI / 180.0;
    double M  = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * std::sin(dr * M);
    DL += (0.019993 - 0.000101 * T) * std::sin(dr * 2.0 * M) + 0.000290 * std::sin(dr * 3.0 * M);
    double L = (L0 + DL) * dr;
    L -= M_PI * 2.0 * std::floor(L / (M_PI * 2.0));
    return L / dr;
}

int sun_sector(int day_number, double tz) {
    return static_cast<int>(std::floor(sun_long_deg(static_cast<double>(day_number) - 0.5 - tz / 24.0) / 30.0));
}

int lunar_m11(int yy, double tz) {
    int off = jdn_from_greg(31, 12, yy) - 2415021;
    int k   = static_cast<int>(std::floor(static_cast<double>(off) / 29.530588853));
    int nm  = nm_day(k, tz);
    if (sun_sector(nm, tz) >= 9) {
        nm = nm_day(k - 1, tz);
    }
    return nm;
}

int leap_offset(int a11, double tz) {
    int k   = static_cast<int>(std::floor((a11 - 2415021.076998695) / 29.530588853 + 0.5));
    int last = 0, i = 1;
    int arc = sun_sector(nm_day(k + i, tz), tz);
    do {
        last = arc;
        i++;
        arc = sun_sector(nm_day(k + i, tz), tz);
    } while (arc != last && i < 14);
    return i - 1;
}

struct FullLunarInfo {
    int  day;
    int  month;
    int  year;
    bool leap;
    int  jdn;
};

FullLunarInfo solar_to_lunar_full(int dd, int mm, int yy) {
    constexpr double TZ = 7.0;
    int jdn = jdn_from_greg(dd, mm, yy);
    int k   = static_cast<int>(std::floor((jdn - 2415021.076998695) / 29.530588853));
    int ms  = nm_day(k + 1, TZ);
    if (ms > jdn) ms = nm_day(k, TZ);

    int a11 = lunar_m11(yy, TZ);
    int b11 = a11;
    int ly  = yy;
    if (a11 >= ms) {
        ly  = yy;
        a11 = lunar_m11(yy - 1, TZ);
    } else {
        ly  = yy + 1;
        b11 = lunar_m11(yy + 1, TZ);
    }

    int ld   = jdn - ms + 1;
    int diff = static_cast<int>(std::floor((ms - a11) / 29.0));
    bool leap = false;
    int lm   = diff + 11;

    if (b11 - a11 > 365) {
        int loff = leap_offset(a11, TZ);
        if (diff >= loff) {
            lm = diff + 10;
            if (diff == loff) leap = true;
        }
    }
    if (lm > 12) lm -= 12;
    if (lm >= 11 && diff < 4) ly -= 1;

    return {ld, lm, ly, leap, jdn};
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton & Lifecycle
// ---------------------------------------------------------------------------
McpRegistry& McpRegistry::instance() {
    static McpRegistry inst;
    return inst;
}

esp_err_t McpRegistry::init() {
    if (initialized_.load()) return ESP_OK;

    pomodoro_queue_ = xQueueCreate(4, sizeof(PomodoroEvent));
    if (!pomodoro_queue_) {
        return ESP_ERR_NO_MEM;
    }

    worker_running_.store(true);
    if (xTaskCreatePinnedToCore(pomodoro_worker_task, "pomodoro", 8192,
                                this, 4, &pomodoro_task_, 1) != pdPASS) {
        vQueueDelete(pomodoro_queue_);
        pomodoro_queue_ = nullptr;
        worker_running_.store(false);
        return ESP_FAIL;
    }

    esp_timer_create_args_t targs = {};
    targs.callback = pomodoro_timer_cb;
    targs.arg      = this;
    targs.name     = "zclaw_pomo";
    esp_err_t ret  = esp_timer_create(&targs, &pomodoro_timer_);
    if (ret != ESP_OK) {
        deinit();
        return ret;
    }

    initialized_.store(true);
    ESP_LOGI(TAG, "McpRegistry sẵn sàng với 10 công cụ MCP Tiếng Việt");
    return ESP_OK;
}

void McpRegistry::deinit() {
    initialized_.store(false);
    worker_running_.store(false);
    pomodoro_active_.store(false);

    if (pomodoro_timer_) {
        esp_timer_stop(pomodoro_timer_);
        esp_timer_delete(pomodoro_timer_);
        pomodoro_timer_ = nullptr;
    }
    if (pomodoro_queue_) {
        PomodoroEvent dummy = {};
        xQueueSend(pomodoro_queue_, &dummy, 0);
    }
    for (int i = 0; i < 30 && pomodoro_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (pomodoro_task_) {
        vTaskDelete(pomodoro_task_);
        pomodoro_task_ = nullptr;
    }
    if (pomodoro_queue_) {
        vQueueDelete(pomodoro_queue_);
        pomodoro_queue_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// build_tools_schema_json() — 10 Schema MCP OpenAI-compatible 100% Tiếng Việt
// ---------------------------------------------------------------------------
cJSON* McpRegistry::build_tools_schema_json() const {
    cJSON* arr = cJSON_CreateArray();

    auto add_tool = [&](const char* name, const char* desc, cJSON* props, cJSON* req) {
        cJSON* wrapper = cJSON_CreateObject();
        cJSON_AddStringToObject(wrapper, "type", "function");

        cJSON* fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", name);
        cJSON_AddStringToObject(fn, "description", desc);

        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "type", "object");
        cJSON_AddItemToObject(params, "properties", props ? props : cJSON_CreateObject());
        if (req) {
            cJSON_AddItemToObject(params, "required", req);
        }
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToObject(wrapper, "function", fn);
        cJSON_AddItemToArray(arr, wrapper);
    };

    auto add_str_prop = [](cJSON* props, const char* key, const char* desc) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "description", desc);
        cJSON_AddItemToObject(props, key, p);
    };
    auto add_int_prop = [](cJSON* props, const char* key, const char* desc) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "type", "integer");
        cJSON_AddStringToObject(p, "description", desc);
        cJSON_AddItemToObject(props, key, p);
    };
    auto make_req = [](std::initializer_list<const char*> keys) -> cJSON* {
        cJSON* r = cJSON_CreateArray();
        for (const char* k : keys) {
            cJSON_AddItemToArray(r, cJSON_CreateString(k));
        }
        return r;
    };

    // 1. xem_thoi_tiet
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "dia_diem", "Tên tỉnh hoặc thành phố Việt Nam (ví dụ: Hà Nội, TP.HCM, Đà Nẵng, Đà Lạt)");
        add_tool("xem_thoi_tiet",
                 "Xem thời tiết hiện tại (nhiệt độ, độ ẩm, khả năng mưa, tốc độ gió) và cập nhật lên đồng hồ màn hình",
                 p, make_req({"dia_diem"}));
    }

    // 2. xem_thoi_gian_lich_am
    {
        cJSON* p = cJSON_CreateObject();
        add_tool("xem_thoi_gian_lich_am",
                 "Xem giờ hiện tại theo múi giờ Việt Nam ICT+7, ngày Dương lịch, ngày Âm lịch, Can Chi và các Giờ Hoàng Đạo trong ngày",
                 p, nullptr);
    }

    // 3. doc_tin_tuc
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "chu_de", "Chủ đề tin tức: thoi_su, the_gioi, cong_nghe, kinh_doanh, the_thao, giai_tri, moi_nhat");
        add_int_prop(p, "so_luong", "Số bản tin cần đọc (từ 1 đến 6, mặc định 3)");
        add_tool("doc_tin_tuc",
                 "Tải và tóm tắt tin tức mới nhất trong ngày từ VNExpress theo chủ đề tiếng Việt",
                 p, make_req({"chu_de"}));
    }

    // 4. tra_cuu_thong_tin
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "tu_khoa", "Từ khóa, nhân vật lịch sử, địa danh hoặc khái niệm cần tra cứu");
        add_tool("tra_cuu_thong_tin",
                 "Tra cứu kiến thức bách khoa chính xác qua Wikipedia Tiếng Việt",
                 p, make_req({"tu_khoa"}));
    }

    // 5. phat_nhac_radio
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "ten_bai_hat_hoac_kenh", "Tên kênh radio (vov1, vov2, vov3, vov giao thong, nhac) hoặc URL luồng MP3 trực tiếp");
        add_tool("phat_nhac_radio",
                 "Phát radio VOV trực tuyến hoặc luồng âm thanh MP3 qua loa của thiết bị",
                 p, make_req({"ten_bai_hat_hoac_kenh"}));
    }

    // 6. mo_video
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "tu_khoa_hoac_url", "Từ khóa tìm video YouTube hoặc đường dẫn URL video");
        add_tool("mo_video",
                 "Gửi yêu cầu mở video sang điện thoại qua BLE Chronos hoặc thiết bị ESP-NOW và hiển thị tiêu đề lên màn hình",
                 p, make_req({"tu_khoa_hoac_url"}));
    }

    // 7. suy_luan_cao
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "van_de_phuc_tap", "Đề bài toán, lập trình, logic hoặc bài phân tích cần suy luận chuyên sâu từng bước");
        add_tool("suy_luan_cao",
                 "Kích hoạt chế độ suy luận sâu từng bước cho các câu hỏi phức tạp, tính toán, lập trình hoặc lập kế hoạch",
                 p, make_req({"van_de_phuc_tap"}));
    }

    // 8. dieu_khien_thiet_bi
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "lenh", "Tên lệnh: am_luong (0-100), tat_tieng (0 hoặc 1), do_nhay_mic (1-10), do_sang (5-100), xoay_man_hinh (0/90/180/270), man_hinh (0=mat, 1=dong_ho)");
        add_int_prop(p, "gia_tri", "Giá trị số nguyên tương ứng với lệnh điều khiển");
        add_tool("dieu_khien_thiet_bi",
                 "Điều khiển phần cứng thiết bị tại chỗ: âm lượng loa, tắt tiếng, độ nhạy micro, độ sáng/xoay màn hình ST7789",
                 p, make_req({"lenh", "gia_tri"}));
    }

    // 9. gui_lenh_esp_now
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "mac_dich", "Địa chỉ MAC thiết bị nhận dạng AA:BB:CC:DD:EE:FF hoặc 'broadcast' để gửi toàn mạng");
        add_str_prop(p, "noi_dung_lenh", "Nội dung tin nhắn hoặc lệnh điều khiển (hỗ trợ tiền tố ALERT:, CONTROL:, STATUS:, hoặc văn bản)");
        add_tool("gui_lenh_esp_now",
                 "Gửi tin nhắn hoặc lệnh điều khiển không dây qua ESP-NOW tới thiết bị ESP32 khác",
                 p, make_req({"mac_dich", "noi_dung_lenh"}));
    }

    // 10. hen_gio_pomodoro
    {
        cJSON* p = cJSON_CreateObject();
        add_int_prop(p, "so_phut", "Số phút đếm ngược (1 đến 180; nhập 0 để hủy hẹn giờ đang chạy)");
        add_str_prop(p, "loi_nhac", "Lời nhắc sẽ hiển thị và đọc bằng giọng nói khi hết giờ");
        add_tool("hen_gio_pomodoro",
                 "Đặt hẹn giờ đếm ngược Pomodoro hoặc nhắc việc, khi hết giờ sẽ phát chuông, rung mắt và đọc lời nhắc",
                 p, make_req({"so_phut"}));
    }

    return arr;
}

// ---------------------------------------------------------------------------
// execute_tool()
// ---------------------------------------------------------------------------
std::string McpRegistry::execute_tool(const char* tool_name, const char* args_json) {
    if (!tool_name || tool_name[0] == '\0') {
        return make_error_json("Thiếu tên công cụ MCP");
    }

    cJSON* args = (args_json && args_json[0] != '\0') ? cJSON_Parse(args_json) : cJSON_CreateObject();
    if (!args) {
        args = cJSON_CreateObject();
    }

    ESP_LOGI(TAG, "Thực thi MCP [%s] args=%s", tool_name, args_json ? args_json : "{}");

    std::string result;
    if (strcmp(tool_name, "xem_thoi_tiet") == 0) {
        result = tool_xem_thoi_tiet(args);
    } else if (strcmp(tool_name, "xem_thoi_gian_lich_am") == 0) {
        result = tool_xem_thoi_gian_lich_am(args);
    } else if (strcmp(tool_name, "doc_tin_tuc") == 0) {
        result = tool_doc_tin_tuc(args);
    } else if (strcmp(tool_name, "tra_cuu_thong_tin") == 0) {
        result = tool_tra_cuu_thong_tin(args);
    } else if (strcmp(tool_name, "phat_nhac_radio") == 0) {
        result = tool_phat_nhac_radio(args);
    } else if (strcmp(tool_name, "mo_video") == 0) {
        result = tool_mo_video(args);
    } else if (strcmp(tool_name, "suy_luan_cao") == 0) {
        result = tool_suy_luan_cao(args);
    } else if (strcmp(tool_name, "dieu_khien_thiet_bi") == 0) {
        result = tool_dieu_khien_thiet_bi(args);
    } else if (strcmp(tool_name, "gui_lenh_esp_now") == 0) {
        result = tool_gui_lenh_esp_now(args);
    } else if (strcmp(tool_name, "hen_gio_pomodoro") == 0) {
        result = tool_hen_gio_pomodoro(args);
    } else {
        result = make_error_json("Công cụ MCP không tồn tại");
    }

    cJSON_Delete(args);
    return result;
}

// ---------------------------------------------------------------------------
// 1. xem_thoi_tiet(dia_diem)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_xem_thoi_tiet(cJSON* args) {
    const cJSON* j_loc = cJSON_GetObjectItem(args, "dia_diem");
    const char* raw_loc = (cJSON_IsString(j_loc) && j_loc->valuestring && j_loc->valuestring[0])
                              ? j_loc->valuestring
                              : "Hà Nội";

    std::string folded = fold_vn_lower(raw_loc);
    double lat = 21.0285, lon = 105.8542;
    std::string resolved_name = raw_loc;
    bool found_local = false;

    for (const auto& city : kVnCities) {
        if (folded.find(city.match_key) != std::string::npos) {
            lat = city.lat;
            lon = city.lon;
            resolved_name = city.display_name;
            found_local = true;
            break;
        }
    }

    // Fallback qua Open-Meteo Geocoding nếu địa danh không nằm trong bảng tỉnh phổ biến
    if (!found_local) {
        char geo_url[256];
        std::string enc = url_encode(raw_loc, false);
        snprintf(geo_url, sizeof(geo_url),
                 "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=vi&format=json",
                 enc.c_str());
        std::string geo_resp;
        int status = 0;
        if (http_get_text(geo_url, geo_resp, &status, 8192) == ESP_OK && status == 200) {
            cJSON* gj = cJSON_Parse(geo_resp.c_str());
            if (gj) {
                cJSON* results = cJSON_GetObjectItem(gj, "results");
                cJSON* first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : nullptr;
                if (first) {
                    cJSON* jlat = cJSON_GetObjectItem(first, "latitude");
                    cJSON* jlon = cJSON_GetObjectItem(first, "longitude");
                    cJSON* jname = cJSON_GetObjectItem(first, "name");
                    if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
                        lat = jlat->valuedouble;
                        lon = jlon->valuedouble;
                        if (cJSON_IsString(jname) && jname->valuestring) {
                            resolved_name = jname->valuestring;
                        }
                        found_local = true;
                    }
                }
                cJSON_Delete(gj);
            }
        }
    }

    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?"
             "latitude=%.4f&longitude=%.4f&"
             "current=temperature_2m,relative_humidity_2m,precipitation_probability,weather_code,wind_speed_10m&"
             "timezone=Asia%%2FBangkok",
             lat, lon);

    std::string resp;
    int status = 0;
    if (http_get_text(url, resp, &status, 12288) != ESP_OK || status != 200) {
        return make_error_json("Không kết nối được máy chủ thời tiết Open-Meteo");
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) {
        return make_error_json("Dữ liệu thời tiết trả về không hợp lệ");
    }

    cJSON* cur = cJSON_GetObjectItem(root, "current");
    if (!cur) {
        cJSON_Delete(root);
        return make_error_json("Thiếu trường current từ Open-Meteo");
    }

    const double temp = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "temperature_2m"));
    const double hum  = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "relative_humidity_2m"));
    const double rain = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "precipitation_probability"));
    const double wind = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_speed_10m"));
    const int    wmo  = static_cast<int>(cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "weather_code")));
    cJSON_Delete(root);

    const char* short_ui = "Nắng";
    const char* desc_vi  = wmo_code_to_vi(wmo, &short_ui);

    // Cập nhật trực tiếp lên Dashboard Đồng hồ của UiEyesClock
    WeatherData wd = {};
    wd.temp_c       = static_cast<float>(temp);
    wd.humidity_pct = static_cast<uint8_t>(std::max(0.0, std::min(100.0, hum)));
    wd.wind_kmh     = static_cast<float>(wind);
    wd.rain_pct     = static_cast<uint8_t>(std::max(0.0, std::min(100.0, rain)));
    snprintf(wd.condition, sizeof(wd.condition), "%s", short_ui);
    wd.valid        = true;
    UiEyesClock::instance().update_weather(wd);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "dia_diem", resolved_name.c_str());
    cJSON_AddNumberToObject(out, "nhiet_do_c", temp);
    cJSON_AddNumberToObject(out, "do_am_phan_tram", wd.humidity_pct);
    cJSON_AddNumberToObject(out, "kha_nang_mua_phan_tram", wd.rain_pct);
    cJSON_AddNumberToObject(out, "toc_do_gio_kmh", wind);
    cJSON_AddStringToObject(out, "tinh_trang", desc_vi);
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 2. xem_thoi_gian_lich_am()
// ---------------------------------------------------------------------------
std::string McpRegistry::get_vietnamese_time_lunar_json() {
    time_t now = time(nullptr);
    struct tm tm_info = {};
    localtime_r(&now, &tm_info);

    const int year = tm_info.tm_year + 1900;
    const int mon  = tm_info.tm_mon + 1;
    const int day  = tm_info.tm_mday;

    if (year < 2020) {
        return make_error_json("Đồng hồ hệ thống chưa đồng bộ NTP hoặc BLE Chronos");
    }

    static const char* const kThu[] = {
        "Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"
    };
    static const char* const kCan[] = {
        "Giáp", "Ất", "Bính", "Đinh", "Mậu", "Kỷ", "Canh", "Tân", "Nhâm", "Quý"
    };
    static const char* const kChi[] = {
        "Tý", "Sửu", "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi"
    };
    static const char* const kGioRange[] = {
        "Tý (23-01h)", "Sửu (01-03h)", "Dần (03-05h)", "Mão (05-07h)",
        "Thìn (07-09h)", "Tỵ (09-11h)", "Ngọ (11-13h)", "Mùi (13-15h)",
        "Thân (15-17h)", "Dậu (17-19h)", "Tuất (19-21h)", "Hợi (21-23h)"
    };
    static const uint8_t kHoangDaoTable[6][6] = {
        {0, 1, 3, 6, 8, 9},   // Ngày Tý / Ngọ
        {2, 3, 5, 8, 10, 11}, // Ngày Sửu / Mùi
        {0, 1, 4, 5, 7, 10},  // Ngày Dần / Thân
        {0, 2, 3, 6, 7, 9},   // Ngày Mão / Dậu
        {2, 4, 5, 8, 9, 11},  // Ngày Thìn / Tuất
        {1, 4, 6, 7, 10, 11}, // Ngày Tỵ / Hợi
    };

    const FullLunarInfo ln = solar_to_lunar_full(day, mon, year);

    const int can_nam = (ln.year + 6) % 10;
    const int chi_nam = (ln.year + 8) % 12;
    const int can_thang_gieng = ((can_nam % 5) * 2 + 2) % 10;
    const int can_thang = (can_thang_gieng + (ln.month - 1)) % 10;
    const int chi_thang = (ln.month + 1) % 12;
    const int can_ngay  = (ln.jdn + 9) % 10;
    const int chi_ngay  = (ln.jdn + 1) % 12;

    std::string gio_hd;
    const int row = chi_ngay % 6;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) gio_hd += ", ";
        gio_hd += kGioRange[kHoangDaoTable[row][i]];
    }

    char gio_buf[16];
    snprintf(gio_buf, sizeof(gio_buf), "%02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    char duong_buf[48];
    snprintf(duong_buf, sizeof(duong_buf), "%s, %02d/%02d/%04d",
             kThu[tm_info.tm_wday % 7], day, mon, year);

    char am_buf[64];
    snprintf(am_buf, sizeof(am_buf), "Ngày %d tháng %d%s năm %s %s (%d)",
             ln.day, ln.month, ln.leap ? " (Nhuận)" : "",
             kCan[can_nam], kChi[chi_nam], ln.year);

    char canchi_buf[64];
    snprintf(canchi_buf, sizeof(canchi_buf), "Ngày %s %s, tháng %s %s",
             kCan[can_ngay], kChi[chi_ngay], kCan[can_thang], kChi[chi_thang]);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "gio_hien_tai", gio_buf);
    cJSON_AddStringToObject(out, "duong_lich", duong_buf);
    cJSON_AddStringToObject(out, "am_lich", am_buf);
    cJSON_AddStringToObject(out, "can_chi", canchi_buf);
    cJSON_AddStringToObject(out, "gio_hoang_dao", gio_hd.c_str());
    return cjson_to_string_and_free(out);
}

std::string McpRegistry::tool_xem_thoi_gian_lich_am(cJSON* /*args*/) {
    return get_vietnamese_time_lunar_json();
}

// ---------------------------------------------------------------------------
// 3. doc_tin_tuc(chu_de, so_luong)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_doc_tin_tuc(cJSON* args) {
    const cJSON* j_topic = cJSON_GetObjectItem(args, "chu_de");
    const cJSON* j_cnt   = cJSON_GetObjectItem(args, "so_luong");

    std::string topic = fold_vn_lower(cJSON_IsString(j_topic) ? j_topic->valuestring : "thoi_su");
    int count = cJSON_IsNumber(j_cnt) ? j_cnt->valueint : 3;
    count = std::max(1, std::min(6, count));

    const char* rss_url = "https://vnexpress.net/rss/tin-moi-nhat.rss";
    const char* topic_label = "Tin mới nhất";
    if (topic.find("thoi_su") != std::string::npos || topic.find("thoi su") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/thoi-su.rss";
        topic_label = "Thời sự";
    } else if (topic.find("the_gioi") != std::string::npos || topic.find("the gioi") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/the-gioi.rss";
        topic_label = "Thế giới";
    } else if (topic.find("cong_nghe") != std::string::npos || topic.find("cong nghe") != std::string::npos ||
               topic.find("so hoa") != std::string::npos || topic.find("khoa hoc") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/so-hoa.rss";
        topic_label = "Công nghệ";
    } else if (topic.find("kinh_doanh") != std::string::npos || topic.find("kinh doanh") != std::string::npos ||
               topic.find("kinh te") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/kinh-doanh.rss";
        topic_label = "Kinh doanh";
    } else if (topic.find("the_thao") != std::string::npos || topic.find("the thao") != std::string::npos ||
               topic.find("bong da") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/the-thao.rss";
        topic_label = "Thể thao";
    } else if (topic.find("giai_tri") != std::string::npos || topic.find("giai tri") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/giai-tri.rss";
        topic_label = "Giải trí";
    } else if (topic.find("suc_khoe") != std::string::npos || topic.find("suc khoe") != std::string::npos) {
        rss_url = "https://vnexpress.net/rss/suc-khoe.rss";
        topic_label = "Sức khỏe";
    }

    std::string xml;
    int status = 0;
    if (http_get_text(rss_url, xml, &status, 24576) != ESP_OK || status != 200) {
        return make_error_json("Không tải được luồng RSS VNExpress");
    }

    auto extract_tag = [](const std::string& src, size_t start, size_t end, const char* tag) -> std::string {
        std::string open_t = std::string("<") + tag + ">";
        std::string close_t = std::string("</") + tag + ">";
        size_t a = src.find(open_t, start);
        if (a == std::string::npos || a >= end) return "";
        a += open_t.size();
        size_t b = src.find(close_t, a);
        if (b == std::string::npos || b > end) return "";
        return strip_html_and_cdata(src.substr(a, b - a));
    };

    cJSON* items_arr = cJSON_CreateArray();
    size_t cursor = 0;
    int extracted = 0;

    while (extracted < count) {
        size_t item_a = xml.find("<item>", cursor);
        if (item_a == std::string::npos) break;
        size_t item_b = xml.find("</item>", item_a);
        if (item_b == std::string::npos) break;

        std::string title = extract_tag(xml, item_a, item_b, "title");
        std::string desc  = extract_tag(xml, item_a, item_b, "description");
        if (!title.empty()) {
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "tieu_de", title.c_str());
            if (desc.size() > 180) {
                desc = desc.substr(0, 180) + "...";
            }
            cJSON_AddStringToObject(obj, "tom_tat", desc.c_str());
            cJSON_AddItemToArray(items_arr, obj);
            extracted++;
        }
        cursor = item_b + 7;
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", extracted > 0);
    cJSON_AddStringToObject(out, "nguon", "VNExpress");
    cJSON_AddStringToObject(out, "chu_de", topic_label);
    cJSON_AddItemToObject(out, "danh_sach_tin", items_arr);
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 4. tra_cuu_thong_tin(tu_khoa) — Wikipedia Tiếng Việt REST API
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_tra_cuu_thong_tin(cJSON* args) {
    const cJSON* j_kw = cJSON_GetObjectItem(args, "tu_khoa");
    if (!cJSON_IsString(j_kw) || !j_kw->valuestring || !j_kw->valuestring[0]) {
        return make_error_json("Thiếu từ khóa cần tra cứu");
    }

    std::string kw = j_kw->valuestring;
    std::string enc_title = url_encode(kw.c_str(), true);
    std::string url = "https://vi.wikipedia.org/api/rest_v1/page/summary/" + enc_title;

    std::string resp;
    int status = 0;
    static constexpr const char* kWikiUA = "XiaozhiZClawVN/1.0 (https://github.com/espressif/esp-idf)";
    http_get_text(url.c_str(), resp, &status, 16384, kWikiUA);

    // Nếu không khớp chính xác tiêu đề -> thử opensearch để lấy tiêu đề đúng nhất
    if (status != 200) {
        std::string search_url =
            "https://vi.wikipedia.org/w/api.php?action=opensearch&limit=1&namespace=0&format=json&search=" +
            url_encode(kw.c_str(), false);
        std::string sresp;
        int sstatus = 0;
        if (http_get_text(search_url.c_str(), sresp, &sstatus, 8192, kWikiUA) == ESP_OK && sstatus == 200) {
            cJSON* sj = cJSON_Parse(sresp.c_str());
            if (sj && cJSON_IsArray(sj)) {
                cJSON* hits = cJSON_GetArrayItem(sj, 1);
                cJSON* first_hit = cJSON_IsArray(hits) ? cJSON_GetArrayItem(hits, 0) : nullptr;
                if (cJSON_IsString(first_hit) && first_hit->valuestring && first_hit->valuestring[0]) {
                    std::string exact_url = "https://vi.wikipedia.org/api/rest_v1/page/summary/" +
                                            url_encode(first_hit->valuestring, true);
                    http_get_text(exact_url.c_str(), resp, &status, 16384, kWikiUA);
                }
                cJSON_Delete(sj);
            }
        }
    }

    if (status != 200 || resp.empty()) {
        return make_error_json("Không tìm thấy mục phù hợp trên Wikipedia Tiếng Việt");
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) {
        return make_error_json("Lỗi phân tích dữ liệu Wikipedia");
    }

    const cJSON* jtitle   = cJSON_GetObjectItem(root, "title");
    const cJSON* jdesc    = cJSON_GetObjectItem(root, "description");
    const cJSON* jextract = cJSON_GetObjectItem(root, "extract");

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "tieu_de", cJSON_IsString(jtitle) ? jtitle->valuestring : kw.c_str());
    if (cJSON_IsString(jdesc)) {
        cJSON_AddStringToObject(out, "mo_ta_ngan", jdesc->valuestring);
    }
    std::string extract = cJSON_IsString(jextract) ? jextract->valuestring : "";
    if (extract.size() > 900) {
        extract = extract.substr(0, 900) + "...";
    }
    cJSON_AddStringToObject(out, "tom_tat", extract.c_str());
    cJSON_Delete(root);
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 5. phat_nhac_radio(ten_bai_hat_hoac_kenh)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_phat_nhac_radio(cJSON* args) {
    const cJSON* j_req = cJSON_GetObjectItem(args, "ten_bai_hat_hoac_kenh");
    const char* raw = (cJSON_IsString(j_req) && j_req->valuestring) ? j_req->valuestring : "vov1";

    struct RadioChannel {
        const char* key;
        const char* label;
        const char* stream_url;
    };
    static constexpr RadioChannel kChannels[] = {
        {"vov giao thong hcm", "VOV Giao thông TP.HCM", "http://media.kythuatvov.vn:7008/;stream/1"},
        {"vov giao thong",     "VOV Giao thông Hà Nội", "http://media.kythuatvov.vn:7006/;stream/1"},
        {"vov1",               "VOV1 Thời sự",          "http://media.kythuatvov.vn:7000/;stream/1"},
        {"vov2",               "VOV2 Văn hóa Xã hội",   "http://media.kythuatvov.vn:7002/;stream/1"},
        {"vov3",               "VOV3 Âm nhạc Giải trí", "http://media.kythuatvov.vn:7004/;stream/1"},
        {"nhac",               "VOV3 Âm nhạc",          "http://media.kythuatvov.vn:7004/;stream/1"},
    };

    std::string chosen_url;
    std::string chosen_name;

    if (strncasecmp(raw, "http://", 7) == 0 || strncasecmp(raw, "https://", 8) == 0) {
        chosen_url  = raw;
        chosen_name = "Luồng MP3 trực tuyến";
    } else {
        std::string folded = fold_vn_lower(raw);
        for (const auto& ch : kChannels) {
            if (folded.find(ch.key) != std::string::npos) {
                chosen_url  = ch.stream_url;
                chosen_name = ch.label;
                break;
            }
        }
        if (chosen_url.empty()) {
            chosen_url  = kChannels[4].stream_url; // Mặc định VOV3
            chosen_name = std::string("VOV3 Âm nhạc (theo yêu cầu: ") + raw + ")";
        }
    }

    // Xếp hàng phát ngay sau khi Bot nói xong câu xác nhận để tránh tranh I2S với TTS
    ZClawLlmClient::instance().schedule_radio_playback(chosen_url.c_str(), chosen_name.c_str());

    ChronosNotification notif = {};
    snprintf(notif.app_name, sizeof(notif.app_name), "Radio / Nhạc");
    snprintf(notif.title, sizeof(notif.title), "%s", chosen_name.c_str());
    snprintf(notif.body, sizeof(notif.body), "Nhấn nút chính để dừng phát");
    notif.unread = true;
    UiEyesClock::instance().show_notification(notif);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "kenh", chosen_name.c_str());
    cJSON_AddStringToObject(out, "url", chosen_url.c_str());
    cJSON_AddStringToObject(out, "ghi_chu", "Đã lên lịch phát luồng MP3 ngay sau khi bot trả lời. Nhấn nút chính để dừng.");
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 6. mo_video(tu_khoa_hoac_url)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_mo_video(cJSON* args) {
    const cJSON* j_val = cJSON_GetObjectItem(args, "tu_khoa_hoac_url");
    if (!cJSON_IsString(j_val) || !j_val->valuestring || !j_val->valuestring[0]) {
        return make_error_json("Thiếu từ khóa hoặc URL video");
    }

    const char* raw = j_val->valuestring;
    std::string target_url;
    if (strncasecmp(raw, "http://", 7) == 0 || strncasecmp(raw, "https://", 8) == 0) {
        target_url = raw;
    } else {
        target_url = "https://www.youtube.com/results?search_query=" + url_encode(raw, false);
    }

    // Hiển thị tiêu đề lên màn hình
    ChronosNotification notif = {};
    snprintf(notif.app_name, sizeof(notif.app_name), "Mở Video");
    snprintf(notif.title, sizeof(notif.title), "%s", raw);
    snprintf(notif.body, sizeof(notif.body), "%s", target_url.c_str());
    notif.unread = true;
    UiEyesClock::instance().show_notification(notif);

    // Đẩy qua BLE Chronos (gói 0xD2 OPEN_URL) nếu điện thoại đang kết nối
    bool sent_ble = false;
    if (ChronosBleService::instance().is_connected()) {
        std::string pkt;
        pkt.reserve(5 + target_url.size());
        const uint16_t plen = static_cast<uint16_t>(target_url.size() + 1);
        const uint8_t hdr[5] = {
            0xAB, 0x00,
            static_cast<uint8_t>((plen >> 8) & 0xFF),
            static_cast<uint8_t>(plen & 0xFF),
            0xD2 // Custom ZClaw OPEN_URL command
        };
        pkt.assign(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        pkt.append(target_url);
        sent_ble = (ChronosBleService::instance().send_response(
                        reinterpret_cast<const uint8_t*>(pkt.data()), pkt.size()) == ESP_OK);
    }

    // Đồng thời gửi broadcast ESP-NOW tới các node ngoại vi
    char now_msg[200];
    snprintf(now_msg, sizeof(now_msg), "VIDEO:%s", target_url.c_str());
    const bool sent_espnow =
        (EspNowMeshManager::instance().is_initialized() &&
         EspNowMeshManager::instance().broadcast(EspNowCmd::TEXT, now_msg) == ESP_OK);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "url", target_url.c_str());
    cJSON_AddBoolToObject(out, "da_gui_ble_chronos", sent_ble);
    cJSON_AddBoolToObject(out, "da_gui_esp_now", sent_espnow);
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 7. suy_luan_cao(van_de_phuc_tap)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_suy_luan_cao(cJSON* args) {
    const cJSON* j_prob = cJSON_GetObjectItem(args, "van_de_phuc_tap");
    if (!cJSON_IsString(j_prob) || !j_prob->valuestring || !j_prob->valuestring[0]) {
        return make_error_json("Thiếu nội dung vấn đề cần suy luận sâu");
    }

    UiEyesClock::instance().set_emotion(EyeEmotion::SUY_NGHI);
    ZClawLlmClient::instance().set_reasoning_mode(true);

    std::string analysis;
    esp_err_t ret = ZClawLlmClient::instance().run_deep_reasoning(j_prob->valuestring, analysis);
    if (ret != ESP_OK || analysis.empty()) {
        return make_error_json("Không thực hiện được lượt suy luận sâu trên LLM");
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "ket_qua_suy_luan", analysis.c_str());
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 8. dieu_khien_thiet_bi(lenh, gia_tri)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_dieu_khien_thiet_bi(cJSON* args) {
    const cJSON* j_cmd = cJSON_GetObjectItem(args, "lenh");
    const cJSON* j_val = cJSON_GetObjectItem(args, "gia_tri");
    if (!cJSON_IsString(j_cmd) || !j_cmd->valuestring) {
        return make_error_json("Thiếu lệnh điều khiển");
    }

    std::string cmd = fold_vn_lower(j_cmd->valuestring);
    const int val   = cJSON_IsNumber(j_val) ? j_val->valueint : 0;

    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "lenh", cmd.c_str());

    if (cmd == "am_luong" || cmd == "volume") {
        uint8_t v = static_cast<uint8_t>(std::max(0, std::min(100, val)));
        AudioHal::instance().set_speaker_volume(v, true);
        UiEyesClock::instance().show_volume_osd(v);
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddNumberToObject(out, "gia_tri_moi", v);
    } else if (cmd == "tat_tieng" || cmd == "mute") {
        bool m = (val != 0);
        AudioHal::instance().set_muted(m);
        UiEyesClock::instance().show_volume_osd(
            m ? 0 : AudioHal::instance().get_speaker_volume());
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddBoolToObject(out, "tat_tieng", m);
    } else if (cmd == "do_nhay_mic" || cmd == "mic_gain") {
        uint8_t lvl = static_cast<uint8_t>(std::max(1, std::min(10, val)));
        AudioHal::instance().set_mic_sensitivity_level(lvl, true);
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddNumberToObject(out, "muc_nhay_mic", lvl);
    } else if (cmd == "do_sang" || cmd == "brightness") {
        uint8_t b = static_cast<uint8_t>(std::max(5, std::min(100, val)));
        UiEyesClock::instance().set_backlight(b, true);
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddNumberToObject(out, "do_sang_moi", b);
    } else if (cmd == "xoay_man_hinh" || cmd == "rotation") {
        uint16_t deg = static_cast<uint16_t>(((val % 360) + 360) % 360);
        esp_err_t err = UiEyesClock::instance().set_rotation(deg, true);
        cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
        cJSON_AddNumberToObject(out, "goc_xoay", UiEyesClock::instance().get_rotation());
    } else if (cmd == "man_hinh" || cmd == "screen") {
        if (val == 1) {
            UiEyesClock::instance().switch_to_clock_screen();
        } else {
            UiEyesClock::instance().switch_to_chat_screen();
        }
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "che_do", val == 1 ? "dong_ho" : "mat_chatbot");
    } else {
        cJSON_Delete(out);
        return make_error_json("Lệnh điều khiển thiết bị không được hỗ trợ");
    }

    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 9. gui_lenh_esp_now(mac_dich, noi_dung_lenh)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_gui_lenh_esp_now(cJSON* args) {
    const cJSON* j_mac = cJSON_GetObjectItem(args, "mac_dich");
    const cJSON* j_msg = cJSON_GetObjectItem(args, "noi_dung_lenh");

    const char* mac_str = (cJSON_IsString(j_mac) && j_mac->valuestring) ? j_mac->valuestring : "broadcast";
    const char* msg_str = (cJSON_IsString(j_msg) && j_msg->valuestring) ? j_msg->valuestring : "";

    uint8_t mac[6] = {};
    if (!EspNowMeshManager::parse_mac_str(mac_str, mac)) {
        return make_error_json("Địa chỉ MAC đích không hợp lệ (dùng dạng AA:BB:CC:DD:EE:FF hoặc 'broadcast')");
    }

    EspNowCmd cmd = EspNowCmd::TEXT;
    const char* payload = msg_str;
    if (strncasecmp(msg_str, "ALERT:", 6) == 0) {
        cmd = EspNowCmd::ALERT;
        payload = msg_str + 6;
    } else if (strncasecmp(msg_str, "CONTROL:", 8) == 0) {
        cmd = EspNowCmd::CONTROL;
        payload = msg_str + 8;
    } else if (strncasecmp(msg_str, "VOL:", 4) == 0 ||
               strncasecmp(msg_str, "MUTE:", 5) == 0 ||
               strncasecmp(msg_str, "EMOTION:", 8) == 0) {
        cmd = EspNowCmd::CONTROL;
    } else if (strncasecmp(msg_str, "STATUS:", 7) == 0) {
        cmd = EspNowCmd::STATUS;
        payload = msg_str + 7;
    }

    esp_err_t ret = EspNowMeshManager::instance().send_command(mac, cmd, payload);
    if (ret != ESP_OK) {
        return make_error_json("Gửi gói tin ESP-NOW thất bại");
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "mac_dich", mac_str);
    cJSON_AddStringToObject(out, "noi_dung", payload);
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 10. hen_gio_pomodoro(so_phut, loi_nhac)
// ---------------------------------------------------------------------------
void McpRegistry::pomodoro_timer_cb(void* arg) {
    auto* self = static_cast<McpRegistry*>(arg);
    if (!self || !self->pomodoro_queue_ || !self->pomodoro_active_.exchange(false)) return;

    PomodoroEvent ev = {};
    snprintf(ev.reminder, sizeof(ev.reminder), "%s", self->pomodoro_msg_);
    xQueueSend(self->pomodoro_queue_, &ev, 0);
}

void McpRegistry::pomodoro_worker_task(void* arg) {
    auto* self = static_cast<McpRegistry*>(arg);
    PomodoroEvent ev = {};

    while (self->worker_running_.load()) {
        if (xQueueReceive(self->pomodoro_queue_, &ev, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (!self->worker_running_.load()) break;
            if (ev.reminder[0] == '\0') continue;

            ESP_LOGI(TAG, "Hết giờ Pomodoro: %s", ev.reminder);
            UiEyesClock::instance().switch_to_chat_screen();
            UiEyesClock::instance().set_emotion(EyeEmotion::NGAC_NHIEN);

            ChronosNotification notif = {};
            snprintf(notif.app_name, sizeof(notif.app_name), "Pomodoro");
            snprintf(notif.title, sizeof(notif.title), "Hết giờ rồi!");
            snprintf(notif.body, sizeof(notif.body), "%s", ev.reminder);
            notif.unread = true;
            UiEyesClock::instance().show_notification(notif);

            AudioHal::instance().play_chime(AudioHal::ChimeType::POMODORO);
            UiEyesClock::instance().set_emotion(EyeEmotion::VUI_VE);

            std::string tts_msg = std::string("Đã hết giờ hẹn rồi! ") + ev.reminder;
            ZClawLlmClient::instance().speak_vietnamese(tts_msg);
            UiEyesClock::instance().set_emotion(EyeEmotion::BINH_THUONG);
        }
    }

    self->pomodoro_task_ = nullptr;
    vTaskDelete(nullptr);
}

std::string McpRegistry::tool_hen_gio_pomodoro(cJSON* args) {
    const cJSON* j_min = cJSON_GetObjectItem(args, "so_phut");
    const cJSON* j_msg = cJSON_GetObjectItem(args, "loi_nhac");

    int mins = cJSON_IsNumber(j_min) ? j_min->valueint : 25;
    if (!pomodoro_timer_) {
        return make_error_json("Timer Pomodoro chưa được khởi tạo");
    }

    esp_timer_stop(pomodoro_timer_);

    if (mins <= 0) {
        pomodoro_active_.store(false);
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "trang_thai", "Đã hủy hẹn giờ Pomodoro");
        return cjson_to_string_and_free(out);
    }

    mins = std::min(180, mins);
    const char* reminder = (cJSON_IsString(j_msg) && j_msg->valuestring && j_msg->valuestring[0])
                               ? j_msg->valuestring
                               : "Hãy nghỉ ngơi thư giãn một chút nhé!";
    snprintf(pomodoro_msg_, sizeof(pomodoro_msg_), "%s", reminder);
    pomodoro_active_.store(true);

    const uint64_t us = static_cast<uint64_t>(mins) * 60ULL * 1000000ULL;
    esp_err_t ret = esp_timer_start_once(pomodoro_timer_, us);
    if (ret != ESP_OK) {
        pomodoro_active_.store(false);
        return make_error_json("Không thể bắt đầu bộ đếm thời gian Pomodoro");
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "so_phut", mins);
    cJSON_AddStringToObject(out, "loi_nhac", pomodoro_msg_);
    return cjson_to_string_and_free(out);
}

} // namespace zclaw
