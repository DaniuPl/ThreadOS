/* The camera in a laptop's lid: USB Video Class, first stage
 *
 * A UVC camera describes itself at length: a VideoControl interface
 * (what the camera is made of) and a VideoStreaming interface whose
 * class-specific descriptors list every format it sends - MJPEG, or
 * uncompressed YUY2 - and every frame size of each, followed by the
 * alternate settings of that interface, each with one isochronous
 * endpoint of a different size. Alternate 0 has none: a camera that is
 * not sending takes no room on the bus.
 *
 * Which alternate a mode needs is not in the descriptors. The camera
 * is asked: a PROBE control set to the format, the frame size and the
 * interval, then read back, says how many bytes a (micro)frame it
 * would send - and the alternate to use is the smallest one carrying
 * that. Asking is all this stage does. Nothing is committed and no
 * alternate other than 0 is chosen, so the camera stays dark; the
 * picture needs isochronous transfers, which are the next stage.
 *
 * The descriptor walk is a function of the bytes alone (uvc_parse), so
 * the host model of a laptop's USB and the fuzzer can run it as it is.
 */

#include "include/uvc.h"
#include "include/cam_info.h"
#include "include/usbhc.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/pmm.h"
#include "include/gpt.h"

#define CS_INTERFACE 0x24
#define VC_HEADER    0x01
#define VS_INPUT_HEADER 0x01
#define VS_FORMAT_UNCOMPRESSED 0x04
#define VS_FRAME_UNCOMPRESSED  0x05
#define VS_FORMAT_MJPEG 0x06
#define VS_FRAME_MJPEG  0x07
#define CC_VIDEO 0x0E
#define SC_VIDEOCONTROL 0x01
#define SC_VIDEOSTREAMING 0x02

#define SET_CUR 0x01
#define GET_CUR 0x81
#define VS_PROBE_CONTROL 0x01
#define VS_COMMIT_CONTROL 0x02

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* --- the descriptors --------------------------------------------------- */

int uvc_parse(const uint8_t *cfg, int len, struct uvc_desc *out)
{
    memset(out, 0, sizeof(*out));
    int have_vc = 0, have_vs = 0;
    int in_vc = 0, in_vs = 0, alt = 0;
    uint32_t fourcc = 0;
    int format = -1;
    for (int i = 0; i + 2 <= len;) {
        int dlen = cfg[i], type = cfg[i + 1];
        /* A length that does not move the walk on, or runs past the
           end, ends it: what came before still stands */
        if (dlen < 2 || i + dlen > len)
            break;
        const uint8_t *d = cfg + i;
        if (type == 0x04 && dlen >= 9) {        /* an interface */
            in_vc = in_vs = 0;
            if (d[5] == CC_VIDEO && d[6] == SC_VIDEOCONTROL &&
                (!have_vc || d[2] == out->vc_iface)) {
                have_vc = 1;
                out->vc_iface = d[2];
                in_vc = 1;
            } else if (d[5] == CC_VIDEO && d[6] == SC_VIDEOSTREAMING &&
                       (!have_vs || d[2] == out->vs_iface)) {
                /* The first streaming interface: a second one is a
                   second camera (or a still-image pipe), not this one */
                have_vs = 1;
                out->vs_iface = d[2];
                in_vs = 1;
                alt = d[3];
            }
        } else if (type == CS_INTERFACE && in_vc && dlen >= 5 &&
                   d[2] == VC_HEADER) {
            out->bcd_uvc = (uint16_t)(d[3] | d[4] << 8);
        } else if (type == CS_INTERFACE && in_vs && alt == 0 && dlen >= 3) {
            int sub = d[2];
            if (sub == VS_FORMAT_MJPEG && dlen >= 11) {
                format = d[3];
                fourcc = 'M' | 'J' << 8 | 'P' << 16 | (uint32_t)'G' << 24;
            } else if (sub == VS_FORMAT_UNCOMPRESSED && dlen >= 27) {
                format = d[3];
                fourcc = le32(d + 5);       /* the GUID begins with it */
            } else if (sub == VS_FORMAT_UNCOMPRESSED ||
                       sub == VS_FORMAT_MJPEG) {
                format = -1;                /* too short to trust */
            } else if ((sub == VS_FRAME_MJPEG || sub == VS_FRAME_UNCOMPRESSED)
                       && dlen >= 26 && format >= 0 &&
                       out->nmodes < UVC_MODES_MAX) {
                struct uvc_mode *m = &out->modes[out->nmodes++];
                m->fourcc = fourcc;
                m->format = (uint8_t)format;
                m->frame = d[3];
                m->w = (uint16_t)(d[5] | d[6] << 8);
                m->h = (uint16_t)(d[7] | d[8] << 8);
                m->interval = le32(d + 21);
                m->alt = -1;
            }
        } else if (type == 0x05 && in_vs && dlen >= 7 && (d[2] & 0x80)) {
            int kind = d[3] & 3;
            uint16_t raw = (uint16_t)(d[4] | d[5] << 8);
            if (kind == 1 && alt > 0 && out->nalts < UVC_ALTS_MAX) {
                struct uvc_alt *a = &out->alts[out->nalts++];
                a->alt = (uint8_t)alt;
                a->ep = d[2] & 0x0F;
                a->raw_mps = raw;
                a->interval = d[6];
            } else if (kind == 2 && alt == 0) {
                out->bulk_ep = d[2] & 0x0F;
                out->bulk_mps = raw & 0x7FF;
            }
        }
        i += dlen;
    }
    out->found = have_vc && have_vs && out->nmodes > 0;
    return out->found ? 0 : -1;
}

int uvc_probe_size(uint16_t bcd_uvc)
{
    return bcd_uvc < 0x0110 ? 26 : bcd_uvc < 0x0150 ? 34 : 48;
}

/* What an alternate carries in a (micro)frame: its packet, and up to
   two more in the same one at high speed (bits 12:11) */
static uint32_t alt_bytes(const struct uvc_alt *a)
{
    return (uint32_t)(a->raw_mps & 0x7FF) * (1u + ((a->raw_mps >> 11) & 3u));
}

/* --- the camera -------------------------------------------------------- */

static struct uvc_desc cam;
static uint8_t cfg_buf[UVC_CFG_MAX];
static int present;
static int cam_port = -1, cam_dev = -1;
/* Streaming, its thread running, and a stop asked of that thread */
static volatile int streaming, pump_running, stop_req;
static uint16_t cam_vid, cam_pid;
static const char *gave_up;

static int give_up(const char *why)
{
    gave_up = why;
    return 0;
}

/* SET_CUR then GET_CUR of the PROBE control for one mode: what the
   camera answers is what it would send. 0, or -1 with no answer. */
static int probe(struct uvc_mode *m)
{
    int n = uvc_probe_size(cam.bcd_uvc);
    uint8_t ask[48], got[48];
    memset(ask, 0, sizeof(ask));
    ask[0] = 1;                       /* bmHint: keep the interval */
    ask[2] = m->format;
    ask[3] = m->frame;
    ask[4] = (uint8_t)m->interval;
    ask[5] = (uint8_t)(m->interval >> 8);
    ask[6] = (uint8_t)(m->interval >> 16);
    ask[7] = (uint8_t)(m->interval >> 24);
    uint8_t setup[8] = { 0x21, SET_CUR, 0, VS_PROBE_CONTROL,
                         cam.vs_iface, 0, (uint8_t)n, 0 };
    if (usbhc_control(cam_dev, setup, ask, n, 0) < 0)
        return -1;
    setup[0] = 0xA1;
    setup[1] = GET_CUR;
    memset(got, 0, sizeof(got));
    if (usbhc_control(cam_dev, setup, got, n, 1) < 26)
        return -1;
    m->payload = le32(got + 22);
    return 0;
}

/* The smallest alternate that carries what the mode needs */
static void choose_alt(struct uvc_mode *m)
{
    m->alt = -1;
    m->alt_bytes = 0;
    if (!cam.nalts && cam.bulk_ep) {
        m->alt = 0;                   /* a bulk camera: no alternates */
        return;
    }
    for (int k = 0; k < cam.nalts; k++) {
        uint32_t b = alt_bytes(&cam.alts[k]);
        if (b >= m->payload && (m->alt < 0 || b < m->alt_bytes)) {
            m->alt = cam.alts[k].alt;
            m->alt_bytes = b;
        }
    }
}

static void hex4(char *o, uint16_t v)
{
    for (int i = 0; i < 4; i++)
        o[i] = "0123456789abcdef"[(v >> (12 - 4 * i)) & 0xF];
    o[4] = 0;
}

/* "1.00" out of 0x0100 */
static void version(char *o, uint16_t bcd)
{
    o[0] = "0123456789abcdef"[(bcd >> 8) & 0xF];
    o[1] = '.';
    o[2] = "0123456789abcdef"[(bcd >> 4) & 0xF];
    o[3] = "0123456789abcdef"[bcd & 0xF];
    o[4] = 0;
}

static int try_port(int port)
{
    int d = usbhc_attach(port);
    if (d < 0)
        return 0;
    uint8_t dev_desc[18];
    if (usbhc_descriptor(d, 0x01, 0, dev_desc, 18) < 18)
        return 0;
    if (usbhc_descriptor(d, 0x02, 0, cfg_buf, 9) < 9)
        return 0;
    int total = cfg_buf[2] | (cfg_buf[3] << 8);
    if (total > UVC_CFG_MAX)
        total = UVC_CFG_MAX;
    if (total < 9 || usbhc_descriptor(d, 0x02, 0, cfg_buf, total) < total)
        return 0;
    struct uvc_desc found;
    if (uvc_parse(cfg_buf, total, &found) < 0)
        return 0;

    /* From here on it IS a camera, and a step that fails says which */
    cam = found;
    cam_dev = d;
    cam_vid = (uint16_t)(dev_desc[8] | dev_desc[9] << 8);
    cam_pid = (uint16_t)(dev_desc[10] | dev_desc[11] << 8);
    if (usbhc_request(d, 0x00, 0x09, cfg_buf[5], 0) < 0)
        return give_up("a camera is there, but would not take its "
                       "configuration");
    /* Alternate 0: nothing on the bus until a picture is wanted */
    if (usbhc_request(d, 0x01, 0x0B, 0, cam.vs_iface) < 0)
        return give_up("a camera is there, but would not stay idle "
                       "(SET_INTERFACE)");
    for (int k = 0; k < cam.nmodes; k++) {
        struct uvc_mode *m = &cam.modes[k];
        m->payload = 0;
        m->alt = -2;
        if (probe(m) == 0)
            choose_alt(m);
    }
    usbhc_claim(port, "uvc");
    cam_port = port;
    present = 1;
    char v[5], p[5], ver[5];
    hex4(v, cam_vid);
    hex4(p, cam_pid);
    version(ver, cam.bcd_uvc);
    printk("[ OK ] Camera: %s:%s, UVC %s, %d modes\n", v, p, ver,
           cam.nmodes);
    return 1;
}

static int on_plug(int port)
{
    if (present)
        return 0;
    return try_port(port);
}

static void on_unplug(int port)
{
    if (!present || port != cam_port)
        return;
    stop_req = 1;                     /* the stream's thread tidies up */
    present = 0;
    cam_port = cam_dev = -1;
    give_up("the camera went away");
}

int uvc_init(void)
{
    static int watching;
    if (present)
        return 1;
    if (!usbhc_init())
        return give_up("there is no USB controller");
    if (!watching) {
        usbhc_on_plug(on_plug);
        usbhc_on_unplug(on_unplug);
        usbhc_watch();
        watching = 1;
    }
    gave_up = (const char *)0;
    for (int port = 0; port < usbhc_ports(); port++)
        if (try_port(port))
            return 1;
    if (gave_up)
        return 0;
    return give_up("there is no camera on the USB bus");
}

int uvc_present(void)
{
    return present;
}

const char *uvc_why(void)
{
    return present ? (const char *)0
                   : (gave_up ? gave_up : "the camera was never looked for");
}

const struct uvc_desc *uvc_desc(void)
{
    return present ? &cam : (const struct uvc_desc *)0;
}

void uvc_print(void)
{
    if (!present) {
        printk("camera: none - %s\n", uvc_why());
        return;
    }
    char v[5], p[5], ver[5];
    hex4(v, cam_vid);
    hex4(p, cam_pid);
    version(ver, cam.bcd_uvc);
    printk("camera: %s:%s, UVC %s, port %d, %d modes\n", v, p, ver,
           cam_port + 1, cam.nmodes);
    for (int k = 0; k < cam.nmodes; k++) {
        const struct uvc_mode *m = &cam.modes[k];
        char cc[5];
        for (int i = 0; i < 4; i++) {
            char c = (char)(m->fourcc >> (8 * i));
            cc[i] = c >= 0x20 && c < 0x7F ? c : '?';
        }
        cc[4] = 0;
        uint32_t hundredths = m->interval ? 1000000000u / m->interval : 0;
        printk("  mode %d: %s %ux%u %u.%u%u fps", k, cc, (uint32_t)m->w,
               (uint32_t)m->h, hundredths / 100, (hundredths / 10) % 10,
               hundredths % 10);
        if (m->alt == -2)
            printk(", no answer to PROBE\n");
        else if (m->alt < 0)
            printk(", payload %u B, no alternate carries it\n", m->payload);
        else if (m->alt == 0)
            printk(", payload %u B, bulk\n", m->payload);
        else
            printk(", payload %u B, alternate %d (%u B)\n", m->payload,
                   m->alt, m->alt_bytes);
    }
}

/* --- pictures ------------------------------------------------------------
 *
 * A frame comes in payloads: every isochronous packet starts with a
 * header of its own (its length, then a bit field: FID, which flips
 * with every frame; EOF on a frame's last payload; ERR when the camera
 * knows the frame is bad) and goes on with a piece of the frame. The
 * pieces are put together in the fill buffer; a frame ends at EOF or
 * when FID flips, and only a whole one is handed out: a lost TD, an
 * ERR, too many bytes or (uncompressed) the wrong number of them throw
 * it away. The first frame after starting is thrown away too, as it
 * began before anybody was listening.
 *
 * Three buffers: the pump fills one, the newest whole frame waits in
 * the second, and the reader has the third; a reader takes the newest
 * by swapping under a lock, so nothing is ever torn and nobody waits.
 */

static struct spinlock frames_lock = SPINLOCK_FREE;
static uint8_t *fb[3];               /* fill, ready, read */
static uint32_t fb_len[3], fb_pages, frame_max, frame_exact;
static uint32_t ready_seq;           /* 0 = nothing ready yet */
static int fill_bad, synced, fid = -1, after_eof;
static int stream_mode = -1, stream_alt, stream_owner = -1;
static uint32_t n_frames, n_dropped, n_lost;

static void frame_end(void)
{
    uint32_t len = fb_len[0];
    int ok = synced && !fill_bad && len > 0;
    const struct uvc_mode *m = &cam.modes[stream_mode];
    if (ok && frame_exact && len != frame_exact)
        ok = 0;                       /* an uncompressed frame is exact */
    if (ok && m->fourcc == ('M' | 'J' << 8 | 'P' << 16 | (uint32_t)'G' << 24)) {
        /* A JPEG starts with SOI; whatever a camera pads after EOI is
           not the picture */
        if (len < 4 || fb[0][0] != 0xFF || fb[0][1] != 0xD8)
            ok = 0;
        while (ok && len >= 2 && !(fb[0][len - 2] == 0xFF &&
                                   fb[0][len - 1] == 0xD9))
            len--;
        if (len < 4)
            ok = 0;
    }
    if (ok) {
        irqflags_t f = spin_lock_irq(&frames_lock);
        uint8_t *t = fb[0];
        fb[0] = fb[1];
        fb[1] = t;
        fb_len[1] = len;
        ready_seq++;
        spin_unlock_irq(&frames_lock, f);
        n_frames++;
    } else if (synced && fb_len[0]) {
        n_dropped++;
    }
    fb_len[0] = 0;
    fill_bad = 0;
}

static void payload(void *arg, const uint8_t *d, int len, int lost)
{
    (void)arg;
    if (lost) {
        n_lost++;
        fill_bad = 1;
        return;
    }
    if (len == 0)
        return;                       /* nothing in this interval */
    int hl = d[0];
    if (hl < 2 || hl > len) {
        fill_bad = 1;
        return;
    }
    int info = d[1], f = info & 1;
    if (fid >= 0 && f != fid) {
        /* a new frame: what came before ends here, if EOF did not end
           it already */
        if (!after_eof)
            frame_end();
        if (!synced) {
            synced = 1;               /* from here on frames are whole */
            fb_len[0] = 0;
            fill_bad = 0;
        }
        after_eof = 0;
    }
    fid = f;
    if (after_eof)
        return;                       /* headers after the end of a frame */
    if (info & 0x40)
        fill_bad = 1;                 /* ERR: the camera says it is bad */
    uint32_t n = (uint32_t)(len - hl);
    if (fb_len[0] + n > frame_max) {
        fill_bad = 1;
    } else {
        memcpy(fb[0] + fb_len[0], d + hl, n);
        fb_len[0] += n;
    }
    if (info & 2) {
        frame_end();
        after_eof = 1;
    }
}

static void frames_free(void);

/* The stream's own thread: it reads, and it is also what stops the
   stream - however the stop was asked for. A program that ends asks
   from the middle of its exit, where nothing may wait for a USB
   command; so the asking only sets a flag, and this does the rest. */
static void pump(void *arg)
{
    (void)arg;
    while (!stop_req) {
        usbhc_iso_poll(cam_dev, payload, (void *)0);
        task_sleep(1);
    }
    usbhc_iso_close(cam_dev);
    /* Back to alternate 0: the camera stops, and its light goes out */
    usbhc_request(cam_dev, 0x01, 0x0B, 0, cam.vs_iface);
    irqflags_t f = spin_lock_irq(&frames_lock);
    streaming = 0;
    spin_unlock_irq(&frames_lock, f);
    frames_free();
    stream_mode = -1;
    stream_owner = -1;
    pump_running = 0;
}

static void frames_free(void)
{
    for (int k = 0; k < 3; k++) {
        if (fb[k])
            for (uint32_t p = 0; p < fb_pages; p++)
                pmm_free_frame((uint32_t)(uintptr_t)fb[k] + p * 4096);
        fb[k] = (uint8_t *)0;
        fb_len[k] = 0;
    }
}

int uvc_start(int mode, int owner)
{
    if (!present || mode < 0 || mode >= cam.nmodes)
        return -1;
    if (streaming || pump_running)
        return -6;
    struct uvc_mode *m = &cam.modes[mode];
    if (m->alt == 0)
        return -5;
    const struct uvc_alt *a = (const struct uvc_alt *)0;
    for (int k = 0; k < cam.nalts; k++)
        if (cam.alts[k].alt == m->alt)
            a = &cam.alts[k];
    if (!a)
        return -2;

    /* Asked again, and what the camera answers is what is committed:
       the same bytes, as the specification wants */
    int n = uvc_probe_size(cam.bcd_uvc);
    uint8_t ask[48], got[48];
    memset(ask, 0, sizeof(ask));
    ask[0] = 1;
    ask[2] = m->format;
    ask[3] = m->frame;
    ask[4] = (uint8_t)m->interval;
    ask[5] = (uint8_t)(m->interval >> 8);
    ask[6] = (uint8_t)(m->interval >> 16);
    ask[7] = (uint8_t)(m->interval >> 24);
    uint8_t setup[8] = { 0x21, SET_CUR, 0, VS_PROBE_CONTROL,
                         cam.vs_iface, 0, (uint8_t)n, 0 };
    memset(got, 0, sizeof(got));
    if (usbhc_control(cam_dev, setup, ask, n, 0) < 0)
        return -3;
    setup[0] = 0xA1;
    setup[1] = GET_CUR;
    if (usbhc_control(cam_dev, setup, got, n, 1) < 26)
        return -3;
    setup[0] = 0x21;
    setup[1] = SET_CUR;
    setup[3] = VS_COMMIT_CONTROL;
    if (usbhc_control(cam_dev, setup, got, n, 0) < 0)
        return -3;

    /* Room for a frame: what the camera says one can be, or what an
       uncompressed one is */
    int yuy2 = m->fourcc == ('Y' | 'U' << 8 | 'Y' << 16 | (uint32_t)'2' << 24);
    frame_exact = yuy2 ? (uint32_t)m->w * m->h * 2 : 0;
    frame_max = le32(got + 18);
    if (frame_max < frame_exact)
        frame_max = frame_exact;
    if (!frame_max)
        frame_max = (uint32_t)m->w * m->h * 2;
    fb_pages = (frame_max + 4095) / 4096;
    for (int k = 0; k < 3; k++) {
        uint32_t at = pmm_alloc_contiguous(fb_pages);
        if (!at) {
            frames_free();
            return -4;
        }
        fb[k] = (uint8_t *)(uintptr_t)at;
    }

    /* The endpoint first - a controller with no room says so here -
       then the camera to that alternate, then the TDs */
    int r = usbhc_iso_open(cam_dev, a->ep, a->raw_mps, a->interval);
    if (r < 0) {
        frames_free();
        return r == -2 ? -2 : -1;
    }
    if (usbhc_request(cam_dev, 0x01, 0x0B, (uint16_t)m->alt, cam.vs_iface) < 0) {
        usbhc_iso_close(cam_dev);
        frames_free();
        return -3;
    }
    ready_seq = 0;
    fill_bad = synced = after_eof = 0;
    fid = -1;
    n_frames = n_dropped = n_lost = 0;
    stream_mode = mode;
    stream_alt = m->alt;
    stream_owner = owner;
    stop_req = 0;
    streaming = pump_running = 1;
    usbhc_iso_start(cam_dev);
    task_create("uvc", pump, (void *)0);
    return 0;
}

void uvc_stop(void)
{
    if (!pump_running)
        return;
    stop_req = 1;
    for (int i = 0; i < 300 && pump_running; i++)
        task_sleep(1);
}

void uvc_release(int tgid)
{
    if (pump_running && stream_owner == tgid && tgid >= 0)
        stop_req = 1;
}

int uvc_owner(void)
{
    return streaming ? stream_owner : -1;
}

void uvc_info(struct cam_info *out)
{
    memset(out, 0, sizeof(*out));
    out->present = present;
    out->streaming = streaming ? stream_mode : -1;
    out->owner = uvc_owner();
    out->frames = n_frames;
    out->dropped = n_dropped;
    out->lost = n_lost;
    if (!present) {
        const char *w = uvc_why();
        int i = 0;
        for (; w && w[i] && i < (int)sizeof(out->why) - 1; i++)
            out->why[i] = w[i];
        return;
    }
    hex4(out->name, cam_vid);
    out->name[4] = ':';
    hex4(out->name + 5, cam_pid);
    out->nmodes = cam.nmodes < CAM_MODES ? cam.nmodes : CAM_MODES;
    for (int k = 0; k < out->nmodes; k++) {
        out->modes[k].fourcc = cam.modes[k].fourcc;
        out->modes[k].w = cam.modes[k].w;
        out->modes[k].h = cam.modes[k].h;
        out->modes[k].interval = cam.modes[k].interval;
        out->modes[k].alt = cam.modes[k].alt;
    }
}

int uvc_streaming(void)
{
    return streaming;
}

const uint8_t *uvc_frame(uint32_t *len, uint32_t *seq)
{
    irqflags_t f = spin_lock_irq(&frames_lock);
    if (!streaming || !ready_seq || ready_seq == *seq) {
        spin_unlock_irq(&frames_lock, f);
        return (const uint8_t *)0;
    }
    uint8_t *t = fb[2];
    fb[2] = fb[1];
    fb[1] = t;
    fb_len[2] = fb_len[1];
    *seq = ready_seq;
    spin_unlock_irq(&frames_lock, f);
    *len = fb_len[2];
    return fb[2];
}

void uvc_counts(uint32_t *frames, uint32_t *dropped, uint32_t *lost)
{
    *frames = n_frames;
    *dropped = n_dropped;
    *lost = n_lost;
}

void uvc_grab(int mode, int n)
{
    int r = uvc_start(mode, -1);
    if (r < 0) {
        static const char *const why[] = {
            "", "no such mode, or no camera", "no room on the USB bus",
            "the camera said no", "no memory for the frames",
            "a bulk camera: it streams in a later stage", "already busy" };
        printk("camera: cannot start mode %d - %s\n", mode,
               why[-r < 7 ? -r : 1]);
        return;
    }
    uint32_t seq = 0;
    int got = 0;
    uint32_t until = pit_ticks() + 300 + (uint32_t)n * 100;
    while (got < n && pit_ticks() < until) {
        uint32_t len;
        const uint8_t *f = uvc_frame(&len, &seq);
        if (!f) {
            task_sleep(1);
            continue;
        }
        const struct uvc_mode *m = &cam.modes[mode];
        char cc[5];
        for (int i = 0; i < 4; i++)
            cc[i] = (char)(m->fourcc >> (8 * i));
        cc[4] = 0;
        printk("frame %d: %ux%u %s, %u bytes, crc32 %x\n", got,
               (uint32_t)m->w, (uint32_t)m->h, cc, len, crc32(f, len));
        got++;
    }
    uint32_t fr, dr, lo;
    uvc_counts(&fr, &dr, &lo);
    uvc_stop();
    printk("camera: %d of %d frames, %u dropped, %u lost\n", got, n, dr, lo);
}
