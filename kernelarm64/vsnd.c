/* The phone's speaker and microphone: virtio-sound over MMIO
 *
 * QEMU's virtio-sound-device: a control queue that sets streams up -
 * their parameters, prepared, started, stopped, released - and one
 * queue of buffers each way. A buffer is a chain: which stream, the
 * samples, and the device's word on how it went; playing, the device
 * reads the samples and hands the chain back once they have gone out
 * (at the pace they sound), recording, it writes them.
 *
 * Both streams run as the PC's sound does (kernel/hda.c): 48 kHz,
 * 16-bit, two channels, so a program written for the PC plays and
 * records here unchanged (SYS64_SND_STREAM, SYS64_MIC). One program
 * plays at a time and one records; the loudness is applied here, to
 * the samples, as the shell's Settings set it.
 *
 * Waiting is polling - a millisecond's sleep between looks - by the
 * thread that plays or records: a buffer lasts over twenty.
 */

#include <stdint.h>
#include "arm64.h"
#include "netglue.h"
#include "thread.h"
#include "virtio.h"
#include "vsnd.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/string.h"

#define VIRTIO_ID_SOUND 25

/* The control requests and answers (virtio 1.2, 5.14) */
#define R_PCM_INFO       0x0100
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE    0x0102
#define R_PCM_RELEASE    0x0103
#define R_PCM_START      0x0104
#define R_PCM_STOP       0x0105
#define S_OK             0x8000
#define FMT_S16          5
#define RATE_48000       7
#define DIR_OUTPUT       0
#define DIR_INPUT        1

#define Q_CONTROL 0
#define Q_TX      2
#define Q_RX      3

#define PERIOD   1024            /* frames a buffer: 21 ms */
#define BUFS     4               /* in flight each way */
#define FRAME    4               /* two 16-bit samples */
#define PLAYOUT_MS 100           /* the device's own buffer, and more */

struct pcm_hdr {
    uint32_t code, stream;
};

struct pcm_info {
    uint32_t nid, features;
    uint64_t formats, rates;
    uint8_t direction, ch_min, ch_max, pad[5];
};

struct set_params {
    struct pcm_hdr hdr;
    uint32_t buffer_bytes, period_bytes, features;
    uint8_t channels, format, rate, pad;
};

struct xfer_status {
    uint32_t status, latency;
};

struct slot {
    uint32_t stream;             /* the chain's first part */
    int16_t pcm[PERIOD * 2];
    struct xfer_status st;
    uint32_t frames;
    int busy;
};

static struct virtio_dev *dev;
static struct virtq cq, eq, txq, rxq;
static int out_id = -1, in_id = -1;
static struct spinlock lock = SPINLOCK_FREE;
static struct slot tx[BUFS], rx[BUFS];
static int playing = -1, recording = -1;     /* their owners */
static int volume = 100, muted;
static int rx_next, rx_used;                 /* the next filled, how far */

static uint8_t creq[64], cresp[64 + 8 * sizeof(struct pcm_info)];
/* The control queue's one chain and its buffers are shared: a program
   starting to play and the shell starting to record take turns. The
   answer is waited for asleep, so the turn is a flag, not a spinlock */
static volatile int ctl_busy;

static void ctl_take(void)
{
    while (__atomic_exchange_n(&ctl_busy, 1, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
}

static void ctl_give(void)
{
    __atomic_store_n(&ctl_busy, 0, __ATOMIC_RELEASE);
}

/* One control request and its answer: 0 when the device said OK */
static int control(void *req, uint32_t len, void *resp, uint32_t rlen)
{
    struct virtio_buf b[2] = { { req, len, 0 }, { resp, rlen, 1 } };
    memset(resp, 0, rlen);
    virtio_chain(&cq, 0, b, 2);
    virtio_notify(dev, Q_CONTROL);
    uint64_t give_up = counter_now() + timer_freq();
    while (virtio_take(&cq, 0) < 0) {
        if (counter_now() > give_up) {
            printk("[ !! ] Sound: no answer to request %x\n",
                   *(uint32_t *)req);
            return -1;
        }
        thread_sleep_ms(1);
    }
    return *(uint32_t *)resp == S_OK ? 0 : -1;
}

static int stream_cmd(uint32_t code, int stream)
{
    struct pcm_hdr *h = (struct pcm_hdr *)creq;
    h->code = code;
    h->stream = (uint32_t)stream;
    return control(creq, sizeof(*h), cresp, 4);
}

/* Parameters, prepared, started: the stream runs */
static int stream_up(int stream)
{
    ctl_take();
    struct set_params *p = (struct set_params *)creq;
    memset(p, 0, sizeof(*p));
    p->hdr.code = R_PCM_SET_PARAMS;
    p->hdr.stream = (uint32_t)stream;
    p->buffer_bytes = PERIOD * FRAME * BUFS;
    p->period_bytes = PERIOD * FRAME;
    p->channels = 2;
    p->format = FMT_S16;
    p->rate = RATE_48000;
    int r = control(creq, sizeof(*p), cresp, 4) < 0 ||
            stream_cmd(R_PCM_PREPARE, stream) < 0 ||
            stream_cmd(R_PCM_START, stream) < 0 ? -1 : 0;
    ctl_give();
    return r;
}

static void stream_down(int stream)
{
    ctl_take();
    stream_cmd(R_PCM_STOP, stream);
    stream_cmd(R_PCM_RELEASE, stream);
    ctl_give();
}

int vsnd_init(struct virtio_dev *devs, int n)
{
    for (int i = 0; i < n && !dev; i++)
        if (devs[i].device_id == VIRTIO_ID_SOUND)
            dev = &devs[i];
    if (!dev)
        return -1;
    if (virtio_begin(dev) < 0 || virtio_queue(dev, Q_CONTROL, &cq, 4) < 0 ||
        virtio_queue(dev, 1, &eq, 4) < 0 ||
        virtio_queue(dev, Q_TX, &txq, 3 * BUFS) < 0 ||
        virtio_queue(dev, Q_RX, &rxq, 3 * BUFS) < 0) {
        dev = 0;
        return -1;
    }
    virtio_ready(dev);
    uint32_t streams = virtio_cfg32(dev, 4);
    if (streams > 8)
        streams = 8;
    struct {
        uint32_t code, start, count, size;
    } *q = (void *)creq;
    q->code = R_PCM_INFO;
    q->start = 0;
    q->count = streams;
    q->size = sizeof(struct pcm_info);
    if (!streams || control(creq, sizeof(*q), cresp,
                            4 + streams * sizeof(struct pcm_info)) < 0) {
        printk("[ !! ] Sound: virtio-sound would not say its streams\n");
        dev = 0;
        return -1;
    }
    const struct pcm_info *info = (const struct pcm_info *)(cresp + 4);
    for (uint32_t i = 0; i < streams; i++) {
        int s16_48k = (info[i].formats & (1ULL << FMT_S16)) &&
                      (info[i].rates & (1ULL << RATE_48000)) &&
                      info[i].ch_min <= 2 && info[i].ch_max >= 2;
        if (!s16_48k)
            continue;
        if (info[i].direction == DIR_OUTPUT && out_id < 0)
            out_id = (int)i;
        if (info[i].direction == DIR_INPUT && in_id < 0)
            in_id = (int)i;
    }
    printk("[ OK ] Sound: virtio-sound, %u streams - %s, %s\n", streams,
           out_id >= 0 ? "a speaker" : "no speaker",
           in_id >= 0 ? "a microphone" : "no microphone");
    return out_id >= 0 || in_id >= 0 ? 0 : -1;
}

int vsnd_present(void) { return dev && out_id >= 0; }
int vsnd_mic_present(void) { return dev && in_id >= 0; }

void vsnd_volume(int vol, int mute)
{
    if (vol >= 0)
        volume = vol > 100 ? 100 : vol;
    if (mute >= 0)
        muted = mute;
}

void vsnd_get(int *vol, int *mute)
{
    *vol = volume;
    *mute = muted;
}

/* --- playing ------------------------------------------------------------ */

int vsnd_begin(int owner)
{
    if (!vsnd_present())
        return -1;
    irqflags_t f = spin_lock_irq(&lock);
    if (playing >= 0) {
        spin_unlock_irq(&lock, f);
        return -2;
    }
    playing = owner;
    spin_unlock_irq(&lock, f);
    for (int i = 0; i < BUFS; i++)
        tx[i].busy = 0;
    if (stream_up(out_id) < 0) {
        playing = -1;
        return -1;
    }
    return 0;
}

int vsnd_owner(void) { return playing; }

/* Chains the device has finished with, free again */
static void reclaim(void)
{
    int id;
    while ((id = virtio_take(&txq, 0)) >= 0)
        if (id % 3 == 0 && id / 3 < BUFS)
            tx[id / 3].busy = 0;
}

int vsnd_write(const int16_t *frames, uint32_t n)
{
    if (!dev || playing < 0)
        return -1;
    uint32_t done = 0;
    while (done < n) {
        int k = -1;
        uint64_t give_up = counter_now() + 2 * timer_freq();
        while (k < 0) {
            reclaim();
            for (int i = 0; i < BUFS && k < 0; i++)
                if (!tx[i].busy)
                    k = i;
            if (k < 0) {
                if (counter_now() > give_up)
                    return done ? (int)done : -1;   /* the device stopped */
                thread_sleep_ms(1);
            }
        }
        struct slot *s = &tx[k];
        uint32_t m = n - done > PERIOD ? PERIOD : n - done;
        for (uint32_t i = 0; i < m * 2; i++) {
            int v = frames[done * 2 + i];
            s->pcm[i] = (int16_t)(muted ? 0 : volume == 100 ? v :
                                  v * volume / 100);
        }
        s->stream = (uint32_t)out_id;
        s->frames = m;
        s->busy = 1;
        struct virtio_buf b[3] = {
            { &s->stream, 4, 0 }, { s->pcm, m * FRAME, 0 },
            { &s->st, sizeof(s->st), 1 },
        };
        virtio_chain(&txq, (unsigned)(3 * k), b, 3);
        virtio_notify(dev, Q_TX);
        done += m;
    }
    return (int)done;
}

int vsnd_end(void)
{
    if (!dev || playing < 0)
        return -1;
    /* what is queued, played out first */
    uint64_t give_up = counter_now() + 2 * timer_freq();
    for (;;) {
        reclaim();
        int busy = 0;
        for (int i = 0; i < BUFS; i++)
            busy |= tx[i].busy;
        if (!busy || counter_now() > give_up)
            break;
        thread_sleep_ms(1);
    }
    /* A buffer comes back once the device has taken its samples, not
       once they have sounded: what it still holds would be cut off by
       the stop - and sound first when the stream next starts */
    thread_sleep_ms(PLAYOUT_MS);
    stream_down(out_id);
    playing = -1;
    return 0;
}

/* --- recording ------------------------------------------------------------ */

static void rx_queue(int k)
{
    struct slot *s = &rx[k];
    s->stream = (uint32_t)in_id;
    s->busy = 1;
    struct virtio_buf b[3] = {
        { &s->stream, 4, 0 }, { s->pcm, PERIOD * FRAME, 1 },
        { &s->st, sizeof(s->st), 1 },
    };
    virtio_chain(&rxq, (unsigned)(3 * k), b, 3);
}

int vsnd_mic_start(int owner)
{
    if (!vsnd_mic_present())
        return -1;
    irqflags_t f = spin_lock_irq(&lock);
    if (recording >= 0) {
        spin_unlock_irq(&lock, f);
        return -2;
    }
    recording = owner;
    spin_unlock_irq(&lock, f);
    if (stream_up(in_id) < 0) {
        recording = -1;
        return -1;
    }
    for (int k = 0; k < BUFS; k++)
        rx_queue(k);
    virtio_notify(dev, Q_RX);
    rx_next = 0;
    rx_used = 0;
    return 0;
}

int vsnd_mic_who(void) { return recording; }

int vsnd_mic_read(int16_t *out, uint32_t n)
{
    if (!dev || recording < 0)
        return -1;
    uint32_t done = 0;
    uint64_t give_up = counter_now() + 2 * timer_freq();
    while (done < n) {
        int id;
        uint32_t len;
        while ((id = virtio_take(&rxq, &len)) >= 0)
            if (id % 3 == 0 && id / 3 < BUFS) {
                struct slot *s = &rx[id / 3];
                s->busy = 0;
                s->frames = len > sizeof(s->st) ?
                            (len - (uint32_t)sizeof(s->st)) / FRAME : 0;
            }
        /* the buffers come back in the order they went */
        struct slot *s = &rx[rx_next];
        if (s->busy) {
            if (done || counter_now() > give_up)
                break;
            thread_sleep_ms(1);
            continue;
        }
        uint32_t m = s->frames - (uint32_t)rx_used;
        if (m > n - done)
            m = n - done;
        memcpy(out + done * 2, s->pcm + rx_used * 2, m * FRAME);
        done += m;
        rx_used += (int)m;
        if ((uint32_t)rx_used >= s->frames) {
            rx_queue(rx_next);
            virtio_notify(dev, Q_RX);
            rx_next = (rx_next + 1) % BUFS;
            rx_used = 0;
        }
    }
    return (int)done;
}

void vsnd_mic_stop(void)
{
    if (!dev || recording < 0)
        return;
    stream_down(in_id);
    /* what the device still held comes back with the stop */
    for (int i = 0; i < 50; i++) {
        while (virtio_take(&rxq, 0) >= 0)
            ;
        thread_sleep_ms(1);
    }
    recording = -1;
}
