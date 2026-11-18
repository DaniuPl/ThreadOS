/* threadosaudiosink: GStreamer's sound out through ThreadOS's own call
 *
 * Where a Linux system has PulseAudio or ALSA, ThreadOS has one call,
 * SYS64_SND_STREAM: begin, a block of frames, end - sixteen-bit stereo
 * at 48 kHz, the format every HDA codec plays and virtio-sound is asked
 * for. The PC's HDA (kernel/hda.c) and the phone's virtio-sound
 * (kernelarm64/vsnd.c) both answer it. A write waits for room, which is
 * what paces the playing: the ring buffer's thread blocks in it as it
 * would in ALSA's.
 *
 * The stream belongs to the program, not the thread: GStreamer prepares
 * a sink on its state change, writes from the ring buffer's thread and
 * stops from the state change again. Another program playing already
 * (one plays at a time) is waited for, a little at a time, so that a
 * page's sound starts when the music before it ends rather than never.
 *
 * Ranked primary, as a Linux system's sinks are: autoaudiosink, which
 * WebKit plays its audio into, finds it.
 */

#include <string.h>
/* (GST_PLUGIN_DEFINE names the package a plugin comes from) */
#define PACKAGE "threados"
#include <gst/gst.h>
#include <gst/audio/gstaudiosink.h>
#include "usys64.h"

#define SND_STREAM_BEGIN 0
#define SND_STREAM_WRITE 1
#define SND_STREAM_END   2
#define BLOCK_FRAMES     1024          /* the most one write takes */

#define GST_TYPE_THREADOS_SINK (gst_threados_sink_get_type())
G_DECLARE_FINAL_TYPE(GstThreadOSSink, gst_threados_sink, GST, THREADOS_SINK,
                     GstAudioSink)

struct _GstThreadOSSink {
    GstAudioSink parent;
    gboolean begun;
};

G_DEFINE_TYPE(GstThreadOSSink, gst_threados_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format = (string) S16LE, "
                    "layout = (string) interleaved, rate = (int) 48000, "
                    "channels = (int) 2"));

static long stream(int op, const void *frames, long n)
{
    return syscall64_3(SYS64_SND_STREAM, op, (int64_t)(uintptr_t)frames, n);
}

static gboolean sink_open(GstAudioSink *sink)
{
    (void)sink;
    return TRUE;
}

static gboolean sink_prepare(GstAudioSink *sink, GstAudioRingBufferSpec *spec)
{
    GstThreadOSSink *self = GST_THREADOS_SINK(sink);
    /* the ring buffer's segments a block each, as many as half a
       second holds */
    spec->segsize = BLOCK_FRAMES * 4;
    spec->segtotal = 12;
    self->begun = FALSE;
    return TRUE;
}

static gint sink_write(GstAudioSink *sink, gpointer data, guint length)
{
    GstThreadOSSink *self = GST_THREADOS_SINK(sink);
    const guint8 *p = data;
    guint left = length / 4;
    if (!self->begun) {
        /* another program's sound first: a few tries, then this block
           goes by unheard rather than holding the page up */
        int tries = 0;
        while (stream(SND_STREAM_BEGIN, NULL, 0) < 0) {
            if (++tries == 20)
                return (gint)length;
            g_usleep(50000);
        }
        self->begun = TRUE;
    }
    while (left) {
        guint n = left < BLOCK_FRAMES ? left : BLOCK_FRAMES;
        if (stream(SND_STREAM_WRITE, p, n) < 0) {
            self->begun = FALSE;     /* taken from us: try again later */
            break;
        }
        p += n * 4;
        left -= n;
    }
    return (gint)length;
}

static guint sink_delay(GstAudioSink *sink)
{
    (void)sink;
    return 0;
}

static void sink_reset(GstAudioSink *sink)
{
    (void)sink;
}

static gboolean sink_unprepare(GstAudioSink *sink)
{
    GstThreadOSSink *self = GST_THREADOS_SINK(sink);
    if (self->begun)
        stream(SND_STREAM_END, NULL, 0);
    self->begun = FALSE;
    return TRUE;
}

static gboolean sink_close(GstAudioSink *sink)
{
    (void)sink;
    return TRUE;
}

static void gst_threados_sink_class_init(GstThreadOSSinkClass *klass)
{
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *audio = GST_AUDIO_SINK_CLASS(klass);
    gst_element_class_set_static_metadata(element, "ThreadOS audio sink",
        "Sink/Audio", "Plays audio through ThreadOS's sound stream",
        "ThreadOS");
    gst_element_class_add_static_pad_template(element, &sink_template);
    audio->open = sink_open;
    audio->prepare = sink_prepare;
    audio->write = sink_write;
    audio->delay = sink_delay;
    audio->reset = sink_reset;
    audio->unprepare = sink_unprepare;
    audio->close = sink_close;
}

static void gst_threados_sink_init(GstThreadOSSink *self)
{
    self->begun = FALSE;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "threadosaudiosink", GST_RANK_PRIMARY,
                                GST_TYPE_THREADOS_SINK);
}

/* (the macro leaves the description's padding to be zero) */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, threados,
                  "ThreadOS's sound", plugin_init, "1.0", "LGPL", "ThreadOS",
                  "https://github.com/DaniuPl/ThreadOS")
