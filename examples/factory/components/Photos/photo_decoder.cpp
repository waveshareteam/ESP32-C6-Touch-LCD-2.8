/* SPDX-License-Identifier: Apache-2.0 */
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
constexpr size_t WORK_BYTES = 4096;
constexpr char PREFIX[] = "C6PHOTO:";
struct Decoder {
    JDEC jpeg = {};
    FILE *file = nullptr;
    uint8_t work[WORK_BYTES];
    uint16_t *strip = nullptr;
    lv_draw_buf_t row = {};
    int width = 0, height = 0, source_y = -1, next_y = 0;
    ~Decoder() { if (file) fclose(file); delete[] strip; }
};
const char *path(const lv_image_decoder_dsc_t *dsc)
{
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return nullptr;
    const auto *image = static_cast<const lv_image_dsc_t *>(dsc->src);
    if (image->header.cf != LV_COLOR_FORMAT_RAW || image->data_size <= sizeof(PREFIX) ||
        !image->data || memcmp(image->data, PREFIX, sizeof(PREFIX)-1)) return nullptr;
    return reinterpret_cast<const char *>(image->data) + sizeof(PREFIX)-1;
}
size_t input(JDEC *jpeg, uint8_t *buffer, size_t size)
{
    auto *state = static_cast<Decoder *>(jpeg->device);
    return buffer ? fread(buffer, 1, size, state->file) :
           fseek(state->file, static_cast<long>(size), SEEK_CUR) == 0 ? size : 0;
}
bool prepare(Decoder &state, const char *name)
{
    state.file = fopen(name, "rb");
    if (!state.file || jd_prepare(&state.jpeg, input, state.work, sizeof(state.work), &state) != JDR_OK ||
        !state.jpeg.width || !state.jpeg.height ||
        uint32_t(state.jpeg.width) * state.jpeg.height > 4 * 1024 * 1024) return false;
    state.width = BSP_LCD_H_RES;
    state.height = int(uint32_t(state.jpeg.height) * state.width / state.jpeg.width);
    if (state.height > BSP_LCD_V_RES) {
        state.height = BSP_LCD_V_RES;
        state.width = int(uint32_t(state.jpeg.width) * state.height / state.jpeg.height);
    }
    state.width = std::max(state.width, 1); state.height = std::max(state.height, 1);
    return true;
}
lv_result_t info(lv_image_decoder_t *, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    const char *name = path(dsc); if (!name) return LV_RESULT_INVALID;
    auto *state = new(std::nothrow) Decoder;
    if (!state) return LV_RESULT_INVALID;
    const bool ok = prepare(*state, name);
    if (ok) {
        header->cf = LV_COLOR_FORMAT_RGB565;
        header->w = state->width; header->h = state->height; header->stride = state->width * 2;
    }
    delete state;
    return ok ? LV_RESULT_OK : LV_RESULT_INVALID;
}
lv_result_t open(lv_image_decoder_t *, lv_image_decoder_dsc_t *dsc)
{
    const char *name = path(dsc); if (!name) return LV_RESULT_INVALID;
    auto *state = new(std::nothrow) Decoder;
    if (!state) return LV_RESULT_INVALID;
    if (!prepare(*state, name)) { delete state; return LV_RESULT_INVALID; }
    state->strip = new(std::nothrow) uint16_t[state->width * state->jpeg.msy * 8];
    if (!state->strip) { delete state; return LV_RESULT_INVALID; }
    state->row.header = dsc->header;
    state->row.header.h = 1;
    state->row.data_size = state->width * 2;
    dsc->user_data = state;
    return LV_RESULT_OK;
}
int output(JDEC *jpeg, void *bitmap, JRECT *rect)
{
    auto *state = static_cast<Decoder *>(jpeg->device);
    const auto *pixels = static_cast<const uint8_t *>(bitmap);
    const unsigned pitch = rect->right - rect->left + 1;
    const int x1 = (uint32_t(rect->left) * state->width + jpeg->width - 1) / jpeg->width;
    const int x2 = (uint32_t(rect->right + 1) * state->width + jpeg->width - 1) / jpeg->width;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        auto *row = state->strip + (y - state->source_y) * state->width;
        for (int x = x1; x < x2; ++x) {
            const unsigned sx = uint32_t(x) * jpeg->width / state->width;
            const auto *p = pixels + ((y - rect->top) * pitch + sx - rect->left) * 3;
            // LVGL's TJpgDec build supplies BGR888 pixels.
            row[x] = uint16_t((p[2] & 0xf8) << 8 | (p[1] & 0xfc) << 3 | p[0] >> 3);
        }
    }
    return 1;
}
bool decode_strip(Decoder &state)
{
    auto &jpeg = state.jpeg;
    state.source_y = state.source_y < 0 ? 0 : state.source_y + jpeg.msy * 8;
    if (state.source_y >= jpeg.height) return false;
    for (unsigned x = 0; x < jpeg.width; x += jpeg.msx * 8) {
        if (jpeg.nrst && jpeg.rst++ == jpeg.nrst) {
            if (jd_restart(&jpeg, jpeg.rsc++) != JDR_OK) return false;
            jpeg.rst = 1;
        }
        if (jd_mcu_load(&jpeg) != JDR_OK ||
            jd_mcu_output(&jpeg, output, x, state.source_y) != JDR_OK) return false;
    }
    vTaskDelay(1);
    return true;
}
lv_result_t area(lv_image_decoder_t *, lv_image_decoder_dsc_t *dsc,
                 const lv_area_t *full, lv_area_t *decoded)
{
    auto &state = *static_cast<Decoder *>(dsc->user_data);
    if (decoded->y1 == LV_COORD_MIN) {
        // LVGL can request a new draw region from the same open decoder.
        if (state.source_y >= 0) {
            if (fseek(state.file, 0, SEEK_SET) != 0) return LV_RESULT_INVALID;
            state.jpeg = {};
            if (jd_prepare(&state.jpeg, input, state.work, sizeof(state.work), &state) != JDR_OK)
                return LV_RESULT_INVALID;
            state.source_y = -1;
        }
        state.next_y = std::max(0, int(full->y1));
    }
    const int y = state.next_y++;
    if (y >= state.height || y > full->y2) return LV_RESULT_INVALID;
    const int sy = uint32_t(y) * state.jpeg.height / state.height;
    while (state.source_y < 0 || sy >= state.source_y + state.jpeg.msy * 8)
        if (!decode_strip(state)) return LV_RESULT_INVALID;
    state.row.data = reinterpret_cast<uint8_t *>(state.strip + (sy - state.source_y) * state.width);
    dsc->decoded = &state.row;
    *decoded = {0, y, state.width - 1, y};
    return LV_RESULT_OK;
}
void close(lv_image_decoder_t *, lv_image_decoder_dsc_t *dsc)
{
    delete static_cast<Decoder *>(dsc->user_data);
    dsc->user_data = nullptr; dsc->decoded = nullptr;
}
}
void photos_decoder_init()
{
    static bool ready = false;
    if (ready) return;
    auto *decoder = lv_image_decoder_create();
    if (!decoder) return;
    lv_image_decoder_set_info_cb(decoder, info);
    lv_image_decoder_set_open_cb(decoder, open);
    lv_image_decoder_set_get_area_cb(decoder, area);
    lv_image_decoder_set_close_cb(decoder, close);
    ready = true;
}
