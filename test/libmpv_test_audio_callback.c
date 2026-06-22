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
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <mpv/audio_cb.h>
#include <mpv/stream_cb.h>

#include "libmpv_common.h"

#define NS_PER_S INT64_C(1000000000)

struct cb_state {
    atomic_uint_fast64_t callbacks;
    atomic_uint_fast64_t last_sequence;
    atomic_uint_fast64_t dropped_samples;
    atomic_bool bad_info;
    bool slow;
};

struct wav_source {
    uint8_t *data;
    size_t size;
};

struct wav_stream {
    const uint8_t *data;
    size_t size;
    size_t pos;
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

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = value & 0xff;
    dst[1] = value >> 8;
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = value & 0xff;
    dst[1] = (value >> 8) & 0xff;
    dst[2] = (value >> 16) & 0xff;
    dst[3] = value >> 24;
}

static void make_wav_source(struct wav_source *source, int milliseconds)
{
    const int sample_rate = 48000;
    const int channels = 2;
    const int bytes_per_sample = 2;
    const size_t header_size = 44;
    int samples = sample_rate * milliseconds / 1000;

    if (samples <= 0)
        fail("invalid audio source duration\n");

    uint64_t pcm_bytes = (uint64_t)samples * channels * bytes_per_sample;
    uint64_t total_size = header_size + pcm_bytes;
    if (pcm_bytes > UINT32_MAX || total_size > SIZE_MAX)
        fail("audio source is too large\n");

    uint8_t *data = malloc((size_t)total_size);
    if (!data)
        fail("failed to allocate audio source\n");

    memcpy(data + 0, "RIFF", 4);
    write_le32(data + 4, (uint32_t)(total_size - 8));
    memcpy(data + 8, "WAVE", 4);
    memcpy(data + 12, "fmt ", 4);
    write_le32(data + 16, 16);
    write_le16(data + 20, 1);
    write_le16(data + 22, channels);
    write_le32(data + 24, sample_rate);
    write_le32(data + 28, sample_rate * channels * bytes_per_sample);
    write_le16(data + 32, channels * bytes_per_sample);
    write_le16(data + 34, bytes_per_sample * 8);
    memcpy(data + 36, "data", 4);
    write_le32(data + 40, (uint32_t)pcm_bytes);

    uint8_t *out = data + header_size;
    for (int n = 0; n < samples; n++) {
        int16_t sample = (n % 96) < 48 ? 12000 : -12000;
        for (int ch = 0; ch < channels; ch++) {
            write_le16(out, (uint16_t)sample);
            out += bytes_per_sample;
        }
    }

    *source = (struct wav_source){
        .data = data,
        .size = (size_t)total_size,
    };
}

static void free_wav_source(struct wav_source *source)
{
    free(source->data);
    *source = (struct wav_source){0};
}

static int64_t wav_read(void *cookie, char *buf, uint64_t nbytes)
{
    struct wav_stream *stream = cookie;
    uint64_t remaining = stream->size - stream->pos;
    uint64_t read_size = nbytes < remaining ? nbytes : remaining;

    memcpy(buf, stream->data + stream->pos, (size_t)read_size);
    stream->pos += (size_t)read_size;
    return (int64_t)read_size;
}

static int64_t wav_seek(void *cookie, int64_t offset)
{
    struct wav_stream *stream = cookie;

    if (offset < 0 || (uint64_t)offset > stream->size)
        return MPV_ERROR_GENERIC;

    stream->pos = (size_t)offset;
    return offset;
}

static int64_t wav_size(void *cookie)
{
    struct wav_stream *stream = cookie;
    return (int64_t)stream->size;
}

static void wav_close(void *cookie)
{
    free(cookie);
}

static int open_wav(void *user_data, char *uri, mpv_stream_cb_info *info)
{
    struct wav_source *source = user_data;

    (void)uri;

    if (!source || !source->data)
        return MPV_ERROR_LOADING_FAILED;

    struct wav_stream *stream = malloc(sizeof(*stream));
    if (!stream)
        return MPV_ERROR_LOADING_FAILED;

    *stream = (struct wav_stream){
        .data = source->data,
        .size = source->size,
    };
    info->cookie = stream;
    info->read_fn = wav_read;
    info->seek_fn = wav_seek;
    info->size_fn = wav_size;
    info->close_fn = wav_close;
    return 0;
}

static void register_audio_stream(struct wav_source *source)
{
    int ret = mpv_stream_cb_add_ro(ctx, "audcb", source, open_wav);
    if (ret < 0)
        fail("mpv API error while registering audio stream: %s\n",
             mpv_error_string(ret));
}

static void audio_cb(void *userdata, const void *data,
                     const mpv_audio_output_cb_info *info)
{
    struct cb_state *state = userdata;
    bool bad = !data || !info ||
               info->format != MPV_AUDIO_OUTPUT_CB_FORMAT_S16LE ||
               info->sample_rate <= 0 ||
               info->channels != 2 ||
               info->samples <= 0 ||
               info->bytes != (uint64_t)info->samples * info->channels * 2;

    uint64_t count = atomic_load(&state->callbacks);
    uint64_t last_sequence = atomic_load(&state->last_sequence);
    if (count && info->sequence <= last_sequence)
        bad = true;

    atomic_store(&state->last_sequence, info->sequence);
    atomic_store(&state->dropped_samples, info->dropped_samples);
    atomic_fetch_add(&state->callbacks, 1);

    if (bad)
        atomic_store(&state->bad_info, true);
    if (state->slow)
        sleep_ms(60);
}

static void create_context(void)
{
    ctx = mpv_create();
    if (!ctx)
        fail("failed to create mpv context\n");

    set_option_string_checked("vo", "null");
    set_option_string_checked("ao", "callback");
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

static void load_wav(void)
{
    const char *cmd[] = {"loadfile", "audcb://audio.wav", NULL};
    command(cmd);
}

static void test_audio_callback(void)
{
    struct cb_state state = {0};
    struct wav_source source = {0};

    create_context();
    make_wav_source(&source, 1000);
    register_audio_stream(&source);
    mpv_set_audio_output_callback(ctx, audio_cb, &state);
    initialize_context();

    load_wav();
    mpv_event_end_file end = {0};
    bool got_end = false;
    if (!wait_callbacks(&state, 1, &end, &got_end))
        fail("audio callback was not called\n");

    if (!got_end)
        end = wait_end_file();
    if (end.reason != MPV_END_FILE_REASON_EOF)
        fail("expected EOF, got end-file reason %d error %d\n", end.reason, end.error);

    mpv_set_audio_output_callback(ctx, NULL, NULL);
    if (atomic_load(&state.bad_info))
        fail("audio callback received invalid info\n");

    destroy_context();
    free_wav_source(&source);
}

static void test_missing_registration(void)
{
    struct wav_source source = {0};

    create_context();
    make_wav_source(&source, 100);
    register_audio_stream(&source);
    initialize_context();

    load_wav();
    mpv_event_end_file end = wait_end_file();
    if (end.reason != MPV_END_FILE_REASON_ERROR ||
        end.error != MPV_ERROR_AO_INIT_FAILED)
    {
        fail("expected AO init failure, got end-file reason %d error %d\n",
             end.reason, end.error);
    }

    destroy_context();
    free_wav_source(&source);
}

static void test_slow_callback_drops(void)
{
    struct cb_state state = {.slow = true};
    struct wav_source source = {0};

    create_context();
    make_wav_source(&source, 600);
    register_audio_stream(&source);
    set_option_string_checked("ao-callback-buffer", "0.02");
    mpv_set_audio_output_callback(ctx, audio_cb, &state);
    initialize_context();

    load_wav();
    mpv_event_end_file end = {0};
    bool got_end = false;
    if (!wait_callbacks(&state, 1, &end, &got_end))
        fail("slow audio callback was not called\n");

    if (!got_end)
        end = wait_end_file();
    if (end.reason != MPV_END_FILE_REASON_EOF)
        fail("expected EOF, got end-file reason %d error %d\n", end.reason, end.error);

    mpv_set_audio_output_callback(ctx, NULL, NULL);
    if (atomic_load(&state.bad_info))
        fail("slow audio callback received invalid info\n");
    if (!atomic_load(&state.dropped_samples))
        fail("slow audio callback did not report dropped samples\n");

    destroy_context();
    free_wav_source(&source);
}

int main(int argc, char *argv[])
{
    if (argc != 1)
        return 1;

    atexit(exit_cleanup);

    const char *fmt = "================ TEST: %s ================\n";
    printf(fmt, "test_audio_callback");
    test_audio_callback();
    printf(fmt, "test_missing_registration");
    test_missing_registration();
    printf(fmt, "test_slow_callback_drops");
    test_slow_callback_drops();
    printf("================ SHUTDOWN ================\n");

    return 0;
}
