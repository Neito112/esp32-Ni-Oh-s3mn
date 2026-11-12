// mcp_vietnam_tools.cpp
// S.W.Ni-Oh — Vietnamese Native MCP (10 công cụ 100% Tiếng Việt)
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
#include <vector>

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
                        const char* user_agent = "SW-NiOh/1.0",
                        const char* extra_hdr_name = nullptr,
                        const char* extra_hdr_val = nullptr) {
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
    if (extra_hdr_name && extra_hdr_val && extra_hdr_name[0] && extra_hdr_val[0]) {
        esp_http_client_set_header(client, extra_hdr_name, extra_hdr_val);
    }

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

esp_err_t http_post_json(const char* url,
                         const char* json_body,
                         std::string& out_body,
                         int* out_status = nullptr,
                         const char* hdr_name = nullptr,
                         const char* hdr_val = nullptr,
                         size_t max_bytes = 24576) {
    out_body.clear();
    if (!url || !json_body) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_POST;
    cfg.timeout_ms        = 12000;
    cfg.buffer_size       = 2048;
    cfg.buffer_size_tx    = 1024;
    cfg.user_agent        = "SW-NiOh/1.0";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (hdr_name && hdr_val && hdr_name[0] && hdr_val[0]) {
        esp_http_client_set_header(client, hdr_name, hdr_val);
    }

    const int blen = static_cast<int>(strlen(json_body));
    esp_err_t err = esp_http_client_open(client, blen);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (esp_http_client_write(client, json_body, blen) != blen ||
        esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (out_status) *out_status = esp_http_client_get_status_code(client);

    char* chunk_buf = static_cast<char*>(board_malloc_prefer_spiram(2048));
    if (chunk_buf) {
        while (out_body.size() < max_bytes) {
            int r = esp_http_client_read(client, chunk_buf, 2048);
            if (r <= 0) break;
            out_body.append(chunk_buf, static_cast<size_t>(r));
        }
        board_free(chunk_buf);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
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

    // Đăng ký provider & executor sang nioh_agent (tách rời dependency vòng)
    ZClawLlmClient::instance().register_mcp_tools(
        [this]() -> cJSON* { return this->build_tools_schema_json(); },
        [this](const char* name, const char* args_json) -> std::string {
            return this->execute_tool(name, args_json);
        });
    ZClawLlmClient::instance().register_chronos_status_provider([]() -> std::string {
        const ChronosSyncStatus st = ChronosBleService::instance().get_sync_status();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "connected", st.connected);
        cJSON_AddNumberToObject(j, "phone_battery_pct", st.phone_battery_pct);
        cJSON_AddNumberToObject(j, "rx_packets", st.rx_packets);
        cJSON_AddStringToObject(j, "last_time_sync", st.last_time_sync);
        cJSON_AddBoolToObject(j, "weather_valid", st.last_weather.valid);
        cJSON_AddNumberToObject(j, "temp_c", static_cast<double>(st.last_weather.temp_c));
        cJSON_AddStringToObject(j, "condition", st.last_weather.condition);
        cJSON_AddStringToObject(j, "notif_title", st.last_notif.title);
        cJSON_AddStringToObject(j, "notif_app", st.last_notif.app_name);
        cJSON_AddBoolToObject(j, "nav_active", st.last_nav.active);
        cJSON_AddStringToObject(j, "nav_dir", st.last_nav.direction);
        cJSON_AddNumberToObject(j, "nav_dist", static_cast<double>(st.last_nav.distance_m));
        cJSON_AddStringToObject(j, "nav_street", st.last_nav.street_name);
        char* s = cJSON_PrintUnformatted(j);
        std::string out = s ? s : "{}";
        if (s) cJSON_free(s);
        cJSON_Delete(j);
        return out;
    });

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
        add_str_prop(p, "tu_khoa", "Từ khóa, sự kiện, nhân vật, địa danh hoặc câu hỏi cần tìm kiếm trên Web / Wikipedia");
        add_tool("tra_cuu_thong_tin",
                 "Tìm kiếm thông tin trên Web (DuckDuckGo / Tavily / Brave / Serper / SearXNG) và Wikipedia Tiếng Việt theo cấu hình Dashboard",
                 p, make_req({"tu_khoa"}));
    }

    // 5. phat_nhac_truc_tuyen
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "tu_khoa_hoac_url", "Tên bài hát, tên ca sĩ, link YouTube/SoundCloud/MP3 (hoặc kênh VOV chỉ khi người dùng nói rõ Radio/VOV)");
        add_str_prop(p, "nen_tang", "Mặc định chọn \"youtube\" hoặc \"soundcloud\". Chỉ chọn \"radio\" khi người dùng nói rõ đài Radio/VOV.");
        add_tool("phat_nhac_truc_tuyen",
                 "Phát nhạc trực tuyến YouTube hoặc SoundCloud trực tiếp ra loa (hoặc Radio VOV khi được yêu cầu rõ ràng)",
                 p, make_req({"tu_khoa_hoac_url"}));
    }

    // 6. xem_video_truc_tuyen
    {
        cJSON* p = cJSON_CreateObject();
        add_str_prop(p, "tu_khoa_hoac_url", "Từ khóa tìm kiếm video YouTube hoặc đường dẫn URL luồng video MJPEG trực tiếp");
        add_str_prop(p, "do_phan_giai", "Độ phân giải tối ưu cho màn ST7789: \"240x240\" hoặc \"240x180\" (mặc định \"240x240\")");
        add_tool("xem_video_truc_tuyen",
                 "Phát và hiển thị Video MJPEG trực tuyến (240x240 / 240x180, 12-15 FPS) ngay trên màn hình ST7789 của mạch",
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
    } else if (strcmp(tool_name, "phat_nhac_truc_tuyen") == 0 || strcmp(tool_name, "phat_nhac_radio") == 0) {
        result = tool_phat_nhac_truc_tuyen(args);
    } else if (strcmp(tool_name, "xem_video_truc_tuyen") == 0 || strcmp(tool_name, "mo_video") == 0) {
        result = tool_xem_video_truc_tuyen(args);
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

    char am_buf[96];
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
// 4. tra_cuu_thong_tin(tu_khoa) — Web Search (DuckDuckGo / Tavily / Brave / Serper / SearXNG) + Wikipedia VI
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_tra_cuu_thong_tin(cJSON* args) {
    const cJSON* j_kw = cJSON_GetObjectItem(args, "tu_khoa");
    if (!cJSON_IsString(j_kw) || !j_kw->valuestring || !j_kw->valuestring[0]) {
        return make_error_json("Thiếu từ khóa cần tra cứu");
    }

    const ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    const std::string kw  = j_kw->valuestring;
    const std::string eng = cfg.search_engine[0] ? cfg.search_engine : "ddg_wiki";

    // 1) Tavily AI Search
    if (eng == "tavily" && cfg.search_api_key[0] != '\0') {
        cJSON* req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "api_key", cfg.search_api_key);
        cJSON_AddStringToObject(req, "query", kw.c_str());
        cJSON_AddNumberToObject(req, "max_results", 3);
        cJSON_AddBoolToObject(req, "include_answer", true);
        char* bstr = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        std::string resp;
        int st = 0;
        if (bstr && http_post_json("https://api.tavily.com/search", bstr, resp, &st) == ESP_OK && st == 200) {
            cJSON_free(bstr);
            cJSON* root = cJSON_Parse(resp.c_str());
            if (root) {
                cJSON* out = cJSON_CreateObject();
                cJSON_AddBoolToObject(out, "ok", true);
                cJSON_AddStringToObject(out, "cong_cu", "Tavily AI Search");
                cJSON* jans = cJSON_GetObjectItemCaseSensitive(root, "answer");
                if (cJSON_IsString(jans) && jans->valuestring) {
                    cJSON_AddStringToObject(out, "tom_tat", jans->valuestring);
                }
                cJSON_AddItemToObject(out, "ket_qua", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(root, "results"), true));
                cJSON_Delete(root);
                return cjson_to_string_and_free(out);
            }
        } else if (bstr) {
            cJSON_free(bstr);
        }
    }

    // 2) Brave Search API
    if (eng == "brave" && cfg.search_api_key[0] != '\0') {
        std::string url = "https://api.search.brave.com/res/v1/web/search?count=3&q=" + url_encode(kw.c_str(), false);
        std::string resp;
        int st = 0;
        if (http_get_text(url.c_str(), resp, &st, 24576, "SW-NiOh/1.0", "X-Subscription-Token", cfg.search_api_key) == ESP_OK && st == 200) {
            cJSON* root = cJSON_Parse(resp.c_str());
            if (root) {
                cJSON* web = cJSON_GetObjectItemCaseSensitive(root, "web");
                cJSON* res = web ? cJSON_GetObjectItemCaseSensitive(web, "results") : nullptr;
                cJSON* arr = cJSON_CreateArray();
                if (cJSON_IsArray(res)) {
                    const int n = std::min<int>(cJSON_GetArraySize(res), 3);
                    for (int i = 0; i < n; ++i) {
                        cJSON* r0 = cJSON_GetArrayItem(res, i);
                        cJSON* item = cJSON_CreateObject();
                        cJSON* jt = cJSON_GetObjectItemCaseSensitive(r0, "title");
                        cJSON* jd = cJSON_GetObjectItemCaseSensitive(r0, "description");
                        if (cJSON_IsString(jt)) cJSON_AddStringToObject(item, "tieu_de", jt->valuestring);
                        if (cJSON_IsString(jd)) cJSON_AddStringToObject(item, "mo_ta", jd->valuestring);
                        cJSON_AddItemToArray(arr, item);
                    }
                }
                cJSON_Delete(root);
                cJSON* out = cJSON_CreateObject();
                cJSON_AddBoolToObject(out, "ok", true);
                cJSON_AddStringToObject(out, "cong_cu", "Brave Search");
                cJSON_AddItemToObject(out, "ket_qua", arr);
                return cjson_to_string_and_free(out);
            }
        }
    }

    // 3) Serper.dev Google Search
    if (eng == "serper" && cfg.search_api_key[0] != '\0') {
        cJSON* req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "q", kw.c_str());
        cJSON_AddStringToObject(req, "gl", "vn");
        cJSON_AddStringToObject(req, "hl", "vi");
        char* bstr = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        std::string resp;
        int st = 0;
        if (bstr && http_post_json("https://google.serper.dev/search", bstr, resp, &st, "X-API-KEY", cfg.search_api_key) == ESP_OK && st == 200) {
            cJSON_free(bstr);
            cJSON* root = cJSON_Parse(resp.c_str());
            if (root) {
                cJSON* org = cJSON_GetObjectItemCaseSensitive(root, "organic");
                cJSON* arr = cJSON_CreateArray();
                if (cJSON_IsArray(org)) {
                    const int n = std::min<int>(cJSON_GetArraySize(org), 3);
                    for (int i = 0; i < n; ++i) {
                        cJSON* r0 = cJSON_GetArrayItem(org, i);
                        cJSON* item = cJSON_CreateObject();
                        cJSON* jt = cJSON_GetObjectItemCaseSensitive(r0, "title");
                        cJSON* js = cJSON_GetObjectItemCaseSensitive(r0, "snippet");
                        if (cJSON_IsString(jt)) cJSON_AddStringToObject(item, "tieu_de", jt->valuestring);
                        if (cJSON_IsString(js)) cJSON_AddStringToObject(item, "mo_ta", js->valuestring);
                        cJSON_AddItemToArray(arr, item);
                    }
                }
                cJSON_Delete(root);
                cJSON* out = cJSON_CreateObject();
                cJSON_AddBoolToObject(out, "ok", true);
                cJSON_AddStringToObject(out, "cong_cu", "Serper Google Search");
                cJSON_AddItemToObject(out, "ket_qua", arr);
                return cjson_to_string_and_free(out);
            }
        } else if (bstr) {
            cJSON_free(bstr);
        }
    }

    // 4) SearXNG Custom Instance
    if (eng == "searxng" && cfg.searxng_url[0] != '\0') {
        std::string base = cfg.searxng_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string surl = base + "/search?format=json&language=vi&q=" + url_encode(kw.c_str(), false);
        std::string resp;
        int st = 0;
        if (http_get_text(surl.c_str(), resp, &st, 24576) == ESP_OK && st == 200) {
            cJSON* root = cJSON_Parse(resp.c_str());
            if (root) {
                cJSON* res = cJSON_GetObjectItemCaseSensitive(root, "results");
                cJSON* arr = cJSON_CreateArray();
                if (cJSON_IsArray(res)) {
                    const int n = std::min<int>(cJSON_GetArraySize(res), 3);
                    for (int i = 0; i < n; ++i) {
                        cJSON* r0 = cJSON_GetArrayItem(res, i);
                        cJSON* item = cJSON_CreateObject();
                        cJSON* jt = cJSON_GetObjectItemCaseSensitive(r0, "title");
                        cJSON* jc = cJSON_GetObjectItemCaseSensitive(r0, "content");
                        if (cJSON_IsString(jt)) cJSON_AddStringToObject(item, "tieu_de", jt->valuestring);
                        if (cJSON_IsString(jc)) cJSON_AddStringToObject(item, "mo_ta", jc->valuestring);
                        cJSON_AddItemToArray(arr, item);
                    }
                }
                cJSON_Delete(root);
                cJSON* out = cJSON_CreateObject();
                cJSON_AddBoolToObject(out, "ok", true);
                cJSON_AddStringToObject(out, "cong_cu", "SearXNG");
                cJSON_AddItemToObject(out, "ket_qua", arr);
                return cjson_to_string_and_free(out);
            }
        }
    }

    // 5) Mặc định miễn phí: DuckDuckGo Instant Answer + Wikipedia Tiếng Việt
    std::string ddg_abstract;
    {
        std::string ddg_url = "https://api.duckduckgo.com/?format=json&no_html=1&skip_disambig=1&q=" +
                              url_encode(kw.c_str(), false);
        std::string dresp;
        int dst = 0;
        if (http_get_text(ddg_url.c_str(), dresp, &dst, 16384) == ESP_OK && dst == 200) {
            cJSON* dj = cJSON_Parse(dresp.c_str());
            if (dj) {
                cJSON* ja = cJSON_GetObjectItemCaseSensitive(dj, "AbstractText");
                if (cJSON_IsString(ja) && ja->valuestring && ja->valuestring[0]) {
                    ddg_abstract = ja->valuestring;
                }
                cJSON_Delete(dj);
            }
        }
    }

    std::string enc_title = url_encode(kw.c_str(), true);
    std::string url = "https://vi.wikipedia.org/api/rest_v1/page/summary/" + enc_title;

    std::string resp;
    int status = 0;
    static constexpr const char* kWikiUA = "SW-NiOh/1.0 (https://github.com/Neito112/esp32-Ni-Oh-s3mn)";
    http_get_text(url.c_str(), resp, &status, 16384, kWikiUA);

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

    if ((status != 200 || resp.empty()) && ddg_abstract.empty()) {
        return make_error_json("Không tìm thấy thông tin phù hợp trên Web/Wikipedia");
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "cong_cu", "DuckDuckGo + Wikipedia VI");
    if (!ddg_abstract.empty()) {
        cJSON_AddStringToObject(out, "duckduckgo", ddg_abstract.c_str());
    }
    if (status == 200 && !resp.empty()) {
        cJSON* root = cJSON_Parse(resp.c_str());
        if (root) {
            const cJSON* jtitle   = cJSON_GetObjectItem(root, "title");
            const cJSON* jdesc    = cJSON_GetObjectItem(root, "description");
            const cJSON* jextract = cJSON_GetObjectItem(root, "extract");
            cJSON_AddStringToObject(out, "tieu_de", cJSON_IsString(jtitle) ? jtitle->valuestring : kw.c_str());
            if (cJSON_IsString(jdesc)) cJSON_AddStringToObject(out, "mo_ta_ngan", jdesc->valuestring);
            std::string extract = cJSON_IsString(jextract) ? jextract->valuestring : "";
            if (extract.size() > 900) extract = extract.substr(0, 900) + "...";
            cJSON_AddStringToObject(out, "tom_tat", extract.c_str());
            cJSON_Delete(root);
        }
    }
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// Helpers cho YouTube (Piped API) & SoundCloud API v2
// ---------------------------------------------------------------------------
namespace {

std::string build_proxy_url(const char* base, const char* path, const char* query, const char* extra_qs = "") {
    std::string b = base ? base : "";
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b + path + "?q=" + url_encode(query, false) + extra_qs;
}

// Trích Video ID từ URL youtube.com/watch?v=..., youtu.be/..., youtube.com/shorts/...
std::string extract_youtube_id(const char* url) {
    if (!url) return "";
    const char* v = strstr(url, "v=");
    if (v) {
        v += 2;
        std::string id;
        while (*v && *v != '&' && *v != '#') id.push_back(*v++);
        return id;
    }
    const char* be = strstr(url, "youtu.be/");
    if (be) {
        be += 9;
        std::string id;
        while (*be && *be != '?' && *be != '&' && *be != '/') id.push_back(*be++);
        return id;
    }
    const char* sh = strstr(url, "/shorts/");
    if (sh) {
        sh += 8;
        std::string id;
        while (*sh && *sh != '?' && *sh != '&' && *sh != '/') id.push_back(*sh++);
        return id;
    }
    return "";
}

// Tự dò SoundCloud public client_id nếu người dùng chưa cấu hình tay
std::string fetch_soundcloud_client_id(const ZClawConfig& cfg) {
    if (cfg.soundcloud_client_id[0] != '\0') {
        return cfg.soundcloud_client_id;
    }
    std::string page;
    int st = 0;
    if (http_get_text("https://soundcloud.com", page, &st, 24576) != ESP_OK || st != 200) {
        return "";
    }
    // Quét script URL dạng https://a-v2.sndcdn.com/assets/...js
    size_t pos = 0;
    int checked = 0;
    while (checked < 4 && (pos = page.find("https://a-v2.sndcdn.com/assets/", pos)) != std::string::npos) {
        size_t end_q = page.find('"', pos);
        if (end_q == std::string::npos || end_q - pos > 160) break;
        std::string js_url = page.substr(pos, end_q - pos);
        pos = end_q;
        checked++;
        if (js_url.find(".js") == std::string::npos) continue;

        std::string js;
        int jst = 0;
        if (http_get_text(js_url.c_str(), js, &jst, 32768) == ESP_OK && jst == 200) {
            size_t cid_pos = js.find("client_id:\"");
            if (cid_pos == std::string::npos) cid_pos = js.find("client_id=\"");
            if (cid_pos != std::string::npos) {
                cid_pos += 11;
                size_t cid_end = js.find('"', cid_pos);
                if (cid_end != std::string::npos && cid_end - cid_pos >= 16 && cid_end - cid_pos <= 48) {
                    return js.substr(cid_pos, cid_end - cid_pos);
                }
            }
        }
    }
    return "";
}

// Tìm track SoundCloud & lấy URL MP3 progressive
bool resolve_soundcloud_mp3(const char* query_or_url,
                            const std::string& client_id,
                            std::string& out_mp3_url,
                            std::string& out_title,
                            std::string& out_artist,
                            int& out_duration_sec) {
    if (client_id.empty() || !query_or_url || !query_or_url[0]) return false;

    std::string api_resp;
    int status = 0;
    cJSON* track = nullptr;
    cJSON* root  = nullptr;

    if (strstr(query_or_url, "soundcloud.com") != nullptr) {
        std::string res_url = "https://api-v2.soundcloud.com/resolve?url=" +
                              url_encode(query_or_url, false) + "&client_id=" + client_id;
        if (http_get_text(res_url.c_str(), api_resp, &status, 24576) == ESP_OK && status == 200) {
            root = cJSON_Parse(api_resp.c_str());
            track = root;
        }
    } else {
        std::string search_url = "https://api-v2.soundcloud.com/search/tracks?q=" +
                                 url_encode(query_or_url, false) +
                                 "&client_id=" + client_id + "&limit=5";
        if (http_get_text(search_url.c_str(), api_resp, &status, 32768) == ESP_OK && status == 200) {
            root = cJSON_Parse(api_resp.c_str());
            if (root) {
                cJSON* col = cJSON_GetObjectItemCaseSensitive(root, "collection");
                if (cJSON_IsArray(col)) {
                    const int n = cJSON_GetArraySize(col);
                    for (int i = 0; i < n; ++i) {
                        cJSON* item = cJSON_GetArrayItem(col, i);
                        cJSON* media = cJSON_GetObjectItemCaseSensitive(item, "media");
                        cJSON* trs   = media ? cJSON_GetObjectItemCaseSensitive(media, "transcodings") : nullptr;
                        if (cJSON_IsArray(trs) && cJSON_GetArraySize(trs) > 0) {
                            track = item;
                            break;
                        }
                    }
                }
            }
        }
    }

    bool ok = false;
    if (track) {
        cJSON* jtitle = cJSON_GetObjectItemCaseSensitive(track, "title");
        cJSON* juser  = cJSON_GetObjectItemCaseSensitive(track, "user");
        cJSON* juname = juser ? cJSON_GetObjectItemCaseSensitive(juser, "username") : nullptr;
        cJSON* jdur   = cJSON_GetObjectItemCaseSensitive(track, "duration");
        if (cJSON_IsString(jtitle) && jtitle->valuestring) out_title = jtitle->valuestring;
        if (cJSON_IsString(juname) && juname->valuestring) out_artist = juname->valuestring;
        if (cJSON_IsNumber(jdur)) out_duration_sec = static_cast<int>(jdur->valuedouble / 1000.0);

        cJSON* media = cJSON_GetObjectItemCaseSensitive(track, "media");
        cJSON* trs   = media ? cJSON_GetObjectItemCaseSensitive(media, "transcodings") : nullptr;
        if (cJSON_IsArray(trs)) {
            const int cnt = cJSON_GetArraySize(trs);
            for (int i = 0; i < cnt; ++i) {
                cJSON* tc  = cJSON_GetArrayItem(trs, i);
                cJSON* fmt = cJSON_GetObjectItemCaseSensitive(tc, "format");
                cJSON* pro = fmt ? cJSON_GetObjectItemCaseSensitive(fmt, "protocol") : nullptr;
                cJSON* mim = fmt ? cJSON_GetObjectItemCaseSensitive(fmt, "mime_type") : nullptr;
                cJSON* tul = cJSON_GetObjectItemCaseSensitive(tc, "url");

                const bool is_prog = (cJSON_IsString(pro) && pro->valuestring &&
                                      strcmp(pro->valuestring, "progressive") == 0);
                const bool is_mp3  = (cJSON_IsString(mim) && mim->valuestring &&
                                      strstr(mim->valuestring, "mpeg") != nullptr);
                if (is_prog && is_mp3 && cJSON_IsString(tul) && tul->valuestring) {
                    std::string resolve_mp3 = std::string(tul->valuestring) + "?client_id=" + client_id;
                    std::string mp3_json;
                    int mstat = 0;
                    if (http_get_text(resolve_mp3.c_str(), mp3_json, &mstat, 4096) == ESP_OK && mstat == 200) {
                        cJSON* mj = cJSON_Parse(mp3_json.c_str());
                        if (mj) {
                            cJSON* jreal = cJSON_GetObjectItemCaseSensitive(mj, "url");
                            if (cJSON_IsString(jreal) && jreal->valuestring && jreal->valuestring[0]) {
                                out_mp3_url = jreal->valuestring;
                                ok = true;
                            }
                            cJSON_Delete(mj);
                        }
                    }
                    if (ok) break;
                }
            }
        }
    }

    if (root) cJSON_Delete(root);
    return ok;
}

// Truy vấn danh sách Piped API cấu hình trong Dashboard để tìm metadata & audio stream YouTube
bool resolve_youtube_piped(const ZClawConfig& cfg,
                           const char* query_or_url,
                           std::string& out_title,
                           int& out_duration_sec,
                           std::string& out_mp3_url,
                           std::string& out_video_id) {
    std::vector<std::string> hosts;
    if (cfg.piped_instances[0] != '\0') {
        const char* p = cfg.piped_instances;
        while (*p) {
            while (*p == ' ' || *p == ',') p++;
            const char* end = strchr(p, ',');
            std::string h = end ? std::string(p, end - p) : std::string(p);
            while (!h.empty() && (h.back() == ' ' || h.back() == '/')) h.pop_back();
            if (!h.empty()) hosts.push_back(h);
            if (!end) break;
            p = end + 1;
        }
    }
    if (hosts.empty()) {
        hosts.push_back("https://pipedapi.kavin.rocks");
        hosts.push_back("https://pipedapi.adminforge.de");
        hosts.push_back("https://pipedapi.tokhmi.xyz");
    }

    out_video_id = extract_youtube_id(query_or_url);
    for (const std::string& host : hosts) {
        if (out_video_id.empty()) {
            std::string surl = host + "/search?q=" +
                               url_encode(query_or_url, false) + "&filter=music_songs";
            std::string resp;
            int st = 0;
            if (http_get_text(surl.c_str(), resp, &st, 20480) == ESP_OK && st == 200) {
                cJSON* root = cJSON_Parse(resp.c_str());
                if (root) {
                    cJSON* items = cJSON_GetObjectItemCaseSensitive(root, "items");
                    cJSON* item0 = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : nullptr;
                    if (item0) {
                        cJSON* jurl   = cJSON_GetObjectItemCaseSensitive(item0, "url");
                        cJSON* jtitle = cJSON_GetObjectItemCaseSensitive(item0, "title");
                        cJSON* jdur   = cJSON_GetObjectItemCaseSensitive(item0, "duration");
                        if (cJSON_IsString(jtitle) && jtitle->valuestring) out_title = jtitle->valuestring;
                        if (cJSON_IsNumber(jdur)) out_duration_sec = jdur->valueint;
                        if (cJSON_IsString(jurl) && jurl->valuestring) {
                            out_video_id = extract_youtube_id(jurl->valuestring);
                            if (out_video_id.empty() && strncmp(jurl->valuestring, "/watch?v=", 9) == 0) {
                                out_video_id = jurl->valuestring + 9;
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }

        if (!out_video_id.empty()) {
            std::string strm_url = host + "/streams/" + out_video_id;
            std::string resp;
            int st = 0;
            if (http_get_text(strm_url.c_str(), resp, &st, 32768) == ESP_OK && st == 200) {
                cJSON* root = cJSON_Parse(resp.c_str());
                if (root) {
                    cJSON* jtitle = cJSON_GetObjectItemCaseSensitive(root, "title");
                    cJSON* jdur   = cJSON_GetObjectItemCaseSensitive(root, "duration");
                    if (out_title.empty() && cJSON_IsString(jtitle) && jtitle->valuestring) {
                        out_title = jtitle->valuestring;
                    }
                    if (out_duration_sec == 0 && cJSON_IsNumber(jdur)) {
                        out_duration_sec = jdur->valueint;
                    }
                    cJSON* a_arr = cJSON_GetObjectItemCaseSensitive(root, "audioStreams");
                    if (cJSON_IsArray(a_arr)) {
                        const int n = cJSON_GetArraySize(a_arr);
                        for (int i = 0; i < n; ++i) {
                            cJSON* s = cJSON_GetArrayItem(a_arr, i);
                            cJSON* jmime = cJSON_GetObjectItemCaseSensitive(s, "mimeType");
                            cJSON* jurl  = cJSON_GetObjectItemCaseSensitive(s, "url");
                            if (cJSON_IsString(jmime) && jmime->valuestring &&
                                strstr(jmime->valuestring, "audio/mpeg") != nullptr &&
                                cJSON_IsString(jurl) && jurl->valuestring) {
                                out_mp3_url = jurl->valuestring;
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root);
                    return true;
                }
            }
        }
    }
    return !out_title.empty() || !out_video_id.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// 5. phat_nhac_truc_tuyen(tu_khoa_hoac_url, nen_tang)
// Mặc định LUÔN là YouTube / SoundCloud; CHỈ mở Radio VOV khi người dùng nói rõ Radio/VOV
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_phat_nhac_truc_tuyen(cJSON* args) {
    const cJSON* j_req = cJSON_GetObjectItem(args, "tu_khoa_hoac_url");
    if (!j_req) j_req = cJSON_GetObjectItem(args, "ten_bai_hat_hoac_kenh"); // tương thích ngược
    const cJSON* j_plat = cJSON_GetObjectItem(args, "nen_tang");

    const char* raw = (cJSON_IsString(j_req) && j_req->valuestring && j_req->valuestring[0])
                          ? j_req->valuestring
                          : "nhac chill lo-fi";
    std::string plat = fold_vn_lower(
        (cJSON_IsString(j_plat) && j_plat->valuestring) ? j_plat->valuestring : "");

    // Chỉ mở Radio VOV nếu người dùng chỉ rõ "radio" / "vov" / "bat dai" / "kenh vov"
    std::string folded_q = fold_vn_lower(raw);
    const bool explicit_radio =
        (folded_q.find("vov") != std::string::npos ||
         folded_q.find("radio") != std::string::npos ||
         folded_q.find("bat dai") != std::string::npos ||
         folded_q.find("kenh dai") != std::string::npos);

    if (plat == "radio" && !explicit_radio) {
        // Ngăn LLM ép nhầm bài hát bình thường thành Radio VOV
        plat = "youtube";
    }
    if (plat.empty()) {
        if (strstr(raw, "soundcloud.com") != nullptr) {
            plat = "soundcloud";
        } else if (explicit_radio) {
            plat = "radio";
        } else {
            plat = "youtube";
        }
    }

    const ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    std::string chosen_url;
    std::string chosen_title = raw;
    std::string source_tag   = "[YouTube]";
    std::string artist;
    int duration_sec = 0;

    if (plat == "radio") {
        source_tag = "[Radio]";
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
        };

        if (strncasecmp(raw, "http://", 7) == 0 || strncasecmp(raw, "https://", 8) == 0) {
            chosen_url   = raw;
            chosen_title = "Luồng Radio MP3";
        } else {
            for (const auto& ch : kChannels) {
                if (folded_q.find(ch.key) != std::string::npos) {
                    chosen_url   = ch.stream_url;
                    chosen_title = ch.label;
                    break;
                }
            }
            if (chosen_url.empty()) {
                chosen_url   = kChannels[4].stream_url;
                chosen_title = "VOV3 Âm nhạc";
            }
        }
    } else if (plat == "soundcloud") {
        source_tag = "[SoundCloud]";
        if ((strncasecmp(raw, "http://", 7) == 0 || strncasecmp(raw, "https://", 8) == 0) &&
            strstr(raw, ".mp3") != nullptr) {
            chosen_url = raw;
        } else {
            std::string cid = fetch_soundcloud_client_id(cfg);
            resolve_soundcloud_mp3(raw, cid, chosen_url, chosen_title, artist, duration_sec);

            if (chosen_url.empty() && cfg.media_proxy_url[0] != '\0') {
                chosen_url = build_proxy_url(cfg.media_proxy_url, "/stream", raw, "&source=soundcloud");
            }
            if (chosen_url.empty()) {
                return make_error_json(
                    "Không tìm được luồng MP3 progressive từ SoundCloud. "
                    "Hãy kiểm tra SoundCloud Client ID hoặc Media Gateway URL trong tab Dịch vụ & Tools trên Dashboard.");
            }
        }
    } else {
        // YouTube / YouTube Music mặc định
        source_tag = "[YouTube]";
        std::string video_id;
        resolve_youtube_piped(cfg, raw, chosen_title, duration_sec, chosen_url, video_id);
        if (chosen_title.empty()) chosen_title = raw;

        if (cfg.media_proxy_url[0] != '\0') {
            const char* query_for_proxy = !video_id.empty() ? video_id.c_str() : raw;
            chosen_url = build_proxy_url(cfg.media_proxy_url, "/stream", query_for_proxy, "&source=youtube");
        } else if (chosen_url.empty()) {
            // Fallback sang SoundCloud progressive MP3 nếu Piped không trả về định dạng audio/mpeg
            std::string cid = fetch_soundcloud_client_id(cfg);
            std::string sc_url, sc_title, sc_artist;
            int sc_dur = 0;
            if (resolve_soundcloud_mp3(raw, cid, sc_url, sc_title, sc_artist, sc_dur)) {
                chosen_url   = sc_url;
                chosen_title = sc_title;
                artist       = sc_artist;
                duration_sec = sc_dur;
                source_tag   = "[SoundCloud]";
            } else {
                return make_error_json(
                    "Không tìm thấy luồng MP3 trực tiếp từ YouTube/SoundCloud cho minimp3 trên ESP32-S3. "
                    "Hãy nhập Custom yt-dlp / Media Gateway URL trong tab Dịch vụ & Tools trên Dashboard.");
            }
        }
    }

    // Xếp hàng phát MP3 ngay sau khi Bot nói xong lời giới thiệu để tránh tranh bus I2S
    ZClawLlmClient::instance().schedule_media_playback(
        MediaKind::AUDIO_MP3, chosen_url.c_str(), source_tag.c_str(), chosen_title.c_str());

    ChronosNotification notif = {};
    snprintf(notif.app_name, sizeof(notif.app_name), "%s", source_tag.c_str());
    snprintf(notif.title, sizeof(notif.title), "%s", chosen_title.c_str());
    snprintf(notif.body, sizeof(notif.body), "Nhấn BTN_MAIN (GPIO 3) để dừng nhạc");
    notif.unread = true;
    UiEyesClock::instance().show_notification(notif);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "nguon", source_tag.c_str());
    cJSON_AddStringToObject(out, "ten_bai_hat", chosen_title.c_str());
    if (!artist.empty()) cJSON_AddStringToObject(out, "nghe_si", artist.c_str());
    if (duration_sec > 0) cJSON_AddNumberToObject(out, "thoi_luong_giay", duration_sec);
    cJSON_AddStringToObject(out, "url", chosen_url.c_str());
    cJSON_AddStringToObject(out, "ghi_chu", "Đã lên lịch phát nhạc ra loa kèm sóng âm và biểu cảm vui vẻ. Nhấn nút chính để dừng.");
    return cjson_to_string_and_free(out);
}

// ---------------------------------------------------------------------------
// 6. xem_video_truc_tuyen(tu_khoa_hoac_url, do_phan_giai)
// ---------------------------------------------------------------------------
std::string McpRegistry::tool_xem_video_truc_tuyen(cJSON* args) {
    const cJSON* j_val = cJSON_GetObjectItem(args, "tu_khoa_hoac_url");
    if (!cJSON_IsString(j_val) || !j_val->valuestring || !j_val->valuestring[0]) {
        return make_error_json("Thiếu từ khóa tìm kiếm hoặc URL luồng video");
    }

    const ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    const cJSON* j_res = cJSON_GetObjectItem(args, "do_phan_giai");
    std::string res = cfg.video_res[0] ? cfg.video_res : "240x240";
    if (cJSON_IsString(j_res) && j_res->valuestring &&
        (strcmp(j_res->valuestring, "240x180") == 0 || strcmp(j_res->valuestring, "240x240") == 0)) {
        res = j_res->valuestring;
    }

    const char* raw = j_val->valuestring;
    std::string stream_url;
    std::string display_title = raw;
    std::string source_tag    = "[Video]";

    const bool is_http = (strncasecmp(raw, "http://", 7) == 0 || strncasecmp(raw, "https://", 8) == 0);
    const bool is_yt_url = is_http && (strstr(raw, "youtube.com") != nullptr || strstr(raw, "youtu.be") != nullptr);

    if (is_http && !is_yt_url) {
        stream_url = raw;
    } else {
        if (cfg.media_proxy_url[0] == '\0') {
            return make_error_json(
                "Chưa cấu hình Máy chủ Stream Video (Media Proxy URL). "
                "Vui lòng vào Dashboard -> Dịch vụ & Tools nhập server MJPEG Proxy hoặc truyền trực tiếp URL luồng MJPEG.");
        }

        source_tag = "[YouTube]";
        std::string piped_title, piped_mp3, piped_vid;
        int piped_dur = 0;
        resolve_youtube_piped(cfg, raw, piped_title, piped_dur, piped_mp3, piped_vid);
        if (!piped_title.empty()) {
            display_title = piped_title;
        }

        const char* q_arg = !piped_vid.empty() ? piped_vid.c_str() : raw;
        char vqs[96];
        snprintf(vqs, sizeof(vqs), "&source=youtube&res=%s&fps=%u&qlt=%u",
                 res.c_str(),
                 static_cast<unsigned>(cfg.video_fps ? cfg.video_fps : 12),
                 static_cast<unsigned>(cfg.video_jpeg_quality ? cfg.video_jpeg_quality : 75));
        stream_url = build_proxy_url(cfg.media_proxy_url, "/video", q_arg, vqs);
    }

    ZClawLlmClient::instance().schedule_media_playback(
        MediaKind::VIDEO_MJPEG, stream_url.c_str(), source_tag.c_str(), display_title.c_str());

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "tieu_de", display_title.c_str());
    cJSON_AddStringToObject(out, "do_phan_giai", res.c_str());
    cJSON_AddStringToObject(out, "url_mjpeg", stream_url.c_str());
    cJSON_AddStringToObject(out, "ghi_chu", "Video MJPEG sẽ phát ngay trên màn hình ST7789 sau lời giới thiệu. Nhấn BTN_MAIN (GPIO 3) để thoát về mắt Chatbot.");
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
            snprintf(notif.body, sizeof(notif.body), "%.*s",
                     static_cast<int>(sizeof(notif.body) - 1), ev.reminder);
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
