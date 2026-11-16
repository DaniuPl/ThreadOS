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
#include "include/usbhc.h"
#include "include/printk.h"
#include "include/string.h"

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
