/* The containers a page's media comes in, as GStreamer opens them:
 * MP4 (qtdemux) and WebM/Matroska (matroskademux), the two that
 * YouTube's streams and a page's <video> are. The bytes are a file
 * the page names, so whoever serves it writes them.
 *
 * One input, through one demuxer (DEMUX, its caps CAPS - the Makefile's
 * fuzz build compiles this once for each): pushed by an appsrc, every
 * stream the demuxer finds taken by a fakesink, the pipeline run to its
 * end or its error. A crash or a sanitizer's report is the finding;
 * an error on the bus is the demuxer doing its job.
 */

#include <stdint.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(isomp4);
GST_PLUGIN_STATIC_DECLARE(matroska);

static void found_pad(GstElement *demux, GstPad *pad, gpointer pipeline)
{
    (void)demux;
    GstElement *sink = gst_element_factory_make("fakesink", NULL);
    g_object_set(sink, "sync", FALSE, "async", FALSE, NULL);
    gst_bin_add(GST_BIN(pipeline), sink);
    gst_element_sync_state_with_parent(sink);
    GstPad *in = gst_element_get_static_pad(sink, "sink");
    gst_pad_link(pad, in);
    gst_object_unref(in);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int ready;
    if (!ready) {
        gst_init(NULL, NULL);
        GST_PLUGIN_STATIC_REGISTER(coreelements);
        GST_PLUGIN_STATIC_REGISTER(app);
        GST_PLUGIN_STATIC_REGISTER(isomp4);
        GST_PLUGIN_STATIC_REGISTER(matroska);
        ready = 1;
    }
    GstElement *pipeline = gst_pipeline_new(NULL);
    GstElement *src = gst_element_factory_make("appsrc", NULL);
    GstElement *demux = gst_element_factory_make(DEMUX, NULL);
    if (!pipeline || !src || !demux)
        return 0;
    GstCaps *caps = gst_caps_from_string(CAPS);
    g_object_set(src, "caps", caps, "format", GST_FORMAT_BYTES, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline), src, demux, NULL);
    gst_element_link(src, demux);
    g_signal_connect(demux, "pad-added", G_CALLBACK(found_pad), pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(src),
                            gst_buffer_new_memdup(data, size));
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (m)
        gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 0;
}
