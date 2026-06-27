/*
 * callback video output driver
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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "player/client.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"

#include "vo.h"

struct cb_frame {
    struct cb_frame *next;
    mpv_video_output_cb_info info;
    uint8_t data[];
};

struct priv {
    mp_mutex lock;
    mp_cond wakeup;
    mp_thread thread;
    bool thread_valid;
    bool terminate;

    int buffer_frames;
    int queue_limit_frames;
    int queued_frames;
    struct cb_frame *first;
    struct cb_frame *last;
    uint64_t next_sequence;
    uint64_t dropped_frames;

    struct mp_sws_context *sws;
    struct mp_image *frame;
    struct mp_image_params dst_params;
    mpv_video_output_cb_info frame_info;
    bool frame_valid;
};

static MP_THREAD_VOID callback_thread(void *arg);

static void clear_queue_locked(struct priv *p)
{
    while (p->first) {
        struct cb_frame *cur = p->first;
        p->first = cur->next;
        talloc_free(cur);
    }
    p->last = NULL;
    p->queued_frames = 0;
}

static void drop_oldest_locked(struct priv *p)
{
    struct cb_frame *cur = p->first;
    if (!cur)
        return;

    p->first = cur->next;
    if (!p->first)
        p->last = NULL;
    p->queued_frames -= 1;
    p->dropped_frames += 1;
    talloc_free(cur);
}

static void enqueue_frame_locked(struct priv *p, struct cb_frame *frame)
{
    frame->next = NULL;
    frame->info.sequence = p->next_sequence++;

    if (p->last) {
        p->last->next = frame;
    } else {
        p->first = frame;
    }
    p->last = frame;
    p->queued_frames += 1;

    while (p->queued_frames > p->queue_limit_frames && p->first != p->last)
        drop_oldest_locked(p);

    mp_cond_broadcast(&p->wakeup);
}

static void clear_rgba(struct mp_image *img, struct mp_rect rc)
{
    for (int y = rc.y0; y < rc.y1; y++) {
        uint8_t *ptr = img->planes[0] + (ptrdiff_t)y * img->stride[0] +
                       (ptrdiff_t)rc.x0 * 4;
        for (int x = rc.x0; x < rc.x1; x++) {
            ptr[0] = 0;
            ptr[1] = 0;
            ptr[2] = 0;
            ptr[3] = 0xff;
            ptr += 4;
        }
    }
}

static void set_alpha_opaque(struct mp_image *img)
{
    for (int y = 0; y < img->h; y++) {
        uint8_t *ptr = img->planes[0] + (ptrdiff_t)y * img->stride[0] + 3;
        for (int x = 0; x < img->w; x++) {
            *ptr = 0xff;
            ptr += 4;
        }
    }
}

static int info_flags(struct vo_frame *frame)
{
    int flags = 0;

    if (frame->repeat)
        flags |= MPV_VIDEO_OUTPUT_CB_FLAG_REPEAT;
    if (frame->redraw)
        flags |= MPV_VIDEO_OUTPUT_CB_FLAG_REDRAW;
    if (frame->still)
        flags |= MPV_VIDEO_OUTPUT_CB_FLAG_STILL;

    return flags;
}

static double frame_duration(struct vo_frame *frame)
{
    if (frame->approx_duration > 0)
        return frame->approx_duration;
    if (frame->current && frame->current->pkt_duration > 0)
        return frame->current->pkt_duration;
    if (frame->duration >= 0)
        return MP_TIME_NS_TO_S(frame->duration);
    return -1;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (!mp_client_video_output_cb_registered(vo->global->client_api)) {
        MP_ERR(vo, "No libmpv video output callback registered.\n");
        return -1;
    }

    p->sws = mp_sws_alloc(vo);
    p->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(p->sws, vo->global);

    p->queue_limit_frames = MPMAX(1, p->buffer_frames);

    mp_mutex_init(&p->lock);
    mp_cond_init(&p->wakeup);

    p->thread_valid = true;
    if (mp_thread_create(&p->thread, callback_thread, vo)) {
        p->thread_valid = false;
        mp_cond_destroy(&p->wakeup);
        mp_mutex_destroy(&p->lock);
        MP_ERR(vo, "Failed to create callback thread.\n");
        return -1;
    }

    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

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

    talloc_free(p->frame);
    p->frame = NULL;

    mp_cond_destroy(&p->wakeup);
    mp_mutex_destroy(&p->lock);
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;

    return mp_sws_supports_formats(p->sws, IMGFMT_RGBA, format);
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    if (!mp_sws_supports_formats(p->sws, IMGFMT_RGBA, params->imgfmt))
        return -1;

    talloc_free(p->frame);
    p->frame = mp_image_alloc(IMGFMT_RGBA, vo->dwidth, vo->dheight);
    if (!p->frame)
        return -1;

    p->dst_params = (struct mp_image_params) {
        .imgfmt = IMGFMT_RGBA,
        .w = vo->dwidth,
        .h = vo->dheight,
        .p_w = 1,
        .p_h = 1,
    };
    mp_image_params_guess_csp(&p->dst_params);
    mp_image_set_params(p->frame, &p->dst_params);

    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = &p->dst_params;
    mp_mutex_unlock(&vo->params_mutex);

    mp_mutex_lock(&p->lock);
    clear_queue_locked(p);
    mp_mutex_unlock(&p->lock);

    p->frame_valid = false;
    vo->want_redraw = true;
    return 0;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    p->frame_valid = false;

    if (!p->frame)
        return VO_FALSE;

    struct mp_rect src_rc;
    struct mp_rect dst_rc;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src_rc, &dst_rc, &osd);

    clear_rgba(p->frame, (struct mp_rect){0, 0, p->frame->w, p->frame->h});

    struct mp_image *img = frame->current;
    if (img) {
        struct mp_image src = *img;
        src_rc.x0 = MP_ALIGN_DOWN(src_rc.x0, src.fmt.align_x);
        src_rc.y0 = MP_ALIGN_DOWN(src_rc.y0, src.fmt.align_y);
        mp_image_crop_rc(&src, src_rc);

        struct mp_image dst = *p->frame;
        mp_image_crop_rc(&dst, dst_rc);

        if (mp_sws_scale(p->sws, &dst, &src) < 0) {
            clear_rgba(p->frame,
                       (struct mp_rect){0, 0, p->frame->w, p->frame->h});
            return VO_FALSE;
        }
    }

    set_alpha_opaque(p->frame);
    osd_draw_on_image(vo->osd, osd, img ? img->pts : 0, 0, p->frame);
    set_alpha_opaque(p->frame);

    uint64_t bytes = (uint64_t)p->frame->stride[0] * p->frame->h;
    p->frame_info = (mpv_video_output_cb_info) {
        .format = MPV_VIDEO_OUTPUT_CB_FORMAT_RGBA8,
        .width = p->frame->w,
        .height = p->frame->h,
        .stride = p->frame->stride[0],
        .bytes = bytes,
        .frame_id = img ? frame->frame_id : 0,
        .flags = info_flags(frame),
        .pts = img ? img->pts : 0,
        .duration = frame_duration(frame),
    };
    p->frame_valid = true;
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (!p->frame_valid)
        return;

    p->frame_valid = false;

    if (p->frame->stride[0] <= 0)
        return;

    uint64_t bytes = (uint64_t)p->frame->stride[0] * p->frame->h;
    if (bytes > (uint64_t)PTRDIFF_MAX - sizeof(struct cb_frame))
        return;

    size_t frame_bytes = (size_t)bytes;
    struct cb_frame *frame =
        talloc_size(NULL, sizeof(*frame) + frame_bytes);
    if (!frame)
        return;

    *frame = (struct cb_frame){
        .info = p->frame_info,
    };
    memcpy(frame->data, p->frame->planes[0], frame_bytes);

    mp_mutex_lock(&p->lock);
    enqueue_frame_locked(p, frame);
    mp_mutex_unlock(&p->lock);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    (void)data;

    switch (request) {
    case VOCTRL_RESET:
        p->frame_valid = false;
        mp_mutex_lock(&p->lock);
        clear_queue_locked(p);
        mp_mutex_unlock(&p->lock);
        return VO_TRUE;
    }

    return VO_NOTIMPL;
}

static MP_THREAD_VOID callback_thread(void *arg)
{
    struct vo *vo = arg;
    struct priv *p = vo->priv;

    mp_thread_set_name("vo/callback");

    while (1) {
        mp_mutex_lock(&p->lock);
        while (!p->terminate && !p->first)
            mp_cond_wait(&p->wakeup, &p->lock);

        if (p->terminate) {
            mp_mutex_unlock(&p->lock);
            break;
        }

        struct cb_frame *frame = p->first;
        p->first = frame->next;
        if (!p->first)
            p->last = NULL;
        p->queued_frames -= 1;
        uint64_t dropped_frames = p->dropped_frames;
        mp_mutex_unlock(&p->lock);

        mpv_video_output_cb_info info = frame->info;
        info.dropped_frames = dropped_frames;
        mp_client_video_output_cb_call(vo->global->client_api,
                                       frame->data, &info);
        talloc_free(frame);
    }

    MP_THREAD_RETURN();
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_callback = {
    .description = "libmpv video output callback",
    .name = "callback",
    .caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .buffer_frames = 2,
    },
    .options = (const struct m_option[]) {
        {"buffer", OPT_INT(buffer_frames), M_RANGE(1, 100000)},
        {0}
    },
    .options_prefix = "vo-callback",
};
