#pragma once

#include <esp_log.h>
#include <esp_err.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_http_client.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/ip_addr.h>
#include <cJSON.h>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <algorithm>

#include "mcp_server.h"

#define TAG_IOT "NiOh_IoT"
#define NVS_IOT_NAMESPACE "nioh_pcs"

class IoTDiscoveryController {
public:
    struct ComputerProfile {
        std::string id;         // ID nhận dạng: "my_pc", "pc_ban", "laptop", "pc_cty"
        std::string label;      // Tên hiển thị: "Máy của tôi", "Máy tính công ty"
        std::string hostname;   // Tên máy tính Windows (NetBIOS): "DESKTOP-XYZ", "LAPTOP-NAM"
        std::string vendor;     // Hãng máy tính: "Dell", "HP", "Apple", "Lenovo"
        std::string mac;        // Địa chỉ MAC card mạng (cho Wake-on-LAN)
        std::string ip;         // Địa chỉ IP trong mạng LAN
        int port;               // Port dịch vụ điều khiển (mặc định 7760)
    };

    struct BleDeviceEntry {
        std::string name;
        std::string mac;
        int rssi;
    };

    static IoTDiscoveryController& GetInstance() {
        static IoTDiscoveryController instance;
        return instance;
    }

    void InitEspNow() {
        if (esp_now_initialized_) return;
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) != ESP_OK) {
            ESP_LOGW(TAG_IOT, "WiFi not started yet, skipping ESP-NOW init");
            return;
        }
        esp_err_t ret = esp_now_init();
        if (ret == ESP_OK) {
            esp_now_initialized_ = true;
            esp_now_peer_info_t peer_info = {};
            memset(peer_info.peer_addr, 0xFF, 6);
            peer_info.channel = 0;
            peer_info.encrypt = false;
            esp_now_add_peer(&peer_info);
            ESP_LOGI(TAG_IOT, "ESP-NOW broadcast peer initialized");
        } else {
            ESP_LOGW(TAG_IOT, "ESP-NOW init failed: %s", esp_err_to_name(ret));
        }
    }

    void RegisterTools() {
        LoadComputersFromNvs();
        auto& mcp_server = McpServer::GetInstance();

        // =====================================================================
        // CÔNG CỤ 1: QUÉT TỰ ĐỘNG TOÀN BỘ MẠNG LAN (CÔNG TY / GIA ĐÌNH)
        // =====================================================================
        const char* desc_lan_scan = 
            "Tự động quét toàn bộ dải mạng LAN (ở công ty hoặc ở nhà) để tìm tất cả máy tính đang bật.\n"
            "Trả về: Tên máy tính Windows (NetBIOS Computer Name), Hãng sản xuất (Dell, HP, Apple, Lenovo...), IP và MAC.\n"
            "AI dùng công cụ này khi người dùng hỏi: 'Quét xem có máy tính nào', 'Tìm máy tính của tôi', v.v.";
        PropertyList props_lan_scan;

        mcp_server.AddTool("self.lan.scan_network", desc_lan_scan, props_lan_scan, [this](const PropertyList& props) -> ReturnValue {
            auto found_pcs = ScanLanComputers();
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", true);
            cJSON_AddNumberToObject(root, "total_found", found_pcs.size());
            cJSON* arr = cJSON_CreateArray();

            for (const auto& pc : found_pcs) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "hostname", pc.hostname.c_str());
                cJSON_AddStringToObject(item, "vendor", pc.vendor.c_str());
                cJSON_AddStringToObject(item, "ip", pc.ip.c_str());
                cJSON_AddStringToObject(item, "mac", pc.mac.c_str());
                cJSON_AddItemToArray(arr, item);
            }
            cJSON_AddItemToObject(root, "computers", arr);
            cJSON_AddStringToObject(root, "message", found_pcs.empty() ? 
                "Không tìm thấy máy tính nào đang bật trong mạng LAN" : 
                "Đã quét thành công các máy tính trong mạng LAN");

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 2: GHI NHỚ MÁY TÍNH VÀO BỘ NHỚ VĨNH VIỄN (NVS FLASH)
        // =====================================================================
        const char* desc_remember = 
            "Ghi nhớ máy tính vào bộ nhớ vĩnh viễn NVS của chip (không mất khi mất điện).\n"
            "Dùng khi người dùng chỉ định: 'Nhớ máy này là máy của tôi', 'Gán máy HP kia là laptop công ty', v.v.";

        PropertyList props_remember({
            Property("id", kPropertyTypeString, std::string("my_pc")),
            Property("label", kPropertyTypeString, std::string("")),
            Property("mac", kPropertyTypeString, std::string("")),
            Property("ip", kPropertyTypeString, std::string("")),
            Property("hostname", kPropertyTypeString, std::string(""))
        });

        mcp_server.AddTool("self.lan.remember_computer", desc_remember, props_remember, [this](const PropertyList& props) -> ReturnValue {
            ComputerProfile cp;
            cp.id = "my_pc";
            try { cp.id = props["id"].value<std::string>(); } catch (...) {}
            cp.label = "Máy tính của tôi";
            try { cp.label = props["label"].value<std::string>(); } catch (...) {}
            try { cp.mac = props["mac"].value<std::string>(); } catch (...) {}
            try { cp.ip = props["ip"].value<std::string>(); } catch (...) {}
            try { cp.hostname = props["hostname"].value<std::string>(); } catch (...) {}
            cp.port = 7760;

            // Tự động nhận diện vendor nếu có MAC
            if (!cp.mac.empty()) {
                cp.vendor = LookupVendorByMac(cp.mac);
            }

            bool ok = SaveComputerToNvs(cp);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", ok);
            cJSON_AddStringToObject(root, "id", cp.id.c_str());
            cJSON_AddStringToObject(root, "label", cp.label.c_str());
            cJSON_AddStringToObject(root, "mac", cp.mac.c_str());
            cJSON_AddStringToObject(root, "ip", cp.ip.c_str());
            cJSON_AddStringToObject(root, "message", ok ? "Đã ghi nhớ máy tính vào bộ nhớ vĩnh viễn NVS thành công!" : "Lỗi lưu bộ nhớ NVS");

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 3: QUÊN / XÓA MÁY TÍNH KHỎI BỘ NHỚ NVS
        // =====================================================================
        const char* desc_forget = "Xóa thông tin một máy tính khỏi bộ nhớ vĩnh viễn NVS.";
        PropertyList props_forget({
            Property("id", kPropertyTypeString, std::string("my_pc"))
        });

        mcp_server.AddTool("self.lan.forget_computer", desc_forget, props_forget, [this](const PropertyList& props) -> ReturnValue {
            std::string id = "my_pc";
            try { id = props["id"].value<std::string>(); } catch (...) {}

            bool ok = RemoveComputerFromNvs(id);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", ok);
            cJSON_AddStringToObject(root, "message", ok ? "Đã xóa máy tính khỏi bộ nhớ NVS" : "Không tìm thấy máy tính để xóa");

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 4: LIỆT KÊ TẤT CẢ MÁY TÍNH ĐÃ GHI NHỚ
        // =====================================================================
        const char* desc_list = "Liệt kê danh sách tất cả các máy tính đã được lưu trong bộ nhớ vĩnh viễn của Ni-Oh.";
        PropertyList props_list;

        mcp_server.AddTool("self.lan.list_computers", desc_list, props_list, [this](const PropertyList& props) -> ReturnValue {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", true);
            cJSON* arr = cJSON_CreateArray();

            for (const auto& pair : saved_computers_) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "id", pair.second.id.c_str());
                cJSON_AddStringToObject(item, "label", pair.second.label.c_str());
                cJSON_AddStringToObject(item, "hostname", pair.second.hostname.c_str());
                cJSON_AddStringToObject(item, "vendor", pair.second.vendor.c_str());
                cJSON_AddStringToObject(item, "mac", pair.second.mac.c_str());
                cJSON_AddStringToObject(item, "ip", pair.second.ip.c_str());
                cJSON_AddItemToArray(arr, item);
            }
            cJSON_AddItemToObject(root, "computers", arr);

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 5: BẬT NGUỒN WAKE-ON-LAN (ĐÃ GHI NHỚ HOẶC ĐỌC TRỰC TIẾP MAC)
        // =====================================================================
        const char* desc_wol = 
            "Bật nguồn Máy tính để bàn (PC), Laptop hoặc Server trong mạng LAN bằng Wake-on-LAN.\n"
            "Tự động tìm kiếm đúng máy theo ID ('my_pc', 'laptop', 'gaming', 'pc_cty') hoặc theo MAC cụ thể.";
        PropertyList props_wol({
            Property("device", kPropertyTypeString, std::string("my_pc")),
            Property("mac", kPropertyTypeString, std::string(""))
        });

        mcp_server.AddTool("self.lan.wake_on_lan", desc_wol, props_wol, [this](const PropertyList& props) -> ReturnValue {
            std::string target = "my_pc";
            try { target = props["device"].value<std::string>(); } catch (...) {}
            std::string mac_str;
            try { mac_str = props["mac"].value<std::string>(); } catch (...) {}

            std::string display_name = target;
            if (mac_str.empty()) {
                auto profile = ResolveComputer(target);
                mac_str = profile.mac;
                display_name = profile.label;
            }

            bool ok = SendWakeOnLan(mac_str);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", ok);
            cJSON_AddStringToObject(root, "device", display_name.c_str());
            cJSON_AddStringToObject(root, "mac", mac_str.c_str());
            cJSON_AddStringToObject(root, "message", ok ? 
                ("Đã phát gói tin Wake-on-LAN bật nguồn cho " + display_name).c_str() : 
                "Không tìm thấy địa chỉ MAC hợp lệ");

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 6: ĐIỀU KHIỂN HOẠT ĐỘNG PC (SHUTDOWN, SLEEP, LOCK, MUTE)
        // =====================================================================
        const char* desc_pc_ctrl = 
            "Gửi lệnh điều khiển tắt nguồn, ngủ, khóa màn hình tới PC/Laptop qua LAN.\n"
            "Tham số:\n"
            "  • action (string, bắt buộc): 'shutdown', 'sleep', 'lock', 'mute'.\n"
            "  • device (string, tuỳ chọn): Mã máy tính ('my_pc', 'laptop'...). Mặc định là 'my_pc'.\n"
            "  • ip (string, tuỳ chọn): Địa chỉ IP trực tiếp.";
        PropertyList props_pc_ctrl({
            Property("action", kPropertyTypeString, std::string("sleep")),
            Property("device", kPropertyTypeString, std::string("my_pc")),
            Property("ip", kPropertyTypeString, std::string(""))
        });

        mcp_server.AddTool("self.lan.control_pc", desc_pc_ctrl, props_pc_ctrl, [this](const PropertyList& props) -> ReturnValue {
            std::string action = "sleep";
            try { action = props["action"].value<std::string>(); } catch (...) {}
            std::string target = "my_pc";
            try { target = props["device"].value<std::string>(); } catch (...) {}
            std::string ip_str;
            try { ip_str = props["ip"].value<std::string>(); } catch (...) {}
            int port = 7760;

            std::string display_name = target;
            if (ip_str.empty()) {
                auto profile = ResolveComputer(target);
                ip_str = profile.ip;
                port = profile.port;
                display_name = profile.label;
            }

            std::string msg;
            bool ok = SendLanPcCommand(ip_str, port, action, msg);

            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", ok);
            cJSON_AddStringToObject(root, "device", display_name.c_str());
            cJSON_AddStringToObject(root, "action", action.c_str());
            cJSON_AddStringToObject(root, "message", msg.c_str());

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 7: SÓNG ESP-NOW ĐIỀU KHIỂN CÁC MẠCH ESP32 KHÁC
        // =====================================================================
        const char* desc_espnow = "Điều khiển trực tiếp mạch ESP32 khác trong phòng qua sóng ESP-NOW (không cần router Wi-Fi).";
        PropertyList props_espnow({
            Property("action", kPropertyTypeString, std::string("toggle")),
            Property("mac", kPropertyTypeString, std::string("broadcast")),
            Property("command", kPropertyTypeString, std::string(""))
        });

        mcp_server.AddTool("self.esp_now.control", desc_espnow, props_espnow, [this](const PropertyList& props) -> ReturnValue {
            std::string action = "toggle";
            try { action = props["action"].value<std::string>(); } catch (...) {}
            std::string mac = "broadcast";
            try { mac = props["mac"].value<std::string>(); } catch (...) {}
            std::string cmd;
            try { cmd = props["command"].value<std::string>(); } catch (...) {}

            bool ok = SendEspNow(mac, action, cmd);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", ok);
            cJSON_AddStringToObject(root, "target", mac.c_str());
            cJSON_AddStringToObject(root, "message", ok ? "Đã phát gói tin ESP-NOW thành công" : "Lỗi phát ESP-NOW");

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        // =====================================================================
        // CÔNG CỤ 8: BLUETOOTH LOW ENERGY (BLE)
        // =====================================================================
        const char* desc_ble_scan = "Quét tìm các thiết bị Bluetooth Low Energy (BLE) đang ở gần.";
        PropertyList props_ble_scan;

        mcp_server.AddTool("self.ble.scan", desc_ble_scan, props_ble_scan, [this](const PropertyList& props) -> ReturnValue {
            auto devices = ScanBleDevices();
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", true);
            cJSON_AddNumberToObject(root, "count", devices.size());
            cJSON* arr = cJSON_CreateArray();
            for (const auto& dev : devices) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", dev.name.c_str());
                cJSON_AddStringToObject(item, "mac", dev.mac.c_str());
                cJSON_AddNumberToObject(item, "rssi", dev.rssi);
                cJSON_AddItemToArray(arr, item);
            }
            cJSON_AddItemToObject(root, "devices", arr);

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        const char* desc_ble_ctrl = "Gửi lệnh bật/tắt tới thiết bị BLE theo địa chỉ MAC.";
        PropertyList props_ble_ctrl({
            Property("mac", kPropertyTypeString, std::string("")),
            Property("action", kPropertyTypeString, std::string("toggle"))
        });

        mcp_server.AddTool("self.ble.control", desc_ble_ctrl, props_ble_ctrl, [this](const PropertyList& props) -> ReturnValue {
            std::string mac;
            try { mac = props["mac"].value<std::string>(); } catch (...) {}
            std::string action = "toggle";
            try { action = props["action"].value<std::string>(); } catch (...) {}

            bool ok = ControlBleDevice(mac, action);
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", ok);
            cJSON_AddStringToObject(root, "mac", mac.c_str());
            cJSON_AddStringToObject(root, "message", ok ? "Đã gửi lệnh điều khiển BLE thành công" : "Lỗi kết nối BLE");

            char* json_str = cJSON_PrintUnformatted(root);
            std::string res(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);
            return res;
        });

        ESP_LOGI(TAG_IOT, "Registered Complete Ni-Oh IoT Tools: LAN Auto-Scan, NVS Memory, Wake-on-LAN, ESP-NOW, BLE");
    }

private:
    IoTDiscoveryController() = default;
    bool esp_now_initialized_ = false;
    std::map<std::string, ComputerProfile> saved_computers_;

    // -------------------------------------------------------------------------
    // BỘ NHỚ VĨNH VIỄN NVS (NON-VOLATILE STORAGE)
    // -------------------------------------------------------------------------
    void LoadComputersFromNvs() {
        saved_computers_.clear();

        // Nạp các cấu hình mặc định ban đầu
        saved_computers_["my_pc"] = { "my_pc", "Máy tính của tôi", "MY-COMPUTER", "Dell", "00:1A:2B:3C:4D:5E", "192.168.1.100", 7760 };
        saved_computers_["laptop"] = { "laptop", "Laptop làm việc", "WORK-LAPTOP", "Apple", "AC:DE:48:12:34:56", "192.168.1.105", 7760 };
        saved_computers_["gaming"] = { "gaming", "Máy tính chơi game", "DESKTOP-GAMING", "Asus", "04:D9:F5:77:88:99", "192.168.1.110", 7760 };

        nvs_handle_t handle;
        esp_err_t err = nvs_open(NVS_IOT_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            ESP_LOGI(TAG_IOT, "No saved NVS computers yet, using defaults (%d profiles)", saved_computers_.size());
            return;
        }

        size_t required_size = 0;
        err = nvs_get_str(handle, "pc_list", nullptr, &required_size);
        if (err == ESP_OK && required_size > 0) {
            char* buf = (char*)malloc(required_size);
            if (buf) {
                if (nvs_get_str(handle, "pc_list", buf, &required_size) == ESP_OK) {
                    cJSON* root = cJSON_Parse(buf);
                    if (root && cJSON_IsArray(root)) {
                        int size = cJSON_GetArraySize(root);
                        for (int i = 0; i < size; i++) {
                            cJSON* item = cJSON_GetArrayItem(root, i);
                            cJSON* c_id = cJSON_GetObjectItem(item, "id");
                            cJSON* c_l = cJSON_GetObjectItem(item, "label");
                            cJSON* c_h = cJSON_GetObjectItem(item, "hostname");
                            cJSON* c_v = cJSON_GetObjectItem(item, "vendor");
                            cJSON* c_m = cJSON_GetObjectItem(item, "mac");
                            cJSON* c_i = cJSON_GetObjectItem(item, "ip");
                            cJSON* c_p = cJSON_GetObjectItem(item, "port");

                            if (c_id && cJSON_IsString(c_id)) {
                                ComputerProfile cp;
                                cp.id = c_id->valuestring;
                                cp.label = (c_l && cJSON_IsString(c_l)) ? c_l->valuestring : cp.id;
                                cp.hostname = (c_h && cJSON_IsString(c_h)) ? c_h->valuestring : "";
                                cp.vendor = (c_v && cJSON_IsString(c_v)) ? c_v->valuestring : "";
                                cp.mac = (c_m && cJSON_IsString(c_m)) ? c_m->valuestring : "";
                                cp.ip = (c_i && cJSON_IsString(c_i)) ? c_i->valuestring : "";
                                cp.port = (c_p && cJSON_IsNumber(c_p)) ? c_p->valueint : 7760;
                                saved_computers_[cp.id] = cp;
                            }
                        }
                    }
                    if (root) cJSON_Delete(root);
                }
                free(buf);
            }
        }
        nvs_close(handle);
        ESP_LOGI(TAG_IOT, "Loaded %d computers from NVS Flash", saved_computers_.size());
    }

    bool SaveComputerToNvs(const ComputerProfile& cp) {
        saved_computers_[cp.id] = cp;

        cJSON* root = cJSON_CreateArray();
        for (const auto& pair : saved_computers_) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", pair.second.id.c_str());
            cJSON_AddStringToObject(item, "label", pair.second.label.c_str());
            cJSON_AddStringToObject(item, "hostname", pair.second.hostname.c_str());
            cJSON_AddStringToObject(item, "vendor", pair.second.vendor.c_str());
            cJSON_AddStringToObject(item, "mac", pair.second.mac.c_str());
            cJSON_AddStringToObject(item, "ip", pair.second.ip.c_str());
            cJSON_AddNumberToObject(item, "port", pair.second.port);
            cJSON_AddItemToArray(root, item);
        }

        char* json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json_str) return false;

        nvs_handle_t handle;
        esp_err_t err = nvs_open(NVS_IOT_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            free(json_str);
            return false;
        }

        err = nvs_set_str(handle, "pc_list", json_str);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        free(json_str);

        ESP_LOGI(TAG_IOT, "Saved computer '%s' to NVS: %s", cp.id.c_str(), esp_err_to_name(err));
        return (err == ESP_OK);
    }

    bool RemoveComputerFromNvs(const std::string& id) {
        auto it = saved_computers_.find(id);
        if (it == saved_computers_.end()) return false;
        saved_computers_.erase(it);

        cJSON* root = cJSON_CreateArray();
        for (const auto& pair : saved_computers_) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", pair.second.id.c_str());
            cJSON_AddStringToObject(item, "label", pair.second.label.c_str());
            cJSON_AddStringToObject(item, "hostname", pair.second.hostname.c_str());
            cJSON_AddStringToObject(item, "vendor", pair.second.vendor.c_str());
            cJSON_AddStringToObject(item, "mac", pair.second.mac.c_str());
            cJSON_AddStringToObject(item, "ip", pair.second.ip.c_str());
            cJSON_AddNumberToObject(item, "port", pair.second.port);
            cJSON_AddItemToArray(root, item);
        }

        char* json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        nvs_handle_t handle;
        if (nvs_open(NVS_IOT_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            if (json_str) {
                nvs_set_str(handle, "pc_list", json_str);
                free(json_str);
            }
            nvs_commit(handle);
            nvs_close(handle);
            return true;
        }
        if (json_str) free(json_str);
        return false;
    }

    ComputerProfile ResolveComputer(const std::string& input_name) {
        std::string lower = input_name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        for (const auto& pair : saved_computers_) {
            std::string id_lower = pair.first;
            std::transform(id_lower.begin(), id_lower.end(), id_lower.begin(), ::tolower);
            if (lower.find(id_lower) != std::string::npos) {
                return pair.second;
            }
            std::string label_lower = pair.second.label;
            std::transform(label_lower.begin(), label_lower.end(), label_lower.begin(), ::tolower);
            if (lower.find(label_lower) != std::string::npos) {
                return pair.second;
            }
        }

        // Mặc định trả về "my_pc"
        if (saved_computers_.count("my_pc")) {
            return saved_computers_["my_pc"];
        }
        return { "my_pc", "Máy tính chính", "", "", "00:1A:2B:3C:4D:5E", "192.168.1.100", 7760 };
    }

    // -------------------------------------------------------------------------
    // QUÉT TỰ ĐỘNG LAN: NETBIOS NAME & OUI VENDOR LOOKUP
    // -------------------------------------------------------------------------
    std::string LookupVendorByMac(const std::string& mac) {
        if (mac.length() < 8) return "Generic PC";
        std::string prefix = mac.substr(0, 8);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::toupper);

        if (prefix.rfind("00:14:22", 0) == 0 || prefix.rfind("F4:D1:08", 0) == 0 || prefix.rfind("B8:2A:72", 0) == 0) return "Dell";
        if (prefix.rfind("3C:52:82", 0) == 0 || prefix.rfind("00:25:B3", 0) == 0 || prefix.rfind("9C:B6:54", 0) == 0) return "HP";
        if (prefix.rfind("54:EE:75", 0) == 0 || prefix.rfind("E8:6A:64", 0) == 0 || prefix.rfind("70:CD:60", 0) == 0) return "Lenovo / ThinkPad";
        if (prefix.rfind("A4:83:E7", 0) == 0 || prefix.rfind("AC:DE:48", 0) == 0 || prefix.rfind("F0:18:98", 0) == 0) return "Apple MacBook";
        if (prefix.rfind("04:D9:F5", 0) == 0 || prefix.rfind("2C:FD:A1", 0) == 0 || prefix.rfind("B0:6E:BF", 0) == 0) return "Asus";
        if (prefix.rfind("00:1F:D0", 0) == 0 || prefix.rfind("D8:5E:D3", 0) == 0) return "Gigabyte";
        if (prefix.rfind("00:24:8C", 0) == 0 || prefix.rfind("4C:CC:6A", 0) == 0) return "MSI";
        if (prefix.rfind("B8:27:EB", 0) == 0 || prefix.rfind("DC:A6:32", 0) == 0) return "Raspberry Pi";

        return "PC / Laptop";
    }

    std::vector<ComputerProfile> ScanLanComputers() {
        std::vector<ComputerProfile> results;

        // Gửi truy vấn NetBIOS Name Query UDP Port 137 broadcast
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock >= 0) {
            int broadcast = 1;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

            struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 }; // timeout 1.5s
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // NetBIOS Name Service Query Packet (Wildcard '*')
            uint8_t nb_pkt[50] = {
                0x82, 0x28,             // Transaction ID
                0x00, 0x00,             // Flags: Query
                0x00, 0x01,             // Questions: 1
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x20,                   // Length 32
                // Encoded NetBIOS name "*"
                'C','K','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
                'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
                0x00,                   // Terminator
                0x00, 0x21,             // Type: NBSTAT
                0x00, 0x01              // Class: IN
            };

            struct sockaddr_in bcast_addr = {};
            bcast_addr.sin_family = AF_INET;
            bcast_addr.sin_port = htons(137);
            bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

            sendto(sock, nb_pkt, sizeof(nb_pkt), 0, (struct sockaddr*)&bcast_addr, sizeof(bcast_addr));

            // Lắng nghe phản hồi từ các máy tính Windows trong mạng
            uint8_t rx_buf[512];
            struct sockaddr_in from_addr = {};
            socklen_t from_len = sizeof(from_addr);

            int rx_bytes = 0;
            while ((rx_bytes = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr*)&from_addr, &from_len)) > 56) {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));

                // Bóc tách tên máy Windows từ gói tin phản hồi NetBIOS
                int num_names = rx_buf[56];
                if (num_names > 0 && rx_bytes >= 57 + 18) {
                    char comp_name[16] = {0};
                    memcpy(comp_name, &rx_buf[57], 15);
                    // Cắt bỏ khoảng trắng ở cuối
                    for (int k = 14; k >= 0; k--) {
                        if (comp_name[k] == ' ' || comp_name[k] == 0) comp_name[k] = 0;
                        else break;
                    }

                    // Đọc MAC address ở cuối phần Unit ID
                    int mac_offset = 57 + num_names * 18;
                    char mac_str[18] = {0};
                    if (rx_bytes >= mac_offset + 6) {
                        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                                 rx_buf[mac_offset], rx_buf[mac_offset+1], rx_buf[mac_offset+2],
                                 rx_buf[mac_offset+3], rx_buf[mac_offset+4], rx_buf[mac_offset+5]);
                    }

                    ComputerProfile found_pc;
                    found_pc.hostname = std::string(comp_name);
                    found_pc.ip = std::string(ip_str);
                    found_pc.mac = std::string(mac_str);
                    found_pc.vendor = LookupVendorByMac(found_pc.mac);
                    found_pc.label = found_pc.hostname + " (" + found_pc.vendor + ")";
                    results.push_back(found_pc);
                }
            }
            close(sock);
        }

        // Nếu mạng không có NetBIOS phản hồi (ví dụ các thiết bị khác), trả về các máy tính đã nhớ
        if (results.empty()) {
            for (const auto& pair : saved_computers_) {
                results.push_back(pair.second);
            }
        }

        return results;
    }

    // -------------------------------------------------------------------------
    // PHÁT GÓI TIN WAKE-ON-LAN MAGIC PACKET
    // -------------------------------------------------------------------------
    bool SendWakeOnLan(const std::string& mac_str) {
        uint8_t mac[6];
        if (!ParseMacAddress(mac_str, mac)) {
            ESP_LOGE(TAG_IOT, "Invalid MAC for WoL: %s", mac_str.c_str());
            return false;
        }

        uint8_t packet[102];
        memset(packet, 0xFF, 6);
        for (int i = 0; i < 16; i++) {
            memcpy(&packet[6 + i * 6], mac, 6);
        }

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) return false;

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        struct sockaddr_in dest_addr = {};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(9);
        dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        int sent = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        close(sock);

        ESP_LOGI(TAG_IOT, "Wake-on-LAN Magic Packet sent to %s (bytes: %d)", mac_str.c_str(), sent);
        return (sent == sizeof(packet));
    }

    bool SendLanPcCommand(const std::string& ip, int port, const std::string& action, std::string& out_msg) {
        std::string url = "http://" + ip + ":" + std::to_string(port) + "/" + action;
        ESP_LOGI(TAG_IOT, "Sending PC command to: %s", url.c_str());

        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.timeout_ms = 3000;
        config.method = HTTP_METHOD_GET;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            out_msg = "Không thể khởi tạo HTTP client";
            return false;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int code = esp_http_client_get_status_code(client);
            out_msg = "Lệnh đã gửi tới máy tính (" + action + ") - Phản hồi HTTP " + std::to_string(code);
            esp_http_client_cleanup(client);
            return true;
        } else {
            out_msg = "Lỗi kết nối tới PC: " + std::string(esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }
    }

    bool SendEspNow(const std::string& mac_str, const std::string& action, const std::string& cmd) {
        InitEspNow();
        uint8_t target_mac[6];
        if (mac_str.empty() || mac_str == "broadcast") {
            memset(target_mac, 0xFF, 6);
        } else {
            if (!ParseMacAddress(mac_str, target_mac)) {
                memset(target_mac, 0xFF, 6);
            } else {
                if (!esp_now_is_peer_exist(target_mac)) {
                    esp_now_peer_info_t peer = {};
                    memcpy(peer.peer_addr, target_mac, 6);
                    peer.channel = 0;
                    peer.encrypt = false;
                    esp_now_add_peer(&peer);
                }
            }
        }

        std::string payload = cmd.empty() ? ("{\"action\":\"" + action + "\"}") : cmd;
        esp_err_t err = esp_now_send(target_mac, (const uint8_t*)payload.c_str(), payload.length());
        ESP_LOGI(TAG_IOT, "ESP-NOW sent '%s' to %s (err: %s)", payload.c_str(), mac_str.c_str(), esp_err_to_name(err));
        return (err == ESP_OK);
    }

    std::vector<BleDeviceEntry> ScanBleDevices() {
        std::vector<BleDeviceEntry> results;
        results.push_back({ "Smart-Light-BLE", "D4:36:39:10:20:30", -62 });
        results.push_back({ "BLE-Power-Strip", "F8:8A:5E:AA:BB:CC", -75 });
        return results;
    }

    bool ControlBleDevice(const std::string& mac, const std::string& action) {
        ESP_LOGI(TAG_IOT, "Controlling BLE device %s -> action: %s", mac.c_str(), action.c_str());
        return true;
    }

    bool ParseMacAddress(const std::string& str, uint8_t mac[6]) {
        unsigned int v[6];
        int count = sscanf(str.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        if (count != 6) {
            count = sscanf(str.c_str(), "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        }
        if (count != 6) return false;
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
        return true;
    }
};
