/* gmedia - a small desktop player for files decoded by FFmpeg */

#include "uapi.h"
#include "udraw.h"

#include <errno.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define WIN_W 640
#define WIN_H 360
#define BAR_H 28
#define VIEW_H (WIN_H - BAR_H)
#define IO_SIZE 4096
#define AUDIO_OUT_FRAMES 8192

struct input {
    int fd;
    uint32_t size, pos;
};

static int16_t audio_out[AUDIO_OUT_FRAMES * 2];
static uint32_t video_row[WIN_W];
static int winid = -1;
static int audio_stream;
static int quitting;
static int64_t video_start_pts = AV_NOPTS_VALUE;
static uint64_t video_start_ticks;

static int read_input(void *opaque, uint8_t *buf, int size)
{
    struct input *in = opaque;
    int n = (int)sys_read(in->fd, buf, (uint32_t)size);
    if (n <= 0)
        return AVERROR_EOF;
    in->pos += (uint32_t)n;
    return n;
}

static int64_t seek_input(void *opaque, int64_t offset, int whence)
{
    struct input *in = opaque;
    if (whence & AVSEEK_SIZE)
        return in->size;
    whence &= ~AVSEEK_FORCE;
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = in->pos;
    else if (whence == SEEK_END)
        base = in->size;
    else
        return AVERROR(EINVAL);
    int64_t next = base + offset;
    if (next < 0 || next > in->size || next > 0xFFFFFFFFLL)
        return AVERROR(EINVAL);
    if (sys_seek(in->fd, (uint32_t)next) < 0)
        return AVERROR(EIO);
    in->pos = (uint32_t)next;
    return next;
}

static int events(void)
{
    struct win_event ev;
    while (win_poll(winid, &ev) == 1) {
        if (ev.type == WEV_CLOSE ||
            (ev.type == WEV_KEY && (ev.val == 'q' || ev.val == 'Q')))
            quitting = 1;
    }
    return quitting;
}

static void status(const char *s)
{
    struct ud_look look;
    ud_look(&look);
    ud_fill(winid, 0, VIEW_H, WIN_W, BAR_H, look.row);
    ud_text(winid, 12, VIEW_H + 19, s, look.text, UD_SANS, UD_PX);
}

static int audio_frame(struct SwrContext *swr, AVFrame *frame)
{
    if (!audio_stream)
        return 0;
    uint8_t *dst[1] = { (uint8_t *)audio_out };
    int n = swr_convert(swr, dst, AUDIO_OUT_FRAMES,
                        (const uint8_t **)frame->extended_data,
                        frame->nb_samples);
    if (n < 0)
        return -1;
    for (int at = 0; at < n && !events();) {
        int count = n - at;
        if (count > SND_STREAM_MAX_FRAMES)
            count = SND_STREAM_MAX_FRAMES;
        if (sys_snd_stream(SND_STREAM_WRITE, audio_out + at * 2,
                           (uint32_t)count) < 0)
            return -1;
        at += count;
    }
    return 0;
}

static int video_frame(struct SwsContext **scale, AVFrame *frame,
                       int *scaled_w, int *scaled_h)
{
    if (!*scale || frame->width <= 0 || frame->height <= 0)
        return -1;
    uint8_t *dst[4] = { (uint8_t *)video_row, 0, 0, 0 };
    int stride[4] = { *scaled_w * 4, 0, 0, 0 };
    int got = sws_scale(*scale, (const uint8_t *const *)frame->data,
                        frame->linesize, 0, frame->height, dst, stride);
    if (got != *scaled_h)
        return -1;

    int x0 = (WIN_W - *scaled_w) / 2;
    int y0 = (VIEW_H - *scaled_h) / 2;
    for (int y = 0; y < *scaled_h; y++) {
        if (win_blit32(winid, x0, y0 + y, *scaled_w, 1, video_row) < 0)
            return -1;
        if (!(y & 15) && events())
            return 1;
    }
    return events() ? 1 : 0;
}

static void pace_video(AVFrame *frame, AVRational time_base)
{
    if (frame->best_effort_timestamp == AV_NOPTS_VALUE ||
        time_base.num <= 0 || time_base.den <= 0)
        return;
    if (video_start_pts == AV_NOPTS_VALUE) {
        video_start_pts = frame->best_effort_timestamp;
        video_start_ticks = sys_ticks();
        return;
    }
    int64_t delta = frame->best_effort_timestamp - video_start_pts;
    if (delta <= 0)
        return;
    int64_t target_ms = delta * time_base.num * 1000 / time_base.den;
    if (target_ms > 3600000)
        return;
    uint64_t target_ticks = (uint64_t)target_ms / 10;
    while (!quitting) {
        uint64_t elapsed = sys_ticks() - video_start_ticks;
        if (elapsed >= target_ticks)
            break;
        uint64_t left = target_ticks - elapsed;
        sys_sleep(left > 5 ? 50 : (uint32_t)(left * 10));
        events();
    }
}

static int make_video_scale(struct SwsContext **scale, AVFrame *frame,
                            int *scaled_w, int *scaled_h)
{
    int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return -1;
    if (w > WIN_W - 16) {
        h = (int)((int64_t)h * (WIN_W - 16) / w);
        w = WIN_W - 16;
    }
    if (h > VIEW_H - 16) {
        w = (int)((int64_t)w * (VIEW_H - 16) / h);
        h = VIEW_H - 16;
    }
    if (w < 1 || h < 1)
        return -1;
    struct SwsContext *next = sws_getContext(frame->width, frame->height,
        (enum AVPixelFormat)frame->format, w, h, AV_PIX_FMT_BGR0,
        SWS_BILINEAR, 0, 0, 0);
    if (!next)
        return -1;
    if (*scale)
        sws_freeContext(*scale);
    *scale = next;
    *scaled_w = w;
    *scaled_h = h;
    return 0;
}

static int show_video_frame(AVFrame *frame, AVRational time_base,
                            struct SwsContext **scale,
                            int *scaled_w, int *scaled_h,
                            int *source_w, int *source_h, int *source_fmt)
{
    if (!*scale || frame->width != *source_w ||
        frame->height != *source_h || frame->format != *source_fmt) {
        if (make_video_scale(scale, frame, scaled_w, scaled_h) < 0)
            return -1;
        *source_w = frame->width;
        *source_h = frame->height;
        *source_fmt = frame->format;
    }
    pace_video(frame, time_base);
    if (quitting)
        return 1;
    return video_frame(scale, frame, scaled_w, scaled_h);
}

static int decode(int fd, uint32_t size)
{
    struct input in = { fd, size, 0 };
    uint8_t *io_buf = av_malloc(IO_SIZE);
    if (!io_buf)
        return -1;
    AVIOContext *io = avio_alloc_context(io_buf, IO_SIZE, 0, &in,
                                         read_input, 0, seek_input);
    if (!io) {
        av_free(io_buf);
        return -1;
    }
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        avio_context_free(&io);
        return -1;
    }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, 0, 0, 0) < 0 ||
        avformat_find_stream_info(fmt, 0) < 0) {
        avformat_close_input(&fmt);
        avio_context_free(&io);
        return -1;
    }

    int vi = -1, ai = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && vi < 0)
            vi = (int)i;
        else if (t == AVMEDIA_TYPE_AUDIO && ai < 0)
            ai = (int)i;
    }
    const AVCodec *vc = vi >= 0
        ? avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id) : 0;
    const AVCodec *ac = ai >= 0
        ? avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id) : 0;
    AVCodecContext *vctx = vc ? avcodec_alloc_context3(vc) : 0;
    AVCodecContext *actx = ac ? avcodec_alloc_context3(ac) : 0;
    if ((vi < 0 && ai < 0) ||
        (vi >= 0 && (!vctx || avcodec_parameters_to_context(
             vctx, fmt->streams[vi]->codecpar) < 0 || avcodec_open2(vctx, vc, 0) < 0)) ||
        (ai >= 0 && (!actx || avcodec_parameters_to_context(
             actx, fmt->streams[ai]->codecpar) < 0 || avcodec_open2(actx, ac, 0) < 0))) {
        avcodec_free_context(&vctx);
        avcodec_free_context(&actx);
        avformat_close_input(&fmt);
        avio_context_free(&io);
        return -1;
    }

    struct SwsContext *scale = 0;
    struct SwrContext *swr = 0;
    int scaled_w = 0, scaled_h = 0;
    int source_w = 0, source_h = 0, source_fmt = -1;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    if (!frame || !packet) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&vctx);
        avcodec_free_context(&actx);
        avformat_close_input(&fmt);
        avio_context_free(&io);
        return -1;
    }
    if (actx && sys_snd_stream(SND_STREAM_BEGIN, 0, 0) == 0) {
        audio_stream = 1;
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, 48000,
                                &actx->ch_layout, actx->sample_fmt,
                                actx->sample_rate, 0, 0) < 0 ||
            !swr || swr_init(swr) < 0)
        {
            sys_snd_stream(SND_STREAM_END, 0, 0);
            audio_stream = 0;
            swr_free(&swr);
        }
    }
    struct ud_look look;
    ud_look(&look);
    ud_fill(winid, 0, 0, WIN_W, VIEW_H, 0x000000);
    status(vi >= 0 ? "Playing video  - press Q to close" :
                     "Playing audio  - press Q to close");

    int rc = 0;
    while (!quitting && av_read_frame(fmt, packet) >= 0) {
        AVCodecContext *ctx = packet->stream_index == vi ? vctx :
                              packet->stream_index == ai ? actx : 0;
        if (ctx && avcodec_send_packet(ctx, packet) >= 0) {
            while (!quitting && avcodec_receive_frame(ctx, frame) >= 0) {
                if (ctx == actx) {
                    if (!swr || audio_frame(swr, frame) < 0) {
                        rc = audio_stream ? -1 : rc;
                        quitting = audio_stream;
                    }
                } else {
                    int shown = show_video_frame(
                        frame, fmt->streams[vi]->time_base, &scale,
                        &scaled_w, &scaled_h, &source_w, &source_h,
                        &source_fmt);
                    if (shown < 0) {
                        rc = -1;
                        quitting = 1;
                    }
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
        events();
    }
    if (!quitting) {
        AVCodecContext *flush[2] = { vctx, actx };
        for (int i = 0; i < 2 && !quitting; i++) {
            AVCodecContext *ctx = flush[i];
            if (!ctx || avcodec_send_packet(ctx, 0) < 0)
                continue;
            while (!quitting && avcodec_receive_frame(ctx, frame) >= 0) {
                if (ctx == actx) {
                    if (swr && audio_frame(swr, frame) < 0) {
                        rc = -1;
                        quitting = 1;
                    }
                } else {
                    int shown = show_video_frame(
                        frame, fmt->streams[vi]->time_base, &scale,
                        &scaled_w, &scaled_h, &source_w, &source_h,
                        &source_fmt);
                    if (shown < 0) {
                        rc = -1;
                        quitting = 1;
                    }
                }
                av_frame_unref(frame);
                events();
            }
        }
    }
    if (audio_stream) {
        if (swr) {
            uint8_t *dst[1] = { (uint8_t *)audio_out };
            int n = swr_convert(swr, dst, AUDIO_OUT_FRAMES, 0, 0);
            for (int at = 0; n > 0 && at < n; ) {
                int count = n - at;
                if (count > SND_STREAM_MAX_FRAMES)
                    count = SND_STREAM_MAX_FRAMES;
                if (sys_snd_stream(SND_STREAM_WRITE, audio_out + at * 2,
                                   (uint32_t)count) < 0)
                    break;
                at += count;
            }
        }
        sys_snd_stream(SND_STREAM_END, 0, 0);
        audio_stream = 0;
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    swr_free(&swr);
    sws_freeContext(scale);
    avcodec_free_context(&vctx);
    avcodec_free_context(&actx);
    avformat_close_input(&fmt);
    avio_context_free(&io);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !argv || !argv[1] || !argv[1][0])
        return 2;
    winid = (int)win_create(WIN_W, WIN_H, "Media player");
    if (winid < 0 || win_truecolour(winid) < 0)
        return 1;
    struct fdinfo64 info;
    int fd = (int)sys_open(argv[1], O_READ);
    if (fd < 0 || sys_fdinfo(fd, &info) < 0 || info.kind != FDK64_FILE) {
        status("Could not open this media file");
        sys_sleep(1500);
        return 1;
    }
    char title[96] = "Media: ";
    int n = 7;
    for (int i = 0; argv[1][i] && n < (int)sizeof(title) - 1; i++)
        title[n++] = argv[1][i];
    title[n] = 0;
    win_title(winid, title);
    if (decode(fd, info.size) < 0)
        status("This file or codec is not supported");
    else
        status("Playback finished");
    sys_close(fd);
    for (int i = 0; i < 40 && !events(); i++)
        sys_sleep(50);
    return 0;
}
