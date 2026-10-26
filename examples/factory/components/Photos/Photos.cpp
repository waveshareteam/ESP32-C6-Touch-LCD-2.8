/* SPDX-License-Identifier: Apache-2.0 */
#include "Photos.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <cstdio>
#include <new>
LV_IMAGE_DECLARE(img_app_gallery);
void photos_decoder_init();
namespace esp_brookesia::apps {
Photos *Photos::instance_;
Photos *Photos::requestInstance() { if (!instance_) instance_ = new(std::nothrow) Photos; return instance_; }
Photos::Photos(): App("Photos", &img_app_gallery, true, false, false) {}
Photos::~Photos() { close(); if (mutex_) vSemaphoreDelete(mutex_); instance_ = nullptr; }
bool Photos::run()
{
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    photos_decoder_init();
    wanted_ = 0; shown_ = -1;
    snprintf(message_, sizeof(message_), "Reading SD card...");
    root_ = app_ui::box(lv_screen_active(), 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, 0, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root_, event, LV_EVENT_ALL, this);
    image_obj_ = lv_image_create(root_);
    lv_obj_remove_flag(image_obj_, LV_OBJ_FLAG_CLICKABLE);
    message_obj_ = app_ui::label(root_, "Reading SD card...", 12, 132, 216, &lv_font_montserrat_16, 0xffffff);
    lv_obj_set_style_text_align(message_obj_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(message_obj_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(message_obj_, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(message_obj_, 8, 0);
    timer_ = lv_timer_create(poll, 100, this);
    if (!timer_ || !start("photos", 4096, 2)) { close(); return false; }
    return true;
}
void Photos::work()
{
    media_files_t files = {}; char error[128] = {};
    const esp_err_t ret = bsp_sdcard_acquire();
    mounted_ = ret == ESP_OK;
    if (!mounted_) snprintf(error, sizeof(error), "SD unavailable: %s\nSwipe up from bottom to exit", esp_err_to_name(ret));
    else media_files_scan("/sdcard/photo", ".jpg", ".jpeg", &files, cancel, this, error, sizeof(error));
    xSemaphoreTake(mutex_, portMAX_DELAY);
    media_files_free(&files_); files_ = files;
    snprintf(message_, sizeof(message_), "%s", error);
    xSemaphoreGive(mutex_);
}
void Photos::refresh()
{
    char message[160] = {}; std::string selected;
    const int wanted = wanted_.load();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!files_.count) snprintf(message, sizeof(message), "%s", message_[0] ? message_ : "No JPEG photos.\nUse /photo on the SD card.");
    else selected = files_.paths[wanted % files_.count];
    xSemaphoreGive(mutex_);
    if (!selected.empty() && shown_ != wanted) {
        lv_image_set_src(image_obj_, nullptr);
        lv_image_cache_drop(&source_);
        path_ = "C6PHOTO:" + selected;
        source_ = {};
        source_.header.magic = LV_IMAGE_HEADER_MAGIC;
        source_.header.cf = LV_COLOR_FORMAT_RAW;
        source_.data = reinterpret_cast<const uint8_t *>(path_.c_str());
        source_.data_size = path_.size() + 1;
        lv_image_header_t header = {};
        if (lv_image_decoder_get_info(&source_, &header) == LV_RESULT_OK) {
            lv_image_set_src(image_obj_, &source_); lv_obj_center(image_obj_);
        } else snprintf(message_, sizeof(message_), "Unsupported / damaged JPEG\nSwipe left / right to try another");
        shown_ = wanted;
    }
    if (!selected.empty() && !lv_image_get_src(image_obj_)) snprintf(message, sizeof(message), "%s", message_);
    app_ui::text(message_obj_, message);
    if (message[0]) lv_obj_remove_flag(message_obj_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(message_obj_, LV_OBJ_FLAG_HIDDEN);
}
void Photos::event(lv_event_t *e)
{
    auto *self = static_cast<Photos *>(lv_event_get_user_data(e)); auto *input = lv_indev_active(); if (!input) return;
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) lv_indev_get_point(input, &self->touch_start_);
    if (code != LV_EVENT_GESTURE) return;
    const auto direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_TOP && self->touch_start_.y >= BSP_LCD_V_RES - 50) { lv_indev_wait_release(input); self->back(); return; }
    if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
    xSemaphoreTake(self->mutex_, portMAX_DELAY); const size_t count = self->files_.count; xSemaphoreGive(self->mutex_);
    if (count) {
        const int index = self->wanted_.load() % count;
        self->wanted_ = (index + (direction == LV_DIR_LEFT ? 1 : int(count) - 1)) % count;
        self->refresh();
    }
    lv_indev_wait_release(input);
}
void Photos::poll(lv_timer_t *timer) { static_cast<Photos *>(lv_timer_get_user_data(timer))->refresh(); }
void Photos::release()
{
    stop();
    if (image_obj_) lv_image_set_src(image_obj_, nullptr);
    lv_image_cache_drop(&source_); path_.clear(); shown_ = -1;
    if (mounted_) { bsp_sdcard_release(); mounted_ = false; }
    media_files_free(&files_);
}
bool Photos::pause() { if (timer_) lv_timer_pause(timer_); release(); return true; }
bool Photos::resume()
{
    snprintf(message_, sizeof(message_), "Reading SD card...");
    if (timer_) lv_timer_resume(timer_);
    return start("photos", 4096, 2);
}
bool Photos::close()
{
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    release();
    if (root_) lv_obj_delete(root_);
    root_ = image_obj_ = message_obj_ = nullptr;
    return true;
}
}
