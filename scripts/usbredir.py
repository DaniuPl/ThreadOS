"""USB devices made up by a test, plugged into QEMU through usbredir.

QEMU's usb-redir device forwards everything a guest says to a USB
device over a socket, in the usbredir protocol (spice-space.org's
usb-redirection-protocol.md), and expects a real device - usually
libusb's - on the other end. Here the other end is this module: a
device described by its descriptors, which answers control requests
from them and sends interrupt reports when the test says so. That way a
test can plug in a device QEMU has no model of - a keyboard with media
keys on a second interface, a volume knob - and the kernel's drivers
meet the same bytes as from the real thing.

Control transfers on endpoint 0, interrupt IN endpoints, and bulk
endpoints that take what is sent to them (scripts/btadapter.py's
Bluetooth adapter has two; nothing is ever sent back on them). QEMU
attaches a redirected device to an xHCI controller only from a peer
with 64-bit packet ids and 32-bit bulk lengths, so those are offered.

A device is anything with the descriptors (device, config, string()),
control(), iface_infos() and endpoints(); HidDevice is one, and a
device with an attach(redir) is handed the connection, to send what it
has to say on its own. A device may say its speed (SPEED_HIGH for a
camera; full speed otherwise), and one with alternate settings gives
endpoints(alts) - the endpoints of the alternates in force, by
interface - instead: QEMU forwards a SET_INTERFACE as a packet of its
own, not as a control transfer, and is told the new endpoints before
the answer, as the protocol requires. What the guest set is kept in
the device's requests as ("set_alt", interface, alternate).

Isochronous IN: when QEMU starts a stream (the guest has queued its
first TDs), a device with iso_packet(ep) is asked for one packet an
interval - iso_rate(ep) of them a second - and they are sent in
batches every ten milliseconds, as a camera sends them whether anybody
reads or not, until QEMU stops the stream.
"""

import socket
import struct
import threading
import time

# packet types
HELLO, DEVICE_CONNECT, RESET = 0, 1, 3
INTERFACE_INFO, EP_INFO = 4, 5
SET_CONFIGURATION, GET_CONFIGURATION, CONFIGURATION_STATUS = 6, 7, 8
SET_ALT_SETTING, GET_ALT_SETTING, ALT_SETTING_STATUS = 9, 10, 11
START_ISO_STREAM, STOP_ISO_STREAM, ISO_STREAM_STATUS = 12, 13, 14
START_INTERRUPT, STOP_INTERRUPT, INTERRUPT_STATUS = 15, 16, 17
CANCEL_DATA = 21
CONTROL_PACKET, BULK_PACKET, ISO_PACKET, INTERRUPT_PACKET = 100, 101, 102, 103

CAP_CONNECT_DEVICE_VERSION, CAP_EP_INFO_MAX_PACKET_SIZE = 1, 4
CAP_64BITS_IDS, CAP_32BITS_BULK_LENGTH = 5, 6
SUCCESS, STALL = 0, 4
SPEED_FULL, SPEED_HIGH = 1, 2
TYPE_CONTROL, TYPE_ISO, TYPE_BULK, TYPE_INTERRUPT, TYPE_INVALID = \
    0, 1, 2, 3, 255


def ep_index(addr):
    return ((addr & 0x80) >> 3) | (addr & 0x0F)


class HidInterface:
    def __init__(self, subclass, protocol, report_desc, ep, maxpacket=8,
                 interval=10):
        self.subclass, self.protocol = subclass, protocol
        self.report_desc = bytes(report_desc)
        self.ep, self.maxpacket, self.interval = ep, maxpacket, interval


class HidDevice:
    """A full-speed device of HID interfaces, its descriptors built from
    them the way the USB 2.0 and HID 1.11 specifications lay them out"""

    def __init__(self, interfaces, vendor=0x1209, product=0x0001,
                 name="ThreadOS test device"):
        self.interfaces = interfaces
        self.vendor, self.product, self.name = vendor, product, name
        self.device = struct.pack("<BBHBBBBHHHBBBB", 18, 1, 0x0110, 0, 0, 0,
                                  8, vendor, product, 0x0100, 1, 2, 0, 1)
        body = b""
        for n, f in enumerate(interfaces):
            body += struct.pack("<BBBBBBBBB", 9, 4, n, 0, 1, 3, f.subclass,
                                f.protocol, 0)
            body += struct.pack("<BBHBBBH", 9, 0x21, 0x0111, 0, 1, 0x22,
                                len(f.report_desc))
            body += struct.pack("<BBBBHB", 7, 5, 0x80 | f.ep, 3, f.maxpacket,
                                f.interval)
        self.config = struct.pack("<BBHBBBBB", 9, 2, 9 + len(body),
                                  len(interfaces), 1, 0, 0x80, 50) + body
        self.requests = []           # (requesttype, request, value, index)

    def iface_infos(self):
        """(class, subclass, protocol) of every interface"""
        return [(3, f.subclass, f.protocol) for f in self.interfaces]

    def endpoints(self):
        """(address, type, interval, interface, max packet) of every
        endpoint but the control one"""
        return [(0x80 | f.ep, TYPE_INTERRUPT, f.interval, i, f.maxpacket)
                for i, f in enumerate(self.interfaces)]

    def since_reset(self):
        """The requests after the last reset: the firmware enumerates a
        keyboard before the kernel does, and resets it on the way"""
        last = max((i for i, r in enumerate(self.requests)
                    if r == ("reset",)), default=-1)
        return self.requests[last + 1:]

    def string(self, index):
        if index == 0:
            return struct.pack("<BBH", 4, 3, 0x0409)
        text = {1: "ThreadOS tests", 2: self.name}.get(index)
        if text is None:
            return None
        raw = text.encode("utf-16-le")
        return struct.pack("<BB", 2 + len(raw), 3) + raw

    def control(self, rtype, req, value, index, length, data):
        """(status, data) for a control transfer"""
        self.requests.append((rtype, req, value, index))
        if rtype == 0x80 and req == 6:                 # GET_DESCRIPTOR
            kind, i = value >> 8, value & 0xFF
            out = {1: self.device, 2: self.config}.get(kind)
            if kind == 3:
                out = self.string(i)
            return (SUCCESS, out[:length]) if out else (STALL, b"")
        if rtype == 0x81 and req == 6 and value >> 8 == 0x22:  # report desc
            if index < len(self.interfaces):
                return SUCCESS, self.interfaces[index].report_desc[:length]
            return STALL, b""
        if rtype == 0x80 and req == 0:                 # GET_STATUS
            return SUCCESS, b"\0\0"[:length]
        if rtype == 0x21 and req in (0x0A, 0x0B, 0x09):  # SET_IDLE/PROTOCOL/REPORT
            return SUCCESS, b""
        if rtype == 0xA1 and req == 0x01:              # GET_REPORT
            return SUCCESS, bytes(length)
        if rtype & 0x80 == 0:                          # other OUT requests
            return SUCCESS, b""
        return STALL, b""


class Redir:
    """One usbredir connection: QEMU connects to it as a chardev client.
    Start before QEMU, plug in on QEMU's connection."""

    def __init__(self, device):
        self.device = device
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.sock = None
        self.lock = threading.Lock()
        self.receiving = set()       # endpoints QEMU asked reports of
        self.next_id = 1
        self.alive = True
        self.ids64 = False           # once both hellos have said so
        self.configured = threading.Event()
        self.alts = {}               # interface -> the alternate in force
        self.iso_on = {}             # IN endpoint -> streaming
        if hasattr(device, "attach"):
            device.attach(self)
        threading.Thread(target=self.serve, daemon=True).start()

    def qemu_args(self, n, bus="xhci.0"):
        return ["-chardev", f"socket,id=redir{n},host=127.0.0.1,"
                f"port={self.port}",
                "-device", f"usb-redir,chardev=redir{n},bus={bus}"]

    # --- the wire ----------------------------------------------------

    def send(self, kind, header=b"", data=b"", pid=0):
        with self.lock:
            if self.sock:
                fmt = "<IIQ" if self.ids64 else "<III"
                self.sock.sendall(struct.pack(fmt, kind,
                                              len(header) + len(data), pid)
                                  + header + data)

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            part = self.sock.recv(n - len(buf))
            if not part:
                raise EOFError
            buf += part
        return buf

    def serve(self):
        try:
            self.sock, _ = self.srv.accept()
            caps = (1 << CAP_CONNECT_DEVICE_VERSION) | \
                   (1 << CAP_EP_INFO_MAX_PACKET_SIZE) | \
                   (1 << CAP_64BITS_IDS) | (1 << CAP_32BITS_BULK_LENGTH)
            self.send(HELLO, b"threados-test".ljust(64, b"\0") +
                      struct.pack("<I", caps))
            hello = False
            while self.alive:
                if self.ids64:
                    kind, length, pid = struct.unpack("<IIQ",
                                                      self.recv_exact(16))
                else:
                    kind, length, pid = struct.unpack("<III",
                                                      self.recv_exact(12))
                body = self.recv_exact(length) if length else b""
                if kind == HELLO and not hello:
                    hello = True
                    theirs = struct.unpack("<I", body[64:68])[0] \
                        if len(body) >= 68 else 0
                    # Both sides have offered 64-bit ids: every header
                    # after the two hellos carries one
                    self.ids64 = bool(theirs & (1 << CAP_64BITS_IDS))
                    self.plug_in()
                else:
                    self.packet(kind, pid, body)
        except (EOFError, OSError):
            pass

    def describe(self):
        """INTERFACE_INFO and EP_INFO: the interfaces, and the endpoints
        of the alternates in force"""
        d = self.device
        infos = d.iface_infos()
        n = len(infos)
        pad = lambda vals, size=32: bytes(vals) + bytes(size - len(vals))
        self.send(INTERFACE_INFO, struct.pack("<I", n) +
                  pad(range(n)) + pad([c for c, _, _ in infos]) +
                  pad([s for _, s, _ in infos]) +
                  pad([p for _, _, p in infos]))
        types, intervals, ifaces = [TYPE_INVALID] * 32, [0] * 32, [0] * 32
        sizes = [0] * 32
        ep0 = getattr(d, "ep0_size", 8)
        types[0] = types[16] = TYPE_CONTROL
        sizes[0] = sizes[16] = ep0
        eps = d.endpoints(dict(self.alts)) if getattr(d, "has_alts", False) \
            else d.endpoints()
        for addr, kind, interval, i, maxpacket in eps:
            e = ep_index(addr)
            types[e], intervals[e], ifaces[e], sizes[e] = \
                kind, interval, i, maxpacket
        self.send(EP_INFO, bytes(types) + bytes(intervals) + bytes(ifaces) +
                  struct.pack("<32H", *sizes))

    def plug_in(self):
        d = self.device
        self.describe()
        self.send(DEVICE_CONNECT, struct.pack("<BBBBHHH",
                                              getattr(d, "speed", SPEED_FULL),
                                              0, 0, 0, d.vendor, d.product,
                                              0x0100))

    def packet(self, kind, pid, body):
        if kind == CONTROL_PACKET:
            ep, req, rtype, _, value, index, length = \
                struct.unpack("<BBBBHHH", body[:10])
            status, data = self.device.control(rtype, req, value, index,
                                               length, body[10:])
            if rtype & 0x80:
                reply = struct.pack("<BBBBHHH", ep, req, rtype, status, value,
                                    index, len(data))
                self.send(CONTROL_PACKET, reply, data, pid)
            else:
                reply = struct.pack("<BBBBHHH", ep, req, rtype, status, value,
                                    index, length if status == SUCCESS else 0)
                self.send(CONTROL_PACKET, reply, b"", pid)
            # What the device says because of it - an HCI event after a
            # command - goes after the transfer's own answer, as on a
            # real bus
            if hasattr(self.device, "after_control"):
                self.device.after_control()
        elif kind == BULK_PACKET:
            ep, _, length, stream = struct.unpack("<BBHI", body[:8])
            if not ep & 0x80:
                # OUT: taken. IN is never answered - there is nothing to
                # read - and QEMU cancels it when the guest gives up.
                self.send(BULK_PACKET, struct.pack("<BBHIH", ep, SUCCESS,
                                                   length & 0xFFFF, stream,
                                                   length >> 16), pid=pid)
        elif kind == SET_CONFIGURATION:
            self.configured.set()
            self.send(CONFIGURATION_STATUS, struct.pack("<BB", SUCCESS,
                                                        body[0]), pid=pid)
        elif kind == GET_CONFIGURATION:
            self.send(CONFIGURATION_STATUS,
                      struct.pack("<BB", SUCCESS,
                                  1 if self.configured.is_set() else 0),
                      pid=pid)
        elif kind == SET_ALT_SETTING:
            iface, alt = body[0], body[1]
            self.alts[iface] = alt
            # The endpoints change with the alternate: told before the
            # answer, as the protocol says
            if getattr(self.device, "has_alts", False):
                self.device.requests.append(("set_alt", iface, alt))
                self.describe()
            self.send(ALT_SETTING_STATUS, struct.pack("<BBB", SUCCESS,
                                                      iface, alt), pid=pid)
        elif kind == GET_ALT_SETTING:
            self.send(ALT_SETTING_STATUS,
                      struct.pack("<BBB", SUCCESS, body[0],
                                  self.alts.get(body[0], 0)), pid=pid)
        elif kind == START_ISO_STREAM:
            ep = body[0]
            self.send(ISO_STREAM_STATUS, struct.pack("<BB", SUCCESS, ep),
                      pid=pid)
            if hasattr(self.device, "iso_packet") and not self.iso_on.get(ep):
                self.iso_on[ep] = True
                self.device.requests.append(("start_iso", ep))
                threading.Thread(target=self.iso_sender, args=(ep,),
                                 daemon=True).start()
        elif kind == STOP_ISO_STREAM:
            ep = body[0]
            if self.iso_on.get(ep):
                self.device.requests.append(("stop_iso", ep))
            self.iso_on[ep] = False
            self.send(ISO_STREAM_STATUS, struct.pack("<BB", SUCCESS, ep),
                      pid=pid)
        elif kind == START_INTERRUPT:
            self.receiving.add(body[0])
            self.send(INTERRUPT_STATUS, struct.pack("<BB", SUCCESS, body[0]),
                      pid=pid)
        elif kind == STOP_INTERRUPT:
            self.receiving.discard(body[0])
            self.send(INTERRUPT_STATUS, struct.pack("<BB", SUCCESS, body[0]),
                      pid=pid)
        elif kind == RESET:
            # (a mark for the test: what came after the last reset is
            # what the system that reset the device asked)
            self.device.requests.append(("reset",))
        # CANCEL_DATA needs no answer

    def iso_sender(self, ep):
        """One packet an interval, sent ten milliseconds' worth at a
        time, paced by the clock rather than by how fast QEMU takes
        them: QEMU keeps about sixty milliseconds and throws away what
        comes on top"""
        rate = self.device.iso_rate(ep)
        start = time.monotonic()
        sent = 0
        while self.alive and self.iso_on.get(ep):
            due = int((time.monotonic() - start) * rate)
            while sent < due and self.iso_on.get(ep):
                data = self.device.iso_packet(ep)
                with self.lock:
                    pid = self.next_id
                    self.next_id += 1
                self.send(ISO_PACKET, struct.pack("<BBH", ep, SUCCESS,
                                                  len(data)), data, pid)
                sent += 1
            time.sleep(0.01)

    # --- for the test ------------------------------------------------

    def reading(self, ep):
        """Has the guest started reading this IN endpoint?"""
        return (0x80 | ep) in self.receiving

    def report(self, ep, data):
        """An interrupt report from IN endpoint ep, as the device would
        send it the moment something changed"""
        with self.lock:
            pid = self.next_id
            self.next_id += 1
        self.send(INTERRUPT_PACKET, struct.pack("<BBH", 0x80 | ep, SUCCESS,
                                                len(data)), bytes(data), pid)

    def press(self, ep, down, up, gap=0.15):
        self.report(ep, down)
        time.sleep(gap)
        self.report(ep, up)
        time.sleep(gap)

    def close(self):
        self.alive = False
        for s in (self.sock, self.srv):
            try:
                if s:
                    s.close()
            except OSError:
                pass


# --- report descriptors, item by item (HID 1.11, 6.2.2) ------------------

def item(tag, kind, value=None, size=None):
    """One short item: tag and type (0 main, 1 global, 2 local)"""
    if value is None:
        return bytes([tag << 4 | kind << 2])
    if size is None:
        size = 1 if -128 <= value <= 255 else 2 if -32768 <= value <= 65535 \
            else 4
    code = {1: 1, 2: 2, 4: 3}[size]
    return bytes([tag << 4 | kind << 2 | code]) + \
        (value & (1 << 8 * size) - 1).to_bytes(size, "little")


def usage_page(p): return item(0, 1, p)
def logical_min(v): return item(1, 1, v)
def logical_max(v, size=None): return item(2, 1, v, size)
def report_size(v): return item(7, 1, v)
def report_id(v): return item(8, 1, v)
def report_count(v): return item(9, 1, v)
def usage(u): return item(0, 2, u)
def usage_min(u): return item(1, 2, u)
def usage_max(u): return item(2, 2, u)
def collection(kind): return item(10, 0, kind)
def end_collection(): return item(12, 0)
def input_(flags): return item(8, 0, flags)
def output(flags): return item(9, 0, flags)


BOOT_KEYBOARD = (       # HID 1.11, appendix B.1
    usage_page(0x01) + usage(0x06) + collection(1) +
    usage_page(0x07) + usage_min(0xE0) + usage_max(0xE7) + logical_min(0) +
    logical_max(1) + report_size(1) + report_count(8) + input_(0x02) +
    report_count(1) + report_size(8) + input_(0x01) +
    report_count(5) + report_size(1) + usage_page(0x08) + usage_min(1) +
    usage_max(5) + output(0x02) + report_count(1) + report_size(3) +
    output(0x01) +
    report_count(6) + report_size(8) + logical_min(0) + logical_max(101) +
    usage_page(0x07) + usage_min(0) + usage_max(101) + input_(0x00) +
    end_collection())
