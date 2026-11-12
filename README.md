# Xiaozhi-VN ZClaw Edition (ESP32-S3 SuperMini 4MB)

Firmware trợ lý AI giọng nói tiếng Việt chạy hoàn toàn trực tiếp tới Cloud AI trên **ESP32-S3 SuperMini (4MB Flash, tự nhận diện PSRAM động)** — loại bỏ server `xiaozhi.me` và phân vùng OTA mặc định.

## Tính năng chính
- **Direct Cloud STT / LLM / TTS:**
  - **STT:** Groq Whisper (`whisper-large-v3-turbo`, `language=vi`) streaming HTTPS `multipart/form-data`.
  - **LLM & Function Calling:** Tương thích trực tiếp Groq / OpenAI / DeepSeek / Gemini API + Sliding Window + tag cảm xúc đầu câu.
  - **TTS Free Tiếng Việt:** Microsoft Edge TTS WebSocket (`vi-VN-HoaiMyNeural` / `vi-VN-NamMinhNeural`) tự động dự phòng Google Translate TTS (`tl=vi`), giải mã MP3 trực tiếp trên RAM bằng `minimp3`.
- **Phát Nhạc YouTube / SoundCloud & Xem Video Trực Tuyến 240x240 trên mạch:**
  - **Nhạc YouTube & SoundCloud & Radio VOV (`phat_nhac_truc_tuyen`):**
    - Hỗ trợ tìm kiếm & lấy MP3 progressive qua SoundCloud API v2, Piped YouTube API, danh sách VOV1/VOV2/VOV3/VOV Giao thông hoặc **Custom Media Proxy (`yt-dlp`/`ffmpeg`)**.
    - Giải mã MP3 trên RAM qua `minimp3` ra loa MAX98357A, hiển thị banner `[YouTube]` / `[SoundCloud]` + tên bài hát, mắt `[vui_ve]` và 11 cột sóng âm nhảy theo nhịp.
  - **Xem Video Trực Tuyến ST7789 (`xem_video_truc_tuyen`):**
    - Giải mã luồng `multipart/x-mixed-replace` MJPEG (`240x240` / `240x180`, 12–15 FPS) bằng `espressif/esp_jpeg` (RGB565 Big-Endian), đẩy qua DMA strip lên ST7789 và hỗ trợ phần `audio/mpeg` đi kèm.
    - Hiển thị thanh tiêu đề 3 giây đầu ở cạnh dưới; nhấn `BTN_MAIN` (GPIO 3) 1 lần để thoát Video hoặc dừng Nhạc ngay lập tức.
- **10 Công cụ MCP 100% Tiếng Việt (`McpRegistry`):**
  1. `xem_thoi_tiet` (Open-Meteo + cập nhật Clock Dashboard)
  2. `xem_thoi_gian_lich_am` (NTP `ICT-7` + Âm lịch Hồ Ngọc Đức + Can Chi + Giờ Hoàng Đạo)
  3. `doc_tin_tuc` (RSS VNExpress)
  4. `tra_cuu_thong_tin` (Wikipedia Tiếng Việt REST API)
  5. `phat_nhac_truc_tuyen` (YouTube / SoundCloud / Radio VOV)
  6. `xem_video_truc_tuyen` (MJPEG Video Streaming trực tiếp 240x240 / 240x180 trên ST7789)
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

## Cấu hình Web Captive Portal & Media Proxy

1. Kết nối Wi-Fi `Xiaozhi-ZClaw-VN` -> truy cập `http://192.168.4.1` (hoặc giữ `BTN_MAIN` 5 giây).
2. Điền thông tin Wi-Fi, Groq/LLM API Key và tùy chọn **Máy chủ Stream Nhạc/Video (`media_proxy_url`)** / **SoundCloud Client ID**.
3. Nếu chạy Media Proxy (`yt-dlp` + `ffmpeg`) trên PC/VPS nội bộ, firmware sẽ gọi tự động:
   - Nhạc: `GET <media_proxy_url>/stream?q=<từ_khóa_hoặc_id>&source=youtube|soundcloud` (trả luồng `audio/mpeg` MP3)
   - Video: `GET <media_proxy_url>/video?q=<từ_khóa_hoặc_id>&source=youtube&res=240x240` (trả luồng `multipart/x-mixed-replace` MJPEG, tùy chọn chèn chunk `Content-Type: audio/mpeg`).

## Build & Flash (ESP-IDF v5.x)

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```
