/*
 * callback audio output driver
 *
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

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/bswap.h>

#include "mpv_talloc.h"

#include "audio/format.h"
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "osdep/endian.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "player/client.h"

#include "ao.h"
#include "internal.h"

struct cb_chunk {
    struct cb_chunk *next;
    int samples;
    size_t bytes;
    uint64_t sequence;
    uint8_t data[];
};

struct priv {
    mp_mutex lock;
    mp_cond wakeup;
    mp_thread thread;
    bool thread_valid;

    bool terminate;
    bool paused;
    bool playing;
    double last_time;
    double buffered;             // samples in the virtual device

    float bufferlen;             // seconds
    int outburst;                // samples
    int queue_limit_samples;
    int queued_samples;
    struct cb_chunk *first;
    struct cb_chunk *last;
    uint64_t next_sequence;
    uint64_t dropped_samples;
};

static MP_THREAD_VOID callback_thread(void *arg);

static void clear_queue_locked(struct priv *p)
{
    while (p->first) {
        struct cb_chunk *cur = p->first;
        p->first = cur->next;
        talloc_free(cur);
    }
    p->last = NULL;
    p->queued_samples = 0;
}

static void drop_oldest_locked(struct priv *p)
{
    struct cb_chunk *cur = p->first;
    if (!cur)
        return;

    p->first = cur->next;
    if (!p->first)
        p->last = NULL;
    p->queued_samples -= cur->samples;
    p->dropped_samples += cur->samples;
    talloc_free(cur);
}

static void enqueue_chunk_locked(struct priv *p, struct cb_chunk *chunk)
{
    chunk->sequence = p->next_sequence++;

    if (p->last) {
        p->last->next = chunk;
    } else {
        p->first = chunk;
    }
    p->last = chunk;
    p->queued_samples += chunk->samples;

    while (p->queued_samples > p->queue_limit_samples && p->first != p->last)
        drop_oldest_locked(p);

    mp_cond_broadcast(&p->wakeup);
}

static void drain_locked(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->paused)
        return;

    double now = mp_time_sec();
    if (p->buffered > 0) {
        p->buffered -= (now - p->last_time) * ao->samplerate;
        if (p->buffered < 0)
            p->buffered = 0;
    }
    p->last_time = now;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (!mp_client_audio_output_cb_registered(ao->global->client_api)) {
        MP_ERR(ao, "No libmpv audio output callback registered.\n");
        return -1;
    }

    ao->format = AF_FORMAT_S16;
    ao->channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;

    p->last_time = mp_time_sec();
    p->queue_limit_samples = MPMAX(1, (int)(ao->samplerate * p->bufferlen + 0.5));
    ao->device_buffer = MPMAX(p->queue_limit_samples, p->outburst);

    mp_mutex_init(&p->lock);
    mp_cond_init(&p->wakeup);

    p->thread_valid = true;
    if (mp_thread_create(&p->thread, callback_thread, ao)) {
        p->thread_valid = false;
        mp_cond_destroy(&p->wakeup);
        mp_mutex_destroy(&p->lock);
        MP_ERR(ao, "Failed to create callback thread.\n");
        return -1;
    }

    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->thread_valid) {
        mp_mutex_lock(&p->lock);
        p->terminate = true;
        mp_cond_broadcast(&p->wakeup);
        mp_mutex_unlock(&p->lock);

        mp_thread_join(p->thread);
        p->thread_valid = false;
    }

    mp_mutex_lock(&p->lock);
    clear_queue_locked(p);
    mp_mutex_unlock(&p->lock);

    mp_cond_destroy(&p->wakeup);
    mp_mutex_destroy(&p->lock);
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    mp_mutex_lock(&p->lock);
    p->paused = false;
    p->buffered = 0;
    p->playing = false;
    clear_queue_locked(p);
    mp_cond_broadcast(&p->wakeup);
    mp_mutex_unlock(&p->lock);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    mp_mutex_lock(&p->lock);
    if (p->paused != paused) {
        drain_locked(ao);
        p->paused = paused;
        if (!p->paused)
            p->last_time = mp_time_sec();
        mp_cond_broadcast(&p->wakeup);
    }
    mp_mutex_unlock(&p->lock);

    return true;
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    mp_mutex_lock(&p->lock);
    drain_locked(ao);
    p->paused = false;
    p->last_time = mp_time_sec();
    p->playing = true;
    mp_cond_broadcast(&p->wakeup);
    mp_mutex_unlock(&p->lock);
}

static bool audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *p = ao->priv;
    struct cb_chunk *first = NULL;
    struct cb_chunk *last = NULL;

    for (int pos = 0; pos < samples; ) {
        int chunk_samples = MPMIN(p->outburst, samples - pos);
        uint64_t bytes = (uint64_t)chunk_samples * ao->sstride;

        if (bytes > SIZE_MAX - sizeof(struct cb_chunk))
            goto error;

        struct cb_chunk *chunk =
            talloc_size(NULL, sizeof(*chunk) + (size_t)bytes);
        if (!chunk)
            goto error;

        *chunk = (struct cb_chunk){
            .samples = chunk_samples,
            .bytes = bytes,
        };
        memcpy(chunk->data, (uint8_t *)data[0] + pos * ao->sstride,
               (size_t)bytes);

#if BYTE_ORDER == BIG_ENDIAN
        uint16_t *s = (uint16_t *)chunk->data;
        for (int n = 0; n < chunk_samples * ao->channels.num; n++)
            s[n] = av_bswap16(s[n]);
#endif

        if (last) {
            last->next = chunk;
        } else {
            first = chunk;
        }
        last = chunk;
        pos += chunk_samples;
    }

    mp_mutex_lock(&p->lock);
    drain_locked(ao);
    p->buffered += samples;
    while (first) {
        struct cb_chunk *cur = first;
        first = cur->next;
        cur->next = NULL;
        enqueue_chunk_locked(p, cur);
    }
    mp_mutex_unlock(&p->lock);

    return true;

error:
    while (first) {
        struct cb_chunk *cur = first;
        first = cur->next;
        talloc_free(cur);
    }
    return false;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;

    mp_mutex_lock(&p->lock);
    drain_locked(ao);

    int buffered = MPCLAMP((int)p->buffered, 0, ao->device_buffer);
    int free_samples = ao->device_buffer - buffered;
    free_samples = free_samples / p->outburst * p->outburst;

    *state = (struct mp_pcm_state){
        .free_samples = free_samples,
        .queued_samples = buffered,
        .delay = p->buffered / ao->samplerate,
        .playing = p->playing && p->buffered > 0,
    };

    mp_mutex_unlock(&p->lock);
}

static MP_THREAD_VOID callback_thread(void *arg)
{
    struct ao *ao = arg;
    struct priv *p = ao->priv;

    mp_thread_set_name("ao/callback");

    while (1) {
        mp_mutex_lock(&p->lock);
        while (!p->terminate && (p->paused || !p->first))
            mp_cond_wait(&p->wakeup, &p->lock);

        if (p->terminate) {
            mp_mutex_unlock(&p->lock);
            break;
        }

        struct cb_chunk *chunk = p->first;
        p->first = chunk->next;
        if (!p->first)
            p->last = NULL;
        p->queued_samples -= chunk->samples;
        uint64_t dropped_samples = p->dropped_samples;
        mp_mutex_unlock(&p->lock);

        mpv_audio_output_cb_info info = {
            .format = MPV_AUDIO_OUTPUT_CB_FORMAT_S16LE,
            .sample_rate = ao->samplerate,
            .channels = ao->channels.num,
            .samples = chunk->samples,
            .bytes = chunk->bytes,
            .sequence = chunk->sequence,
            .dropped_samples = dropped_samples,
        };
        mp_client_audio_output_cb_call(ao->global->client_api, chunk->data, &info);
        talloc_free(chunk);
    }

    MP_THREAD_RETURN();
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_callback = {
    .description = "libmpv audio output callback",
    .name      = "callback",
    .init      = init,
    .uninit    = uninit,
    .reset     = reset,
    .get_state = get_state,
    .set_pause = set_pause,
    .write     = audio_write,
    .start     = start,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .bufferlen = 0.02,
        .outburst = 256,
    },
    .options = (const struct m_option[]) {
        {"buffer", OPT_FLOAT(bufferlen), M_RANGE(0, 100)},
        {"outburst", OPT_INT(outburst), M_RANGE(1, 100000)},
        {0}
    },
    .options_prefix = "ao-callback",
};
