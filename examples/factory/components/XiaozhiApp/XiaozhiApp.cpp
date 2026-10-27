#include "bsp_audio_session.h"
/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiApp.hpp"
#include "XiaozhiActivationClient.hpp"
#include "XiaozhiActivationPrompt.hpp"
#include "XiaozhiAudioProcessor.hpp"
#include "XiaozhiLocalization.hpp"
#include "XiaozhiUi.hpp"


#include <new>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

#include "bsp/esp-bsp.h"
#include "wifi_manager.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_xiaozhi_info.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:XiaozhiApp"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(img_app_aichats);

namespace esp_brookesia::apps {

namespace {

constexpr uint32_t RETRY_INTERVAL_MS = 5000;
constexpr uint32_t TRANSPORT_RECONNECT_FALLBACK_MS = 10000;
constexpr uint32_t ACTIVATION_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t ACTIVATION_RETRY_MAX_MS = 10000;
constexpr uint32_t CHAT_SAMPLE_RATE = 16000;
constexpr uint32_t DEVICE_AUDIO_SAMPLE_RATE = 16000;
constexpr uint32_t CHAT_FRAME_DURATION_MS = 20;
constexpr uint32_t PLAYBACK_DRAIN_MS = 200;
constexpr uint32_t UPLINK_FIRST_PACKET_TIMEOUT_MS = 3000;
constexpr uint32_t CONTROL_EVENT_POST_TIMEOUT_MS = 100;
constexpr uint32_t LIFECYCLE_WAIT_MS = 10000;
constexpr uint32_t PLAYBACK_IDLE_WAIT_MS = 1000;
constexpr size_t CONTROL_QUEUE_LENGTH = 8;
constexpr size_t PLAYBACK_QUEUE_LENGTH = 4;
constexpr size_t DEVICE_TDM_CHANNELS = CODEC_VOICE_INPUT_CHANNELS;
constexpr size_t DEVICE_MIC1_SLOT = 0;
static_assert(
    XiaozhiAudioProcessor::INPUT_CHANNELS == 1,
    "C6 2.8 has one ES8311 microphone"
);
constexpr size_t INPUT_READ_FRAMES = DEVICE_AUDIO_SAMPLE_RATE / 100;
constexpr size_t MAX_SERVER_FRAME_DURATION_MS = 120;
constexpr size_t MAX_DEVICE_PCM_SAMPLES =
    DEVICE_AUDIO_SAMPLE_RATE * MAX_SERVER_FRAME_DURATION_MS / 1000;

void logServiceInfoHeap(const char *context)
{
    ESP_UTILS_LOGI(
        "Service info heap (%s): internal free=%u, largest=%u",
        context,
        static_cast<unsigned int>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        )
    );
}

bool netifHasIpv4(const char *if_key)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t info = {};
    return esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0;
}

void copyString(char *destination, size_t capacity, const char *source)
{
    if (destination && capacity > 0) {
        snprintf(destination, capacity, "%s", source ? source : "");
    }
}

uint32_t activationRetryDelay(int timeout_ms)
{
    if (timeout_ms <= 0) {
        return ACTIVATION_RETRY_INTERVAL_MS;
    }
    uint32_t delay = static_cast<uint32_t>(timeout_ms);
    if (delay < 1000) {
        delay = 1000;
    }
    return delay > ACTIVATION_RETRY_MAX_MS ? ACTIVATION_RETRY_MAX_MS : delay;
}

class ScopedSemaphoreLock {
public:
    ScopedSemaphoreLock(SemaphoreHandle_t semaphore, TickType_t timeout):
        _semaphore(semaphore)
    {
        _locked =
            _semaphore && xSemaphoreTake(_semaphore, timeout) == pdTRUE;
    }

    ~ScopedSemaphoreLock()
    {
        if (_locked) {
            xSemaphoreGive(_semaphore);
        }
    }

    explicit operator bool() const
    {
        return _locked;
    }

private:
    SemaphoreHandle_t _semaphore = nullptr;
    bool _locked = false;
};

constexpr uint32_t LIFECYCLE_CLOSED_BIT = 1U << 31;
constexpr uint32_t LIFECYCLE_REF_MASK = LIFECYCLE_CLOSED_BIT - 1;

class ScopedLifecycleAdmission {
public:
    explicit ScopedLifecycleAdmission(std::atomic<uint32_t> &state):
        _state(state)
    {
        uint32_t current = _state.load();
        while ((current & LIFECYCLE_CLOSED_BIT) == 0 &&
                (current & LIFECYCLE_REF_MASK) != LIFECYCLE_REF_MASK) {
            if (_state.compare_exchange_weak(current, current + 1)) {
                _acquired = true;
                break;
            }
        }
    }

    ~ScopedLifecycleAdmission()
    {
        if (_acquired) {
            _state.fetch_sub(1);
        }
    }

    explicit operator bool() const
    {
        return _acquired;
    }

private:
    std::atomic<uint32_t> &_state;
    bool _acquired = false;
};

class ScopedOptionalCounter {
public:
    explicit ScopedOptionalCounter(std::atomic<uint32_t> *counter):
        _counter(counter)
    {
        if (_counter) {
            _counter->fetch_add(1);
        }
    }

    ~ScopedOptionalCounter()
    {
        if (_counter) {
            _counter->fetch_sub(1);
        }
    }

private:
    std::atomic<uint32_t> *_counter = nullptr;
};

class ScopedXiaozhiInfo {
public:
    ScopedXiaozhiInfo() = default;

    ~ScopedXiaozhiInfo()
    {
        (void)esp_xiaozhi_chat_free_info(&value);
    }

    esp_xiaozhi_chat_info_t value = {};

    ScopedXiaozhiInfo(const ScopedXiaozhiInfo &) = delete;
    ScopedXiaozhiInfo &operator=(const ScopedXiaozhiInfo &) = delete;
};

} // namespace

XiaozhiApp *XiaozhiApp::_instance = nullptr;

XiaozhiApp *XiaozhiApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new (std::nothrow) XiaozhiApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

XiaozhiApp::XiaozhiApp(bool use_status_bar, bool use_navigation_bar):
    App("AIChats", &img_app_aichats, true, use_status_bar, use_navigation_bar)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

XiaozhiApp::~XiaozhiApp()
{
    _destroying.store(true);
    while (!deinit()) {
        ESP_UTILS_LOGW("Waiting to deinitialize Xiaozhi safely");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    _lifecycle_refs.fetch_or(LIFECYCLE_CLOSED_BIT);
    while ((_lifecycle_refs.load() & LIFECYCLE_REF_MASK) != 0) {
        vTaskDelay(1);
    }
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
    _instance = nullptr;
}

bool XiaozhiApp::run()
{
    _ready_prompt_played.store(false);
    createUi();
    _visible.store(true);
    updateNetworkState();
    ControlEvent event = {};
    event.type = ControlEventType::AppVisible;
    postControlEvent(event);
    return true;
}

bool XiaozhiApp::back()
{
    _visible.store(false);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    if (!postControlEvent(event, true)) {
        return false;
    }
    return notifyCoreClosed();
}

bool XiaozhiApp::close()
{
    _visible.store(false);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    if (!postControlEvent(event, true)) {
        return false;
    }
    destroyUi();
    return true;
}

bool XiaozhiApp::init()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    bool deinit_was_complete = _deinit_complete.load();
    if (_destroying.load() || _deinit_in_progress.load()) {
        return false;
    }
    if (!_lifecycle_mutex) {
        logServiceInfoHeap("lifecycle mutex allocation failed");
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi lifecycle mutex");
        return false;
    }
    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock) {
        return false;
    }
    if (_destroying.load() || _deinit_in_progress.load() ||
            (!deinit_was_complete && _deinit_complete.load())) {
        return false;
    }
    if (_control_running.load()) {
        return true;
    }

    _deinit_complete.store(false);
    _shutdown.store(false);
    _activation_phase = ActivationPhase::CheckRequired;
    _control_event_lost.store(false);
    logServiceInfoHeap("before synchronization allocation");
    _playback_mutex = _playback_mutex ? _playback_mutex : xSemaphoreCreateMutex();
    _state_mutex = _state_mutex ? _state_mutex : xSemaphoreCreateMutex();
    _audio_tx_mutex = _audio_tx_mutex ? _audio_tx_mutex : xSemaphoreCreateMutex();
    if (!_playback_mutex) {
        logServiceInfoHeap("playback mutex allocation failed");
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi playback mutex");
        return false;
    }
    _control_ack = _control_ack ? _control_ack : xSemaphoreCreateBinary();
    _control_exited = _control_exited ? _control_exited : xSemaphoreCreateBinary();
    // Short bounded queues leave SRAM for codec state, TLS and LCD DMA.
    _control_queue = _control_queue ? _control_queue :
                     xQueueCreateWithCaps(
                         CONTROL_QUEUE_LENGTH, sizeof(ControlEvent),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
                     );
    if (!_state_mutex || !_lifecycle_mutex || !_audio_tx_mutex || !_control_ack ||
            !_control_exited || !_control_queue) {
        ESP_UTILS_LOGE(
            "Xiaozhi allocation handles: state=%p lifecycle=%p audio_tx=%p "
            "ack=%p exited=%p control=%p playback=%p",
            _state_mutex, _lifecycle_mutex, _audio_tx_mutex, _control_ack,
            _control_exited, _control_queue, _playback_queue
        );
        logServiceInfoHeap("synchronization allocation failed");
        const char *failed = !_state_mutex ? "state mutex" :
                             !_audio_tx_mutex ? "audio TX mutex" :
                             !_control_ack ? "control acknowledgement" :
                             !_control_exited ? "control exit semaphore" :
                             "control queue (internal RAM)";
        setError(ESP_ERR_NO_MEM, failed);
        return false;
    }
    logServiceInfoHeap("after synchronization allocation");

    _activation_client = _activation_client ? _activation_client :
                         new (std::nothrow) XiaozhiActivationClient();
    _activation_prompt = _activation_prompt ? _activation_prompt :
                         new (std::nothrow) XiaozhiActivationPrompt();
    _audio_processor = _audio_processor ? _audio_processor :
                       new (std::nothrow) XiaozhiAudioProcessor();
    _ui = _ui ? _ui : new (std::nothrow) XiaozhiUi();
    if (!_activation_client || !_activation_prompt || !_audio_processor || !_ui) {
        logServiceInfoHeap("helper allocation failed");
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi helpers");
        return false;
    }
    (void)_ui->preload();

    // BSP owns netif/radio/default event-loop initialization.
    esp_err_t ret = ESP_OK;
    if (!_ip_event_instance) {
        ret = esp_event_handler_instance_register(
                  IP_EVENT, ESP_EVENT_ANY_ID, ipEventHandler, this, &_ip_event_instance
              );
        if (ret != ESP_OK) {
            setError(ret, "Register network event handler");
            return false;
        }
    }

    _control_running.store(true);
    if (xTaskCreatePinnedToCore(
            controlTaskEntry,
            "xiaozhi_control",
            CONTROL_TASK_STACK_SIZE,
            this,
            5,
            &_control_task,
            0
        ) != pdPASS) {
        _control_running.store(false);
        _control_task = nullptr;
        logServiceInfoHeap("control task allocation failed");
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi control task");
        return false;
    }
    updateNetworkState();
    return true;
}

bool XiaozhiApp::deinit()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    if (_deinit_complete.load()) {
        return true;
    }
    bool expected = false;
    if (!_deinit_in_progress.compare_exchange_strong(expected, true)) {
        TickType_t started = xTaskGetTickCount();
        while (_deinit_in_progress.load() &&
                xTaskGetTickCount() - started <
                    pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return _deinit_complete.load();
    }
    auto finish_deinit = [this](bool complete) {
        _deinit_complete.store(complete);
        _deinit_in_progress.store(false);
        return complete;
    };

    if (!_lifecycle_mutex && !_control_running.load()) {
        _shutdown.store(true);
        return finish_deinit(true);
    }

    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock) {
        ESP_UTILS_LOGE("Timed out waiting for Xiaozhi lifecycle lock");
        return finish_deinit(false);
    }

    _visible.store(false);
    bool was_shutdown = _shutdown.exchange(true);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    bool control_stopped = true;
    if (_control_running.load()) {
        control_stopped = _control_queue && _control_exited;
        if (control_stopped && !was_shutdown) {
            ControlEvent event = {};
            event.type = ControlEventType::Shutdown;
            control_stopped =
                xQueueSendToFront(
                    _control_queue,
                    &event,
                    pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
                ) == pdTRUE;
            if (!control_stopped) {
                _shutdown.store(false);
            }
        }
        if (control_stopped) {
            control_stopped =
                xSemaphoreTake(
                    _control_exited, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
                ) == pdTRUE;
        }
    }
    if (!control_stopped) {
        ESP_UTILS_LOGE("Xiaozhi control task did not stop cleanly");
        return finish_deinit(false);
    }
    if (_ip_event_instance) {
        esp_event_handler_instance_unregister(
            IP_EVENT, ESP_EVENT_ANY_ID, _ip_event_instance
        );
        _ip_event_instance = nullptr;
    }

    TickType_t post_wait_started = xTaskGetTickCount();
    while (_async_post_users.load() != 0 &&
            xTaskGetTickCount() - post_wait_started <
                pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (_async_post_users.load() != 0) {
        ESP_UTILS_LOGE("Xiaozhi callbacks still have active producers");
        return finish_deinit(false);
    }

    // Let the task finish returning from the exit semaphore give before the
    // semaphore and queues are deleted.
    vTaskDelay(1);

    destroyUi();
    delete _ui;
    _ui = nullptr;
    delete _activation_prompt;
    _activation_prompt = nullptr;
    delete _activation_client;
    _activation_client = nullptr;
    delete _audio_processor;
    _audio_processor = nullptr;

    if (_playback_queue) {
        vQueueDeleteWithCaps(_playback_queue);
        _playback_queue = nullptr;
    }
    if (_control_queue) {
        vQueueDeleteWithCaps(_control_queue);
        _control_queue = nullptr;
    }
    if (_control_exited) {
        vSemaphoreDelete(_control_exited);
        _control_exited = nullptr;
    }
    if (_control_ack) {
        vSemaphoreDelete(_control_ack);
        _control_ack = nullptr;
    }
    if (_playback_mutex) {
        vSemaphoreDelete(_playback_mutex);
        _playback_mutex = nullptr;
    }
    if (_audio_tx_mutex) {
        vSemaphoreDelete(_audio_tx_mutex);
        _audio_tx_mutex = nullptr;
    }
    if (_state_mutex) {
        vSemaphoreDelete(_state_mutex);
        _state_mutex = nullptr;
    }
    return finish_deinit(true);
}

bool XiaozhiApp::pause()
{
    _visible.store(false);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    if (!postControlEvent(event, true)) {
        return false;
    }
    if (_ui_timer) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool XiaozhiApp::resume()
{
    if (!_page_root) {
        createUi();
    }
    _visible.store(true);
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    updateNetworkState();
    ControlEvent event = {};
    event.type = ControlEventType::AppVisible;
    postControlEvent(event);
    return true;
}

void XiaozhiApp::createUi()
{
    destroyUi();
    if (!_ui) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    _page_root = lv_obj_create(screen);
    lv_obj_remove_style_all(_page_root);
    lv_obj_set_size(_page_root, lv_pct(100), lv_pct(100));
    lv_obj_center(_page_root);
    lv_obj_set_style_bg_color(_page_root, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_page_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_page_root, LV_OBJ_FLAG_SCROLLABLE);

    if (!_ui->create(_page_root, uiActionCallback, this)) {
        destroyUi();
        return;
    }
    _ui_timer = lv_timer_create(uiTimerCallback, 200, this);
    refreshUi();
}

void XiaozhiApp::destroyUi()
{
    if (_ui_timer) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }
    if (_ui) {
        _ui->destroy();
    }
    if (_page_root) {
        lv_obj_delete(_page_root);
        _page_root = nullptr;
    }
}

void XiaozhiApp::refreshUi()
{
    if (!_ui || !_page_root) {
        return;
    }
    char code[sizeof(_activation_code)] = {};
    char message[sizeof(_activation_message)] = {};
    char subtitle[sizeof(_subtitle_text)] = {};
    if (!_state_mutex ||
            xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    memcpy(code, _activation_code, sizeof(code));
    memcpy(message, _activation_message, sizeof(message));
    memcpy(subtitle, _subtitle_text, sizeof(subtitle));
    xSemaphoreGive(_state_mutex);

    State state = _state.load();
    _ui->setNetworkReady(_network_ready.load());
    _ui->setStatus(stateText(state));
    _ui->setSubtitle(subtitle);
    const bool talking = state == State::Listening || state == State::Processing || state == State::Speaking;
    _ui->setActionEnabled(talking || state == State::Ready);
    _ui->setActivation(
        code,
        message[0] ? message : xiaozhiUiText(
            XiaozhiUiText::ActivationInstructions
        ),
        state == State::ActivationRequired
    );
}

void XiaozhiApp::updateSubtitle(
    uint32_t generation, XiaozhiClient::TextRole role, const std::string &value
)
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user || value.empty()) return;
    ScopedOptionalCounter async_user(&_async_post_users);
    ScopedSemaphoreLock lock(_state_mutex, pdMS_TO_TICKS(20));
    if (!lock || generation != _chat_generation.load() ||
            _shutdown.load() || !_visible.load()) return;

    // Keep only the current sentence and never split a UTF-8 character.
    const char *prefix = role == XiaozhiClient::TextRole::User ? "You: " : "Xiaozhi: ";
    const size_t prefix_size = strlen(prefix);
    const size_t capacity = sizeof(_subtitle_text) - prefix_size - 1;
    const bool truncated = value.size() > capacity;
    size_t count = truncated ? capacity - 3 : value.size();
    while (count < value.size() && count > 0 &&
            (static_cast<unsigned char>(value[count]) & 0xC0) == 0x80) --count;
    memcpy(_subtitle_text, prefix, prefix_size);
    memcpy(_subtitle_text + prefix_size, value.data(), count);
    if (truncated) {
        memcpy(_subtitle_text + prefix_size + count, "...", 3);
        count += 3;
    }
    _subtitle_text[prefix_size + count] = '\0';
}

void XiaozhiApp::setState(State state)
{
    _state.store(state);
}

void XiaozhiApp::setError(esp_err_t error, const char *source)
{
    ESP_UTILS_LOGE(
        "%s: %s", source ? source : "Xiaozhi", esp_err_to_name(error)
    );
    if (_network_ready.load()) {
        setState(State::Error);
    }
}

void XiaozhiApp::copyActivation(const char *code, const char *message)
{
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        copyString(_activation_code, sizeof(_activation_code), code);
        copyString(_activation_message, sizeof(_activation_message), message);
        xSemaphoreGive(_state_mutex);
    }
}

void XiaozhiApp::clearActivation()
{
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        _activation_code[0] = '\0';
        _activation_message[0] = '\0';
        xSemaphoreGive(_state_mutex);
    }
}

bool XiaozhiApp::announceActivation(bool force)
{
    if (!_visible.load() || !_activation_prompt ||
            _state.load() != State::ActivationRequired) {
        return false;
    }
    char code[sizeof(_activation_code)] = {};
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        copyString(code, sizeof(code), _activation_code);
        xSemaphoreGive(_state_mutex);
    }
    return _activation_prompt->start(code, force);
}

void XiaozhiApp::stopActivationPrompt()
{
    if (!_activation_prompt) {
        return;
    }
    _activation_prompt->cancel();
    constexpr int MAX_STOP_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_STOP_ATTEMPTS; ++attempt) {
        if (_activation_prompt->stopAndWait(1000)) {
            return;
        }
        ESP_UTILS_LOGW(
            "Waiting for Xiaozhi prompt audio to stop (%d/%d)",
            attempt,
            MAX_STOP_ATTEMPTS
        );
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // The prompt deliberately retains its codec/session ownership when
    // cleanup fails.  A later stop/start path can retry without freeing live
    // resources or wedging the control task forever.
    ESP_UTILS_LOGE("Xiaozhi prompt cleanup deferred after bounded retries");
}

bool XiaozhiApp::playReadyPromptOnce()
{
    if (_ready_prompt_played.exchange(true)) {
        return true;
    }
    if (!_activation_prompt || !canPrepare()) {
        ESP_UTILS_LOGW("Xiaozhi ready prompt is unavailable");
        return false;
    }

    stopActivationPrompt();
    if (_audio_tasks_running.load()) {
        shutdownAudio();
    }

    bool completed = false;
    if (_activation_prompt->playSuccess()) {
        while (!_activation_prompt->waitUntilIdle(100)) {
            if (!canPrepare()) {
                _activation_prompt->cancel();
                return false;
            }
        }
        completed = _activation_prompt->lastPlaybackCompleted();
    }
    if (completed) {
        ESP_UTILS_LOGI("Xiaozhi self-check completed; success prompt played");
    } else {
        ESP_UTILS_LOGW("Xiaozhi self-check completed without success prompt");
    }
    return completed;
}

bool XiaozhiApp::canPrepare() const
{
    return _visible.load() && _network_ready.load() && isNetworkReady() &&
           !_shutdown.load();
}

bool XiaozhiApp::isNetworkReady() const
{
    return wifi_manager_is_connected() && netifHasIpv4("WIFI_STA_DEF");
}

void XiaozhiApp::updateNetworkState()
{
    bool ready = isNetworkReady();
    if (!ready) {
        if (_activation_prompt) {
            _activation_prompt->cancel();
        }
        if (_activation_client) {
            _activation_client->cancel();
        }
    }
    ControlEvent event = {};
    event.type = ControlEventType::NetworkChanged;
    event.flag = ready;
    postControlEvent(event);
}

bool XiaozhiApp::postControlEvent(const ControlEvent &event, bool wait_for_ack)
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    ScopedOptionalCounter async_post_user(
        wait_for_ack ? nullptr : &_async_post_users
    );
    if (_destroying.load() || _deinit_in_progress.load() ||
            !_control_queue || (!_control_running.load() &&
                            event.type != ControlEventType::Shutdown) ||
            (_shutdown.load() && event.type != ControlEventType::Shutdown)) {
        return false;
    }
    if (!wait_for_ack) {
        TickType_t wait_ticks = pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS);
        bool queued = xQueueSend(_control_queue, &event, wait_ticks) == pdTRUE;
        if (!queued) {
            ESP_UTILS_LOGW(
                "Control queue rejected event %u",
                static_cast<unsigned>(event.type)
            );
            _control_event_lost.store(true);
        }
        return queued;
    }
    if (!_lifecycle_mutex || !_control_ack) {
        return false;
    }
    if (xSemaphoreTake(
                _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
            ) != pdTRUE) {
        return false;
    }
    if (!_control_queue || !_control_running.load() || _shutdown.load()) {
        xSemaphoreGive(_lifecycle_mutex);
        return false;
    }
    xSemaphoreTake(_control_ack, 0);
    bool ok =
        xQueueSend(
            _control_queue, &event, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
        ) == pdTRUE;
    if (ok) {
        ok =
            xSemaphoreTake(
                _control_ack, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
            ) == pdTRUE;
    }
    xSemaphoreGive(_lifecycle_mutex);
    return ok;
}

void XiaozhiApp::controlLoop()
{
    bool exit_requested = false;
    while (!exit_requested) {
        ControlEvent event = {};
        if (xQueueReceive(_control_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (event.type == ControlEventType::Shutdown) {
                exit_requested = true;
            } else {
                handleControlEvent(event);
                if (event.type == ControlEventType::AppHidden && _control_ack) {
                    xSemaphoreGive(_control_ack);
                }
            }
        }
        if (!exit_requested) {
            // The shared Wi-Fi service clears this immediately on disconnect;
            // IP_EVENT_STA_LOST_IP may otherwise be delayed by the IP-loss timer.
            const bool network_ready = isNetworkReady();
            if (network_ready != _network_ready.load()) {
                ControlEvent network_event = {};
                network_event.type = ControlEventType::NetworkChanged;
                network_event.flag = network_ready;
                handleControlEvent(network_event);
            }
            if (_control_event_lost.exchange(false)) {
                ESP_UTILS_LOGE("Recovering after a dropped Xiaozhi control event");
                setAudioModes(false, false);
                closeChannel(true);
                destroyChat();
                if (_network_ready.load()) {
                    setError(ESP_ERR_TIMEOUT, "Xiaozhi control queue");
                    schedulePrepare(RETRY_INTERVAL_MS);
                } else {
                    setState(State::NetworkRequired);
                }
            }
            prepareIfDue();
            TickType_t uplink_started = _uplink_started_tick.load();
            if (uplink_started != 0 &&
                    _state.load() == State::Listening &&
                    _voice_enabled.load() &&
                    _uplink_ready.load() &&
                    _opus_packets_sent.load() == 0 &&
                    xTaskGetTickCount() - uplink_started >=
                        pdMS_TO_TICKS(UPLINK_FIRST_PACKET_TIMEOUT_MS)) {
                ESP_UTILS_LOGE(
                    "No Xiaozhi Opus uplink packet after listening started"
                );
                _uplink_started_tick.store(0);
                closeChannel(true);
                destroyChat();
                if (_network_ready.load()) {
                    setError(ESP_ERR_TIMEOUT, "Start Xiaozhi audio uplink");
                    schedulePrepare(RETRY_INTERVAL_MS);
                } else {
                    setState(State::NetworkRequired);
                }
            }
        }
    }

    _visible.store(false);
    stopActivationPrompt();
    setAudioModes(false, false);
    closeChannel(true);
    destroyChat();
    while (!shutdownAudio()) {
        ESP_UTILS_LOGW("Waiting to release the Xiaozhi audio session");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    _control_task = nullptr;
    _control_running.store(false);
    if (_control_exited) {
        xSemaphoreGive(_control_exited);
    }
    vTaskDelete(nullptr);
}

void XiaozhiApp::handleControlEvent(const ControlEvent &event)
{
    if ((event.chat_generation != 0 &&
            event.chat_generation != _chat_generation.load()) ||
            (event.turn_generation != 0 &&
             event.turn_generation != _turn_generation.load())) {
        ESP_UTILS_LOGD("Ignoring a stale Xiaozhi control event");
        return;
    }

    switch (event.type) {
    case ControlEventType::AppVisible:
        handleAppVisible();
        break;
    case ControlEventType::AppHidden:
        handleAppHidden(event.flag);
        break;
    case ControlEventType::NetworkChanged:
        _network_ready.store(event.flag);
        if (!event.flag) {
            stopActivationPrompt();
            setState(State::NetworkRequired);
            setAudioModes(false, false);
            closeChannel(false);
            destroyChat();
        } else if (!_chat_started.load() && _visible.load()) {
            if (_activation_phase == ActivationPhase::WaitingForBinding ||
                    _activation_phase == ActivationPhase::BoundPendingSuccess) {
                setState(State::ActivationRequired);
                if (_activation_phase == ActivationPhase::WaitingForBinding) {
                    announceActivation(false);
                }
            } else {
                setState(State::Preparing);
            }
            schedulePrepare(0);
        } else if (_transport_connected) {
            handleAppVisible();
        }
        break;
    case ControlEventType::ActionPressed:
        if (_state.load() == State::ActivationRequired) {
            announceActivation(true);
        } else if (_state.load() == State::Ready) {
            startListening(ListenMode::AutoStop);
        } else if (_state.load() == State::Listening &&
                   _listen_mode == ListenMode::Manual) {
            finishListening();
        } else if ((_state.load() == State::Speaking ||
                    _state.load() == State::Processing) &&
                   _listen_mode == ListenMode::Manual) {
            cancelManualConversation();
        } else if (_state.load() == State::Listening ||
                   _state.load() == State::Speaking ||
                   _state.load() == State::Processing) {
            closeChannel(true);
            setState(State::Ready);
            enterReadyAudioMode();
        }
        break;
    case ControlEventType::AnnounceActivation:
        announceActivation(true);
        break;
    case ControlEventType::WakeWord:
    {
        const State state = _state.load();
        XiaozhiClient *protocol = _protocol.load();
        if (_visible.load() && state == State::Ready) {
            startListening(ListenMode::AutoStop, event.text);
        } else if (_visible.load() && state == State::Speaking &&
                   _channel_state.load() == ChannelState::Open &&
                   protocol) {
            esp_err_t abort_ret = ESP_ERR_INVALID_STATE;
            if (_audio_tx_mutex &&
                    xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
                abort_ret = protocol->sendAbortSpeaking(
                    XiaozhiClient::AbortReason::WakeWordDetected
                );
                xSemaphoreGive(_audio_tx_mutex);
            }
            if (abort_ret != ESP_OK) {
                ESP_UTILS_LOGW(
                    "Failed to abort speaking after wake word: 0x%x",
                    static_cast<unsigned int>(abort_ret)
                );
                if (_audio_processor && _audio_processor->isReady()) {
                    _audio_processor->enableWakeWord(true);
                }
            } else {
                startListening(ListenMode::AutoStop, event.text);
            }
        } else if (!_visible.load()) {
            setAudioModes(false, false);
        } else {
            ESP_UTILS_LOGD("Wake word ignored in the current conversation state");
            if (_audio_processor && _audio_processor->isReady()) {
                _audio_processor->enableWakeWord(true);
            }
        }
        break;
    }
    case ControlEventType::TransportConnected:
        _transport_connected = true;
        _server_goodbye_generation.store(0);
        if (_chat_started.load() &&
                (_state.load() == State::Connecting ||
                 _state.load() == State::Preparing ||
                 _state.load() == State::Error)) {
            setState(State::Ready);
            if (_visible.load()) {
                (void)playReadyPromptOnce();
                if (canPrepare() && ensureAudioReady()) {
                    enterReadyAudioMode();
                } else if (canPrepare()) {
                    schedulePrepare(RETRY_INTERVAL_MS);
                }
            }
        }
        break;
    case ControlEventType::TransportDisconnected: {
        const uint32_t goodbye_generation =
            _server_goodbye_generation.exchange(0);
        const bool reconnect_now = goodbye_generation == event.chat_generation;
        const bool drain_final_response =
            _tts_stop_pending.load() && _state.load() == State::Speaking;
        _transport_connected = false;
        if (drain_final_response) {
            _uplink_ready.store(false);
            _channel_state.store(ChannelState::Closing);
            _restart_transport_after_playback.store(true);
            ESP_UTILS_LOGI(
                "Transport closed after final TTS; reconnecting after playback drains"
            );
            break;
        }

        _restart_transport_after_playback.store(false);
        resetChannelState();
        setAudioModes(false, false);
        // Release codec tasks and buffers before the next TLS handshake.
        shutdownAudio();
        logServiceInfoHeap("audio released for transport reconnect");
        if (reconnect_now) {
            ESP_UTILS_LOGI(
                "Transport closed after server goodbye; reconnecting now"
            );
            destroyChat();
        } else {
            ESP_UTILS_LOGW(
                "Xiaozhi transport disconnected; waiting for auto reconnect"
            );
        }
        if (_network_ready.load()) {
            setState(State::Connecting);
            schedulePrepare(
                reconnect_now ? 0 : TRANSPORT_RECONNECT_FALLBACK_MS
            );
        } else {
            setState(State::NetworkRequired);
        }
        break;
    }
    case ControlEventType::ChannelOpened:
        if (_channel_state.load() == ChannelState::Opening) {
            _channel_state.store(ChannelState::Open);
        }
        break;
    case ControlEventType::ChannelClosed:
        if (_channel_state.load() == ChannelState::Closing) {
            resetChannelState();
            if (_network_ready.load() && _transport_connected && _visible.load()) {
                setState(State::Ready);
                enterReadyAudioMode();
            }
        }
        break;
    case ControlEventType::ServerGoodbye:
        _server_goodbye_generation.store(event.chat_generation);
        closeChannel(false);
        if (_network_ready.load() && _transport_connected) {
            ESP_UTILS_LOGI(
                "Server ended the conversation; wake-word standby restored"
            );
            setState(State::Ready);
            enterReadyAudioMode();
        }
        break;
    case ControlEventType::TtsStart: {
        State state = _state.load();
        if (_channel_state.load() == ChannelState::Open &&
                _listen_mode != ListenMode::None &&
                (state == State::Listening ||
                 state == State::Processing ||
                 state == State::Speaking)) {
            // No echo-reference input on this board. Pause wake-word capture
            // while speaking so the device does not wake itself from its reply.
            if (!setAudioModes(false, false) && _visible.load()) {
                closeChannel(false);
                shutdownAudio();
                destroyChat();
                setError(ESP_FAIL, "Pause microphone during reply");
                schedulePrepare(RETRY_INTERVAL_MS);
                break;
            }
            stopEncoder();
            if (!startPlayback()) {
                closeChannel(false);
                shutdownAudio();
                setError(ESP_ERR_NO_MEM, "Create Xiaozhi playback task");
                schedulePrepare(RETRY_INTERVAL_MS);
                break;
            }
            _awaiting_response = false;
            _tts_active = true;
            _tts_stop_pending.store(false);
            _accept_playback.store(true);
            ESP_UTILS_LOGI("Server TTS started");
            setState(State::Speaking);
        }
        break;
    }
    case ControlEventType::TtsStop: {
        if (_channel_state.load() == ChannelState::Open &&
                (_tts_active || _state.load() == State::Speaking)) {
            TickType_t now = xTaskGetTickCount();
            _tts_active = false;
            ESP_UTILS_LOGI("Server TTS stopped; draining playback");
            _tts_stop_tick.store(now);
            _last_audio_packet_tick.store(now);
            _last_audio_write_tick.store(now);
            _drain_event_posted.store(false);
            _tts_stop_pending.store(true);
        }
        break;
    }
    case ControlEventType::PlaybackDrained:
        handlePlaybackDrained();
        break;
    case ControlEventType::ChatError:
        closeChannel(true);
        destroyChat();
        if (_network_ready.load()) {
            setError(event.error, event.detail);
            schedulePrepare(RETRY_INTERVAL_MS);
        } else {
            setState(State::NetworkRequired);
        }
        break;
    case ControlEventType::AudioError:
        closeChannel(true);
        shutdownAudio();
        if (_network_ready.load()) {
            setError(event.error, event.detail);
            bool recovered = false;
            if (_visible.load() && _transport_connected &&
                    _chat_started.load() && ensureAudioReady()) {
                setState(State::Ready);
                recovered = enterReadyAudioMode();
            }
            if (!recovered) {
                schedulePrepare(RETRY_INTERVAL_MS);
            }
        } else {
            setState(State::NetworkRequired);
        }
        break;
    case ControlEventType::Shutdown:
        break;
    }
}

void XiaozhiApp::handleAppVisible()
{
    _visible.store(true);
    if (!_network_ready.load() || !isNetworkReady()) {
        setState(State::NetworkRequired);
    } else if (_activation_phase == ActivationPhase::WaitingForBinding ||
               _activation_phase == ActivationPhase::BoundPendingSuccess) {
        setState(State::ActivationRequired);
        if (_activation_phase == ActivationPhase::WaitingForBinding) {
            announceActivation(false);
        }
        schedulePrepare(0);
    } else if (!_chat_started.load()) {
        setState(State::Preparing);
        schedulePrepare(0);
    } else if (!_transport_connected) {
        setState(State::Connecting);
    } else if (_tts_active || _tts_stop_pending.load()) {
        setState(State::Speaking);
    } else if (_awaiting_response) {
        setState(State::Processing);
    } else {
        setState(State::Ready);
        (void)playReadyPromptOnce();
        if (!ensureAudioReady()) {
            schedulePrepare(RETRY_INTERVAL_MS);
        } else {
            enterReadyAudioMode();
        }
    }
}

void XiaozhiApp::handleAppHidden(bool release_audio)
{
    _visible.store(false);
    stopActivationPrompt();
    setAudioModes(false, false);
    if (release_audio) {
        closeChannel(true);
        shutdownAudio();
    } else if (_listen_mode != ListenMode::None && !_tts_active) {
        stopUplink(true);
    }
}

void XiaozhiApp::prepareIfDue()
{
    if (!canPrepare() ||
            !tickReached(xTaskGetTickCount(), _next_prepare_tick)) {
        return;
    }

    if (_chat_started.load()) {
        if (!_transport_connected && _state.load() == State::Connecting) {
            ESP_UTILS_LOGW(
                "Xiaozhi auto reconnect timed out; restarting transport"
            );
            destroyChat();
            if (canPrepare()) {
                prepareChat();
            }
            return;
        }
        if (_transport_connected && _visible.load() &&
                _state.load() == State::Error &&
                !_audio_tasks_running.load()) {
            if (ensureAudioReady()) {
                setState(State::Ready);
                enterReadyAudioMode();
            } else {
                schedulePrepare(RETRY_INTERVAL_MS);
            }
        }
        return;
    }
    prepareChat();
}

void XiaozhiApp::schedulePrepare(uint32_t delay_ms)
{
    _next_prepare_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
}

bool XiaozhiApp::prepareChat()
{
    if (!canPrepare()) {
        return false;
    }
    if (!_activation_client) {
        setError(ESP_ERR_INVALID_STATE, "Initialize Xiaozhi activation");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    setState(State::Preparing);
    const bool was_waiting =
        _activation_phase == ActivationPhase::WaitingForBinding ||
        _activation_phase == ActivationPhase::BoundPendingSuccess;
    if (_audio_tasks_running.load()) shutdownAudio();
    ScopedXiaozhiInfo service;
    logServiceInfoHeap("before request");
    esp_err_t ret = esp_xiaozhi_chat_get_info(&service.value);
    if (ret != ESP_OK) {
        logServiceInfoHeap(esp_err_to_name(ret));
    }
    if (!canPrepare()) {
        return false;
    }
    if (ret != ESP_OK) {
        if (was_waiting) {
            ESP_UTILS_LOGW(
                "Activation status check failed: %s",
                esp_err_to_name(ret)
            );
            setState(State::ActivationRequired);
        } else {
            setError(ret, "Get Xiaozhi service information");
        }
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    if (service.value.has_activation_code ||
            service.value.has_activation_challenge) {
        copyActivation(
            service.value.activation_code,
            service.value.activation_message
        );
        _activation_phase = ActivationPhase::WaitingForBinding;
        setState(State::ActivationRequired);
        announceActivation(false);

        const uint32_t delay_ms =
            activationRetryDelay(service.value.activation_timeout_ms);
        if (!service.value.has_activation_challenge ||
                !service.value.activation_challenge ||
                service.value.activation_challenge[0] == 0) {
            schedulePrepare(delay_ms);
            return false;
        }

        XiaozhiActivationClient::Info activation = {};
        activation.state =
            XiaozhiActivationClient::BindingState::ActivationRequired;
        activation.challenge = service.value.activation_challenge;
        int http_status = 0;
        ret = _activation_client->activate(activation, http_status);
        if (!canPrepare()) {
            return false;
        }
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_UTILS_LOGW(
                "Activation request failed: %s (HTTP %d)",
                esp_err_to_name(ret),
                http_status
            );
        } else if (ret == ESP_OK) {
            _activation_phase = ActivationPhase::BoundPendingSuccess;
            schedulePrepare(0);
            return false;
        }
        schedulePrepare(delay_ms);
        return false;
    }

    if (was_waiting) {
        stopActivationPrompt();
        if (!canPrepare()) {
            return false;
        }
    }
    clearActivation();
    _activation_phase = ActivationPhase::Complete;

    bool use_mqtt = false;
    bool use_websocket = false;
#if CONFIG_XIAOZHI_APP_TRANSPORT_MQTT
    use_mqtt = service.value.has_mqtt_config;
#elif CONFIG_XIAOZHI_APP_TRANSPORT_AUTO
    use_mqtt = service.value.has_mqtt_config;
    use_websocket = service.value.has_websocket_config;
#else
    use_websocket = service.value.has_websocket_config;
#endif
    if (!use_mqtt && !use_websocket) {
#if CONFIG_XIAOZHI_APP_TRANSPORT_MQTT
        setError(ESP_ERR_NOT_SUPPORTED, "Get Xiaozhi MQTT configuration");
#elif CONFIG_XIAOZHI_APP_TRANSPORT_AUTO
        setError(ESP_ERR_NOT_SUPPORTED, "Get Xiaozhi transport configuration");
#else
        setError(ESP_ERR_NOT_SUPPORTED, "Get Xiaozhi WebSocket configuration");
#endif
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }
    if (!canPrepare()) {
        return false;
    }

    const uint32_t client_generation = _chat_generation.fetch_add(1) + 1;
    auto post_simple = [this, client_generation](ControlEventType type) {
        ControlEvent event = {};
        event.type = type;
        event.chat_generation = client_generation;
        (void)postControlEvent(event);
    };

    XiaozhiClient::Callbacks callbacks = {};
    callbacks.onConnected = [post_simple]() {
        post_simple(ControlEventType::TransportConnected);
    };
    callbacks.onDisconnected = [this, post_simple]() {
        if (_chat_started.load()) {
            post_simple(ControlEventType::TransportDisconnected);
        }
    };
    callbacks.onAudioChannelOpened = [post_simple]() {
        post_simple(ControlEventType::ChannelOpened);
    };
    callbacks.onAudioChannelClosed = [post_simple]() {
        post_simple(ControlEventType::ChannelClosed);
    };
    callbacks.onServerGoodbye = [this, post_simple, client_generation]() {
        if (_chat_generation.load() == client_generation) {
            uint32_t marked_generation =
                _server_goodbye_generation.load();
            while (marked_generation < client_generation &&
                    !_server_goodbye_generation.compare_exchange_weak(
                        marked_generation, client_generation
                    )) {
            }
        }
        post_simple(ControlEventType::ServerGoodbye);
    };
    callbacks.onText = [this, client_generation](
        XiaozhiClient::TextRole role, const std::string &value
    ) {
        updateSubtitle(client_generation, role, value);
    };
    callbacks.onTts = [this, client_generation](
        XiaozhiClient::TtsState state,
        const std::string &text
    ) {
        if (_chat_generation.load() != client_generation) {
            return;
        }
        (void)text;
        if (state == XiaozhiClient::TtsState::SentenceStart) {
            return;
        }
        ControlEvent event = {};
        event.type = state == XiaozhiClient::TtsState::Start ?
                     ControlEventType::TtsStart : ControlEventType::TtsStop;
        event.chat_generation = client_generation;
        event.turn_generation = _turn_generation.load();
        if (state == XiaozhiClient::TtsState::Start &&
                _channel_state.load() == ChannelState::Open &&
                _chat_started.load() && !_shutdown.load()) {
            _accept_playback.store(true);
        }
        (void)postControlEvent(event);
    };
    callbacks.onAudio = [this, client_generation](
        const uint8_t *data,
        size_t len,
        int sample_rate,
        int frame_duration_ms,
        uint32_t timestamp
    ) {
        ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
        if (!lifecycle_user) {
            return;
        }
        ScopedOptionalCounter async_user(&_async_post_users);
        if (_chat_generation.load() != client_generation ||
                !data || len == 0 || len > sizeof(AudioPacket::data) ||
                !_audio_tasks_running.load() ||
                !_accept_playback.load() ||
                _channel_state.load() != ChannelState::Open ||
                _shutdown.load()) {
            return;
        }

        AudioPacket packet = {};
        packet.turn_generation = _turn_generation.load();
        packet.timestamp = timestamp;
        packet.sample_rate = static_cast<uint16_t>(
            sample_rate > 0 && sample_rate <= UINT16_MAX ?
            sample_rate : CHAT_SAMPLE_RATE
        );
        packet.frame_duration_ms = static_cast<uint16_t>(
            frame_duration_ms > 0 && frame_duration_ms <= UINT16_MAX ?
            frame_duration_ms : CHAT_FRAME_DURATION_MS
        );
        packet.size = static_cast<uint16_t>(len);
        memcpy(packet.data, data, len);

        if (!_playback_mutex ||
                xSemaphoreTake(
                    _playback_mutex,
                    pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS)
                ) != pdTRUE) {
            return;
        }
        if (_accept_playback.load() &&
                _chat_generation.load() == client_generation &&
                packet.turn_generation == _turn_generation.load() &&
                _channel_state.load() == ChannelState::Open) {
            if (!_playback_queue) _playback_queue = xQueueCreateWithCaps(
                PLAYBACK_QUEUE_LENGTH, sizeof(AudioPacket), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (_playback_queue && xQueueSend(_playback_queue, &packet, 0) == pdTRUE) {
                _last_audio_packet_tick.store(xTaskGetTickCount());
            }
        }
        xSemaphoreGive(_playback_mutex);
    };
    callbacks.onError = [this, client_generation](
        esp_err_t error,
        const std::string &source
    ) {
        ControlEvent event = {};
        event.type = ControlEventType::ChatError;
        event.error = error;
        event.chat_generation = client_generation;
        event.turn_generation = _turn_generation.load();
        copyString(event.detail, sizeof(event.detail), source.c_str());
        (void)postControlEvent(event);
    };

    XiaozhiClient::Config protocol_config(
        use_mqtt,
        use_websocket,
        CHAT_SAMPLE_RATE,
        1,
        CHAT_FRAME_DURATION_MS,
        DEVICE_AUDIO_SAMPLE_RATE
    );
    auto *protocol = new (std::nothrow) XiaozhiClient(
        std::move(protocol_config), std::move(callbacks)
    );
    if (!protocol) {
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi protocol");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    _turn_generation.fetch_add(1);
    _protocol.store(protocol);
    _chat_initialized.store(true);
    _chat_started.store(true);
    _transport_connected = false;
    setState(State::Connecting);

    ret = protocol->start();
    if (ret != ESP_OK || !canPrepare()) {
        destroyChat();
        if (ret != ESP_OK) {
            setError(ret, "Start Xiaozhi protocol");
            schedulePrepare(RETRY_INTERVAL_MS);
        }
        return false;
    }

    schedulePrepare(TRANSPORT_RECONNECT_FALLBACK_MS);
    ESP_UTILS_LOGI("Xiaozhi transport started; waiting for connection");
    return true;
}

void XiaozhiApp::destroyChat()
{
    setAudioModes(false, false);
    _uplink_ready.store(false);
    _chat_started.store(false);
    _chat_initialized.store(false);
    _channel_state.store(ChannelState::Closing);

    XiaozhiClient *protocol = nullptr;
    if (_audio_tx_mutex &&
            xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
        protocol = _protocol.exchange(nullptr);
        if (protocol) {
            (void)protocol->stop();
        }
        xSemaphoreGive(_audio_tx_mutex);
    } else {
        protocol = _protocol.exchange(nullptr);
    }
    delete protocol;

    _chat_generation.fetch_add(1);
    _turn_generation.fetch_add(1);
    _transport_connected = false;
    _server_goodbye_generation.store(0);
    resetChannelState();
}

bool XiaozhiApp::ensureAudioReady()
{
    if (_audio_tasks_running.load() && _input_task_running.load() &&
            _audio_processor && _audio_processor->isReady()) {
        return true;
    }
    stopActivationPrompt();
    if (!shutdownAudio()) {
        setError(ESP_FAIL, "Release previous audio session");
        return false;
    }

    esp_err_t ret = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
    // Music releases its decoder on its worker after the GUI pause callback.
    // Allow that short handover without displaying a five-second retry error.
    for (int attempt = 0; ret != ESP_OK && attempt < 10 &&
            _visible.load() && !_shutdown.load() &&
            (bsp_extra_audio_session_get_owner() == BSP_EXTRA_AUDIO_OWNER_MUSIC || bsp_extra_audio_session_get_owner() == BSP_EXTRA_AUDIO_OWNER_RECORDER); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ret = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
    }
    if (ret != ESP_OK) {
        ESP_UTILS_LOGW(
            "Xiaozhi audio is busy (%s)",
            bsp_extra_audio_owner_name(bsp_extra_audio_session_get_owner())
        );
        setError(ret, "Acquire audio session");
        return false;
    }
    _audio_session_acquired.store(true);

    ret = bsp_extra_codec_set_voice_fs(DEVICE_AUDIO_SAMPLE_RATE, 16, 2, 3, 1);
    if (ret != ESP_OK) {
        setError(ret, "Configure audio codec");
        shutdownAudio();
        return false;
    }
    _codec_claimed.store(true);

    _audio_processor->setWakeWordCallback([this](const std::string &word) {
        ControlEvent event = {};
        event.type = ControlEventType::WakeWord;
        copyString(event.text, sizeof(event.text), word.c_str());
        if (!postControlEvent(event)) {
            ESP_UTILS_LOGW("Dropped Xiaozhi wake-word control event");
            if (_audio_processor && _audio_processor->isReady()) {
                _audio_processor->enableWakeWord(true);
            }
        }
    });
    _audio_processor->setPcmFrameCallback(
        [this](const int16_t *pcm, size_t count) { encodeAudioFrame(pcm, count); }
    );

    _audio_tasks_running.store(true);
    ESP_UTILS_LOGI(
        "Audio task heap before create: internal free=%u, largest=%u",
        static_cast<unsigned int>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        )
    );
    _input_task_running.store(true);
    if (xTaskCreatePinnedToCore(
            inputTaskEntry,
            "xiaozhi_audio",
            INPUT_TASK_STACK_SIZE,
            this,
            5,
            &_input_task,
            0
        ) != pdPASS) {
        _input_task_running.store(false);
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi input task");
        shutdownAudio();
        return false;
    }
    // Reserve one capture/wake/Opus stack before loading the wake model.
    vTaskDelay(1);
    logServiceInfoHeap("before audio processor initialization");
    if (!_audio_processor || !_audio_processor->initialize()) {
        setError(ESP_ERR_NO_MEM, "Initialize Xiaozhi audio processor");
        shutdownAudio();
        return false;
    }
    return true;
}

bool XiaozhiApp::ensureEncoder()
{
    if (_opus_encoder && _encoded_frame) return true;
    if (!setAudioModes(false, false)) return false;
    if (!_audio_processor || !_audio_processor->suspendWakeWord() || !releasePlaybackResources()) return false;
    stopEncoder();
    logServiceInfoHeap("wake model released; audio worker stack already reserved");
    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = CHAT_SAMPLE_RATE;
    enc.channel = ESP_AUDIO_MONO;
    enc.bits_per_sample = ESP_AUDIO_BIT16;
    enc.bitrate = ESP_OPUS_BITRATE_AUTO;
    enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    enc.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc.complexity = 0;
    enc.enable_fec = false;
    enc.enable_dtx = true;
    enc.enable_vbr = true;
    if (esp_opus_enc_open(&enc, sizeof(enc), &_opus_encoder) != ESP_AUDIO_ERR_OK ||
            !_opus_encoder ||
            esp_opus_enc_get_frame_size(
                _opus_encoder, &_opus_input_size, &_opus_output_size
            ) != ESP_AUDIO_ERR_OK ||
            _opus_input_size != XiaozhiAudioProcessor::PCM_FRAME_SAMPLES * sizeof(int16_t) ||
            _opus_output_size <= 0) {
        setError(ESP_FAIL, "Open OPUS encoder");
        stopEncoder();
        return false;
    }
    _encoded_frame = static_cast<uint8_t *>(malloc(_opus_output_size));
    if (!_encoded_frame) {
        setError(ESP_ERR_NO_MEM, "Allocate OPUS output buffer");
        stopEncoder();
        return false;
    }
    logServiceInfoHeap("Opus ready on shared audio worker");
    return true;
}

void XiaozhiApp::stopEncoder()
{
    // This producer barrier also waits for an in-flight encode/send callback.
    setAudioModes(false, false);
    if (_opus_encoder) esp_opus_enc_close(_opus_encoder);
    _opus_encoder = nullptr;
    free(_encoded_frame);
    _encoded_frame = nullptr;
    _opus_input_size = _opus_output_size = 0;
}

bool XiaozhiApp::startPlayback()
{
    ScopedSemaphoreLock lock(_playback_mutex, portMAX_DELAY);
    if (!lock) return false;
    if (!_playback_queue) _playback_queue = xQueueCreateWithCaps(
        PLAYBACK_QUEUE_LENGTH, sizeof(AudioPacket), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!_playback_queue) return false;
    _playback_enabled.store(true);
    return true;
}

bool XiaozhiApp::releasePlaybackResources()
{
    _accept_playback.store(false);
    if (!resetPlaybackBuffer(false) || !waitForPlaybackIdle(PLAYBACK_IDLE_WAIT_MS)) return false;
    _playback_enabled.store(false);
    while (_playback_active.load()) vTaskDelay(pdMS_TO_TICKS(5));
    ScopedSemaphoreLock lock(_playback_mutex, portMAX_DELAY);
    if (!lock) return false;
    if (_opus_decoder) esp_opus_dec_close(_opus_decoder);
    _opus_decoder = nullptr;
    _decoder_frame_duration_ms = 0;
    if (_playback_queue) vQueueDeleteWithCaps(_playback_queue);
    _playback_queue = nullptr;
    return true;
}

bool XiaozhiApp::shutdownAudio()
{
    setAudioModes(false, false);
    _accept_playback.store(false);
    _audio_tasks_running.store(false);
    _playback_enabled.store(false);
    while (_input_task_running.load() ||
            _playback_active.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _input_task = nullptr;
    if (_audio_processor) {
        _audio_processor->shutdown();
    }
    if (_opus_encoder) {
        esp_opus_enc_close(_opus_encoder);
        _opus_encoder = nullptr;
    }
    if (_playback_mutex &&
            xSemaphoreTake(_playback_mutex, portMAX_DELAY) == pdTRUE) {
        if (_opus_decoder) {
            esp_opus_dec_close(_opus_decoder);
            _opus_decoder = nullptr;
        }
        if (_playback_queue) {
            xQueueReset(_playback_queue);
        }
        xSemaphoreGive(_playback_mutex);
    } else {
        if (_opus_decoder) {
            esp_opus_dec_close(_opus_decoder);
            _opus_decoder = nullptr;
        }
    }
    free(_encoded_frame);
    _encoded_frame = nullptr;
    _opus_input_size = 0;
    _opus_output_size = 0;
    _decoder_frame_duration_ms = 0;
    {
        ScopedSemaphoreLock lock(_playback_mutex, portMAX_DELAY);
        if (_playback_queue) vQueueDeleteWithCaps(_playback_queue);
        _playback_queue = nullptr;
    }
    const bool released = releaseAudioSession();
    if (!released) {
        ESP_UTILS_LOGE("Xiaozhi retained the audio session after shutdown failure");
    }
    return released;
}

bool XiaozhiApp::releaseAudioSession()
{
    if (_codec_claimed.load()) {
        const esp_err_t stop_result = bsp_extra_codec_dev_stop();
        if (stop_result != ESP_OK) {
            ESP_UTILS_LOGE("Xiaozhi codec shutdown failed: %s", esp_err_to_name(stop_result));
            return false;
        }
        _codec_claimed.store(false);
    }
    if (_audio_session_acquired.load()) {
        const esp_err_t release_result =
            bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
        if (release_result != ESP_OK) {
            ESP_UTILS_LOGE("Xiaozhi audio-session release failed: %s",
                           esp_err_to_name(release_result));
            return false;
        }
        _audio_session_acquired.store(false);
    }
    return true;
}

bool XiaozhiApp::setAudioModes(bool wake_word, bool voice)
{
    _capture_enabled.store(false);
    _uplink_ready.store(false);
    _uplink_started_tick.store(0);
    _voice_enabled.store(false);
    bool success = !wake_word && !voice;
    if (_audio_processor && _audio_processor->isReady()) {
        wake_word = wake_word && _audio_processor->hasWakeWord();
        bool voice_ok = _audio_processor->enableVoiceProcessing(voice);
        bool wake_word_ok = _audio_processor->enableWakeWord(wake_word);
        success = voice_ok && wake_word_ok;
        if (!success) {
            (void)_audio_processor->enableWakeWord(false);
            (void)_audio_processor->enableVoiceProcessing(false);
            wake_word = false;
            voice = false;
        }
    }
    _voice_enabled.store(voice && success);
    _capture_enabled.store((wake_word || voice) && success);
    return success;
}

bool XiaozhiApp::enterReadyAudioMode()
{
    stopEncoder();
    if (!_visible.load() || !_network_ready.load() || !_transport_connected || !_chat_started.load())
        return setAudioModes(false, false);
    if (!releasePlaybackResources() || !_audio_processor || !_audio_processor->resumeWakeWord()) {
        setError(ESP_ERR_NO_MEM, "Restore wake word model");
        shutdownAudio();
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }
    const bool enable_wake_word =
        _visible.load() && _network_ready.load() && _transport_connected &&
        _chat_started.load();
    if (setAudioModes(enable_wake_word, false)) {
        return true;
    }

    ESP_UTILS_LOGE("Failed to enable Xiaozhi wake-word detection");
    shutdownAudio();
    setError(ESP_FAIL, "Enable Xiaozhi wake-word detection");
    schedulePrepare(RETRY_INTERVAL_MS);
    return false;
}

bool XiaozhiApp::configureDecoder(
    uint16_t sample_rate,
    uint16_t frame_duration_ms
)
{
    switch (sample_rate) {
    case 8000:
    case 12000:
    case 16000:
    case 24000:
    case 48000:
        break;
    default:
        ESP_UTILS_LOGE(
            "Unsupported Xiaozhi output sample rate: %u",
            static_cast<unsigned int>(sample_rate)
        );
        return false;
    }

    esp_opus_dec_frame_duration_t frame_duration;
    switch (frame_duration_ms) {
    case 5:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_5_MS;
        break;
    case 10:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_10_MS;
        break;
    case 20:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        break;
    case 40:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        break;
    case 60:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        break;
    case 80:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_80_MS;
        break;
    case 100:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_100_MS;
        break;
    case 120:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
        break;
    default:
        ESP_UTILS_LOGE(
            "Unsupported Xiaozhi Opus frame duration: %u ms",
            static_cast<unsigned int>(frame_duration_ms)
        );
        return false;
    }

    if (_opus_decoder && _decoder_frame_duration_ms == frame_duration_ms) {
        return true;
    }

    if (_opus_decoder) {
        esp_opus_dec_close(_opus_decoder);
        _opus_decoder = nullptr;
    }
    _decoder_frame_duration_ms = 0;

    esp_opus_dec_cfg_t decoder_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    // Opus packets support native decoding at any supported output rate.
    decoder_config.sample_rate = DEVICE_AUDIO_SAMPLE_RATE;
    decoder_config.channel = ESP_AUDIO_MONO;
    decoder_config.frame_duration = frame_duration;
    decoder_config.self_delimited = false;
    auto decoder_ret = esp_opus_dec_open(
        &decoder_config, sizeof(decoder_config), &_opus_decoder
    );
    if (decoder_ret != ESP_AUDIO_ERR_OK || !_opus_decoder) {
        ESP_UTILS_LOGE(
            "Open Xiaozhi Opus decoder failed: %d",
            static_cast<int>(decoder_ret)
        );
        _opus_decoder = nullptr;
        return false;
    }

    _decoder_frame_duration_ms = frame_duration_ms;
    ESP_UTILS_LOGI("Xiaozhi output: Opus decoded directly at %u Hz, %u ms",
                   (unsigned)DEVICE_AUDIO_SAMPLE_RATE, (unsigned)frame_duration_ms);
    return true;
}

esp_err_t XiaozhiApp::openAudioChannel()
{
    XiaozhiClient *protocol = _protocol.load();
    if (!protocol) {
        return ESP_ERR_INVALID_STATE;
    }

    ChannelState channel = _channel_state.load();
    if (channel == ChannelState::Open) {
        return ESP_OK;
    }
    if (channel != ChannelState::Closed) {
        return ESP_ERR_INVALID_STATE;
    }

    _channel_state.store(ChannelState::Opening);
    esp_err_t ret = protocol->openAudioChannel();
    if (ret != ESP_OK) {
        resetChannelState();
        return ret;
    }

    _channel_state.store(ChannelState::Open);
    _server_goodbye_generation.store(0);
    return ESP_OK;
}

esp_err_t XiaozhiApp::startAutoStopTurn(const char *wake_word)
{
    if (!ensureEncoder()) return ESP_ERR_NO_MEM;
    esp_err_t ret = openAudioChannel();
    if (ret != ESP_OK) {
        return ret;
    }

    _listen_mode = ListenMode::AutoStop;
    _awaiting_response = false;
    _tts_active = false;
    _tts_stop_pending.store(false);
    _accept_playback.store(false);

    if (!_audio_tx_mutex ||
            xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    XiaozhiClient *protocol = _protocol.load();
    if (!protocol || _channel_state.load() != ChannelState::Open) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        if (wake_word && wake_word[0]) {
            ret = protocol->sendWakeWordDetected(wake_word);
        }
        if (ret == ESP_OK) {
            ret = protocol->sendStartListening(
                XiaozhiClient::ListeningMode::AutoStop
            );
        }
    }
    xSemaphoreGive(_audio_tx_mutex);
    if (ret == ESP_OK) {
        ESP_UTILS_LOGI("AUTO listening turn started");
    }
    return ret;
}

bool XiaozhiApp::startListening(ListenMode mode, const char *wake_word)
{
    auto restore_audio = [this]() {
        const bool can_capture =
            _visible.load() && _network_ready.load() && _transport_connected &&
            _chat_started.load() && _state.load() == State::Ready &&
            _audio_processor && _audio_processor->isReady();
        if (can_capture) {
            enterReadyAudioMode();
        } else {
            setAudioModes(false, false);
        }
    };

    const bool auto_stop_mode = mode == ListenMode::AutoStop;
    _accept_playback.store(false);
    _uplink_ready.store(false);
    if (!_network_ready.load() || !_transport_connected ||
            !_chat_started.load()) {
        restore_audio();
        return false;
    }
    if (!ensureAudioReady()) {
        restore_audio();
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    XiaozhiClient *protocol = _protocol.load();
    ChannelState channel = _channel_state.load();
    if (!protocol ||
            (channel != ChannelState::Closed &&
             channel != ChannelState::Open)) {
        restore_audio();
        return false;
    }

    _turn_generation.fetch_add(1);
    _pcm_frames_captured.store(0);
    _pcm_frames_dropped.store(0);
    _opus_packets_sent.store(0);
    _pcm_peak.store(0);
    _pcm_mean_abs.store(0);
    _input_pcm_peak.store(0);
    _input_pcm_mean_abs.store(0);
    if (!resetPlaybackBuffer(true) ||
            !waitForPlaybackIdle(PLAYBACK_IDLE_WAIT_MS)) {
        closeChannel(true);
        shutdownAudio();
        setError(ESP_ERR_TIMEOUT, "Reset Xiaozhi playback");
        schedulePrepare(RETRY_INTERVAL_MS);
        restore_audio();
        return false;
    }

    if (_channel_state.load() == ChannelState::Closed) {
        setState(State::Connecting);
    }
    esp_err_t ret = ESP_OK;
    if (!ensureEncoder()) {
        setError(ESP_ERR_NO_MEM, "Prepare Xiaozhi encoder");
        shutdownAudio();
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }
    if (auto_stop_mode) {
        ret = startAutoStopTurn(wake_word);
    } else {
        ret = openAudioChannel();
        if (ret == ESP_OK && _audio_tx_mutex &&
                xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
            ret = protocol->sendStartListening(
                XiaozhiClient::ListeningMode::ManualStop
            );
            xSemaphoreGive(_audio_tx_mutex);
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret != ESP_OK) {
        closeChannel(true);
        destroyChat();
        setError(ret, "Start Xiaozhi listening");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    _listen_mode = mode;
    _awaiting_response = false;
    _tts_active = false;
    _tts_stop_pending.store(false);
    setState(State::Listening);
    _pcm_frames_captured.store(0);
    _pcm_frames_dropped.store(0);
    _opus_packets_sent.store(0);
    _pcm_peak.store(0);
    _pcm_mean_abs.store(0);
    _input_pcm_peak.store(0);
    _input_pcm_mean_abs.store(0);
    if (!setAudioModes(false, true)) {
        closeChannel(true);
        shutdownAudio();
        setError(ESP_FAIL, "Enable Xiaozhi voice processing");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }
    _uplink_ready.store(true);
    _uplink_started_tick.store(xTaskGetTickCount());
    ESP_UTILS_LOGI(
        "Listening started (mode=%s, source=%s)",
        auto_stop_mode ? "auto" : "manual",
        wake_word && wake_word[0] ? "wake" : "button"
    );
    return true;
}

void XiaozhiApp::finishListening()
{
    if (_listen_mode != ListenMode::Manual ||
            _channel_state.load() != ChannelState::Open) {
        return;
    }
    esp_err_t ret = stopUplink(true);
    if (ret != ESP_OK) {
        closeChannel(true);
        destroyChat();
        if (_network_ready.load()) {
            setError(ret, "Stop Xiaozhi listening");
            schedulePrepare(RETRY_INTERVAL_MS);
        }
        return;
    }
    _awaiting_response = true;
    setState(State::Processing);
}

esp_err_t XiaozhiApp::stopUplink(bool notify_server)
{
    setAudioModes(false, false);
    if (!_audio_tx_mutex ||
            xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (notify_server) {
        ret = ESP_ERR_INVALID_STATE;
        XiaozhiClient *protocol = _protocol.load();
        if (_channel_state.load() == ChannelState::Open && protocol) {
            ret = protocol->sendStopListening();
        }
    }
    xSemaphoreGive(_audio_tx_mutex);
    return ret;
}

void XiaozhiApp::cancelManualConversation()
{
    closeChannel(true);
    setState(State::Ready);
    enterReadyAudioMode();
}

void XiaozhiApp::closeChannel(bool abort_speaking)
{
    setAudioModes(false, false);
    _accept_playback.store(false);
    _turn_generation.fetch_add(1);
    resetPlaybackBuffer(false);
    XiaozhiClient *protocol = _protocol.load();
    ChannelState channel = _channel_state.load();
    if (protocol && (channel == ChannelState::Open ||
                   channel == ChannelState::Opening)) {
        _channel_state.store(ChannelState::Closing);
        if (_audio_tx_mutex &&
                xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
            if (abort_speaking && (_tts_active ||
                                   _state.load() == State::Speaking)) {
                (void)protocol->sendAbortSpeaking(
                    XiaozhiClient::AbortReason::None
                );
            }
            if (_listen_mode != ListenMode::None) {
                (void)protocol->sendStopListening();
            }
            (void)protocol->closeAudioChannel();
            xSemaphoreGive(_audio_tx_mutex);
        }
    }
    resetChannelState();
}

void XiaozhiApp::resetChannelState()
{
    _uplink_ready.store(false);
    _channel_state.store(ChannelState::Closed);
    _accept_playback.store(false);
    _awaiting_response = false;
    _tts_active = false;
    _tts_stop_pending.store(false);
    _drain_event_posted.store(false);
    _restart_transport_after_playback.store(false);
    _listen_mode = ListenMode::None;
    resetPlaybackBuffer(false);
}

void XiaozhiApp::handlePlaybackDrained()
{
    _accept_playback.store(false);
    bool ready = false;
    if (_playback_mutex &&
            xSemaphoreTake(
                _playback_mutex, pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS)
            ) == pdTRUE) {
        ready = playbackDrainReady();
        xSemaphoreGive(_playback_mutex);
    }
    if (!ready) {
        _accept_playback.store(true);
        _drain_event_posted.store(false);
        return;
    }

    _tts_stop_pending.store(false);
    _drain_event_posted.store(false);
    if (_restart_transport_after_playback.exchange(false)) {
        ESP_UTILS_LOGI(
            "Final playback drained; restarting Xiaozhi transport now"
        );
        destroyChat();
        if (_network_ready.load()) {
            setState(State::Connecting);
            schedulePrepare(0);
        } else {
            setState(State::NetworkRequired);
        }
        return;
    }

    if (!_visible.load() || !_network_ready.load() ||
            _channel_state.load() != ChannelState::Open) {
        return;
    }
    if (_listen_mode == ListenMode::AutoStop) {
        if (!resetPlaybackBuffer(true)) {
            closeChannel(false);
            shutdownAudio();
            setError(ESP_FAIL, "Reset Xiaozhi decoder after playback");
            schedulePrepare(RETRY_INTERVAL_MS);
            return;
        }

        _turn_generation.fetch_add(1);
        _pcm_frames_captured.store(0);
        _pcm_frames_dropped.store(0);
        _opus_packets_sent.store(0);
        _pcm_peak.store(0);
        _pcm_mean_abs.store(0);
        _input_pcm_peak.store(0);
        _input_pcm_mean_abs.store(0);

        esp_err_t ret = startAutoStopTurn();
        if (ret != ESP_OK) {
            closeChannel(false);
            destroyChat();
            setError(ret, "Restart Xiaozhi AUTO listening");
            schedulePrepare(RETRY_INTERVAL_MS);
            return;
        }

        _listen_mode = ListenMode::AutoStop;
        _awaiting_response = false;
        setState(State::Listening);
        if (!setAudioModes(false, true)) {
            closeChannel(false);
            shutdownAudio();
            destroyChat();
            setError(ESP_FAIL, "Resume Xiaozhi voice processing");
            schedulePrepare(RETRY_INTERVAL_MS);
            return;
        }
        _uplink_ready.store(true);
        _uplink_started_tick.store(xTaskGetTickCount());
        ESP_UTILS_LOGI("Listening resumed (mode=auto)");
    } else {
        _listen_mode = ListenMode::None;
        _awaiting_response = false;
        setState(State::Ready);
        enterReadyAudioMode();
    }
}

void XiaozhiApp::checkPlaybackDrained()
{
    if (_drain_event_posted.load() || !playbackDrainReady()) {
        return;
    }
    if (!_drain_event_posted.exchange(true)) {
        ControlEvent event = {};
        event.type = ControlEventType::PlaybackDrained;
        event.chat_generation = _chat_generation.load();
        event.turn_generation = _turn_generation.load();
        if (!postControlEvent(event)) {
            _drain_event_posted.store(false);
        }
    }
}

bool XiaozhiApp::playbackDrainReady() const
{
    if (!_tts_stop_pending.load() || _playback_busy.load() ||
            !_playback_queue || uxQueueMessagesWaiting(_playback_queue) != 0) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t delay = pdMS_TO_TICKS(PLAYBACK_DRAIN_MS);
    return now - _tts_stop_tick.load() >= delay &&
           now - _last_audio_packet_tick.load() >= delay &&
           now - _last_audio_write_tick.load() >= delay;
}

bool XiaozhiApp::resetPlaybackBuffer(bool reset_decoder)
{
    if (!_playback_mutex ||
            xSemaphoreTake(
                _playback_mutex, pdMS_TO_TICKS(PLAYBACK_IDLE_WAIT_MS)
            ) != pdTRUE) {
        return false;
    }

    if (_playback_queue) {
        xQueueReset(_playback_queue);
    }
    bool ok = true;
    if (reset_decoder) {
        ok = !_opus_decoder ||
             esp_opus_dec_reset(_opus_decoder) == ESP_AUDIO_ERR_OK;
    }
    xSemaphoreGive(_playback_mutex);
    return ok;
}

bool XiaozhiApp::waitForPlaybackIdle(uint32_t timeout_ms) const
{
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (_playback_busy.load()) {
        if (xTaskGetTickCount() - started >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void XiaozhiApp::inputAudio()
{
    int16_t interleaved[INPUT_READ_FRAMES * DEVICE_TDM_CHANNELS] = {};
    int16_t mono[INPUT_READ_FRAMES] = {};
    uint32_t failures = 0;
    while (_audio_tasks_running.load()) {
        if (_playback_enabled.load()) {
            _playback_active.store(true);
            // Recheck after publishing activity so teardown can safely join.
            if (_playback_enabled.load() && _audio_tasks_running.load()) playAudio();
            _playback_active.store(false);
            continue;
        }
        if (!_capture_enabled.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t bytes = 0;
        esp_err_t ret = bsp_extra_i2s_read(interleaved, sizeof(interleaved), &bytes, 100);
        if (ret == ESP_OK && bytes && _audio_processor) {
            failures = 0;
            size_t frames = bytes / (DEVICE_TDM_CHANNELS * sizeof(int16_t));
            uint32_t peak = 0, sum = 0;
            for (size_t i = 0; i < frames; ++i) {
                mono[i] = interleaved[i * DEVICE_TDM_CHANNELS + DEVICE_MIC1_SLOT];
                int32_t value = mono[i];
                uint32_t magnitude = value < 0 ? -value : value;
                peak = std::max(peak, magnitude);
                sum += magnitude;
            }
            _input_pcm_peak.store(peak);
            _input_pcm_mean_abs.store(frames ? sum / frames : 0);
            _audio_processor->feed(mono, frames);
        } else if (ret != ESP_OK && (++failures == 1 || failures % 50 == 0)) {
            ESP_UTILS_LOGW("Microphone read failed: %s", esp_err_to_name(ret));
        }
    }
    ESP_UTILS_LOGI("Shared audio worker stack minimum free=%u bytes",
                   (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    _input_task_running.store(false);
    vTaskDelete(nullptr);
}

void XiaozhiApp::encodeAudioFrame(const int16_t *pcm, size_t count)
{
    if (!_audio_tasks_running.load() || !_capture_enabled.load() ||
            !_voice_enabled.load() || !_uplink_ready.load() || !pcm ||
            count != XiaozhiAudioProcessor::PCM_FRAME_SAMPLES ||
            !_opus_encoder || !_encoded_frame) return;

    uint32_t captured = _pcm_frames_captured.fetch_add(1) + 1;
    if (captured == 1) ESP_UTILS_LOGI("Captured first conversation PCM frame (20 ms)");
    esp_audio_enc_in_frame_t input = {};
    input.buffer = reinterpret_cast<uint8_t *>(const_cast<int16_t *>(pcm));
    input.len = count * sizeof(int16_t);
    esp_audio_enc_out_frame_t output = {};
    output.buffer = _encoded_frame;
    output.len = _opus_output_size;
    if (esp_opus_enc_process(_opus_encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
            output.encoded_bytes <= 0) {
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        event.error = ESP_FAIL;
        copyString(event.detail, sizeof(event.detail), "Encode Xiaozhi audio");
        _uplink_ready.store(false);
        postControlEvent(event);
        return;
    }

    esp_err_t ret = ESP_ERR_INVALID_STATE;
    {
        ScopedSemaphoreLock lock(_audio_tx_mutex, pdMS_TO_TICKS(1000));
        XiaozhiClient *protocol = _protocol.load();
        if (lock && _voice_enabled.load() && _uplink_ready.load() &&
                _channel_state.load() == ChannelState::Open && protocol) {
            ret = protocol->sendAudio(_encoded_frame, output.encoded_bytes, 0);
        } else if (!_voice_enabled.load() || !_uplink_ready.load()) {
            return;
        }
    }
    if (ret == ESP_OK) {
        uint32_t sent = _opus_packets_sent.fetch_add(1) + 1;
        if (sent == 1) {
            _uplink_started_tick.store(0);
            ESP_UTILS_LOGI("Sent Opus packet #1 (%d bytes); shared audio stack minimum free=%u bytes",
                           output.encoded_bytes, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    } else {
        _uplink_ready.store(false);
        _voice_enabled.store(false);
        _capture_enabled.store(false);
        ControlEvent event = {};
        event.type = ControlEventType::ChatError;
        event.error = ret;
        event.chat_generation = _chat_generation.load();
        event.turn_generation = _turn_generation.load();
        copyString(event.detail, sizeof(event.detail), "Send Xiaozhi audio data");
        postControlEvent(event);
    }
}

void XiaozhiApp::playAudio()
{
    int16_t *mono = nullptr, *stereo = nullptr;
    size_t mono_capacity = 0, stereo_capacity = 0;
    auto release_buffers = [&]() {
        free(mono); free(stereo);
        mono = stereo = nullptr;
        mono_capacity = stereo_capacity = 0;
    };
    auto prepare_buffers = [&](const AudioPacket &audio) {
        const size_t samples = DEVICE_AUDIO_SAMPLE_RATE * audio.frame_duration_ms / 1000;
        if (!samples || samples > MAX_DEVICE_PCM_SAMPLES) return false;
        const size_t mono_bytes = samples * sizeof(int16_t);
        const size_t stereo_bytes = mono_bytes * 2;
        if (mono_capacity >= mono_bytes && stereo_capacity >= stereo_bytes) return true;
        release_buffers();
        mono = static_cast<int16_t *>(malloc(mono_bytes));
        stereo = static_cast<int16_t *>(malloc(stereo_bytes));
        if (mono && stereo) {
            mono_capacity = mono_bytes; stereo_capacity = stereo_bytes;
            return true;
        }
        release_buffers();
        return false;
    };

    uint32_t decode_failures = 0;
    AudioPacket packet = {};
    while (_audio_tasks_running.load() && _playback_enabled.load()) {
        if (!_playback_queue ||
                xQueuePeek(_playback_queue, &packet, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (_state.load() != State::Speaking) release_buffers();
            checkPlaybackDrained();
            continue;
        }
        if (_state.load() != State::Speaking) {
            release_buffers();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        bool received = false;
        bool decoded = false;
        bool output_error = false;
        esp_err_t output_error_code = ESP_FAIL;
        size_t playback_samples = 0;
        const int16_t *playback_pcm = nullptr;
        if (_playback_mutex &&
                xSemaphoreTake(_playback_mutex, portMAX_DELAY) == pdTRUE) {
            received =
                xQueueReceive(_playback_queue, &packet, 0) == pdTRUE;
            if (received) {
                _playback_busy.store(true);
            }
            if (received && _accept_playback.load() &&
                    packet.turn_generation == _turn_generation.load()) {
                if (!configureDecoder(
                        packet.sample_rate, packet.frame_duration_ms
                    )) {
                    output_error = true;
                } else if (!prepare_buffers(packet)) {
                    output_error = true;
                    output_error_code = ESP_ERR_NO_MEM;
                } else {
                    esp_audio_dec_in_raw_t input = {};
                    input.buffer = packet.data;
                    input.len = packet.size;
                    input.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
                    esp_audio_dec_out_frame_t output = {};
                    output.buffer = reinterpret_cast<uint8_t *>(mono);
                    output.len = mono_capacity;
                    esp_audio_dec_info_t info = {};
                    bool opus_ok =
                        esp_opus_dec_decode(
                            _opus_decoder, &input, &output, &info
                        ) == ESP_AUDIO_ERR_OK &&
                        output.decoded_size > 0 &&
                        output.decoded_size <= mono_capacity;
                    if (opus_ok) {
                        playback_samples = output.decoded_size / sizeof(int16_t);
                        if (playback_samples <= stereo_capacity / (2 * sizeof(int16_t))) {
                            playback_pcm = mono;
                            decoded = true;
                        } else {
                            output_error = true;
                        }
                    }
                }
            }
            xSemaphoreGive(_playback_mutex);
        }
        if (!received) {
            checkPlaybackDrained();
            continue;
        }

        if (output_error) {
            _accept_playback.store(false);
            ControlEvent event = {};
            event.type = ControlEventType::AudioError;
            event.error = output_error_code;
            copyString(
                event.detail,
                sizeof(event.detail),
                output_error_code == ESP_ERR_NO_MEM ?
                "Allocate Xiaozhi playback buffers" : "Configure Xiaozhi output audio"
            );
            postControlEvent(event);
        } else if (!decoded && _accept_playback.load() &&
                packet.turn_generation == _turn_generation.load()) {
            ++decode_failures;
            if (decode_failures == 1 || (decode_failures % 20) == 0) {
                ESP_UTILS_LOGW(
                    "Xiaozhi Opus decode failed (count=%lu)",
                    static_cast<unsigned long>(decode_failures)
                );
            }
        } else if (decoded && _accept_playback.load() &&
                packet.turn_generation == _turn_generation.load()) {
            for (size_t i = 0; i < playback_samples; ++i) {
                stereo[i * 2] = playback_pcm[i];
                stereo[i * 2 + 1] = playback_pcm[i];
            }
            size_t written = 0;
            size_t write_size =
                playback_samples * 2 * sizeof(int16_t);
            esp_err_t write_ret = bsp_extra_i2s_write(
                stereo, write_size, &written, 200
            );
            if (write_ret == ESP_OK && written == write_size) {
                _last_audio_write_tick.store(xTaskGetTickCount());
            } else {
                _accept_playback.store(false);
                ControlEvent event = {};
                event.type = ControlEventType::AudioError;
                event.error = write_ret == ESP_OK ? ESP_FAIL : write_ret;
                copyString(
                    event.detail, sizeof(event.detail), "Write Xiaozhi audio"
                );
                postControlEvent(event);
            }
        }
        _playback_busy.store(false);
        checkPlaybackDrained();
    }
    release_buffers();
    _playback_busy.store(false);
}

const char *XiaozhiApp::stateText(State state)
{
    switch (state) {
    case State::NetworkRequired:
        return xiaozhiUiText(XiaozhiUiText::NetworkRequired);
    case State::Preparing:
        return xiaozhiUiText(XiaozhiUiText::Preparing);
    case State::ActivationRequired:
        return xiaozhiUiText(XiaozhiUiText::ActivationRequired);
    case State::Connecting:
        return xiaozhiUiText(XiaozhiUiText::Connecting);
    case State::Ready:
        return xiaozhiUiText(XiaozhiUiText::Ready);
    case State::Listening:
        return xiaozhiUiText(XiaozhiUiText::Listening);
    case State::Processing:
        return xiaozhiUiText(XiaozhiUiText::Processing);
    case State::Speaking:
        return xiaozhiUiText(XiaozhiUiText::Speaking);
    case State::Error:
        return xiaozhiUiText(XiaozhiUiText::Error);
    }
    return "";
}

bool XiaozhiApp::tickReached(TickType_t now, TickType_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

void XiaozhiApp::controlTaskEntry(void *arg)
{
    static_cast<XiaozhiApp *>(arg)->controlLoop();
}

void XiaozhiApp::inputTaskEntry(void *arg)
{
    static_cast<XiaozhiApp *>(arg)->inputAudio();
}

void XiaozhiApp::ipEventHandler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)event_base;
    (void)event_id;
    (void)event_data;
    auto *instance = static_cast<XiaozhiApp *>(arg);
    if (!instance) {
        return;
    }
    ScopedOptionalCounter async_user(&instance->_async_post_users);
    if (!instance->_shutdown.load()) {
        instance->updateNetworkState();
    }
}

void XiaozhiApp::uiTimerCallback(lv_timer_t *timer)
{
    auto *instance = static_cast<XiaozhiApp *>(lv_timer_get_user_data(timer));
    if (instance) {
        instance->refreshUi();
    }
}

void XiaozhiApp::uiActionCallback(void *context)
{
    auto *instance = static_cast<XiaozhiApp *>(context);
    if (!instance) {
        return;
    }
    ControlEvent event = {};
    event.type = ControlEventType::ActionPressed;
    instance->postControlEvent(event);
}

} // namespace esp_brookesia::apps
