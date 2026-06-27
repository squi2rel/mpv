/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_AUDIO_CB_H_
#define MPV_CLIENT_API_AUDIO_CB_H_

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mpv_audio_output_cb_format {
    MPV_AUDIO_OUTPUT_CB_FORMAT_S16LE = 1,
} mpv_audio_output_cb_format;

#define MPV_AUDIO_OUTPUT_CB_MAX_CHANNELS 64

typedef enum mpv_audio_output_cb_speaker {
    MPV_AUDIO_OUTPUT_CB_SPEAKER_FL = 0,    // FRONT_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_FR,        // FRONT_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_FC,        // FRONT_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_LFE,       // LOW_FREQUENCY
    MPV_AUDIO_OUTPUT_CB_SPEAKER_BL,        // BACK_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_BR,        // BACK_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_FLC,       // FRONT_LEFT_OF_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_FRC,       // FRONT_RIGHT_OF_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_BC,        // BACK_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_SL,        // SIDE_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_SR,        // SIDE_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TC,        // TOP_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TFL,       // TOP_FRONT_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TFC,       // TOP_FRONT_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TFR,       // TOP_FRONT_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TBL,       // TOP_BACK_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TBC,       // TOP_BACK_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TBR,       // TOP_BACK_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_DL = 29,   // STEREO_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_DR,        // STEREO_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_WL,        // WIDE_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_WR,        // WIDE_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_SDL,       // SURROUND_DIRECT_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_SDR,       // SURROUND_DIRECT_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_LFE2,      // LOW_FREQUENCY_2
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TSL,       // TOP_SIDE_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_TSR,       // TOP_SIDE_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_BFC,       // BOTTOM_FRONT_CENTER
    MPV_AUDIO_OUTPUT_CB_SPEAKER_BFL,       // BOTTOM_FRONT_LEFT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_BFR,       // BOTTOM_FRONT_RIGHT
    MPV_AUDIO_OUTPUT_CB_SPEAKER_NA = 64,   // silent/unassigned channel
} mpv_audio_output_cb_speaker;

typedef struct mpv_audio_output_cb_info {
    int format;              // MPV_AUDIO_OUTPUT_CB_FORMAT_S16LE
    int sample_rate;
    int channels;            // audio channels in each interleaved frame
    int samples;             // audio frames in this callback
    uint64_t bytes;          // samples * channels * 2
    uint64_t sequence;       // monotonically increasing chunk id
    uint64_t dropped_samples; // cumulative dropped audio frames
    uint64_t channel_mask;   // WAVEFORMATEXTENSIBLE channel mask
    int channel_layout[MPV_AUDIO_OUTPUT_CB_MAX_CHANNELS]; // speaker ids
} mpv_audio_output_cb_info;

typedef void (*mpv_audio_output_cb_fn)(
    void *userdata,
    const void *data,
    const mpv_audio_output_cb_info *info);

/**
 * Set the callback used by the "callback" audio output.
 *
 * The callback is per mpv core. If multiple mpv_handles refer to the same mpv
 * instance, the last setter wins. Passing NULL as cb unregisters the callback.
 *
 * The "callback" audio output provides raw interleaved signed 16-bit little
 * endian PCM. Callback data is valid only until the callback returns. Dolby
 * and other compressed bitstreams are decoded by mpv before reaching this
 * callback; compressed passthrough frames are not returned.
 *
 * Replacing or unregistering the callback waits for any active callback to
 * finish, so the previous userdata may be freed after this function returns.
 * The callback must not call this function or destroy/terminate the same mpv
 * instance.
 */
MPV_EXPORT void mpv_set_audio_output_callback(
    mpv_handle *ctx,
    mpv_audio_output_cb_fn cb,
    void *userdata);

#ifdef MPV_CPLUGIN_DYNAMIC_SYM

MPV_DEFINE_SYM_PTR(mpv_set_audio_output_callback)
#define mpv_set_audio_output_callback pfn_mpv_set_audio_output_callback

#endif

#ifdef __cplusplus
}
#endif

#endif
