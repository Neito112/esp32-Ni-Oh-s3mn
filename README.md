# S.W.Ni-Oh (ESP32-S3 SuperMini 4MB)

Firmware & Simulator trợ lý AI giọng nói tiếng Việt **S.W.Ni-Oh** chạy hoàn toàn trực tiếp tới Cloud AI trên **ESP32-S3 SuperMini (4MB Flash, tự nhận diện PSRAM động)** — không phụ thuộc server trung gian bên thứ 3, loại bỏ phân vùng OTA mặc định và tổ chức theo chuẩn **ESP-IDF Modular Components**.

## Cấu trúc dự án (`components/`, `main/`, `simulator/`)

```text
.
├── CMakeLists.txt             # project(sw_nioh) -> build/sw_nioh.bin & build/sw_nioh.elf
├── partitions.csv             # Phân vùng 4MB tối ưu (3.2MB factory, không OTA)
├── sdkconfig.defaults         # BLE GAP "S.W.Ni-Oh", USB Serial/JTAG (nhường GPIO 44), Coex 2.4GHz
├── wokwi.toml                 # Cấu hình Wokwi Simulator trỏ build/sw_nioh.bin & .elf
├── diagram.json               # Sơ đồ mạch ESP32-S3 + ST7789 + INMP441 + MAX98357A + 3 nút nhấn
├── components/
│   ├── board_supermini/       # Chân hàn cố định, helper PSRAM & giải phóng GPIO 44
│   │   ├── CMakeLists.txt
│   │   └── include/board_config.h
│   ├── minimp3/               # Header-only MP3 decoder trên RAM (tự tải minimp3.h qua CMake)
│   │   ├── CMakeLists.txt
│   │   └── include/minimp3.h
│   ├── audio_hal/             # Driver I2S INMP441 RX + MAX98357A TX, AGC/VAD, 11-band Goertzel & nút nhấn
│   │   ├── CMakeLists.txt
│   │   ├── audio_hal.cpp
│   │   └── include/audio_hal.h
│   ├── ui_eyes_clock/         # ST7789 240x240 SPI3_HOST + LVGL v8.4 (Mắt vuông xanh dương, Sóng âm, Đồng hồ/Âm lịch, Video Mode)
│   │   ├── CMakeLists.txt
│   │   ├── ui_eyes_clock.cpp
│   │   ├── vn_font_16.c
│   │   └── include/
│   │       ├── ui_eyes_clock.h
│   │       └── vn_fonts.h
│   ├── chronos_ble/           # NimBLE GATT Server "S.W.Ni-Oh" tương thích app Chronos (NUS Time Sync, Thông báo, Google Maps)
│   │   ├── CMakeLists.txt
│   │   ├── chronos_ble.cpp
│   │   └── include/chronos_ble.h
│   ├── espnow_mesh/           # Giao tiếp ESP-NOW 2 chiều (PTT walkie-talkie, ALERT/CONTROL, sync đồng hồ)
│   │   ├── CMakeLists.txt
│   │   ├── espnow_comm.cpp
│   │   └── include/espnow_comm.h
│   ├── nioh_agent/            # Direct STT/LLM/TTS + Web Captive Portal ("S.W.Ni-Oh-Setup") + MP3/MJPEG Streaming (ROM TJpgDec)
│   │   ├── CMakeLists.txt
│   │   ├── zclaw_llm_client.cpp
│   │   └── include/zclaw_llm_client.h
│   └── mcp_vietnam/           # McpRegistry với 10 công cụ MCP 100% Tiếng Việt (tích hợp callback vào nioh_agent)
│       ├── CMakeLists.txt
│       ├── mcp_vietnam_tools.cpp
│       └── include/mcp_vietnam_tools.h
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── main.cpp               # Khởi tạo hệ thống, Wi-Fi/NTP & vòng lặp FSM trạng thái S.W.Ni-Oh
└── simulator/                 # S.W.Ni-Oh Desktop/Web Simulator (chạy thật Mic/Loa PC + Cloud AI + BLE/ESP-NOW + MJPEG)
    ├── server.py
    ├── index.html
    ├── sim_engine.js
    └── style.css
```

## Tính năng chính
- **Direct Cloud STT / LLM / TTS:**
  - **STT:** Groq Whisper (`whisper-large-v3-turbo`, `language=vi`) streaming HTTPS `multipart/form-data`.
  - **LLM & Function Calling:** Tương thích trực tiếp Groq / OpenAI / DeepSeek / Gemini API + Sliding Window + tag cảm xúc đầu câu (`McpRegistry` gắn qua callback tách rời để tránh circular dependency).
  - **TTS Free Tiếng Việt:** Microsoft Edge TTS WebSocket (`vi-VN-HoaiMyNeural` / `vi-VN-NamMinhNeural`) tự động dự phòng Google Translate TTS (`tl=vi`), giải mã MP3 trực tiếp trên RAM bằng `minimp3`.
- **Phát Nhạc YouTube / SoundCloud & Xem Video Trực Tuyến 240x240 trên mạch:**
  - **Nhạc YouTube & SoundCloud & Radio VOV (`phat_nhac_truc_tuyen`):**
    - Hỗ trợ tìm kiếm & lấy MP3 progressive qua SoundCloud API v2, Piped YouTube API, danh sách VOV1/VOV2/VOV3/VOV Giao thông hoặc **Custom Media Proxy (`yt-dlp`/`ffmpeg`)**.
    - Giải mã MP3 trên RAM qua `minimp3` ra loa MAX98357A, hiển thị banner `[YouTube]` / `[SoundCloud]` + tên bài hát, mắt `[vui_ve]` và 11 cột sóng âm nhảy theo nhịp.
  - **Xem Video Trực Tuyến ST7789 (`xem_video_truc_tuyen`):**
    - Giải mã luồng `multipart/x-mixed-replace` MJPEG (`240x240` / `240x180`, 12–15 FPS) bằng **ESP32-S3 ROM TJpgDec (`esp_rom_tjpgd.h`)** (0 byte Flash phụ trợ), chuyển đổi sang RGB565 Big-Endian, đẩy qua DMA strip lên ST7789 và hỗ trợ phần `audio/mpeg` đi kèm.
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
- **Giao diện ST7789 240x240 (LVGL v8.4):** Đôi mắt vuông bo tròn xanh dương (`56x56px`, `radius = 15px`, `#00B4FF` -> `#00E5FF`) đủ 7 trạng thái biểu cảm + 11 thanh sóng âm Goertzel thời gian thực + màn hình Đồng hồ/Âm lịch/Thời tiết tự chuyển sau 5 giây `IDLE`.
- **Kết nối đồng cư sóng 2.4GHz (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`):** Wi-Fi STA/Captive Portal (`S.W.Ni-Oh-Setup`) + NimBLE Chronos (`S.W.Ni-Oh`, Nordic UART Service) + ESP-NOW 2 chiều.

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

1. Kết nối Wi-Fi `S.W.Ni-Oh-Setup` -> truy cập `http://192.168.4.1` (hoặc giữ `BTN_MAIN` 5 giây).
2. Điền thông tin Wi-Fi, Groq/LLM API Key và tùy chọn **Máy chủ Stream Nhạc/Video (`media_proxy_url`)** / **SoundCloud Client ID**.
3. Nếu chạy Media Proxy (`yt-dlp` + `ffmpeg`) trên PC/VPS nội bộ, firmware sẽ gọi tự động:
   - Nhạc: `GET <media_proxy_url>/stream?q=<từ_khóa_hoặc_id>&source=youtube|soundcloud` (trả luồng `audio/mpeg` MP3)
   - Video: `GET <media_proxy_url>/video?q=<từ_khóa_hoặc_id>&source=youtube&res=240x240` (trả luồng `multipart/x-mixed-replace` MJPEG, tùy chọn chèn chunk `Content-Type: audio/mpeg`).

## Build & Flash Firmware (ESP-IDF v5.x)

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Sản phẩm biên dịch xuất ra tại `build/sw_nioh.bin` và `build/sw_nioh.elf`.

## Chạy S.W.Ni-Oh Simulator & Wokwi

### 1. S.W.Ni-Oh Desktop/Web Simulator (Mic/Loa PC + Cloud AI thật)
```powershell
python simulator/server.py
```
Sau đó mở trình duyệt tại `http://127.0.0.1:8765` (hoặc `--port 8765`) để trải nghiệm:
- Màn hình ST7789 `240x240` pixel-perfect (7 biểu cảm mắt, 11 cột sóng âm, Đồng hồ + Âm lịch Hồ Ngọc Đức, trình phát Nhạc & Video MJPEG `240x240`).
- 3 nút vật lý `BTN_MAIN` (GPIO 3, phím `Space`), `VOL+` (GPIO 4, phím `ArrowUp`), `VOL-` (GPIO 5, phím `ArrowDown`) đúng logic Debounce `30ms` và giữ `1.6s` / `5.0s`.
- Kết nối Mic/Loa máy tính chạy trọn vẹn: VAD/Thu âm -> Groq Whisper STT -> Direct LLM + 10 MCP Tools Tiếng Việt -> Edge/Google TTS.
- Bảng điều khiển gửi gói BLE Chronos (Zalo/SMS, Google Maps Turn-by-Turn), gói ESP-NOW 2 chiều (`ALERT`, `CONTROL`), USB Serial/JTAG Monitor và công cụ kiểm tra `build/sw_nioh.bin`.

### 2. Wokwi for VS Code / Wokwi CLI
Mở thư mục dự án trong VS Code cài extension **Wokwi Simulator** (hoặc chạy `wokwi-cli`) với cặp file `wokwi.toml` và `diagram.json` ở thư mục gốc.
