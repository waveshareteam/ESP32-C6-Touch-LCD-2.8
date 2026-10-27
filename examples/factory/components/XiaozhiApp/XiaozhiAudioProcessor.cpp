/* SPDX-License-Identifier: MIT */
#include "XiaozhiAudioProcessor.hpp"
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wn_models.h"
#include "freertos/idf_additions.h"

namespace esp_brookesia::apps {
namespace {
constexpr char TAG[] = "XiaozhiAudio";
constexpr uint32_t TASK_STACK_BYTES = 6 * 1024;
// WakeNet9s peaks at about 35 KB before allocator overhead and network traffic.
constexpr size_t WAKE_INIT_BUDGET = 48 * 1024;
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
    if (ready_.load()) return true;
    stopping_.store(false);
    input_queue_ = xQueueCreateWithCaps(6, sizeof(InputBlock),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    events_ = xEventGroupCreate();
    if (!input_queue_ || !events_) {
        ESP_LOGE(TAG, "Cannot create WakeNet9s audio queue/event group (queue data=%u bytes in internal RAM)",
                 (unsigned)(6 * sizeof(InputBlock)));
        releaseResources();
        return false;
    }
    chunk_fill_ = voice_fill_ = 0;
    wake_enabled_ = voice_enabled_ = false;
    ++generation_;
    if (xTaskCreateWithCaps(taskEntry, "xiaozhi_wn9s", TASK_STACK_BYTES, this, 3, &task_,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create WakeNet9s task (%lu-byte internal RAM stack)",
                 static_cast<unsigned long>(TASK_STACK_BYTES));
        task_ = nullptr;
        releaseResources();
        return false;
    }
    ready_.store(true);
    if (!loadWakeModel()) {
        ready_.store(false);
        stopping_.store(true);
        xEventGroupWaitBits(events_, TASK_EXITED, pdFALSE, pdTRUE, portMAX_DELAY);
        vTaskDelay(1);
        task_ = nullptr;
        releaseResources();
        return false;
    }
    ESP_LOGI(TAG, "C6 WakeNet9s ready; encoder remains unloaded until wakeup");
    return true;
}

bool XiaozhiAudioProcessor::shutdown()
{
    if (!lifecycle_mutex_) return true;
    Lock lifecycle(lifecycle_mutex_);
    if (task_ && xTaskGetCurrentTaskHandle() == task_) return false;
    ready_.store(false);
    wake_available_.store(false);
    stopping_.store(true);
    /* Exclude any feed that observed the previous ready state. */
    Lock input(input_mutex_);
    if (task_) {
        xEventGroupWaitBits(events_, TASK_EXITED, pdFALSE, pdTRUE, portMAX_DELAY);
        vTaskDelay(1);
        task_ = nullptr;
    }
    Lock state(state_mutex_);
    releaseResources();
    stopping_.store(false);
    return true;
}

void XiaozhiAudioProcessor::releaseResources()
{
    releaseWakeModel();
    if (input_queue_) vQueueDeleteWithCaps(input_queue_);
    input_queue_ = nullptr;
    if (events_) vEventGroupDelete(events_);
    events_ = nullptr;
    std::vector<int16_t>().swap(wake_chunk_);
    wake_enabled_ = voice_enabled_ = false;
    wake_available_.store(false);
}

bool XiaozhiAudioProcessor::feed(const int16_t *pcm, size_t frames)
{
    if (!pcm || !frames || !ready_.load()) return false;
    Lock input(input_mutex_);
    if (!ready_.load() || stopping_.load() || !input_queue_) return false;
    uint32_t generation;
    {
        Lock state(state_mutex_);
        if (!wake_enabled_ && !voice_enabled_) return false;
        generation = generation_;
    }
    while (frames) {
        InputBlock block{};
        block.generation = generation;
        block.count = std::min(frames, size_t{160});
        memcpy(block.samples, pcm, block.count * sizeof(int16_t));
        if (xQueueSend(input_queue_, &block, pdMS_TO_TICKS(20)) != pdTRUE) {
            /* Do not splice PCM across a gap if the CPU cannot keep up. */
            Lock state(state_mutex_);
            ++generation_;
            chunk_fill_ = voice_fill_ = 0;
            xQueueReset(input_queue_);
            ESP_LOGW(TAG, "Input queue full; discarded discontinuous PCM (WakeNet model retained)");
            return false;
        }
        pcm += block.count;
        frames -= block.count;
    }
    return true;
}

bool XiaozhiAudioProcessor::enableWakeWord(bool enable)
{
    Lock state(state_mutex_);
    if (!ready_.load() || stopping_.load()) return false;
    if (enable && !wake_available_.load()) return false;
    // Reset PCM generations instead of calling the unsafe library clean().
    if (wake_enabled_ != enable) {
        wake_enabled_ = enable;
        ++generation_;
        chunk_fill_ = 0;
        ESP_LOGI(TAG, "Wake word detection %s (model retained; PCM generation=%lu)",
                 enable ? "enabled" : "disabled", static_cast<unsigned long>(generation_));
    }
    return true;
}

bool XiaozhiAudioProcessor::enableVoiceProcessing(bool enable)
{
    Lock state(state_mutex_);
    if (!ready_.load() || stopping_.load()) return false;
    if (voice_enabled_ != enable) {
        voice_enabled_ = enable;
        voice_fill_ = 0;
        chunk_fill_ = 0;
        ++generation_;
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
    // The library dereferences failed allocations; check its measured peak first.
    void *probe = heap_caps_malloc(WAKE_INIT_BUDGET, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!probe) {
        ESP_LOGE(TAG, "Wake model budget unavailable: free=%u, largest=%u, required=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)WAKE_INIT_BUDGET);
        return false;
    }
    heap_caps_free(probe);
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
    Lock state(state_mutex_);
    ++generation_;
    releaseWakeModel();
    return true;
}

bool XiaozhiAudioProcessor::resumeWakeWord()
{
    Lock lifecycle(lifecycle_mutex_);
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
    /* Speech endpoint detection remains on the server, as in the template. */
    Lock state(state_mutex_); vad_callback_ = std::move(callback);
}

void XiaozhiAudioProcessor::processingLoop()
{
    InputBlock block{};
    while (!stopping_.load()) {
        if (xQueueReceive(input_queue_, &block, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        WakeWordCallback wake_cb;
        PcmFrameCallback pcm_cb;
        std::string word;
        std::array<int16_t, PCM_FRAME_SAMPLES> frame{};
        bool have_frame = false;
        {
            Lock state(state_mutex_);
            if (stopping_.load() || block.generation != generation_) continue;
            for (size_t i = 0; i < block.count; ++i) {
                const int16_t sample = block.samples[i];
                if (wake_enabled_) {
                    wake_chunk_[chunk_fill_++] = sample;
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
                    voice_frame_[voice_fill_++] = sample;
                    if (voice_fill_ == PCM_FRAME_SAMPLES) {
                        voice_fill_ = 0;
                        frame = voice_frame_;
                        have_frame = true;
                        pcm_cb = pcm_callback_;
                    }
                }
            }
        }
        /* Callbacks can change modes, so they must run outside state_mutex_. */
        if (wake_cb && !stopping_.load()) wake_cb(word);
        if (have_frame && pcm_cb && !stopping_.load()) pcm_cb(frame.data(), frame.size());
    }
}

void XiaozhiAudioProcessor::taskEntry(void *arg)
{
    auto *self = static_cast<XiaozhiAudioProcessor *>(arg);
    self->processingLoop();
    xEventGroupSetBits(self->events_, TASK_EXITED);
    vTaskDeleteWithCaps(nullptr);
}
} // namespace esp_brookesia::apps
