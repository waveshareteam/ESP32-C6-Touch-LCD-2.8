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

bool XiaozhiUi::create(lv_obj_t *parent, ActionCallback action, void *context, bool multiline)
{
    (void)multiline;
    if (!parent) return false;
    destroy(); _action_callback = action; _context = context;
    _root = box(parent, 0, 0, 240, 320);
    auto *title = label(_root, "AIChats", 48, 14, 144, &lv_font_montserrat_20);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    auto *back = lv_button_create(_root); plain(back);
    lv_obj_set_pos(back, 4, 4); lv_obj_set_size(back, 40, 40);
    auto *arrow = label(back, LV_SYMBOL_LEFT, 0, 0, 28, &lv_font_montserrat_20);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, this);
    _network = label(_root, "Offline", 8, 46, 224, &lv_font_montserrat_12);
    lv_obj_set_style_text_align(_network, LV_TEXT_ALIGN_CENTER, 0);
    auto *smile = lv_image_create(_root);
    lv_image_set_src(smile, &emoji_1f642_64);
    lv_obj_set_pos(smile, 88, 90);
    _status = label(_root, "Preparing", 8, 178, 224, &font_puhui_16_4);
    lv_obj_set_style_text_align(_status, LV_TEXT_ALIGN_CENTER, 0);
    auto *hint = label(_root, "Say \"你好小智\"\nor tap Talk.", 8, 216, 224, &font_puhui_16_4);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    _action = lv_button_create(_root); plain(_action);
    lv_obj_set_pos(_action, 50, 276); lv_obj_set_size(_action, 140, 36);
    lv_obj_set_style_bg_color(_action, lv_color_hex(0x376df6), 0);
    lv_obj_set_style_bg_opa(_action, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_action, 12, 0);
    _action_label = label(_action, "Talk", 0, 0, 132, &lv_font_montserrat_16);
    lv_obj_set_style_text_color(_action_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(_action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(_action_label);
    lv_obj_add_event_cb(_action, onAction, LV_EVENT_CLICKED, this);

    _activation = box(_root, 8, 70, 224, 196);
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
    _action = _action_label = nullptr;
    _activation = _code = _instruction = nullptr;
}
void XiaozhiUi::setNetworkReady(bool ready) { text(_network, ready ? "Wi-Fi " LV_SYMBOL_OK : "Offline"); }
void XiaozhiUi::setStatus(const char *value) { text(_status, value); }
void XiaozhiUi::setAction(const char *value, bool enabled)
{
    text(_action_label, value);
    if (!_action) return;
    lv_obj_set_style_border_width(_action, enabled ? 2 : 1, 0);
    if (enabled) lv_obj_remove_state(_action, LV_STATE_DISABLED);
    else lv_obj_add_state(_action, LV_STATE_DISABLED);
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
void XiaozhiUi::onBack(lv_event_t *event)
{
    auto *self = static_cast<XiaozhiUi *>(lv_event_get_user_data(event));
    if (self->_back_callback) self->_back_callback(self->_context);
}
}
