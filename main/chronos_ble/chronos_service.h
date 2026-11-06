#pragma once

#include <string>
#include <functional>
#include <esp_err.h>

class ChronosService {
public:
    struct Notification {
        std::string app_name;
        std::string title;
        std::string message;
        uint8_t icon_id;
    };

    struct MusicInfo {
        std::string title;
        std::string artist;
        std::string album;
        uint8_t state; // 0 = stopped, 1 = playing, 2 = paused
        uint32_t duration_sec;
        uint32_t progress_sec;
    };

    using NotificationCallback = std::function<void(const Notification&)>;
    using MusicCallback = std::function<void(const MusicInfo&)>;
    using BatteryCallback = std::function<void(uint8_t level)>;

    static ChronosService& GetInstance();

    esp_err_t Start(const std::string& device_name = "Xiaozhi S3");
    void Stop();

    bool IsConnected() const { return connected_; }
    uint8_t GetPhoneBattery() const { return phone_battery_; }

    void SetNotificationCallback(NotificationCallback cb) { on_notification_ = cb; }
    void SetMusicCallback(MusicCallback cb) { on_music_ = cb; }
    void SetBatteryCallback(BatteryCallback cb) { on_battery_ = cb; }

    // Music remote control commands (ESP32 -> Phone)
    void SendMusicControl(uint8_t action); // 1 = Play/Pause, 2 = Next, 3 = Prev, 4 = Vol Up, 5 = Vol Down

    // Process incoming packet from Chronos App
    void HandlePacket(const uint8_t* data, size_t length);

private:
    ChronosService() = default;
    ~ChronosService() = default;

    bool connected_ = false;
    uint8_t phone_battery_ = 0;
    std::string device_name_ = "Xiaozhi S3";

    NotificationCallback on_notification_ = nullptr;
    MusicCallback on_music_ = nullptr;
    BatteryCallback on_battery_ = nullptr;

    void ParseNotification(const uint8_t* data, size_t len);
    void ParseTime(const uint8_t* data, size_t len);
    void ParseBattery(const uint8_t* data, size_t len);
    void ParseMusic(const uint8_t* data, size_t len);
};

