/* SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0 */
#include "XiaozhiUi.hpp"
#include <cstdio>
#include <cstring>

/* Full common Chinese fonts, linked into the app: no SD/SPIFFS dependency. */
LV_FONT_DECLARE(font_puhui_16_4);
LV_IMAGE_DECLARE(emoji_1f642_64);

namespace esp_brookesia::apps {
namespace {
void plain(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(object, lv_color_hex(0x1d2b42), 0);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_16, 0);
}
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, bool black = false)
{
    auto *object = lv_obj_create(parent); plain(object);
    lv_obj_set_pos(object, x, y); lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, black ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}
lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, int w, const lv_font_t *font)
{
    auto *object = lv_label_create(parent); plain(object);
    lv_obj_set_pos(object, x, y); lv_obj_set_width(object, w);
    lv_obj_set_style_text_font(object, font, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(object, value);
    return object;
}
bool text(lv_obj_t *object, const char *value)
{
    if (!object) return false;
    value = value ? value : "";
    if (!strcmp(lv_label_get_text(object), value)) return false;
    lv_label_set_text(object, value); return true;
}
void hidden(lv_obj_t *object, bool value)
{
    if (value) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}
}

bool XiaozhiUi::create(lv_obj_t *parent, ActionCallback action, void *context)
{
    if (!parent) return false;
    destroy(); _action_callback = action; _context = context;
    _root = box(parent, 0, 0, 240, 320);
    _network = label(_root, "", 212, 10, 24, &lv_font_montserrat_16);
    _status = label(_root, "Preparing", 28, 10, 180, &font_puhui_16_4);
    lv_obj_set_style_text_align(_status, LV_TEXT_ALIGN_CENTER, 0);
    _action = lv_image_create(_root);
    lv_image_set_src(_action, &emoji_1f642_64);
    lv_obj_center(_action);
    lv_obj_add_flag(_action, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_action, onAction, LV_EVENT_CLICKED, this);

    _activation = box(_root, 8, 60, 224, 210);
    auto *activation_title = label(_activation, "DEVICE ACTIVATION", 0, 4, 224, &lv_font_montserrat_14);
    lv_obj_set_style_text_align(activation_title, LV_TEXT_ALIGN_CENTER, 0);
    _code = label(_activation, "", 0, 33, 224, &lv_font_montserrat_32);
    lv_obj_set_style_text_align(_code, LV_TEXT_ALIGN_CENTER, 0);
    _instruction = label(_activation, "", 4, 86, 216, &font_puhui_16_4);
    lv_obj_set_height(_instruction, 106);
    lv_obj_set_style_text_align(_instruction, LV_TEXT_ALIGN_CENTER, 0);
    hidden(_activation, true);
    return true;
}
void XiaozhiUi::destroy()
{
    if (_root) lv_obj_delete(_root);
    _root = _network = _status = nullptr;
    _action = nullptr;
    _activation = _code = _instruction = nullptr;
}
void XiaozhiUi::setNetworkReady(bool ready) { text(_network, ready ? LV_SYMBOL_WIFI : ""); }
void XiaozhiUi::setStatus(const char *value) { text(_status, value); }
void XiaozhiUi::setActionEnabled(bool enabled)
{
    if (!_action) return;
    if (enabled) lv_obj_add_flag(_action, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(_action, LV_OBJ_FLAG_CLICKABLE);
}
void XiaozhiUi::setActivation(const char *code, const char *message, bool visible)
{
    if (!_activation) return;
    text(_code, code); text(_instruction, message);
    hidden(_activation, !visible);
}
void XiaozhiUi::onAction(lv_event_t *event)
{
    auto *self = static_cast<XiaozhiUi *>(lv_event_get_user_data(event));
    if (self->_action_callback) self->_action_callback(self->_context);
}
}
