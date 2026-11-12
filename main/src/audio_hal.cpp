#include "audio_hal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

namespace zclaw {

namespace {

constexpr const char* TAG = "AudioHal";
constexpr const char* kNvsKeyVolume = "spk_vol";
constexpr const char* kNvsKeyMicGainMilli = "mic_gain_x1000";
constexpr float kPi = 3.14159265358979323846f;

// 6 tần số trung tâm (Hz) cho bộ lọc Goertzel trích xuất phổ giọng nói & nhạc
constexpr std::array<float, 6> kGoertzelFreqsHz = {
    240.0f, 520.0f, 950.0f, 1750.0f, 2900.0f, 4600.0f
};

inline int16_t clamp_s16(int32_t val)
{
    if (val > 32767) {
        return 32767;
    }
    if (val < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(val);
}

inline uint8_t clamp_u8(int32_t val, uint8_t lo, uint8_t hi)
{
    if (val < static_cast<int32_t>(lo)) {
        return lo;
    }
    if (val > static_cast<int32_t>(hi)) {
        return hi;
    }
    return static_cast<uint8_t>(val);
}

}  // namespace

AudioHal& AudioHal::instance()
{
    static AudioHal s_instance;
    return s_instance;
}

AudioHal::~AudioHal()
{
    deinit();
}

esp_err_t AudioHal::init()
{
    if (initialized_) {
        return ESP_OK;
    }

    if (rx_mutex_ == nullptr) {
        rx_mutex_ = xSemaphoreCreateMutex();
    }
    if (tx_mutex_ == nullptr) {
        tx_mutex_ = xSemaphoreCreateMutex();
    }
    if (rx_mutex_ == nullptr || tx_mutex_ == nullptr) {
        ESP_LOGE(TAG, "Không thể tạo FreeRTOS mutex cho I2S");
        return ESP_ERR_NO_MEM;
    }

    rx_raw_dma_buf_ = static_cast<int32_t*>(
        board_malloc_dma(kMicFrameSamples * sizeof(int32_t)));
    tx_dma_buf_ = static_cast<int16_t*>(
        board_malloc_dma(kTxChunkSamples * sizeof(int16_t)));

    if (rx_raw_dma_buf_ == nullptr || tx_dma_buf_ == nullptr) {
        ESP_LOGE(TAG, "Thiếu Internal DMA RAM cho buffer I2S");
        deinit();
        return ESP_ERR_NO_MEM;
    }

    // Nạp volume & mic_gain từ NVS nếu đã từng lưu
    load_nvs_settings();

    // BẮT BUỘC: Giải phóng GPIO 44 khỏi UART0 RX trước khi mở I2S_NUM_1
    esp_err_t err = board_release_gpio44();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Giải phóng GPIO44 thất bại: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    err = init_mic_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo INMP441 (I2S_NUM_0) thất bại: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    err = init_speaker_i2s(tx_sample_rate_hz_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo MAX98357A (I2S_NUM_1) thất bại: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    initialized_ = true;
    ESP_LOGI(TAG,
             "AudioHal sẵn sàng | Mic=I2S0(16kHz, gain=%.2fx) | Spk=I2S1(%luHz, vol=%u%%) | PSRAM=%s",
             static_cast<double>(mic_gain_.load()),
             static_cast<unsigned long>(tx_sample_rate_hz_),
             static_cast<unsigned>(speaker_volume_.load()),
             board_has_psram() ? "YES" : "NO");
    return ESP_OK;
}

void AudioHal::deinit()
{
    if (rx_chan_ != nullptr) {
        i2s_channel_disable(rx_chan_);
        i2s_del_channel(rx_chan_);
        rx_chan_ = nullptr;
    }
    if (tx_chan_ != nullptr) {
        i2s_channel_disable(tx_chan_);
        i2s_del_channel(tx_chan_);
        tx_chan_ = nullptr;
    }
    if (rx_raw_dma_buf_ != nullptr) {
        board_free(rx_raw_dma_buf_);
        rx_raw_dma_buf_ = nullptr;
    }
    if (tx_dma_buf_ != nullptr) {
        board_free(tx_dma_buf_);
        tx_dma_buf_ = nullptr;
    }
    if (rx_mutex_ != nullptr) {
        vSemaphoreDelete(rx_mutex_);
        rx_mutex_ = nullptr;
    }
    if (tx_mutex_ != nullptr) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
    initialized_ = false;
}

esp_err_t AudioHal::init_mic_i2s()
{
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6;
    rx_chan_cfg.dma_frame_num = kMicFrameSamples;

    esp_err_t err = i2s_new_channel(&rx_chan_cfg, nullptr, &rx_chan_);
    if (err != ESP_OK) {
        return err;
    }

    // INMP441 xuất 24-bit left-justified trong khung 32-bit Philips
    i2s_std_config_t rx_std_cfg = {};
    rx_std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kMicSampleRateHz);
    rx_std_cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    rx_std_cfg.slot_cfg.slot_mask = MIC_I2S_SLOT_MASK;
    rx_std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    rx_std_cfg.gpio_cfg.bclk = MIC_BCLK_GPIO;
    rx_std_cfg.gpio_cfg.ws = MIC_WS_GPIO;
    rx_std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_std_cfg.gpio_cfg.din = MIC_DIN_GPIO;
    rx_std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    rx_std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    rx_std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(rx_chan_, &rx_std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(rx_chan_);
        rx_chan_ = nullptr;
        return err;
    }

    err = i2s_channel_enable(rx_chan_);
    if (err != ESP_OK) {
        i2s_del_channel(rx_chan_);
        rx_chan_ = nullptr;
        return err;
    }
    return ESP_OK;
}

esp_err_t AudioHal::init_speaker_i2s(uint32_t sample_rate_hz)
{
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 6;
    tx_chan_cfg.dma_frame_num = 480;
    tx_chan_cfg.auto_clear = true;  // Tự chèn 0 nếu underflow để tránh tiếng rè

    esp_err_t err = i2s_new_channel(&tx_chan_cfg, &tx_chan_, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    // MAX98357A: phát 16-bit Philips Mono nhân đôi ra cả 2 slot Left/Right
    // giúp hoạt động ổn định dù chân SD_MODE hàn theo chế độ Left, Right hay (L+R)/2
    i2s_std_config_t tx_std_cfg = {};
    tx_std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    tx_std_cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    tx_std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    tx_std_cfg.gpio_cfg.bclk = SPK_BCLK_GPIO;
    tx_std_cfg.gpio_cfg.ws = SPK_WS_GPIO;
    tx_std_cfg.gpio_cfg.dout = SPK_DOUT_GPIO;
    tx_std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    tx_std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    tx_std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    tx_std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(tx_chan_, &tx_std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(tx_chan_);
        tx_chan_ = nullptr;
        return err;
    }

    err = i2s_channel_enable(tx_chan_);
    if (err != ESP_OK) {
        i2s_del_channel(tx_chan_);
        tx_chan_ = nullptr;
        return err;
    }

    tx_sample_rate_hz_ = sample_rate_hz;
    return ESP_OK;
}

esp_err_t AudioHal::set_speaker_volume(uint8_t percent, bool persist_nvs)
{
    uint8_t clamped = std::min<uint8_t>(percent, 100);
    speaker_volume_.store(clamped, std::memory_order_relaxed);
    if (persist_nvs) {
        save_nvs_settings();
    }
    return ESP_OK;
}

uint8_t AudioHal::adjust_speaker_volume(int8_t delta_percent, bool persist_nvs)
{
    int32_t current = static_cast<int32_t>(speaker_volume_.load(std::memory_order_relaxed));
    uint8_t next = clamp_u8(current + static_cast<int32_t>(delta_percent), 0, 100);
    set_speaker_volume(next, persist_nvs);
    return next;
}

esp_err_t AudioHal::set_muted(bool muted)
{
    muted_.store(muted, std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t AudioHal::set_mic_gain(float gain, bool persist_nvs)
{
    float clamped = std::max(1.0f, std::min(gain, 5.0f));
    mic_gain_.store(clamped, std::memory_order_relaxed);
    if (persist_nvs) {
        save_nvs_settings();
    }
    return ESP_OK;
}

esp_err_t AudioHal::set_mic_sensitivity_level(uint8_t level_1_to_10, bool persist_nvs)
{
    uint8_t lvl = clamp_u8(level_1_to_10, 1, 10);
    float gain = 1.0f + static_cast<float>(lvl - 1U) * (4.0f / 9.0f);
    return set_mic_gain(gain, persist_nvs);
}

uint8_t AudioHal::get_mic_sensitivity_level() const
{
    float gain = mic_gain_.load(std::memory_order_relaxed);
    float lvl = 1.0f + (gain - 1.0f) * (9.0f / 4.0f);
    return clamp_u8(static_cast<int32_t>(lroundf(lvl)), 1, 10);
}

esp_err_t AudioHal::read_mic_frame(int16_t* out_pcm,
                                   size_t max_samples,
                                   size_t* out_samples,
                                   uint16_t* out_rms,
                                   TickType_t timeout_ticks)
{
    if (!initialized_ || rx_chan_ == nullptr || out_pcm == nullptr || max_samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(rx_mutex_, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t samples_to_read = std::min(max_samples, kMicFrameSamples);
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_chan_,
                                     rx_raw_dma_buf_,
                                     samples_to_read * sizeof(int32_t),
                                     &bytes_read,
                                     timeout_ticks);
    if (err != ESP_OK) {
        xSemaphoreGive(rx_mutex_);
        return err;
    }

    size_t n_samples = bytes_read / sizeof(int32_t);
    const float gain = mic_gain_.load(std::memory_order_relaxed);
    constexpr float kDcAlpha = 0.995f;
    constexpr float kLimiterKnee = 26000.0f;
    constexpr float kLimiterHeadroom = 6767.0f;  // 32767 - 26000

    uint64_t sum_sq = 0;
    for (size_t i = 0; i < n_samples; ++i) {
        // Trích xuất 16-bit có dấu từ khung 24-bit left-justified của INMP441
        int32_t raw16 = rx_raw_dma_buf_[i] >> 16;

        // Bộ lọc thông cao bậc 1 khử DC offset của MEMS mic
        float x = static_cast<float>(raw16);
        float y = (x - dc_prev_x_) + kDcAlpha * dc_prev_y_;
        dc_prev_x_ = x;
        dc_prev_y_ = y;

        // Khuếch đại phần mềm 1.0x -> 5.0x kèm nén đỉnh mềm chống tràn (soft-knee anti-clipping)
        float amplified = y * gain;
        float abs_val = fabsf(amplified);
        if (abs_val > kLimiterKnee) {
            float excess = abs_val - kLimiterKnee;
            float compressed = kLimiterKnee + (kLimiterHeadroom * excess) / (excess + kLimiterHeadroom);
            amplified = (amplified >= 0.0f) ? compressed : -compressed;
        }

        int16_t s16 = clamp_s16(static_cast<int32_t>(lroundf(amplified)));
        out_pcm[i] = s16;
        int32_t s32 = static_cast<int32_t>(s16);
        sum_sq += static_cast<uint64_t>(s32 * s32);
    }

    xSemaphoreGive(rx_mutex_);

    uint16_t rms = (n_samples > 0)
                       ? static_cast<uint16_t>(sqrtf(static_cast<float>(sum_sq) / static_cast<float>(n_samples)))
                       : 0;

    if (out_samples != nullptr) {
        *out_samples = n_samples;
    }
    if (out_rms != nullptr) {
        *out_rms = rms;
    }

    if (n_samples > 0) {
        update_waveform_from_pcm(out_pcm, n_samples, kMicSampleRateHz, AudioSource::MIC_RX);
    }

    return ESP_OK;
}

esp_err_t AudioHal::record_utterance_with_vad(const VadConfig& cfg, CapturedAudio& out_audio)
{
    out_audio.release();
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    clear_stop_capture();

    const bool has_psram = board_has_psram();
    uint32_t target_max_ms = has_psram ? cfg.max_record_ms_psram : cfg.max_record_ms_sram;
    target_max_ms = std::max<uint32_t>(target_max_ms, 1500);

    size_t max_pcm_bytes = (static_cast<size_t>(target_max_ms) * kMicSampleRateHz / 1000U) * sizeof(int16_t);
    constexpr size_t kFrameBytes = kMicFrameSamples * sizeof(int16_t);

    // Nếu không có PSRAM, kiểm tra block SRAM lớn nhất để chừa lại >= 40KB cho TLS/HTTP/JSON
    if (!has_psram) {
        size_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        constexpr size_t kTlsHeadroomBytes = 40960;
        if (largest_free <= kTlsHeadroomBytes + kWavHeaderBytes + kFrameBytes * 10) {
            ESP_LOGE(TAG, "Không đủ Internal SRAM liên tục để thu âm (largest=%u)",
                     static_cast<unsigned>(largest_free));
            return ESP_ERR_NO_MEM;
        }
        size_t safe_budget = largest_free - kTlsHeadroomBytes - kWavHeaderBytes;
        if (max_pcm_bytes > safe_budget) {
            max_pcm_bytes = (safe_budget / kFrameBytes) * kFrameBytes;
        }
    }

    const size_t total_alloc_bytes = kWavHeaderBytes + max_pcm_bytes;
    uint8_t* wav_buf = static_cast<uint8_t*>(board_malloc_prefer_spiram(total_alloc_bytes));
    if (wav_buf == nullptr) {
        ESP_LOGE(TAG, "Cấp phát buffer WAV (%u bytes) thất bại", static_cast<unsigned>(total_alloc_bytes));
        return ESP_ERR_NO_MEM;
    }

    // Đệm vòng pre-roll giữ lại các âm tiết đầu câu trước thời điểm VAD kích hoạt
    const size_t pre_roll_frames = std::max<size_t>(1, cfg.pre_roll_ms / 20U);
    int16_t* pre_roll_ring = static_cast<int16_t*>(
        board_calloc_prefer_spiram(pre_roll_frames * kMicFrameSamples, sizeof(int16_t)));
    if (pre_roll_ring == nullptr) {
        board_free(wav_buf);
        return ESP_ERR_NO_MEM;
    }

    // Xả 2 frame đầu để ổn định bộ lọc DC-blocker & clock I2S
    int16_t frame_buf[kMicFrameSamples] = {};
    for (int i = 0; i < 2; ++i) {
        size_t dummy_n = 0;
        uint16_t dummy_rms = 0;
        read_mic_frame(frame_buf, kMicFrameSamples, &dummy_n, &dummy_rms, pdMS_TO_TICKS(100));
    }

    float noise_floor = static_cast<float>(cfg.min_speech_rms) * 0.5f;
    bool calibrated = false;
    size_t calib_frames = 0;
    float calib_sum = 0.0f;

    size_t ring_write_idx = 0;
    size_t ring_count = 0;

    bool speech_started = false;
    int consecutive_speech_frames = 0;
    uint32_t wait_elapsed_ms = 0;
    uint32_t silence_elapsed_ms = 0;
    size_t pcm_bytes_written = 0;
    bool stopped_by_user = false;

    while (true) {
        if (is_capture_stop_requested()) {
            stopped_by_user = true;
            break;
        }

        size_t samples_read = 0;
        uint16_t frame_rms = 0;
        esp_err_t err = read_mic_frame(frame_buf,
                                       kMicFrameSamples,
                                       &samples_read,
                                       &frame_rms,
                                       pdMS_TO_TICKS(150));
        if (err != ESP_OK || samples_read == 0) {
            continue;
        }

        const uint32_t frame_ms = static_cast<uint32_t>((samples_read * 1000U) / kMicSampleRateHz);
        const size_t copy_bytes = samples_read * sizeof(int16_t);

        if (!calibrated) {
            calib_sum += static_cast<float>(frame_rms);
            calib_frames++;
            if (calib_frames >= 5) {  // ~100ms hiệu chuẩn nhiễu nền
                noise_floor = calib_sum / static_cast<float>(calib_frames);
                calibrated = true;
            }
        }

        const float speech_thresh = std::max(
            static_cast<float>(cfg.min_speech_rms),
            noise_floor * cfg.speech_snr_ratio);

        if (!speech_started) {
            // Lưu vào pre-roll ring buffer
            memcpy(&pre_roll_ring[ring_write_idx * kMicFrameSamples],
                   frame_buf,
                   std::min(copy_bytes, kFrameBytes));
            ring_write_idx = (ring_write_idx + 1U) % pre_roll_frames;
            if (ring_count < pre_roll_frames) {
                ring_count++;
            }

            if (calibrated && static_cast<float>(frame_rms) >= speech_thresh) {
                consecutive_speech_frames++;
                if (consecutive_speech_frames >= 2) {  // >= 40ms liên tục mới kích hoạt nói
                    speech_started = true;
                    silence_elapsed_ms = 0;

                    // Chép toàn bộ pre-roll vào đầu payload WAV theo đúng thứ tự thời gian
                    size_t start_idx = (ring_count == pre_roll_frames) ? ring_write_idx : 0U;
                    for (size_t k = 0; k < ring_count; ++k) {
                        size_t idx = (start_idx + k) % pre_roll_frames;
                        if (pcm_bytes_written + kFrameBytes <= max_pcm_bytes) {
                            memcpy(wav_buf + kWavHeaderBytes + pcm_bytes_written,
                                   &pre_roll_ring[idx * kMicFrameSamples],
                                   kFrameBytes);
                            pcm_bytes_written += kFrameBytes;
                        }
                    }
                    ESP_LOGI(TAG, "VAD phát hiện tiếng nói (rms=%u, thresh=%.0f, noise=%.0f)",
                             static_cast<unsigned>(frame_rms),
                             static_cast<double>(speech_thresh),
                             static_cast<double>(noise_floor));
                }
            } else {
                consecutive_speech_frames = 0;
                if (calibrated) {
                    noise_floor = 0.90f * noise_floor + 0.10f * static_cast<float>(frame_rms);
                }
            }

            wait_elapsed_ms += frame_ms;
            if (!speech_started && wait_elapsed_ms >= cfg.no_speech_timeout_ms) {
                ESP_LOGI(TAG, "Hết thời gian chờ giọng nói (%lums)",
                         static_cast<unsigned long>(cfg.no_speech_timeout_ms));
                break;
            }
        } else {
            if (pcm_bytes_written + copy_bytes > max_pcm_bytes) {
                ESP_LOGW(TAG, "Đạt giới hạn bộ đệm thu âm (%u bytes)", static_cast<unsigned>(max_pcm_bytes));
                break;
            }

            memcpy(wav_buf + kWavHeaderBytes + pcm_bytes_written, frame_buf, copy_bytes);
            pcm_bytes_written += copy_bytes;

            // Độ trễ trễ ngưỡng (hysteresis 0.78x) tránh ngắt câu giữa các âm tiết ngắn
            if (static_cast<float>(frame_rms) >= speech_thresh * 0.78f) {
                silence_elapsed_ms = 0;
            } else {
                silence_elapsed_ms += frame_ms;
                if (silence_elapsed_ms >= cfg.silence_timeout_ms) {
                    ESP_LOGI(TAG, "VAD kết thúc thu âm do im lặng (%lums)",
                             static_cast<unsigned long>(silence_elapsed_ms));
                    break;
                }
            }
        }
    }

    board_free(pre_roll_ring);
    decay_waveform_to_idle();

    if (!speech_started || pcm_bytes_written == 0) {
        board_free(wav_buf);
        out_audio.speech_detected = false;
        out_audio.stopped_by_user = stopped_by_user;
        return ESP_OK;
    }

    const size_t final_wav_size = kWavHeaderBytes + pcm_bytes_written;
    write_wav_header(wav_buf, static_cast<uint32_t>(pcm_bytes_written), kMicSampleRateHz, 1, 16);

    // Thu gọn vùng nhớ ngay lập tức để nhả RAM cho phiên HTTPS STT
    uint8_t* shrunk = static_cast<uint8_t*>(board_realloc_prefer_spiram(wav_buf, final_wav_size));
    out_audio.wav_data = (shrunk != nullptr) ? shrunk : wav_buf;
    out_audio.wav_size = final_wav_size;
    out_audio.pcm_samples = pcm_bytes_written / sizeof(int16_t);
    out_audio.duration_ms = static_cast<uint32_t>((out_audio.pcm_samples * 1000U) / kMicSampleRateHz);
    out_audio.speech_detected = true;
    out_audio.stopped_by_user = stopped_by_user;

    ESP_LOGI(TAG, "Đã thu WAV: %u bytes (%lums)",
             static_cast<unsigned>(out_audio.wav_size),
             static_cast<unsigned long>(out_audio.duration_ms));
    return ESP_OK;
}

esp_err_t AudioHal::set_speaker_sample_rate(uint32_t sample_rate_hz)
{
    if (!initialized_ || tx_chan_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz == tx_sample_rate_hz_) {
        return ESP_OK;
    }

    if (xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (sample_rate_hz != tx_sample_rate_hz_) {
        err = i2s_channel_disable(tx_chan_);
        if (err == ESP_OK) {
            i2s_std_clk_config_t new_clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
            err = i2s_channel_reconfig_std_clock(tx_chan_, &new_clk);
            esp_err_t en_err = i2s_channel_enable(tx_chan_);
            if (err == ESP_OK) {
                err = en_err;
            }
            if (err == ESP_OK) {
                tx_sample_rate_hz_ = sample_rate_hz;
                ESP_LOGI(TAG, "Đổi sample rate loa -> %lu Hz", static_cast<unsigned long>(sample_rate_hz));
            }
        }
    }

    xSemaphoreGive(tx_mutex_);
    return err;
}

esp_err_t AudioHal::write_speaker(const int16_t* pcm,
                                  size_t num_samples_per_ch,
                                  uint8_t channels,
                                  uint32_t sample_rate_hz,
                                  TickType_t timeout_ticks)
{
    if (!initialized_ || tx_chan_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == nullptr || num_samples_per_ch == 0 || (channels != 1 && channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_playback_stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sample_rate_hz != 0 && sample_rate_hz != tx_sample_rate_hz_) {
        esp_err_t rate_err = set_speaker_sample_rate(sample_rate_hz);
        if (rate_err != ESP_OK) {
            return rate_err;
        }
    }

    if (xSemaphoreTake(tx_mutex_, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t vol_q15 = muted_.load(std::memory_order_relaxed)
                                 ? 0U
                                 : volume_percent_to_q15(speaker_volume_.load(std::memory_order_relaxed));

    size_t offset = 0;
    esp_err_t final_err = ESP_OK;

    while (offset < num_samples_per_ch) {
        if (is_playback_stop_requested()) {
            final_err = ESP_ERR_INVALID_STATE;
            break;
        }

        size_t chunk = std::min(num_samples_per_ch - offset, kTxChunkSamples);
        for (size_t i = 0; i < chunk; ++i) {
            int32_t mono = 0;
            if (channels == 1) {
                mono = pcm[offset + i];
            } else {
                size_t idx = (offset + i) * 2U;
                mono = (static_cast<int32_t>(pcm[idx]) + static_cast<int32_t>(pcm[idx + 1U])) >> 1;
            }
            int32_t scaled = (mono * static_cast<int32_t>(vol_q15)) >> 15;
            tx_dma_buf_[i] = clamp_s16(scaled);
        }

        // Cập nhật sóng âm hiển thị (dùng mức tín hiệu ổn định để vẫn thấy sóng nhẹ khi volume nhỏ)
        if (vol_q15 == 0) {
            decay_waveform_to_idle();
        } else {
            update_waveform_from_pcm(tx_dma_buf_, chunk, tx_sample_rate_hz_, AudioSource::SPK_TX);
        }

        size_t bytes_to_write = chunk * sizeof(int16_t);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_chan_,
                                          tx_dma_buf_,
                                          bytes_to_write,
                                          &bytes_written,
                                          timeout_ticks);
        if (err != ESP_OK) {
            final_err = err;
            break;
        }

        offset += chunk;
    }

    xSemaphoreGive(tx_mutex_);
    return final_err;
}

esp_err_t AudioHal::play_chime(ChimeType type)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    clear_stop_playback();

    auto play_tone = [this](float freq_hz, uint32_t dur_ms, float amp) -> esp_err_t {
        constexpr uint32_t kRate = 16000;
        const size_t total = (static_cast<size_t>(dur_ms) * kRate) / 1000U;
        if (total == 0) {
            return ESP_OK;
        }
        int16_t buf[320];
        const size_t fade_samples = std::min<size_t>(160, total / 4U);  // 10ms fade chống click
        size_t generated = 0;

        while (generated < total) {
            if (is_playback_stop_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            size_t n = std::min<size_t>(sizeof(buf) / sizeof(buf[0]), total - generated);
            for (size_t i = 0; i < n; ++i) {
                size_t pos = generated + i;
                float env = 1.0f;
                if (fade_samples > 0) {
                    if (pos < fade_samples) {
                        env = static_cast<float>(pos) / static_cast<float>(fade_samples);
                    } else if (pos > total - fade_samples) {
                        env = static_cast<float>(total - pos) / static_cast<float>(fade_samples);
                    }
                }
                float s = (freq_hz > 0.0f)
                              ? sinf(2.0f * kPi * freq_hz * static_cast<float>(pos) / static_cast<float>(kRate))
                              : 0.0f;
                buf[i] = clamp_s16(static_cast<int32_t>(lroundf(s * amp * env * 22000.0f)));
            }
            esp_err_t err = write_speaker(buf, n, 1, kRate, pdMS_TO_TICKS(200));
            if (err != ESP_OK) {
                return err;
            }
            generated += n;
        }
        return ESP_OK;
    };

    esp_err_t err = ESP_OK;
    switch (type) {
        case ChimeType::WAKE:
            err = play_tone(659.25f, 65, 0.55f);
            if (err == ESP_OK) err = play_tone(987.77f, 90, 0.60f);
            break;
        case ChimeType::DONE:
            err = play_tone(880.0f, 55, 0.45f);
            if (err == ESP_OK) err = play_tone(1174.66f, 75, 0.45f);
            break;
        case ChimeType::ERROR:
            err = play_tone(329.63f, 90, 0.55f);
            if (err == ESP_OK) err = play_tone(0.0f, 35, 0.0f);
            if (err == ESP_OK) err = play_tone(261.63f, 120, 0.55f);
            break;
        case ChimeType::ALERT:
            for (int i = 0; i < 3 && err == ESP_OK; ++i) {
                err = play_tone(1046.50f, 75, 0.65f);
                if (err == ESP_OK && i < 2) err = play_tone(0.0f, 50, 0.0f);
            }
            break;
        case ChimeType::POMODORO:
            err = play_tone(523.25f, 90, 0.60f);
            if (err == ESP_OK) err = play_tone(659.25f, 90, 0.60f);
            if (err == ESP_OK) err = play_tone(783.99f, 90, 0.65f);
            if (err == ESP_OK) err = play_tone(1046.50f, 160, 0.70f);
            break;
    }

    flush_speaker_silence(40);
    return err;
}

void AudioHal::flush_speaker_silence(uint32_t duration_ms)
{
    if (!initialized_ || tx_chan_ == nullptr || duration_ms == 0) {
        return;
    }
    if (xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    size_t silence_samples = (static_cast<size_t>(duration_ms) * tx_sample_rate_hz_) / 1000U;
    memset(tx_dma_buf_, 0, kTxChunkSamples * sizeof(int16_t));

    while (silence_samples > 0) {
        size_t chunk = std::min(silence_samples, kTxChunkSamples);
        size_t written = 0;
        i2s_channel_write(tx_chan_, tx_dma_buf_, chunk * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
        silence_samples -= chunk;
    }

    decay_waveform_to_idle();
    xSemaphoreGive(tx_mutex_);
}

void AudioHal::update_waveform_from_pcm(const int16_t* mono_pcm,
                                        size_t sample_count,
                                        uint32_t sample_rate_hz,
                                        AudioSource source)
{
    if (mono_pcm == nullptr || sample_count == 0 || sample_rate_hz == 0) {
        return;
    }

    // Giới hạn tối đa 256 mẫu/chunk để phân tích phổ nhanh (<20us trên ESP32-S3 FPU)
    const size_t n = std::min<size_t>(sample_count, 256);
    const uint16_t rms = compute_rms_s16(mono_pcm, n);

    // Tính 6 dải năng lượng bằng thuật toán Goertzel bậc 2
    std::array<float, 6> mag{};
    const float nyquist_limit = static_cast<float>(sample_rate_hz) * 0.45f;

    for (size_t b = 0; b < kGoertzelFreqsHz.size(); ++b) {
        float target_hz = std::min(kGoertzelFreqsHz[b], nyquist_limit);
        float omega = (2.0f * kPi * target_hz) / static_cast<float>(sample_rate_hz);
        float coeff = 2.0f * cosf(omega);
        float s0 = 0.0f;
        float s1 = 0.0f;
        float s2 = 0.0f;

        for (size_t i = 0; i < n; ++i) {
            float x = static_cast<float>(mono_pcm[i]) / 32768.0f;
            s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        float power = (s1 * s1) + (s2 * s2) - (coeff * s1 * s2);
        if (power < 0.0f) {
            power = 0.0f;
        }
        // Chuẩn hóa biên độ sang thang 0..100 theo đường cong log nén dải động
        float norm = sqrtf(power) / (static_cast<float>(n) * 0.35f);
        float score = 0.0f;
        if (norm > 0.002f) {
            score = (log10f(1.0f + norm * 45.0f) / log10f(46.0f)) * 100.0f;
        }
        mag[b] = std::max(0.0f, std::min(score, 100.0f));
    }

    // Nền RMS tổng giúp thanh giữa luôn phản hồi nhạy với âm lượng giọng nói
    float rms_norm = (rms > 60) ? (log10f(1.0f + (static_cast<float>(rms) / 8000.0f) * 35.0f) / log10f(36.0f)) * 100.0f : 0.0f;
    rms_norm = std::max(0.0f, std::min(rms_norm, 100.0f));

    // Ánh xạ đối xứng 11 thanh (cột giữa idx=5 cao nhất, giảm dần ra hai biên 0 & 10)
    std::array<float, kWaveformBands> target{};
    target[5] = std::min(100.0f, std::max(rms_norm, (mag[0] * 0.45f + mag[1] * 0.55f) * 1.05f));
    target[4] = target[6] = std::min(100.0f, (mag[1] * 0.55f + mag[2] * 0.45f) * 0.90f + rms_norm * 0.15f);
    target[3] = target[7] = std::min(100.0f, (mag[2] * 0.60f + mag[3] * 0.40f) * 0.78f + rms_norm * 0.10f);
    target[2] = target[8] = std::min(100.0f, (mag[3] * 0.60f + mag[4] * 0.40f) * 0.64f);
    target[1] = target[9] = std::min(100.0f, (mag[4] * 0.55f + mag[5] * 0.45f) * 0.48f);
    target[0] = target[10] = std::min(100.0f, (mag[5] * 0.70f + rms_norm * 0.25f) * 0.34f);

    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&vis_spinlock_);
    for (size_t i = 0; i < kWaveformBands; ++i) {
        float alpha = (target[i] > vis_bands_smooth_[i]) ? 0.68f : 0.32f;
        vis_bands_smooth_[i] = vis_bands_smooth_[i] + alpha * (target[i] - vis_bands_smooth_[i]);
    }
    vis_last_rms_ = rms;
    vis_active_source_ = source;
    vis_last_update_us_ = now_us;
    portEXIT_CRITICAL(&vis_spinlock_);
}

void AudioHal::decay_waveform_to_idle()
{
    portENTER_CRITICAL(&vis_spinlock_);
    vis_bands_smooth_.fill(0.0f);
    vis_last_rms_ = 0;
    vis_active_source_ = AudioSource::NONE;
    vis_last_update_us_ = esp_timer_get_time();
    portEXIT_CRITICAL(&vis_spinlock_);
}

AudioHal::WaveformSnapshot AudioHal::get_waveform_snapshot() const
{
    WaveformSnapshot snap;
    const int64_t now_us = esp_timer_get_time();
    constexpr int64_t kStaleTimeoutUs = 140000;  // 140ms không có audio mới -> coi như im lặng

    portENTER_CRITICAL(&vis_spinlock_);
    const bool stale = (now_us - vis_last_update_us_) > kStaleTimeoutUs;
    if (stale || vis_active_source_ == AudioSource::NONE) {
        snap.bands.fill(0);
        snap.rms = 0;
        snap.active_source = AudioSource::NONE;
    } else {
        for (size_t i = 0; i < kWaveformBands; ++i) {
            snap.bands[i] = clamp_u8(static_cast<int32_t>(lroundf(vis_bands_smooth_[i])), 0, 100);
        }
        snap.rms = vis_last_rms_;
        snap.active_source = vis_active_source_;
    }
    portEXIT_CRITICAL(&vis_spinlock_);

    return snap;
}

uint16_t AudioHal::compute_rms_s16(const int16_t* samples, size_t count)
{
    if (samples == nullptr || count == 0) {
        return 0;
    }
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = static_cast<int32_t>(samples[i]);
        sum_sq += static_cast<uint64_t>(v * v);
    }
    return static_cast<uint16_t>(sqrtf(static_cast<float>(sum_sq) / static_cast<float>(count)));
}

uint32_t AudioHal::volume_percent_to_q15(uint8_t percent)
{
    if (percent == 0) {
        return 0U;
    }
    if (percent >= 100) {
        return 32768U;
    }
    // Đường cong bình phương (x^2) cho cảm nhận thính giác tự nhiên hơn tuyến tính
    uint32_t p = static_cast<uint32_t>(percent);
    return (p * p * 32768U) / 10000U;
}

void AudioHal::write_wav_header(uint8_t* dst_44_bytes,
                                uint32_t pcm_data_bytes,
                                uint32_t sample_rate_hz,
                                uint16_t channels,
                                uint16_t bits_per_sample)
{
    if (dst_44_bytes == nullptr) {
        return;
    }

    const uint32_t riff_size = 36U + pcm_data_bytes;
    const uint32_t byte_rate = sample_rate_hz * channels * (bits_per_sample / 8U);
    const uint16_t block_align = channels * (bits_per_sample / 8U);

    memcpy(dst_44_bytes + 0, "RIFF", 4);
    memcpy(dst_44_bytes + 4, &riff_size, 4);
    memcpy(dst_44_bytes + 8, "WAVE", 4);
    memcpy(dst_44_bytes + 12, "fmt ", 4);

    const uint32_t fmt_size = 16U;
    const uint16_t audio_fmt = 1U;  // PCM uncompressed
    memcpy(dst_44_bytes + 16, &fmt_size, 4);
    memcpy(dst_44_bytes + 20, &audio_fmt, 2);
    memcpy(dst_44_bytes + 22, &channels, 2);
    memcpy(dst_44_bytes + 24, &sample_rate_hz, 4);
    memcpy(dst_44_bytes + 28, &byte_rate, 4);
    memcpy(dst_44_bytes + 32, &block_align, 2);
    memcpy(dst_44_bytes + 34, &bits_per_sample, 2);

    memcpy(dst_44_bytes + 36, "data", 4);
    memcpy(dst_44_bytes + 40, &pcm_data_bytes, 4);
}

void AudioHal::load_nvs_settings()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(BOARD_NVS_NS_AUDIO, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_NOT_INITIALIZED) {
            ESP_LOGW(TAG, "nvs_open read cảnh báo: %s", esp_err_to_name(err));
        }
        return;
    }

    uint8_t saved_vol = 75;
    if (nvs_get_u8(handle, kNvsKeyVolume, &saved_vol) == ESP_OK) {
        speaker_volume_.store(std::min<uint8_t>(saved_vol, 100), std::memory_order_relaxed);
    }

    uint32_t gain_milli = 2500;
    if (nvs_get_u32(handle, kNvsKeyMicGainMilli, &gain_milli) == ESP_OK) {
        float g = static_cast<float>(gain_milli) / 1000.0f;
        mic_gain_.store(std::max(1.0f, std::min(g, 5.0f)), std::memory_order_relaxed);
    }

    nvs_close(handle);
}

void AudioHal::save_nvs_settings() const
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(BOARD_NVS_NS_AUDIO, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_INITIALIZED) {
            ESP_LOGW(TAG, "nvs_open write thất bại: %s", esp_err_to_name(err));
        }
        return;
    }

    const uint8_t vol = speaker_volume_.load(std::memory_order_relaxed);
    const uint32_t gain_milli = static_cast<uint32_t>(
        lroundf(mic_gain_.load(std::memory_order_relaxed) * 1000.0f));

    nvs_set_u8(handle, kNvsKeyVolume, vol);
    nvs_set_u32(handle, kNvsKeyMicGainMilli, gain_milli);
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace zclaw

