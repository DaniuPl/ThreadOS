#ifndef THREADOS_UVC_H
#define THREADOS_UVC_H

#include <stdint.h>

/* A USB Video Class camera: the one in a laptop's lid
 *
 * Stage one: the camera is found, its configuration read whole, every
 * format and frame size it offers listed, and each one asked about
 * with a PROBE - how many bytes a (micro)frame it would send - so the
 * alternate setting that carries that is known. No picture yet: that
 * needs isochronous transfers, the next stage.
 *
 * What it says, for the tests to hold it to:
 *   at start   "[ OK ] Camera: 5986:055c, UVC 1.00, 4 modes"
 *   'camera'   "camera: 5986:055c, UVC 1.00, port 2, 4 modes"
 *              "  mode 0: MJPG 1280x720 30.00 fps, payload 3060 B,
 *                 alternate 6 (3072 B)"          (one line)
 *              "  mode 3: YUY2 160x120 30.00 fps, payload 144 B, bulk"
 *              "  mode 1: ... payload 900 B, no alternate carries it"
 *              "  mode 2: ... no answer to PROBE"
 *              or "camera: none - <why>"
 * The fps has two decimals: 10^9 / interval, as hundredths. The modes
 * are in descriptor order: formats as listed, frames within each.
 */

#define UVC_MODES_MAX 16
#define UVC_ALTS_MAX  16
#define UVC_CFG_MAX   4096      /* a configuration descriptor, read whole */

struct uvc_mode {
    uint32_t fourcc;            /* 'MJPG', or the GUID's first four bytes */
    uint16_t w, h;
    uint32_t interval;          /* the default, in 100 ns */
    uint8_t format, frame;      /* bFormatIndex, bFrameIndex */
    uint32_t payload;           /* dwMaxPayloadTransferSize the PROBE gave;
                                   0 = not asked, or no answer */
    int alt;                    /* the alternate setting that carries it;
                                   0 with a bulk endpoint; -1 none fits;
                                   -2 the PROBE was not answered */
    uint32_t alt_bytes;         /* what that alternate carries a (micro)frame */
};

struct uvc_alt {
    uint8_t alt, ep;            /* the alternate and its iso IN endpoint */
    uint16_t raw_mps;           /* wMaxPacketSize as written: bits 12:11 are
                                   the extra transactions */
    uint8_t interval;           /* bInterval */
};

/* What a configuration descriptor says about a camera in it. A pure
   function of the bytes, so the host model and the fuzzer run it */
struct uvc_desc {
    int found;                  /* a VideoControl and a VideoStreaming */
    uint16_t bcd_uvc;
    uint8_t vc_iface, vs_iface;
    uint8_t bulk_ep;            /* a bulk IN endpoint at alternate 0, or 0 */
    uint16_t bulk_mps;
    int nalts;
    struct uvc_alt alts[UVC_ALTS_MAX];
    int nmodes;
    struct uvc_mode modes[UVC_MODES_MAX];
};

int uvc_parse(const uint8_t *cfg, int len, struct uvc_desc *out);

/* The bytes a PROBE control holds for this UVC version: 26 for 1.0,
   34 for 1.1, 48 from 1.5 */
int uvc_probe_size(uint16_t bcd_uvc);

/* Walks the sockets for a camera and watches for one plugged later.
   1 = there is one. */
int uvc_init(void);
int uvc_present(void);
const char *uvc_why(void);
/* The camera's description, with what each mode's PROBE said */
const struct uvc_desc *uvc_desc(void);
/* For 'camera' in the shell, and the Camera line of 'hardware' */
void uvc_print(void);

/* Pictures, one mode at a time (stage two). start: 0; -1 no such mode
   or no camera; -2 no room on the bus; -3 the camera said no; -4 no
   memory; -5 a bulk camera (it streams in a later stage); -6 busy. */
int uvc_start(int mode, int owner);
void uvc_stop(void);
int uvc_streaming(void);
/* The program streaming (its tgid), -1 for none - or for the shell's
   'camera grab' - and, for the end of a program, a stop that does not
   wait: the stream's own thread stops it */
int uvc_owner(void);
void uvc_release(int tgid);
struct cam_info;
void uvc_info(struct cam_info *out);
/* The newest whole frame, if it is newer than *seq: its bytes (valid
   until the next call) and its length, *seq updated; 0 when nothing
   newer has come */
const uint8_t *uvc_frame(uint32_t *len, uint32_t *seq);
/* Frames finished, frames thrown away (torn, marked bad, the wrong
   size), TDs lost */
void uvc_counts(uint32_t *frames, uint32_t *dropped, uint32_t *lost);
/* 'camera grab <mode> <n>': n frames, each with its length and CRC-32 */
void uvc_grab(int mode, int n);

#endif
