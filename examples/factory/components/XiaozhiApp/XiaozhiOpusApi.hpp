/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

// Public libopus C API exported by esp_audio_codec. Using init() with caller
// storage lets the mutually exclusive encoder and decoder share one allocation.
struct OpusEncoder;
struct OpusDecoder;
extern "C" {
int opus_encoder_get_size(int channels);
int opus_encoder_init(OpusEncoder *state, int32_t sample_rate, int channels, int application);
int opus_encoder_ctl(OpusEncoder *state, int request, ...);
int opus_encode(OpusEncoder *state, const int16_t *pcm, int frame_size,
                unsigned char *data, int32_t max_data_bytes);
int opus_decoder_get_size(int channels);
int opus_decoder_init(OpusDecoder *state, int32_t sample_rate, int channels);
int opus_decoder_ctl(OpusDecoder *state, int request, ...);
int opus_decode(OpusDecoder *state, const unsigned char *data, int32_t length,
                int16_t *pcm, int frame_size, int decode_fec);
int opus_packet_get_nb_samples(const unsigned char *packet, int32_t length, int32_t sample_rate);
}

namespace esp_brookesia::apps::opus {
constexpr int OK = 0;
// Restricted low delay selects CELT only. AUDIO/VOIP can enter SILK pitch
// analysis, whose variable stack peak exceeds the C6 audio worker's budget.
constexpr int APPLICATION_RESTRICTED_LOWDELAY = 2051;
constexpr int BITRATE_AUTO = -1000;
constexpr int SET_BITRATE = 4002;
constexpr int SET_VBR = 4006;
constexpr int SET_COMPLEXITY = 4010;
constexpr int SET_INBAND_FEC = 4012;
constexpr int SET_DTX = 4016;
constexpr int RESET_STATE = 4028;
} // namespace esp_brookesia::apps::opus
