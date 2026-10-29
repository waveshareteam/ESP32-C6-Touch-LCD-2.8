/* SPDX-License-Identifier: MIT */
#include "XiaozhiAudioProcessor.hpp"
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wn_models.h"

namespace esp_brookesia::apps {
namespace {
constexpr char TAG[] = "XiaozhiAudio";
// Measured peak: 35 KB plus allocation headers, input buffer and network headroom.
constexpr size_t WAKE_INIT_BUDGET = 48 * 1024;
constexpr size_t WAKE_LARGEST_BLOCK = 8 * 1024;
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex): mutex_(mutex) {
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { if (mutex_) xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
}

XiaozhiAudioProcessor::XiaozhiAudioProcessor()
{
    lifecycle_mutex_ = xSemaphoreCreateMutex();
    state_mutex_ = xSemaphoreCreateMutex();
    input_mutex_ = xSemaphoreCreateMutex();
}
XiaozhiAudioProcessor::~XiaozhiAudioProcessor()
{
    shutdown();
    if (input_mutex_) vSemaphoreDelete(input_mutex_);
    if (state_mutex_) vSemaphoreDelete(state_mutex_);
    if (lifecycle_mutex_) vSemaphoreDelete(lifecycle_mutex_);
}
bool XiaozhiAudioProcessor::initialize()
{
    if (!lifecycle_mutex_ || !state_mutex_ || !input_mutex_) return false;
    Lock lifecycle(lifecycle_mutex_);
    Lock input(input_mutex_);
    Lock state(state_mutex_);
    if (ready_.load()) return true;
    stopping_.store(false);
    chunk_fill_ = voice_fill_ = 0;
    wake_enabled_ = voice_enabled_ = false;
    if (!loadWakeModel()) return false;
    ready_.store(true);
    ESP_LOGI(TAG, "WakeNet9s ready; inference shares the 16-kHz capture task");
    return true;
}
bool XiaozhiAudioProcessor::shutdown()
{
    if (!lifecycle_mutex_) return true;
    Lock lifecycle(lifecycle_mutex_);
    ready_.store(false);
    stopping_.store(true);
    Lock input(input_mutex_);
    Lock state(state_mutex_);
    releaseWakeModel();
    voice_enabled_ = false;
    voice_fill_ = 0;
    stopping_.store(false);
    return true;
}
bool XiaozhiAudioProcessor::feed(const int16_t *pcm, size_t frames)
{
    if (!pcm || !frames || !ready_.load()) return false;
    Lock input(input_mutex_);
    while (frames && ready_.load() && !stopping_.load()) {
        WakeWordCallback wake_cb;
        PcmFrameCallback pcm_cb;
        std::string word;
        bool have_frame = false;
        size_t count = std::min(frames, size_t{160});
        {
            Lock state(state_mutex_);
            if (!wake_enabled_ && !voice_enabled_) return false;
            // Stop exactly at the frame boundary before delivering the member
            // buffer. Partial I2S reads must not overwrite its beginning first.
            if (voice_enabled_) count = std::min(count, PCM_FRAME_SAMPLES - voice_fill_);
            for (size_t i = 0; i < count; ++i) {
                if (wake_enabled_) {
                    wake_chunk_[chunk_fill_++] = pcm[i];
                    if (chunk_fill_ == wake_chunk_.size()) {
                        chunk_fill_ = 0;
                        int detected = wake_iface_->detect(wake_data_, wake_chunk_.data());
                        if (detected > 0) {
                            const char *name = wake_iface_->get_word_name(wake_data_, detected);
                            word = name ? name : "你好小智";
                            wake_enabled_ = false;
                            wake_cb = wake_callback_;
                        }
                    }
                }
                if (voice_enabled_) {
                    voice_frame_[voice_fill_++] = pcm[i];
                    if (voice_fill_ == PCM_FRAME_SAMPLES) {
                        voice_fill_ = 0;
                        have_frame = true;
                        pcm_cb = pcm_callback_;
                    }
                }
            }
        }
        // Mode changes wait for this producer before releasing its codec resources.
        if (wake_cb && !stopping_.load()) wake_cb(word);
        // input_mutex_ stays held across the synchronous callback, so mode
        // changes and subsequent feeds cannot overwrite voice_frame_ here.
        if (have_frame && pcm_cb && !stopping_.load()) pcm_cb(voice_frame_.data(), voice_frame_.size());
        pcm += count;
        frames -= count;
    }
    return true;
}
bool XiaozhiAudioProcessor::enableWakeWord(bool enable)
{
    Lock state(state_mutex_);
    if (!ready_.load() || stopping_.load() || (enable && !wake_available_.load())) return false;
    if (wake_enabled_ != enable) {
        wake_enabled_ = enable;
        chunk_fill_ = 0;
        ESP_LOGI(TAG, "Wake word detection %s", enable ? "enabled" : "disabled");
    }
    return true;
}
bool XiaozhiAudioProcessor::enableVoiceProcessing(bool enable)
{
    Lock input(input_mutex_);
    Lock state(state_mutex_);
    if (!ready_.load() || stopping_.load()) return false;
    if (voice_enabled_ != enable) {
        voice_enabled_ = enable;
        voice_fill_ = chunk_fill_ = 0;
    }
    return true;
}
void XiaozhiAudioProcessor::releaseWakeModel()
{
    wake_available_.store(false);
    wake_enabled_ = false;
    chunk_fill_ = 0;
    if (wake_data_) wake_iface_->destroy(wake_data_);
    wake_data_ = nullptr;
    wake_iface_ = nullptr;
    if (models_) esp_srmodel_deinit(models_);
    models_ = nullptr;
    std::vector<int16_t>().swap(wake_chunk_);
}
bool XiaozhiAudioProcessor::loadWakeModel()
{
    if (wake_data_) return true;
    // This model uses many small allocations, not one contiguous 48-KB block.
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t available = heap_caps_get_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);
    if (available < WAKE_INIT_BUDGET || largest < WAKE_LARGEST_BLOCK) {
        ESP_LOGE(TAG, "Wake model budget unavailable: free=%u, largest=%u, required total=%u, block=%u",
                 (unsigned)available, (unsigned)largest, (unsigned)WAKE_INIT_BUDGET,
                 (unsigned)WAKE_LARGEST_BLOCK);
        return false;
    }
    models_ = esp_srmodel_init("model");
    char *name = models_ ? esp_srmodel_filter(models_, ESP_WN_PREFIX, "nihaoxiaozhi") : nullptr;
    wake_iface_ = name ? esp_wn_handle_from_name(name) : nullptr;
    wake_data_ = wake_iface_ ? wake_iface_->create(name, DET_MODE_90) : nullptr;
    if (wake_data_ && wake_iface_->get_samp_rate(wake_data_) == SAMPLE_RATE &&
            wake_iface_->get_channel_num(wake_data_) == 1 &&
            wake_iface_->get_samp_chunksize(wake_data_) > 0) {
        try {
            wake_chunk_.resize(wake_iface_->get_samp_chunksize(wake_data_));
            wake_available_.store(true);
            return true;
        } catch (const std::bad_alloc &) {
            ESP_LOGE(TAG, "Wake input buffer allocation failed");
        }
    }
    releaseWakeModel();
    return false;
}
bool XiaozhiAudioProcessor::suspendWakeWord()
{
    Lock lifecycle(lifecycle_mutex_);
    Lock input(input_mutex_);
    Lock state(state_mutex_);
    releaseWakeModel();
    return true;
}
bool XiaozhiAudioProcessor::resumeWakeWord()
{
    Lock lifecycle(lifecycle_mutex_);
    Lock input(input_mutex_);
    Lock state(state_mutex_);
    if (!ready_.load() || stopping_.load()) return false;
    return loadWakeModel();
}
void XiaozhiAudioProcessor::setWakeWordCallback(WakeWordCallback callback)
{
    Lock state(state_mutex_); wake_callback_ = std::move(callback);
}
void XiaozhiAudioProcessor::setPcmFrameCallback(PcmFrameCallback callback)
{
    Lock state(state_mutex_); pcm_callback_ = std::move(callback);
}
void XiaozhiAudioProcessor::setVadCallback(VadCallback callback)
{
    Lock state(state_mutex_); vad_callback_ = std::move(callback);
}
} // namespace esp_brookesia::apps
