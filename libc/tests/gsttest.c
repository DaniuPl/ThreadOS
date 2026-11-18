/* gsttest: GStreamer on ThreadOS, held to FFmpeg on the host
 *
 * What WebKit will play a page's media with, run on its own: the
 * plugins built in (apps/webkit/media/gststatic.c), FFmpeg's decoders
 * behind gst-libav, the containers' demuxers, and the sound going out
 * through threadosaudiosink. scripts/tests/t_gstreamer.py makes the
 * files on the host with FFmpeg, serves them, and holds what is printed
 * here to what FFmpeg on the host makes of the same files.
 *
 *   elements          the elements WebKit asks for, each found or not
 *   frames FILE       every video frame decoded, as I420: "frame N md5"
 *                     over its visible pixels, plane by plane, row by
 *                     row - FFmpeg's framemd5 of the same frames
 *   pcm FILE          the sound decoded to 16-bit stereo: "pcm FRAMES
 *                     md5" over all of it
 *   play FILE         the sound played through autoaudiosink to its end
 */

#include <stdio.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

static GstElement *launch(const char *fmt, const char *file)
{
    char *desc = g_strdup_printf(fmt, file);
    GError *error = NULL;
    GstElement *p = gst_parse_launch(desc, &error);
    g_free(desc);
    if (!p) {
        printf("gsttest: %s\n", error ? error->message : "no pipeline");
        g_clear_error(&error);
    }
    return p;
}

/* The bus's error, if one came, said */
static int failed(GstElement *pipeline)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!m)
        return 0;
    GError *error = NULL;
    gst_message_parse_error(m, &error, NULL);
    printf("gsttest: error: %s\n", error ? error->message : "?");
    g_clear_error(&error);
    gst_message_unref(m);
    return 1;
}

static int frames(const char *file)
{
    GstElement *p = launch("filesrc location=%s ! decodebin ! videoconvert ! "
                           "video/x-raw,format=I420 ! appsink name=out "
                           "sync=false", file);
    if (!p)
        return 1;
    GstAppSink *out = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(p), "out"));
    gst_element_set_state(p, GST_STATE_PLAYING);
    int n = 0;
    GstSample *s;
    while ((s = gst_app_sink_pull_sample(out))) {
        GstVideoInfo info;
        GstVideoFrame frame;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(s)) &&
            gst_video_frame_map(&frame, &info, gst_sample_get_buffer(s),
                                GST_MAP_READ)) {
            GChecksum *md5 = g_checksum_new(G_CHECKSUM_MD5);
            for (guint plane = 0; plane < GST_VIDEO_FRAME_N_PLANES(&frame);
                 plane++) {
                const guint8 *row = GST_VIDEO_FRAME_PLANE_DATA(&frame, plane);
                int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
                int w = GST_VIDEO_FRAME_COMP_WIDTH(&frame, plane);
                int h = GST_VIDEO_FRAME_COMP_HEIGHT(&frame, plane);
                for (int y = 0; y < h; y++)
                    g_checksum_update(md5, row + (gsize)y * stride, w);
            }
            printf("frame %d %s\n", n++, g_checksum_get_string(md5));
            g_checksum_free(md5);
            gst_video_frame_unmap(&frame);
        }
        gst_sample_unref(s);
    }
    int bad = failed(p);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(out);
    gst_object_unref(p);
    printf("frames %d\n", n);
    return bad || !n;
}

static int pcm(const char *file)
{
    GstElement *p = launch("filesrc location=%s ! decodebin ! audioconvert ! "
                           "audio/x-raw,format=S16LE,channels=2,"
                           "layout=interleaved ! appsink name=out sync=false",
                           file);
    if (!p)
        return 1;
    GstAppSink *out = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(p), "out"));
    gst_element_set_state(p, GST_STATE_PLAYING);
    GChecksum *md5 = g_checksum_new(G_CHECKSUM_MD5);
    guint64 bytes = 0;
    GstSample *s;
    while ((s = gst_app_sink_pull_sample(out))) {
        GstMapInfo map;
        GstBuffer *b = gst_sample_get_buffer(s);
        if (gst_buffer_map(b, &map, GST_MAP_READ)) {
            g_checksum_update(md5, map.data, map.size);
            bytes += map.size;
            gst_buffer_unmap(b, &map);
        }
        gst_sample_unref(s);
    }
    int bad = failed(p);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(out);
    gst_object_unref(p);
    printf("pcm %llu %s\n", (unsigned long long)(bytes / 4),
           g_checksum_get_string(md5));
    g_checksum_free(md5);
    return bad || !bytes;
}

static int play(const char *file)
{
    GstElement *p = launch("filesrc location=%s ! decodebin ! audioconvert ! "
                           "audioresample ! autoaudiosink", file);
    if (!p)
        return 1;
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    int ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m && !ok) {
        GError *error = NULL;
        gst_message_parse_error(m, &error, NULL);
        printf("gsttest: error: %s\n", error ? error->message : "?");
        g_clear_error(&error);
    }
    if (m)
        gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    printf(ok ? "played\n" : "not played\n");
    return !ok;
}

static int elements(void)
{
    static const char *const names[] = {
        "playbin", "playbin3", "decodebin3", "uridecodebin3", "typefind",
        "appsrc", "appsink", "queue", "audioconvert", "audioresample",
        "videoconvert", "volume", "scaletempo", "autoaudiosink",
        "threadosaudiosink", "qtdemux", "matroskademux", "h264parse",
        "vp9parse", "aacparse", "avdec_h264", "avdec_vp9", "avdec_aac",
        "avdec_opus",
    };
    int missing = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        GstElementFactory *f = gst_element_factory_find(names[i]);
        printf("element %s %s\n", names[i], f ? "found" : "missing");
        missing += !f;
        if (f)
            gst_object_unref(f);
    }
    return missing != 0;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    int r = 2;
    if (argc == 2 && !strcmp(argv[1], "elements"))
        r = elements();
    else if (argc == 3 && !strcmp(argv[1], "frames"))
        r = frames(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "pcm"))
        r = pcm(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "play"))
        r = play(argv[2]);
    else
        printf("gsttest: elements | frames FILE | pcm FILE | play FILE\n");
    printf("gsttest: done\n");
    return r;
}
