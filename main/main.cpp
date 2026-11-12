// main.cpp
// S.W.Ni-Oh — Entry Point & State Machine (ESP32-S3 SuperMini 4MB Flash)
// Production-ready C++17 implementation — no TODOs, no pseudo-code, no exceptions.

#include "audio_hal.h"
#include "board_config.h"
#include "chronos_ble.h"
#include "espnow_comm.h"
#include "mcp_vietnam_tools.h"
#include "ui_eyes_clock.h"
#include "zclaw_llm_client.h"

#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

static const char* TAG = "MainApp";

namespace zclaw {

namespace {

enum class BotState : uint8_t {
    IDLE = 0,
    LISTENING,
    THINKING,
    SPEAKING,
    PLAYING_RADIO,
};

enum class FsmCommand : uint8_t {
    START_VOICE_TURN = 1,
    RESET_SESSION    = 2,
    OPEN_PORTAL      = 3,
};

constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailBit      = BIT1;
constexpr uint32_t    kWifiConnectTimeoutMs = 15000;
constexpr uint32_t    kIdleClockTimeoutMs   = 5000;
constexpr uint32_t    kPortalClockTimeoutMs = 15000;
constexpr uint32_t    kPortalHoldMs         = 5000;

EventGroupHandle_t     g_wifi_events    = nullptr;
QueueHandle_t          g_fsm_queue      = nullptr;
std::atomic<BotState>  g_bot_state{BotState::IDLE};
std::atomic<bool>      g_wifi_connected{false};
std::atomic<bool>      g_has_sta_target{false};
std::atomic<bool>      g_sntp_started{false};
std::atomic<int>       g_wifi_retries{0};
std::atomic<uint32_t>  g_last_activity_ms{0};

inline uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void touch_activity() {
    g_last_activity_ms.store(now_ms(), std::memory_order_relaxed);
}

void start_ntp_once() {
    if (g_sntp_started.exchange(true)) return;
    setenv("TZ", "ICT-7", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "vn.pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "Đã khởi động đồng bộ giờ NTP (ICT-7)");
}

void wifi_event_handler(void* /*arg*/,
                        esp_event_base_t event_base,
                        int32_t event_id,
                        void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_has_sta_target.load()) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected.store(false);
        if (!g_has_sta_target.load()) return;
        const int retries = g_wifi_retries.fetch_add(1);
        if (retries < 15) {
            esp_wifi_connect();
        } else if (g_wifi_events) {
            xEventGroupSetBits(g_wifi_events, kWifiFailBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(event_data);
        g_wifi_retries.store(0);
        g_wifi_connected.store(true);
        ESP_LOGI(TAG, "Wi-Fi đã kết nối, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        if (g_wifi_events) {
            xEventGroupSetBits(g_wifi_events, kWifiConnectedBit);
        }
        start_ntp_once();
        if (EspNowMeshManager::instance().is_initialized()) {
            EspNowMeshManager::instance().sync_channel_with_wifi();
        }
    }
}

void send_fsm_cmd(FsmCommand cmd) {
    if (g_fsm_queue) {
        xQueueSend(g_fsm_queue, &cmd, 0);
    }
}

void show_ui_toast(const char* app, const char* title, const char* body) {
    ChronosNotification notif = {};
    snprintf(notif.app_name, sizeof(notif.app_name), "%s", app ? app : "Hệ thống");
    snprintf(notif.title,    sizeof(notif.title),    "%s", title ? title : "");
    snprintf(notif.body,     sizeof(notif.body),     "%s", body ? body : "");
    notif.unread = true;
    UiEyesClock::instance().show_notification(notif);
}

// ---------------------------------------------------------------------------
// Task 1: Quét 3 nút nhấn ngoại vi (Debounce 30ms, Long press 1600ms / 5000ms)
// ---------------------------------------------------------------------------
void button_scanner_task(void* /*arg*/) {
    constexpr uint32_t kPollMs = 20;

    struct DebouncedBtn {
        gpio_num_t gpio;
        bool       raw_last;
        bool       stable;
        uint32_t   raw_change_ms;
        uint32_t   pressed_at_ms;
        bool       long_fired;
        bool       portal_fired;
        uint32_t   last_repeat_ms;
    };

    DebouncedBtn btns[3] = {
        {BTN_MAIN_GPIO,     false, false, 0, 0, false, false, 0},
        {BTN_VOL_UP_GPIO,   false, false, 0, 0, false, false, 0},
        {BTN_VOL_DOWN_GPIO, false, false, 0, 0, false, false, 0},
    };

    while (true) {
        const uint32_t t = now_ms();

        for (int i = 0; i < 3; ++i) {
            DebouncedBtn& b = btns[i];
            const bool raw = board_read_button_raw(b.gpio);

            if (raw != b.raw_last) {
                b.raw_last      = raw;
                b.raw_change_ms = t;
            }

            if ((t - b.raw_change_ms) >= BTN_DEBOUNCE_MS && raw != b.stable) {
                b.stable = raw;

                if (b.stable) {
                    // Cạnh nhấn xuống (Pressed)
                    b.pressed_at_ms  = t;
                    b.long_fired     = false;
                    b.portal_fired   = false;
                    b.last_repeat_ms = t;

                    if (i == 1 || i == 2) {
                        const int8_t delta = (i == 1) ? 10 : -10;
                        const uint8_t new_vol = AudioHal::instance().adjust_speaker_volume(delta, true);
                        UiEyesClock::instance().show_volume_osd(new_vol);
                        touch_activity();
                    }
                } else {
                    // Cạnh nhả ra (Released)
                    if (i == 0 && !b.long_fired && !b.portal_fired) {
                        // Nhấn đơn BTN_MAIN (GPIO 3)
                        touch_activity();
                        const BotState cur = g_bot_state.load();
                        if (cur == BotState::LISTENING) {
                            ESP_LOGI(TAG, "BTN_MAIN: Dừng thu âm, gửi ngay lên LLM");
                            AudioHal::instance().request_stop_capture();
                        } else if (cur == BotState::SPEAKING || cur == BotState::PLAYING_RADIO) {
                            ESP_LOGI(TAG, "BTN_MAIN: Ngắt phát âm thanh tức thời");
                            ZClawLlmClient::instance().clear_pending_radio();
                            AudioHal::instance().request_stop_playback();
                        } else if (cur == BotState::IDLE) {
                            UiEyesClock::instance().switch_to_chat_screen();
                            send_fsm_cmd(FsmCommand::START_VOICE_TURN);
                        }
                    }
                }
            }

            // Xử lý nhấn giữ
            if (b.stable) {
                const uint32_t held_ms = t - b.pressed_at_ms;
                if (i == 0) {
                    if (!b.long_fired && held_ms >= BTN_LONG_PRESS_MS) {
                        b.long_fired = true;
                        ESP_LOGI(TAG, "BTN_MAIN giữ %ums: Reset trạng thái & xóa lịch sử", static_cast<unsigned>(BTN_LONG_PRESS_MS));
                        AudioHal::instance().request_stop_capture();
                        AudioHal::instance().request_stop_playback();
                        ZClawLlmClient::instance().clear_pending_radio();
                        send_fsm_cmd(FsmCommand::RESET_SESSION);
                    }
                    if (!b.portal_fired && held_ms >= kPortalHoldMs) {
                        b.portal_fired = true;
                        ESP_LOGI(TAG, "BTN_MAIN giữ %ums: Mở Web Captive Portal cấu hình", static_cast<unsigned>(kPortalHoldMs));
                        send_fsm_cmd(FsmCommand::OPEN_PORTAL);
                    }
                } else {
                    // Nhấn giữ nút Volume -> tự tăng/giảm liên tục mỗi 280ms
                    if (held_ms >= 500 && (t - b.last_repeat_ms) >= 280) {
                        b.last_repeat_ms = t;
                        const int8_t delta = (i == 1) ? 10 : -10;
                        const uint8_t new_vol = AudioHal::instance().adjust_speaker_volume(delta, true);
                        UiEyesClock::instance().show_volume_osd(new_vol);
                        touch_activity();
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

// ---------------------------------------------------------------------------
// Task 2: Máy Trạng Thái Hội Thoại Voice ZClaw & Đồng hồ chờ 5 giây
// ---------------------------------------------------------------------------
void bot_fsm_task(void* /*arg*/) {
    touch_activity();

    while (true) {
        FsmCommand cmd = {};
        if (xQueueReceive(g_fsm_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (cmd == FsmCommand::RESET_SESSION) {
                ZClawLlmClient::instance().clear_history();
                ZClawLlmClient::instance().clear_pending_media();
                AudioHal::instance().clear_stop_capture();
                AudioHal::instance().clear_stop_playback();
                UiEyesClock::instance().exit_video_mode();
                UiEyesClock::instance().hide_music_banner();
                AudioHal::instance().play_chime(AudioHal::ChimeType::DONE);
                UiEyesClock::instance().switch_to_chat_screen();
                UiEyesClock::instance().set_emotion(EyeEmotion::BINH_THUONG);
                show_ui_toast("Hệ thống", "Đã đặt lại phiên", "Xóa ngữ cảnh hội thoại tạm thời");
                g_bot_state.store(BotState::IDLE);
                touch_activity();
                continue;
            }

            if (cmd == FsmCommand::OPEN_PORTAL) {
                AudioHal::instance().play_chime(AudioHal::ChimeType::ALERT);
                ZClawLlmClient::instance().start_captive_portal(true);
                UiEyesClock::instance().switch_to_chat_screen();
                UiEyesClock::instance().set_emotion(EyeEmotion::NGAC_NHIEN);
                show_ui_toast("Web Portal", "Wi-Fi: S.W.Ni-Oh-Setup", "Truy cập http://192.168.4.1");
                g_bot_state.store(BotState::IDLE);
                touch_activity();
                continue;
            }

            if (cmd == FsmCommand::START_VOICE_TURN) {
                UiEyesClock::instance().switch_to_chat_screen();

                if (!g_wifi_connected.load()) {
                    AudioHal::instance().play_chime(AudioHal::ChimeType::ERROR);
                    UiEyesClock::instance().set_emotion(EyeEmotion::BUON);
                    show_ui_toast("Mạng Wi-Fi", "Chưa có kết nối Internet", "Giữ nút chính 5s để cấu hình");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    UiEyesClock::instance().set_emotion(EyeEmotion::BINH_THUONG);
                    g_bot_state.store(BotState::IDLE);
                    touch_activity();
                    continue;
                }

                // 1. Thức dậy & Thu âm giọng nói với VAD
                g_bot_state.store(BotState::LISTENING);
                AudioHal::instance().clear_stop_capture();
                AudioHal::instance().clear_stop_playback();
                ZClawLlmClient::instance().clear_pending_radio();

                UiEyesClock::instance().set_emotion(EyeEmotion::DANG_NGHE);
                AudioHal::instance().play_chime(AudioHal::ChimeType::WAKE);

                AudioHal::VadConfig vad_cfg = {};
                AudioHal::CapturedAudio captured;
                esp_err_t rec_err = AudioHal::instance().record_utterance_with_vad(vad_cfg, captured);

                if (rec_err != ESP_OK || !captured.speech_detected) {
                    captured.release();
                    UiEyesClock::instance().set_emotion(EyeEmotion::BINH_THUONG);
                    g_bot_state.store(BotState::IDLE);
                    touch_activity();
                    continue;
                }

                // 2. Gửi STT Groq Whisper & giải phóng ngay buffer WAV khỏi RAM
                g_bot_state.store(BotState::THINKING);
                UiEyesClock::instance().set_emotion(EyeEmotion::SUY_NGHI);

                std::string user_text;
                esp_err_t stt_err = ZClawLlmClient::instance().transcribe_wav(captured, user_text);
                captured.release();

                if (stt_err != ESP_OK || user_text.empty()) {
                    AudioHal::instance().play_chime(AudioHal::ChimeType::ERROR);
                    UiEyesClock::instance().set_emotion(EyeEmotion::BUON);
                    show_ui_toast("Nhận dạng giọng nói", "Không nghe rõ", "Hãy thử nói gần micro hơn");
                    vTaskDelay(pdMS_TO_TICKS(800));
                    UiEyesClock::instance().set_emotion(EyeEmotion::BINH_THUONG);
                    g_bot_state.store(BotState::IDLE);
                    touch_activity();
                    continue;
                }

                // 3. Gọi Direct Cloud LLM + Vòng lặp 10 MCP Tools Tiếng Việt
                std::string reply_text;
                EyeEmotion reply_emotion = EyeEmotion::BINH_THUONG;
                ZClawLlmClient::instance().chat_with_tools(user_text, reply_text, reply_emotion);

                // 4. Đổi biểu cảm mắt & phát TTS Tiếng Việt
                g_bot_state.store(BotState::SPEAKING);
                UiEyesClock::instance().set_emotion(reply_emotion);
                ZClawLlmClient::instance().speak_vietnamese(reply_text);

                // 5. Nếu MCP phat_nhac_truc_tuyen hoặc xem_video_truc_tuyen đã xếp lịch -> phát Media
                PendingMedia media;
                if (ZClawLlmClient::instance().consume_pending_media(media) &&
                    !AudioHal::instance().is_playback_stop_requested()) {
                    g_bot_state.store(BotState::PLAYING_RADIO);
                    if (media.kind == MediaKind::VIDEO_MJPEG) {
                        ESP_LOGI(TAG, "Bắt đầu phát Video ST7789: %s %s",
                                 media.source_tag.c_str(), media.title.c_str());
                        ZClawLlmClient::instance().stream_mjpeg_video_url(
                            media.url, media.title, media.source_tag);
                    } else if (media.kind == MediaKind::AUDIO_MP3) {
                        ESP_LOGI(TAG, "Bắt đầu phát Nhạc/Radio: %s %s",
                                 media.source_tag.c_str(), media.title.c_str());
                        UiEyesClock::instance().set_emotion(EyeEmotion::VUI_VE);
                        UiEyesClock::instance().show_music_banner(
                            media.source_tag.c_str(), media.title.c_str());
                        ZClawLlmClient::instance().stream_mp3_url(media.url.c_str(), 0);
                        UiEyesClock::instance().hide_music_banner();
                    }
                }

                // 6. Kết thúc lượt hội thoại -> về IDLE, bắt đầu đếm 5s sang Clock Dashboard
                UiEyesClock::instance().set_emotion(EyeEmotion::BINH_THUONG);
                g_bot_state.store(BotState::IDLE);
                touch_activity();
            }
            continue;
        }

        // Kiểm tra bộ đếm không hoạt động -> tự chuyển sang Clock & Weather Dashboard
        if (g_bot_state.load() == BotState::IDLE && !UiEyesClock::instance().is_clock_screen_active()) {
            const uint32_t timeout_ms = ZClawLlmClient::instance().is_portal_active()
                                            ? kPortalClockTimeoutMs
                                            : kIdleClockTimeoutMs;
            if ((now_ms() - g_last_activity_ms.load()) >= timeout_ms) {
                UiEyesClock::instance().switch_to_clock_screen();
            }
        }
    }
}

void init_littlefs() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = BOARD_LITTLEFS_MOUNT;
    conf.partition_label        = BOARD_LITTLEFS_LABEL;
    conf.format_if_mount_failed = true;
    conf.dont_mount             = false;
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mount LittleFS (%s) cảnh báo: %s", BOARD_LITTLEFS_LABEL, esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info(BOARD_LITTLEFS_LABEL, &total, &used);
        ESP_LOGI(TAG, "LittleFS sẵn sàng (%u / %u KB)", (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
}

void init_wifi_and_portal() {
    g_wifi_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    const ZClawConfig cfg = ZClawLlmClient::instance().get_config();
    if (!ZClawLlmClient::instance().has_wifi_credentials()) {
        g_has_sta_target.store(false);
        ESP_LOGW(TAG, "Chưa có cấu hình Wi-Fi -> Mở SoftAP & Web Captive Portal");
        ZClawLlmClient::instance().start_captive_portal(true);
        show_ui_toast("Cài đặt lần đầu", "Wi-Fi: S.W.Ni-Oh-Setup", "Mở http://192.168.4.1");
        return;
    }

    g_has_sta_target.store(true);

    wifi_config_t sta_cfg = {};
    memcpy(sta_cfg.sta.ssid, cfg.wifi_ssid, std::min(sizeof(sta_cfg.sta.ssid), strlen(cfg.wifi_ssid)));
    memcpy(sta_cfg.sta.password, cfg.wifi_pass, std::min(sizeof(sta_cfg.sta.password), strlen(cfg.wifi_pass)));
    sta_cfg.sta.threshold.authmode =
        (cfg.wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    show_ui_toast("Đang khởi động", "Kết nối Wi-Fi...", cfg.wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(
        g_wifi_events,
        kWifiConnectedBit | kWifiFailBit,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(kWifiConnectTimeoutMs));

    if ((bits & kWifiConnectedBit) != 0) {
        ESP_LOGI(TAG, "Kết nối Wi-Fi thành công tới %s", cfg.wifi_ssid);
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set("sw-nioh");
            mdns_instance_name_set("S.W.Ni-Oh");
            mdns_service_add("S.W.Ni-Oh Dashboard", "_http", "_tcp", 80, nullptr, 0);
        }
        ZClawLlmClient::instance().start_captive_portal(false);
        show_ui_toast("Wi-Fi Đã Kết Nối", cfg.wifi_ssid, "Mở http://sw-nioh.local");
    } else {
        ESP_LOGW(TAG, "Quá 15s chưa vào được Wi-Fi %s -> Bật Captive Portal dự phòng", cfg.wifi_ssid);
        ZClawLlmClient::instance().start_captive_portal(true);
        show_ui_toast("Lỗi Wi-Fi (AP+STA)", "Wi-Fi: S.W.Ni-Oh-Setup", "Cấu hình tại 192.168.4.1");
    }
}

} // namespace

} // namespace zclaw

// ============================================================================
// ENTRY POINT: app_main()
// ============================================================================
extern "C" void app_main(void) {
    // 1. BẮT BUỘC: Giải phóng GPIO 44 khỏi UART0 RX trước khi khởi tạo I2S_NUM_1 (MAX98357A)
    board_release_gpio44();

    // Thiết lập múi giờ mặc định Việt Nam ICT+7 ngay từ đầu
    setenv("TZ", "ICT-7", 1);
    tzset();

    // 2. Khởi tạo NVS, Netif, Event Loop và LittleFS
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    zclaw::init_littlefs();

    ESP_LOGI(TAG, "=== S.W.Ni-Oh | ESP32-S3 | PSRAM=%s | Heap=%lu KB ===",
             board_has_psram() ? "CO" : "KHONG",
             static_cast<unsigned long>(esp_get_free_heap_size() / 1024));

    // 3. Khởi tạo Giao diện ST7789 + Mắt xanh dương + Sóng âm
    ESP_ERROR_CHECK(zclaw::UiEyesClock::instance().init());
    zclaw::UiEyesClock::instance().set_emotion(zclaw::EyeEmotion::SUY_NGHI);
    zclaw::show_ui_toast("S.W.Ni-Oh", "Đang khởi động...", "Khởi tạo âm thanh & mạng");

    // 4. Khởi tạo Audio HAL (INMP441 I2S0 + MAX98357A I2S1) và phát âm báo WAKE
    ESP_ERROR_CHECK(zclaw::AudioHal::instance().init());
    zclaw::AudioHal::instance().play_chime(zclaw::AudioHal::ChimeType::WAKE);

    // 5. Nạp cấu hình Direct LLM / STT / TTS từ NVS & kết nối Wi-Fi (hoặc mở Portal)
    ESP_ERROR_CHECK(zclaw::ZClawLlmClient::instance().init());
    zclaw::init_wifi_and_portal();

    // 6. Khởi tạo BLE Chronos & ESP-NOW 2 chiều (chạy đồng cư sóng 2.4GHz)
    if (zclaw::ChronosBleService::instance().init("S.W.Ni-Oh") != ESP_OK) {
        ESP_LOGW(TAG, "Khởi tạo BLE Chronos gặp lỗi (tiếp tục chạy Wi-Fi)");
    }

    if (zclaw::EspNowMeshManager::instance().init() == ESP_OK) {
        zclaw::EspNowMeshManager::instance().set_rx_callback(
            [](const uint8_t /*mac*/[6], const zclaw::EspNowPacket& pkt) {
                if (pkt.cmd == zclaw::EspNowCmd::ALERT || pkt.cmd == zclaw::EspNowCmd::CONTROL) {
                    zclaw::touch_activity();
                }
            });
    }

    // 7. Khởi tạo 10 công cụ MCP Tiếng Việt
    ESP_ERROR_CHECK(zclaw::McpRegistry::instance().init());

    // 8. Khởi tạo 3 nút nhấn ngoại vi sau khi boot (an toàn cho GPIO 3 Strapping Pin)
    ESP_ERROR_CHECK(board_init_buttons());

    // 9. Tạo Queue & Khởi chạy Task Quét Nút Nhấn + Task Máy Trạng Thái Hội Thoại
    zclaw::g_fsm_queue = xQueueCreate(8, sizeof(zclaw::FsmCommand));
    configASSERT(zclaw::g_fsm_queue != nullptr);

    zclaw::UiEyesClock::instance().set_emotion(zclaw::EyeEmotion::BINH_THUONG);
    zclaw::touch_activity();

    xTaskCreatePinnedToCore(zclaw::button_scanner_task, "btn_scan", 4096, nullptr, 6, nullptr, 0);
    xTaskCreatePinnedToCore(zclaw::bot_fsm_task,        "nioh_fsm", 12288, nullptr, 5, nullptr, 1);

    ESP_LOGI(TAG, "Hệ thống đã sẵn sàng! Nhấn BTN_MAIN (GPIO %d) để trò chuyện.", BTN_MAIN_GPIO);
}
