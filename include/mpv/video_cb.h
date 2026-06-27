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

#ifndef MPV_CLIENT_API_VIDEO_CB_H_
#define MPV_CLIENT_API_VIDEO_CB_H_

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mpv_video_output_cb_format {
    MPV_VIDEO_OUTPUT_CB_FORMAT_RGBA8 = 1,
} mpv_video_output_cb_format;

typedef enum mpv_video_output_cb_flags {
    MPV_VIDEO_OUTPUT_CB_FLAG_REPEAT = 1 << 0,
    MPV_VIDEO_OUTPUT_CB_FLAG_REDRAW = 1 << 1,
    MPV_VIDEO_OUTPUT_CB_FLAG_STILL  = 1 << 2,
} mpv_video_output_cb_flags;

typedef struct mpv_video_output_cb_info {
    int format;              // MPV_VIDEO_OUTPUT_CB_FORMAT_RGBA8
    int width;
    int height;
    int stride;              // bytes per row
    uint64_t bytes;          // stride * height
    uint64_t sequence;       // monotonically increasing callback frame id
    uint64_t frame_id;       // mpv core video frame id, stable across redraws
    uint64_t dropped_frames; // cumulative frames dropped by this callback VO
    int flags;               // mpv_video_output_cb_flags
    double pts;              // video pts in seconds
    double duration;         // approximate frame duration in seconds, or -1
} mpv_video_output_cb_info;

typedef void (*mpv_video_output_cb_fn)(
    void *userdata,
    const void *data,
    const mpv_video_output_cb_info *info);

/**
 * Set the callback used by the "callback" video output.
 *
 * The callback is per mpv core. If multiple mpv_handles refer to the same mpv
 * instance, the last setter wins. Passing NULL as cb unregisters the callback.
 *
 * The "callback" video output provides composited RGBA8 video frames including
 * subtitles and OSD. Callback data is valid only until the callback returns.
 *
 * Replacing or unregistering the callback waits for any active callback to
 * finish, so the previous userdata may be freed after this function returns.
 * The callback must not call this function or destroy/terminate the same mpv
 * instance.
 */
MPV_EXPORT void mpv_set_video_output_callback(
    mpv_handle *ctx,
    mpv_video_output_cb_fn cb,
    void *userdata);

#ifdef MPV_CPLUGIN_DYNAMIC_SYM

MPV_DEFINE_SYM_PTR(mpv_set_video_output_callback)
#define mpv_set_video_output_callback pfn_mpv_set_video_output_callback

#endif

#ifdef __cplusplus
}
#endif

#endif
