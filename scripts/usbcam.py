"""A USB Video Class camera made up by a test, for usbredir.py.

A high-speed webcam: an interface association, a VideoControl
interface (a camera terminal, a processing unit, an output terminal,
an interrupt endpoint) and a VideoStreaming interface whose class
descriptors list two formats - MJPEG and uncompressed YUY2 - of four
frame sizes each, then eight alternate settings with one isochronous
IN endpoint each, some carrying two or three packets a microframe
(bits 12:11 of wMaxPacketSize). All of it is longer than 512 bytes, as
a real camera's configuration is: it has to be read whole. The layouts are those of the UVC 1.1
specification (3.7, 3.9) and its payload format documents.

It answers the PROBE control as a camera does - SET_CUR is kept,
GET_CUR gives it back with how many bytes a microframe the mode needs
(dwMaxPayloadTransferSize, from its own table) - and keeps every
request, so a test can say what the driver asked. expected() works
out, from its own descriptors and table, what a driver following the
rule "the smallest alternate that carries it" must report: the
independent expectation. bulk=True makes it a camera that streams over
a bulk IN endpoint at alternate 0 instead, with no alternates at all.
"""

import struct

from usbredir import SUCCESS, STALL, SPEED_HIGH, TYPE_INTERRUPT, TYPE_ISO, \
    TYPE_BULK

YUY2_GUID = bytes.fromhex("5955593200001000800000aa00389b71")

# (format index, frame index, width, height, default interval in 100 ns,
#  what the PROBE answers as dwMaxPayloadTransferSize)
MODES = [
    (1, 1, 1280, 720, 333333, 3000),
    (1, 2, 640, 480, 333333, 2400),
    (1, 3, 320, 240, 333333, 900),
    (1, 4, 160, 120, 333333, 300),
    (2, 1, 640, 480, 666666, 2048),     # exactly what alternate 6 carries
    (2, 2, 320, 240, 666666, 1100),
    (2, 3, 176, 144, 666666, 100),
    (2, 4, 160, 120, 666666, 3500),     # more than any alternate carries
]
# wMaxPacketSize of alternates 1..8: 192, 384, 640, 1024, 800x2,
# 1024x2, 896x3 and 1024x3 bytes a microframe
ALTS = [0x00C0, 0x0180, 0x0280, 0x0400, 0x0B20, 0x0C00, 0x1380, 0x1400]


def alt_bytes(raw):
    return (raw & 0x7FF) * (1 + ((raw >> 11) & 3))


def frame_desc(subtype, index, w, h, interval):
    """A frame descriptor with one discrete interval (UVC 1.1, 3.9.2.x)"""
    size = w * h * 2
    return struct.pack("<BBBBBHHIIIIBI", 30, 0x24, subtype, index, 0, w, h,
                       size * 8, size * 8 * 30, size, interval, 1, interval)


class UvcCamera:
    has_alts = True
    speed = SPEED_HIGH
    ep0_size = 64

    def __init__(self, bulk=False, vendor=0x0C45, product=0x6366,
                 name="ThreadOS test camera"):
        self.bulk = bulk
        self.vendor, self.product, self.name = vendor, product, name
        self.requests = []
        self.probe = bytearray(34)
        # bcdUSB 2.00, class EF/02/01 (an interface association)
        self.device = struct.pack("<BBHBBBBHHHBBBB", 18, 1, 0x0200, 0xEF, 2,
                                  1, 64, vendor, product, 0x0100, 1, 2, 0, 1)
        self.config = self._config()

    def _config(self):
        # VideoControl: header, camera terminal, processing unit, output
        # terminal; their total goes into the header
        cam_term = struct.pack("<BBBBHBBHHHB3s", 18, 0x24, 0x02, 1, 0x0201,
                               0, 0, 0, 0, 0, 3, bytes(3))
        proc_unit = struct.pack("<BBBBBHB2sB", 11, 0x24, 0x05, 2, 1, 0, 2,
                                bytes(2), 0)
        out_term = struct.pack("<BBBBHBBB", 9, 0x24, 0x03, 3, 0x0101, 0, 2, 0)
        vc_body = cam_term + proc_unit + out_term
        vc_header = struct.pack("<BBBHHIBB", 13, 0x24, 0x01, 0x0110,
                                13 + len(vc_body), 48000000, 1, 1)
        vc = (struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 1, 0x0E, 0x01, 0, 0) +
              vc_header + vc_body +
              struct.pack("<BBBBHB", 7, 5, 0x83, 0x03, 16, 6) +
              struct.pack("<BBBH", 5, 0x25, 0x03, 16))

        # VideoStreaming, alternate 0: the formats and frames
        n_mjpeg = sum(1 for m in MODES if m[0] == 1)
        n_yuy2 = sum(1 for m in MODES if m[0] == 2)
        mjpeg = struct.pack("<BBBBBBBBBBB", 11, 0x24, 0x06, 1, n_mjpeg, 1, 1,
                            0, 0, 0, 0)
        yuy2 = struct.pack("<BBBBB16sBBBBBB", 27, 0x24, 0x04, 2, n_yuy2,
                           YUY2_GUID, 16, 1, 0, 0, 0, 0)
        formats = mjpeg
        for f, fr, w, h, iv, _ in MODES:
            if f == 1:
                formats += frame_desc(0x07, fr, w, h, iv)
        formats += yuy2
        for f, fr, w, h, iv, _ in MODES:
            if f == 2:
                formats += frame_desc(0x05, fr, w, h, iv)
        formats += struct.pack("<BBBBBB", 6, 0x24, 0x0D, 1, 1, 4)
        ep_addr = 0x82 if self.bulk else 0x81
        in_header = struct.pack("<BBBBHBBBBBBBBB", 15, 0x24, 0x01, 2,
                                15 + len(formats), ep_addr, 0, 3, 0, 0, 0,
                                1, 0, 0)
        vs = struct.pack("<BBBBBBBBB", 9, 4, 1, 0, 1 if self.bulk else 0,
                         0x0E, 0x02, 0, 0) + in_header + formats
        if self.bulk:
            vs += struct.pack("<BBBBHB", 7, 5, 0x82, 0x02, 512, 0)
        else:
            for n, raw in enumerate(ALTS, 1):
                vs += struct.pack("<BBBBBBBBB", 9, 4, 1, n, 1, 0x0E, 0x02, 0,
                                  0)
                vs += struct.pack("<BBBBHB", 7, 5, 0x81, 0x05, raw, 1)

        iad = struct.pack("<BBBBBBBB", 8, 0x0B, 0, 2, 0x0E, 0x03, 0, 0)
        body = iad + vc + vs
        return struct.pack("<BBHBBBBB", 9, 2, 9 + len(body), 2, 1, 0, 0x80,
                           250) + body

    # --- what usbredir.py asks of a device ----------------------------

    def iface_infos(self):
        return [(0x0E, 0x01, 0), (0x0E, 0x02, 0)]

    def endpoints(self, alts):
        eps = [(0x83, TYPE_INTERRUPT, 6, 0, 16)]
        alt = alts.get(1, 0)
        if self.bulk:
            eps.append((0x82, TYPE_BULK, 0, 1, 512))
        elif alt:
            raw = ALTS[alt - 1]
            eps.append((0x81, TYPE_ISO, 1, 1, alt_bytes(raw)))
        return eps

    def string(self, index):
        if index == 0:
            return struct.pack("<BBH", 4, 3, 0x0409)
        text = {1: "ThreadOS tests", 2: self.name}.get(index)
        if text is None:
            return None
        raw = text.encode("utf-16-le")
        return struct.pack("<BB", 2 + len(raw), 3) + raw

    def control(self, rtype, req, value, index, length, data):
        self.requests.append((rtype, req, value, index))
        if rtype == 0x80 and req == 6:                 # GET_DESCRIPTOR
            kind, i = value >> 8, value & 0xFF
            out = {1: self.device, 2: self.config}.get(kind)
            if kind == 3:
                out = self.string(i)
            return (SUCCESS, out[:length]) if out else (STALL, b"")
        if rtype == 0x80 and req == 0:                 # GET_STATUS
            return SUCCESS, b"\0\0"[:length]
        # The VideoStreaming interface's controls (UVC 1.1, 4.3.1.1)
        if index & 0xFF == 1 and value >> 8 in (1, 2):
            which = "probe" if value >> 8 == 1 else "commit"
            if rtype == 0x21 and req == 0x01:          # SET_CUR
                self.requests.append(("set_" + which, length, bytes(data)))
                if which == "probe":
                    self.probe[:len(data)] = data[:34]
                return SUCCESS, b""
            if rtype == 0xA1 and req in (0x81, 0x82, 0x83, 0x87):  # GET_*
                self.requests.append(("get_" + which, length))
                ans = bytearray(self.probe)
                f, fr = ans[2], ans[3]
                mode = next((m for m in MODES if m[:2] == (f, fr)), None)
                if mode:
                    w, h, payload = mode[2], mode[3], mode[5]
                    struct.pack_into("<II", ans, 18, w * h * 2, payload)
                return SUCCESS, bytes(ans[:length])
        if rtype & 0x80 == 0:                          # other OUT requests
            return SUCCESS, b""
        return STALL, b""

    # --- for the test ---------------------------------------------------

    def probes(self):
        """(format, frame, interval, wLength) of every SET_CUR PROBE"""
        out = []
        for r in self.requests:
            if r[0] == "set_probe":
                length, data = r[1], r[2]
                f, fr = data[2], data[3]
                iv = struct.unpack_from("<I", data, 4)[0]
                out.append((f, fr, iv, length))
        return out

    def expected(self):
        """The lines 'camera' must print for each mode, worked out from
        these descriptors by the rule: the smallest alternate whose
        packets carry the PROBE's payload"""
        lines = []
        for k, (f, fr, w, h, iv, payload) in enumerate(MODES):
            cc = "MJPG" if f == 1 else "YUY2"
            hundredths = 10 ** 9 // iv
            head = "  mode %d: %s %dx%d %d.%02d fps, payload %d B" % (
                k, cc, w, h, hundredths // 100, hundredths % 100, payload)
            if self.bulk:
                lines.append(head + ", bulk")
                continue
            fits = [(alt_bytes(raw), n) for n, raw in enumerate(ALTS, 1)
                    if alt_bytes(raw) >= payload]
            if fits:
                b, n = min(fits)
                lines.append(head + ", alternate %d (%d B)" % (n, b))
            else:
                lines.append(head + ", no alternate carries it")
        return lines
