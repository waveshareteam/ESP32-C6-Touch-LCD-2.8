/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "systems/phone/esp_brookesia_phone_app.hpp"
#include "app_support.hpp"
#include "media_files.h"
#include <string>
namespace esp_brookesia::apps {
class Photos : public systems::phone::App, private AppWorker {
public:
    static Photos *requestInstance();
    ~Photos() override;
protected:
    bool init() override { return true; }
    bool run() override;
    bool back() override { return notifyCoreClosed(); }
    bool close() override;
    bool deinit() override { return close(); }
    bool pause() override;
    bool resume() override;
private:
    Photos();
    void work() override;
    void refresh();
    void release();
    static void event(lv_event_t *);
    static void poll(lv_timer_t *);
    static bool cancel(void *ctx) { return static_cast<Photos *>(ctx)->stopping(); }
    static Photos *instance_;
    SemaphoreHandle_t mutex_ = nullptr;
    media_files_t files_ = {};
    std::atomic<int> wanted_{0};
    bool mounted_ = false;
    char message_[128] = {};
    std::string path_;
    int shown_ = -1;
    lv_point_t touch_start_ = {};
    lv_image_dsc_t source_ = {};
    lv_obj_t *root_ = nullptr, *image_obj_ = nullptr, *message_obj_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
}
