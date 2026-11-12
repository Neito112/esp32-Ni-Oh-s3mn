#pragma once

/**
 * @file audio_hal.h
 * @brief Audio Hardware Abstraction Layer cho S.W.Ni-Oh
 *
 * Quản lý đồng thời 2 bus I2S độc lập theo chuẩn ESP-IDF v5.x (driver/i2s_std.h):
 *  - I2S_NUM_0 (RX Master): Microphone INMP441 (GPIO 11/12/13), 16kHz, khuếch đại 1.0x-5.0x + chống clipping, DC-blocker & VAD
 *  - I2S_NUM_1 (TX Master): Amplifier MAX98357A (GPIO 1/2/44), Volume 0-100% đường cong logarit, đổi sample rate động
 *  - Tích hợp bộ trích xuất phổ sóng âm 11 dải tần thời gian thực cho giao diện LVGL
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_config.h"

namespace zclaw {

class AudioHal {
public:
    static constexpr size_t kWaveformBands = 11;
    static constexpr uint32_t kMicSampleRateHz = 16000;
    static constexpr uint32_t kDefaultSpeakerSampleRateHz = 24000;
    static constexpr size_t kMicFrameSamples = 320;   // 20ms @ 16kHz
    static constexpr size_t kTxChunkSamples = 1152;   // 1 frame MP3 tối đa
    static constexpr size_t kWavHeaderBytes = 44;

    enum class AudioSource : uint8_t {
        NONE = 0,
        MIC_RX = 1,
        SPK_TX = 2,
    };

    enum class ChimeType : uint8_t {
        WAKE = 0,
        DONE = 1,
        ERROR = 2,
        ALERT = 3,
        POMODORO = 4,
    };

    struct VadConfig {
        uint16_t min_speech_rms = 420;        // Ngưỡng RMS tối thiểu kích hoạt tiếng nói
        float speech_snr_ratio = 2.4f;        // Tỷ lệ vượt nền nhiễu (adaptive noise floor)
        uint32_t pre_roll_ms = 240;           // Lưu đệm trước khi phát hiện giọng để không mất âm đầu
        uint32_t silence_timeout_ms = 1100;   // Thời gian im lặng sau khi nói để tự dừng thu
        uint32_t no_speech_timeout_ms = 4500; // Thời gian chờ tối đa nếu người dùng không nói gì
        uint32_t max_record_ms_psram = 15000; // Giới hạn thu âm khi có PSRAM
        uint32_t max_record_ms_sram = 5500;   // Giới hạn thu âm an toàn khi chỉ có SRAM
    };

    /**
     * @brief Kết quả thu âm hoàn chỉnh đã đóng gói sẵn RIFF/WAV 44-byte header ở đầu buffer.
     *        Tự giải phóng qua board_free() khi hủy để tránh rò rỉ bộ nhớ.
     */
    struct CapturedAudio {
        uint8_t* wav_data = nullptr;
        size_t wav_size = 0;
        size_t pcm_samples = 0;
        uint32_t duration_ms = 0;
        bool speech_detected = false;
        bool stopped_by_user = false;

        CapturedAudio() = default;
        ~CapturedAudio() { release(); }

        CapturedAudio(const CapturedAudio&) = delete;
        CapturedAudio& operator=(const CapturedAudio&) = delete;

        CapturedAudio(CapturedAudio&& other) noexcept
            : wav_data(other.wav_data),
              wav_size(other.wav_size),
              pcm_samples(other.pcm_samples),
              duration_ms(other.duration_ms),
              speech_detected(other.speech_detected),
              stopped_by_user(other.stopped_by_user) {
            other.wav_data = nullptr;
            other.wav_size = 0;
            other.pcm_samples = 0;
            other.duration_ms = 0;
            other.speech_detected = false;
            other.stopped_by_user = false;
        }

        CapturedAudio& operator=(CapturedAudio&& other) noexcept {
            if (this != &other) {
                release();
                wav_data = other.wav_data;
                wav_size = other.wav_size;
                pcm_samples = other.pcm_samples;
                duration_ms = other.duration_ms;
                speech_detected = other.speech_detected;
                stopped_by_user = other.stopped_by_user;
                other.wav_data = nullptr;
                other.wav_size = 0;
                other.pcm_samples = 0;
                other.duration_ms = 0;
                other.speech_detected = false;
                other.stopped_by_user = false;
            }
            return *this;
        }

        void release() {
            if (wav_data != nullptr) {
                board_free(wav_data);
                wav_data = nullptr;
            }
            wav_size = 0;
            pcm_samples = 0;
            duration_ms = 0;
            speech_detected = false;
            stopped_by_user = false;
        }
    };

    struct WaveformSnapshot {
        std::array<uint8_t, kWaveformBands> bands{};  // Biên độ chuẩn hóa 0..100
        uint16_t rms = 0;
        AudioSource active_source = AudioSource::NONE;
    };

    static AudioHal& instance();

    AudioHal(const AudioHal&) = delete;
    AudioHal& operator=(const AudioHal&) = delete;

    /**
     * @brief Khởi tạo đồng thời I2S_NUM_0 (INMP441) và I2S_NUM_1 (MAX98357A).
     *        Tự động gọi board_release_gpio44() trước khi khởi tạo I2S TX.
     */
    esp_err_t init();
    void deinit();
    bool is_initialized() const { return initialized_; }

    // --- Điều khiển Âm lượng Loa (0..100%) ---
    esp_err_t set_speaker_volume(uint8_t percent, bool persist_nvs = true);
    uint8_t adjust_speaker_volume(int8_t delta_percent, bool persist_nvs = true);
    uint8_t get_speaker_volume() const { return speaker_volume_.load(std::memory_order_relaxed); }
    esp_err_t set_muted(bool muted);
    bool is_muted() const { return muted_.load(std::memory_order_relaxed); }

    // --- Điều khiển Độ nhạy Mic (1.0x .. 5.0x / mức 1..10) ---
    esp_err_t set_mic_gain(float gain, bool persist_nvs = true);
    esp_err_t set_mic_sensitivity_level(uint8_t level_1_to_10, bool persist_nvs = true);
    float get_mic_gain() const { return mic_gain_.load(std::memory_order_relaxed); }
    uint8_t get_mic_sensitivity_level() const;

    // --- Thu âm INMP441 & VAD ---
    esp_err_t read_mic_frame(int16_t* out_pcm,
                             size_t max_samples,
                             size_t* out_samples,
                             uint16_t* out_rms,
                             TickType_t timeout_ticks = pdMS_TO_TICKS(100));

    esp_err_t record_utterance_with_vad(const VadConfig& cfg, CapturedAudio& out_audio);
    void request_stop_capture() { capture_stop_requested_.store(true, std::memory_order_release); }
    void clear_stop_capture() { capture_stop_requested_.store(false, std::memory_order_release); }
    bool is_capture_stop_requested() const {
        return capture_stop_requested_.load(std::memory_order_acquire);
    }

    // --- Phát âm thanh MAX98357A ---
    esp_err_t set_speaker_sample_rate(uint32_t sample_rate_hz);
    uint32_t get_speaker_sample_rate() const { return tx_sample_rate_hz_; }

    esp_err_t write_speaker(const int16_t* pcm,
                            size_t num_samples_per_ch,
                            uint8_t channels,
                            uint32_t sample_rate_hz,
                            TickType_t timeout_ticks = pdMS_TO_TICKS(500));

    esp_err_t play_chime(ChimeType type);
    void flush_speaker_silence(uint32_t duration_ms = 60);
    void request_stop_playback() { playback_stop_requested_.store(true, std::memory_order_release); }
    void clear_stop_playback() { playback_stop_requested_.store(false, std::memory_order_release); }
    bool is_playback_stop_requested() const {
        return playback_stop_requested_.load(std::memory_order_acquire);
    }

    // --- Dữ liệu Sóng âm Thời gian thực cho UI LVGL ---
    WaveformSnapshot get_waveform_snapshot() const;

    // --- Lưu/Đọc cấu hình NVS ---
    void load_nvs_settings();
    void save_nvs_settings() const;

    // --- Tiện ích đóng gói WAV Header ---
    static void write_wav_header(uint8_t* dst_44_bytes,
                                 uint32_t pcm_data_bytes,
                                 uint32_t sample_rate_hz = kMicSampleRateHz,
                                 uint16_t channels = 1,
                                 uint16_t bits_per_sample = 16);

private:
    AudioHal() = default;
    ~AudioHal();

    esp_err_t init_mic_i2s();
    esp_err_t init_speaker_i2s(uint32_t sample_rate_hz);
    void update_waveform_from_pcm(const int16_t* mono_pcm,
                                  size_t sample_count,
                                  uint32_t sample_rate_hz,
                                  AudioSource source);
    void decay_waveform_to_idle();
    static uint16_t compute_rms_s16(const int16_t* samples, size_t count);
    static uint32_t volume_percent_to_q15(uint8_t percent);

    bool initialized_ = false;
    i2s_chan_handle_t rx_chan_ = nullptr;
    i2s_chan_handle_t tx_chan_ = nullptr;

    SemaphoreHandle_t rx_mutex_ = nullptr;
    SemaphoreHandle_t tx_mutex_ = nullptr;
    mutable portMUX_TYPE vis_spinlock_ = portMUX_INITIALIZER_UNLOCKED;

    int32_t* rx_raw_dma_buf_ = nullptr;
    int16_t* tx_dma_buf_ = nullptr;

    uint32_t tx_sample_rate_hz_ = kDefaultSpeakerSampleRateHz;
    std::atomic<uint8_t> speaker_volume_{85};
    std::atomic<bool> muted_{false};
    std::atomic<float> mic_gain_{2.5f};

    std::atomic<bool> capture_stop_requested_{false};
    std::atomic<bool> playback_stop_requested_{false};

    // Trạng thái bộ lọc DC-blocker cho INMP441
    float dc_prev_x_ = 0.0f;
    float dc_prev_y_ = 0.0f;

    // Trạng thái phổ sóng âm cho UI (được bảo vệ bởi vis_spinlock_)
    std::array<float, kWaveformBands> vis_bands_smooth_{};
    uint16_t vis_last_rms_ = 0;
    AudioSource vis_active_source_ = AudioSource::NONE;
    int64_t vis_last_update_us_ = 0;
};

}  // namespace zclaw

