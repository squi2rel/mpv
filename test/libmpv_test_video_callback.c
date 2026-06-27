/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <mpv/video_cb.h>

#include "libmpv_common.h"

#define NS_PER_S INT64_C(1000000000)

struct cb_state {
    atomic_uint_fast64_t callbacks;
    atomic_uint_fast64_t last_sequence;
    atomic_uint_fast64_t dropped_frames;
    atomic_bool bad_info;
    bool slow;
};

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#endif
}

static void set_option_string_checked(const char *name, const char *value)
{
    int ret = mpv_set_option_string(ctx, name, value);
    if (ret < 0)
        fail("mpv API error while setting option '%s' to '%s': %s\n",
             name, value, mpv_error_string(ret));
}

static void video_cb(void *userdata, const void *data,
                     const mpv_video_output_cb_info *info)
{
    struct cb_state *state = userdata;
    bool bad = !data || !info ||
               info->format != MPV_VIDEO_OUTPUT_CB_FORMAT_RGBA8 ||
               info->width <= 0 ||
               info->height <= 0 ||
               info->stride <= 0 ||
               (uint64_t)info->stride < (uint64_t)info->width * 4 ||
               info->bytes != (uint64_t)info->stride * info->height ||
               (info->flags & ~(MPV_VIDEO_OUTPUT_CB_FLAG_REPEAT |
                                MPV_VIDEO_OUTPUT_CB_FLAG_REDRAW |
                                MPV_VIDEO_OUTPUT_CB_FLAG_STILL));

    uint64_t count = atomic_load(&state->callbacks);
    uint64_t last_sequence = atomic_load(&state->last_sequence);
    if (count && info->sequence <= last_sequence)
        bad = true;

    atomic_store(&state->last_sequence, info->sequence);
    atomic_store(&state->dropped_frames, info->dropped_frames);
    atomic_fetch_add(&state->callbacks, 1);

    if (bad)
        atomic_store(&state->bad_info, true);
    if (state->slow)
        sleep_ms(80);
}

static void create_context(void)
{
    ctx = mpv_create();
    if (!ctx)
        fail("failed to create mpv context\n");

    set_option_string_checked("ao", "null");
    set_option_string_checked("vo", "callback");
}

static void destroy_context(void)
{
    if (ctx) {
        mpv_terminate_destroy(ctx);
        ctx = NULL;
    }
}

static void initialize_context(void)
{
    int ret = mpv_initialize(ctx);
    if (ret < 0)
        fail("mpv API error while initializing mpv: %s\n", mpv_error_string(ret));
}

static mpv_event_end_file wait_end_file(void)
{
    int64_t deadline = mpv_get_time_ns(ctx) + 10 * NS_PER_S;

    while (mpv_get_time_ns(ctx) < deadline) {
        mpv_event *event = mpv_wait_event(ctx, 0.1);
        if (event->event_id == MPV_EVENT_END_FILE)
            return *(mpv_event_end_file *)event->data;
    }

    fail("timed out waiting for end-file\n");
}

static bool wait_callbacks(struct cb_state *state, uint64_t count,
                           mpv_event_end_file *end, bool *got_end)
{
    int64_t deadline = mpv_get_time_ns(ctx) + 5 * NS_PER_S;
    *got_end = false;

    while (mpv_get_time_ns(ctx) < deadline) {
        if (atomic_load(&state->callbacks) >= count)
            return true;

        mpv_event *event = mpv_wait_event(ctx, 0.01);
        if (event->event_id == MPV_EVENT_END_FILE) {
            *end = *(mpv_event_end_file *)event->data;
            *got_end = true;
            return atomic_load(&state->callbacks) >= count;
        }
    }

    return atomic_load(&state->callbacks) >= count;
}

static bool wait_dropped_frames(struct cb_state *state, mpv_event_end_file *end,
                                bool *got_end)
{
    int64_t deadline = mpv_get_time_ns(ctx) + 10 * NS_PER_S;
    *got_end = false;

    while (mpv_get_time_ns(ctx) < deadline) {
        if (atomic_load(&state->dropped_frames))
            return true;

        mpv_event *event = mpv_wait_event(ctx, 0.01);
        if (event->event_id == MPV_EVENT_END_FILE) {
            *end = *(mpv_event_end_file *)event->data;
            *got_end = true;
            return atomic_load(&state->dropped_frames) != 0;
        }
    }

    return atomic_load(&state->dropped_frames) != 0;
}

static void load_video(const char *source)
{
    const char *cmd[] = {"loadfile", source, NULL};
    command(cmd);
}

static void test_video_callback(void)
{
    struct cb_state state = {0};

    create_context();
    mpv_set_video_output_callback(ctx, video_cb, &state);
    initialize_context();

    load_video("av://lavfi:testsrc=size=64x48:rate=20:d=0.3");
    mpv_event_end_file end = {0};
    bool got_end = false;
    if (!wait_callbacks(&state, 1, &end, &got_end))
        fail("video callback was not called\n");

    if (!got_end)
        end = wait_end_file();
    if (end.reason != MPV_END_FILE_REASON_EOF)
        fail("expected EOF, got end-file reason %d error %d\n", end.reason, end.error);

    mpv_set_video_output_callback(ctx, NULL, NULL);
    if (atomic_load(&state.bad_info))
        fail("video callback received invalid info\n");

    destroy_context();
}

static void test_missing_registration(void)
{
    create_context();
    initialize_context();

    load_video("av://lavfi:testsrc=size=64x48:rate=20:d=0.1");
    mpv_event_end_file end = wait_end_file();
    if (end.reason != MPV_END_FILE_REASON_ERROR ||
        end.error != MPV_ERROR_VO_INIT_FAILED)
    {
        fail("expected VO init failure, got end-file reason %d error %d\n",
             end.reason, end.error);
    }

    destroy_context();
}

static void test_slow_callback_drops(void)
{
    struct cb_state state = {.slow = true};

    create_context();
    set_option_string_checked("vo-callback-buffer", "1");
    mpv_set_video_output_callback(ctx, video_cb, &state);
    initialize_context();

    load_video("av://lavfi:testsrc=size=64x48:rate=120:d=1.0");
    mpv_event_end_file end = {0};
    bool got_end = false;
    if (!wait_callbacks(&state, 1, &end, &got_end))
        fail("slow video callback was not called\n");
    if (!got_end && !wait_dropped_frames(&state, &end, &got_end))
        fail("slow video callback did not report dropped frames\n");

    if (!got_end)
        end = wait_end_file();
    if (end.reason != MPV_END_FILE_REASON_EOF)
        fail("expected EOF, got end-file reason %d error %d\n", end.reason, end.error);

    mpv_set_video_output_callback(ctx, NULL, NULL);
    if (atomic_load(&state.bad_info))
        fail("slow video callback received invalid info\n");

    destroy_context();
}

int main(int argc, char *argv[])
{
    (void)argv;

    if (argc != 1)
        return 1;

    atexit(exit_cleanup);

    const char *fmt = "================ TEST: %s ================\n";
    printf(fmt, "test_video_callback");
    test_video_callback();
    printf(fmt, "test_missing_registration");
    test_missing_registration();
    printf(fmt, "test_slow_callback_drops");
    test_slow_callback_drops();
    printf("================ SHUTDOWN ================\n");

    return 0;
}
