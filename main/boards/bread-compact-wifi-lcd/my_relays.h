#ifndef __MY_RELAYS_H__
#define __MY_RELAYS_H__

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <cJSON.h>
#include <map>
#include <string>

#include "mcp_server.h"

#define TAG_RELAY "MyRelay"

class RelayController {
private:
    struct RelayItem {
        gpio_num_t gpio;
        bool state;
        esp_timer_handle_t pulse_timer;
    };

    std::map<int, RelayItem> relays_;

    static void RelayPulseCallback(void* arg) {
        RelayItem* item = static_cast<RelayItem*>(arg);
        if (item != nullptr) {
            gpio_set_level(item->gpio, 0);
            item->state = false;
            ESP_LOGI(TAG_RELAY, "Relay pulse auto-off completed on GPIO %d", item->gpio);
        }
    }

    void InitRelay(int relay_num, gpio_num_t gpio_num, int default_state = 0) {
        if (gpio_num == GPIO_NUM_NC) {
            return;
        }

        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_num, default_state);

        RelayItem item;
        item.gpio = gpio_num;
        item.state = (default_state != 0);
        item.pulse_timer = nullptr;

        esp_timer_create_args_t timer_args = {
            .callback = RelayPulseCallback,
            .arg = &relays_[relay_num],
            .dispatch_method = ESP_TIMER_TASK,
            .name = "relay_pulse_off",
            .skip_unhandled_events = true,
        };
        relays_[relay_num] = item;
        esp_timer_create(&timer_args, &relays_[relay_num].pulse_timer);
    }

public:
    RelayController() {
        InitRelay(3, GPIO_NUM_3);
        InitRelay(14, GPIO_NUM_14);
        InitRelay(15, GPIO_NUM_15);
        InitRelay(16, GPIO_NUM_16);

        auto& mcp_server = McpServer::GetInstance();

        const char* description = 
            "Điều khiển các relay vật lý trên bo mạch.\n\n"
            "Bo mạch có 4 relay tương ứng với các chân GPIO sau:\n"
            "  - Relay 3  -> GPIO 3\n"
            "  - Relay 14 -> GPIO 14\n"
            "  - Relay 15 -> GPIO 15\n"
            "  - Relay 16 -> GPIO 16\n\n"
            "Tham số:\n"
            "  • relay (integer, bắt buộc): số hiệu relay vật lý (3, 14, 15, 16).\n"
            "  • device (string, tuỳ chọn): tên thiết bị logic (ví dụ: 'pc', 'den', 'may_lanh').\n"
            "  • action (string, bắt buộc): 'turn_on', 'turn_off', 'toggle', 'get_state'.\n"
            "  • duration_ms (integer, milliseconds): thời gian bật tự tắt (xung). 0 là giữ nguyên.";

        PropertyList properties;
        properties["relay"] = Property(PropertyType::kInteger, "Relay number (3, 14, 15, 16)", true);
        properties["device"] = Property(PropertyType::kString, "Device name label", false);
        properties["action"] = Property(PropertyType::kString, "Action: turn_on, turn_off, toggle, get_state", true);
        properties["duration_ms"] = Property(PropertyType::kInteger, "Duration in milliseconds for pulse auto-off (0 for latch)", false);

        mcp_server.AddTool("self.relay.control", description, properties, [this](const PropertyList& props) -> ReturnValue {
            int relay_num = 0;
            std::string action;
            std::string device;
            int duration_ms = 0;

            auto it_r = props.find("relay");
            if (it_r != props.end()) relay_num = it_r->second.value<int>();

            auto it_a = props.find("action");
            if (it_a != props.end()) action = it_a->second.value<std::string>();

            auto it_d = props.find("device");
            if (it_d != props.end()) device = it_d->second.value<std::string>();

            auto it_dur = props.find("duration_ms");
            if (it_dur != props.end()) duration_ms = it_dur->second.value<int>();

            auto it = relays_.find(relay_num);
            if (it == relays_.end()) {
                ESP_LOGE(TAG_RELAY, "Invalid relay number: %d (device='%s')", relay_num, device.c_str());
                return "{\"error\": \"Invalid relay number\"}";
            }

            RelayItem& item = it->second;

            if (action == "get_state") {
                return item.state ? "{\"state\": \"on\"}" : "{\"state\": \"off\"}";
            } else if (action == "turn_on") {
                item.state = true;
                gpio_set_level(item.gpio, 1);
                if (duration_ms > 0 && duration_ms <= 86400000) {
                    if (item.pulse_timer) {
                        esp_timer_stop(item.pulse_timer);
                        esp_timer_start_once(item.pulse_timer, duration_ms * 1000ULL);
                    }
                }
                return "{\"success\": true, \"state\": \"on\"}";
            } else if (action == "turn_off") {
                if (item.pulse_timer) {
                    esp_timer_stop(item.pulse_timer);
                }
                item.state = false;
                gpio_set_level(item.gpio, 0);
                return "{\"success\": true, \"state\": \"off\"}";
            } else if (action == "toggle") {
                item.state = !item.state;
                gpio_set_level(item.gpio, item.state ? 1 : 0);
                return item.state ? "{\"success\": true, \"state\": \"on\"}" : "{\"success\": true, \"state\": \"off\"}";
            }

            ESP_LOGE(TAG_RELAY, "Unknown action: %s (device='%s')", action.c_str(), device.c_str());
            return "{\"error\": \"Unknown relay action\"}";
        });
    }
};

#endif // __MY_RELAYS_H__
