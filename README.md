# Xiaozhi-VN ZClaw Edition (ESP32-S3 SuperMini 4MB)

Firmware trợ lý AI giọng nói tiếng Việt chạy hoàn toàn trực tiếp tới Cloud AI trên **ESP32-S3 SuperMini (4MB Flash, tự nhận diện PSRAM động)** — loại bỏ server `xiaozhi.me` và phân vùng OTA mặc định.

## Tính năng chính
- **Direct Cloud STT / LLM / TTS:**
  - **STT:** Groq Whisper (`whisper-large-v3-turbo`, `language=vi`) streaming HTTPS `multipart/form-data`.
  - **LLM & Function Calling:** Tương thích trực tiếp Groq / OpenAI / DeepSeek / Gemini API + Sliding Window + tag cảm xúc đầu câu.
  - **TTS & Radio Stream:** Microsoft Edge TTS WebSocket (`vi-VN-HoaiMyNeural` / `vi-VN-NamMinhNeural`) tự động dự phòng Google Translate TTS (`tl=vi`), giải mã MP3 trực tiếp trên RAM bằng `minimp3`.
- **10 Công cụ MCP 100% Tiếng Việt (`McpRegistry`):**
  1. `xem_thoi_tiet` (Open-Meteo + cập nhật Clock Dashboard)
  2. `xem_thoi_gian_lich_am` (NTP `ICT-7` + Âm lịch Hồ Ngọc Đức + Can Chi + Giờ Hoàng Đạo)
  3. `doc_tin_tuc` (RSS VNExpress)
  4. `tra_cuu_thong_tin` (Wikipedia Tiếng Việt REST API)
  5. `phat_nhac_radio` (VOV1/2/3/Giao thông & URL MP3)
  6. `mo_video` (Chuyển qua BLE Chronos / ESP-NOW)
  7. `suy_luan_cao` (Lượt suy luận sâu LLM)
  8. `dieu_khien_thiet_bi` (Loa, Mic Gain, Độ sáng, Xoay LCD)
  9. `gui_lenh_esp_now` (Điều khiển & nhắn tin ESP-NOW)
  10. `hen_gio_pomodoro` (Hẹn giờ + chuông + TTS nhắc việc)
- **Giao diện ST7789 240x240 (LVGL v8.4):** Đôi mắt vuông bo tròn xanh dương 7 trạng thái biểu cảm + 11 thanh sóng âm Goertzel thời gian thực + màn hình Đồng hồ/Âm lịch/Thời tiết tự chuyển sau 5 giây `IDLE`.
- **Kết nối đồng cư sóng 2.4GHz (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`):** Wi-Fi STA/Captive Portal (`Xiaozhi-ZClaw-VN`) + NimBLE Chronos (Nordic UART Service) + ESP-NOW 2 chiều.

## Sơ đồ chân hàn cố định (`board_config.h`)

| Thiết bị | Chân tín hiệu | GPIO | Ghi chú |
|---|---|---|---|
| **ST7789 1.54"** | `MOSI` / `SCLK` | `9` / `10` | `SPI3_HOST` |
| | `DC` / `RST` / `BLK` | `7` / `8` / `6` | `BLK` điều khiển LEDC PWM |
| | `CS` | `-1` (`NC`) | Nối GND cứng, bắt buộc `spi_mode = 3` |
| **INMP441 Mic** | `BCLK` / `WS` / `DIN` | `11` / `12` / `13` | `I2S_NUM_0` RX Master (`16kHz`) |
| **MAX98357A Loa** | `BCLK` / `WS` / `DOUT` | `1` / `2` / `44` | `I2S_NUM_1` TX Master (tắt UART0 trên GPIO 44) |
| **Nút nhấn** | `BTN_MAIN` / `VOL+` / `VOL-` | `3` / `4` / `5` | Active LOW (`BUTTON_ACTIVE_LEVEL = 0`), Debounce `30ms` |

## Build & Flash (ESP-IDF v5.x)

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Mở cổng cấu hình Web Tiếng Việt lần đầu: kết nối Wi-Fi `Xiaozhi-ZClaw-VN` -> `http://192.168.4.1` (hoặc giữ `BTN_MAIN` 5 giây).
