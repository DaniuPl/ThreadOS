"""A Bluetooth adapter made up by a test: an HCI controller on USB.

QEMU has had no Bluetooth device for years, so this is one, plugged in
through usbredir (scripts/usbredir.py): the USB face every adapter has
(class 0xE0/0x01/0x01, commands as class requests on endpoint 0, events
on an interrupt endpoint, ACL data on two bulk ones), and behind it the
part of a controller that finds devices - written from the Core
specification (version 5.4, volume 4, part E), not from the driver.

Around it, two neighbours:

  a phone - classic Bluetooth, found by an inquiry. It gives its name in
  an extended inquiry result when the host asked for those (Write
  Inquiry Mode 2), and otherwise only to a Remote Name Request

  a watch - Bluetooth Low Energy, advertising with no name; the name is
  in its scan response, which only an ACTIVE scan asks for

and a controller as strict as a real one about what it says: an event
the host's event mask leaves out is not sent. That is the mistake that
hid every LE device from the first version of the driver - bit 61, the
LE meta event, is off after a reset.

What the host sent is kept in 'commands', for the test to look at.
"""

import os
import random
import struct
import threading
import time

from usbredir import SUCCESS, STALL, TYPE_INTERRUPT, TYPE_BULK

EVENTS_EP, ACL_OUT, ACL_IN = 1, 2, 2
EVENTS_MAXPACKET = 16          # small, as on many adapters: events in pieces

# Opcodes
RESET, SET_EVENT_MASK, READ_LOCAL_NAME = 0x0C03, 0x0C01, 0x0C14
WRITE_INQUIRY_MODE, WRITE_LE_HOST = 0x0C45, 0x0C6D
READ_VERSION, READ_BDADDR = 0x1001, 0x1009
INQUIRY, INQUIRY_CANCEL, REMOTE_NAME = 0x0401, 0x0402, 0x0419
LE_SCAN_PARAMS, LE_SCAN_ENABLE = 0x200B, 0x200C
# Connections and Secure Simple Pairing (7.1, 7.3.59)
CREATE_CONN, DISCONNECT, AUTH_REQUESTED = 0x0405, 0x0406, 0x0411
LINK_KEY_REPLY, LINK_KEY_NEG = 0x040B, 0x040C
IO_CAP_REPLY, USER_CONFIRM, USER_CONFIRM_NEG = 0x042B, 0x042C, 0x042D
SET_ENCRYPTION, WRITE_SSP_MODE = 0x0413, 0x0C56
HANDLE = 0x0042
LE_CREATE_CONN, LE_START_ENC = 0x200D, 0x2019
LE_HANDLE = 0x0044

# The event mask after a reset (7.3.1): bits 0-44, and not the LE one
DEFAULT_MASK = 0x00001FFFFFFFFFFF


def addr_bytes(text):
    """'58:24:29:a1:b2:c3' as it travels: lowest byte first"""
    return bytes(int(x, 16) for x in reversed(text.split(":")))


def ad(kind, data):
    return bytes([len(data) + 1, kind]) + data


class Phone:
    addr = "58:24:29:a1:b2:c3"
    name = "Pixel 8"
    cod = 0x5A020C               # a smartphone, offering telephony and more
    rssi = -48


class Watch:
    addr = "c0:ff:ee:00:11:22"   # a random static address
    name = "ThreadOS Watch"
    rssi = -61
    io_cap = 0x03                # no input, no output: Just Works


class Keyboard:
    addr = "d0:0d:fe:ed:00:33"   # random static too
    name = "ThreadOS Keyboard"
    rssi = -55
    io_cap = 0x02                # keyboard only: it types the digits shown


class BtAdapter:
    vendor, product = 0x1209, 0x0B70
    address = "00:1b:dc:0f:4e:21"
    local_name = "ThreadOS test adapter"

    def __init__(self, eir=True):
        # eir=False: an adapter from before extended inquiry results
        # (Bluetooth 2.0), which refuses inquiry mode 2 - the phone's
        # name then has to be asked for
        self.eir = eir
        self.device = struct.pack("<BBHBBBBHHHBBBB", 18, 1, 0x0200,
                                  0xE0, 0x01, 0x01, 64, self.vendor,
                                  self.product, 0x0100, 1, 2, 0, 1)
        body = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 3, 0xE0, 0x01, 0x01, 0)
        body += struct.pack("<BBBBHB", 7, 5, 0x80 | EVENTS_EP, 3,
                            EVENTS_MAXPACKET, 1)
        body += struct.pack("<BBBBHB", 7, 5, ACL_OUT, 2, 64, 0)
        body += struct.pack("<BBBBHB", 7, 5, 0x80 | ACL_IN, 2, 64, 0)
        self.config = struct.pack("<BBHBBBBB", 9, 2, 9 + len(body), 1, 1,
                                  0, 0x80, 50) + body
        self.requests = []
        self.commands = []           # (opcode, parameters), in order
        self.pending = []            # events to send after the transfer
        self.redir = None
        self.lock = threading.Lock()
        self.phone, self.watch, self.keyboard = Phone(), Watch(), Keyboard()
        self.le_keys = {}            # LE device address -> (ltk, ediv, rand)
        self.typed = None            # the digits a person typed on the keyboard
        self.smp_log = []            # what the host sent over SMP
        self.reset()

    # --- the USB face ----------------------------------------------------

    def attach(self, redir):
        self.redir = redir

    def iface_infos(self):
        return [(0xE0, 0x01, 0x01)]

    def endpoints(self):
        return [(0x80 | EVENTS_EP, TYPE_INTERRUPT, 1, 0, EVENTS_MAXPACKET),
                (ACL_OUT, TYPE_BULK, 0, 0, 64),
                (0x80 | ACL_IN, TYPE_BULK, 0, 0, 64)]

    def string(self, index):
        if index == 0:
            return struct.pack("<BBH", 4, 3, 0x0409)
        text = {1: "ThreadOS tests", 2: "Bluetooth adapter"}.get(index)
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
        if rtype == 0x80 and req == 0:
            return SUCCESS, b"\0\0"[:length]
        if rtype == 0x20 and len(data) >= 3:           # an HCI command
            op, plen = struct.unpack("<HB", data[:3])
            self.command(op, bytes(data[3:3 + plen]))
            return SUCCESS, b""
        if rtype & 0x80 == 0:
            return SUCCESS, b""
        return STALL, b""

    def after_control(self):
        with self.lock:
            out, self.pending = self.pending, []
        for ev in out:
            self.send_event(ev)

    def send_event(self, ev):
        """One event, in pieces of the endpoint's size"""
        if not self.redir:
            return
        try:
            for at in range(0, len(ev), EVENTS_MAXPACKET):
                self.redir.report(EVENTS_EP, ev[at:at + EVENTS_MAXPACKET])
        except OSError:
            pass                # a late event after the machine went away

    # --- the controller --------------------------------------------------

    def reset(self):
        self.mask = DEFAULT_MASK
        self.le_mask = 0x1F
        self.inquiry_mode = 0
        self.scan_active = False
        self.scanning = False
        self.inquiring = False
        self.ssp = False
        self.connected = False

    # What the phone's controller and ours agreed, in a pairing: the six
    # digits both screens show and the link key - the controllers' own
    # (elliptic curve, f1..f3 of 7.7.7), made up here
    number = None
    link_key = None             # kept across resets: the phone keeps it too
    pairings = 0

    def allowed(self, bit):
        return bool(self.mask >> bit & 1)

    def event(self, code, params, later=None):
        ev = bytes([code, len(params)]) + params
        if later is None:
            with self.lock:
                self.pending.append(ev)
        else:
            threading.Timer(later, self.send_event, (ev,)).start()

    def complete(self, op, params=b"\0"):
        self.event(0x0E, struct.pack("<BH", 1, op) + params)

    def status(self, op, st=0):
        self.event(0x0F, struct.pack("<BBH", st, 1, op))

    def command(self, op, p):
        self.commands.append((op, p))
        if op == RESET:
            self.reset()
            self.complete(op)
        elif op == SET_EVENT_MASK and len(p) == 8:
            self.mask = struct.unpack("<Q", p)[0]
            self.complete(op)
        elif op == READ_BDADDR:
            self.complete(op, b"\0" + addr_bytes(self.address))
        elif op == READ_LOCAL_NAME:
            name = self.local_name.encode()
            self.complete(op, b"\0" + name + bytes(248 - len(name)))
        elif op == READ_VERSION:
            # HCI 4.0 (6), revision, LMP 4.0, maker 0x0002 (Intel), subversion
            self.complete(op, struct.pack("<BBHBHH", 0, 6, 0x1000, 6, 2,
                                          0x0100))
        elif op == WRITE_INQUIRY_MODE and len(p) == 1 and \
                p[0] <= (2 if self.eir else 1):
            self.inquiry_mode = p[0]
            self.complete(op)
        elif op == WRITE_LE_HOST:
            self.complete(op)
        elif op == LE_SCAN_PARAMS and len(p) == 7:
            self.scan_active = p[0] == 1
            self.complete(op)
        elif op == LE_SCAN_ENABLE and len(p) == 2:
            self.scanning = p[0] == 1
            self.complete(op)
            if self.scanning:
                self.advertise()
        elif op == INQUIRY and len(p) == 5:
            self.status(op)
            self.inquiring = True
            self.inquire(p[3] * 1.28)
        elif op == INQUIRY_CANCEL:
            self.inquiring = False
            self.complete(op)
        elif op == REMOTE_NAME and len(p) == 10:
            self.status(op)
            if p[:6] == addr_bytes(self.phone.addr) and self.allowed(7):
                name = self.phone.name.encode()
                self.event(0x07, b"\0" + p[:6] + name +
                           bytes(248 - len(name)), later=0.4)
        elif op == WRITE_SSP_MODE and len(p) == 1:
            self.ssp = p[0] == 1
            self.complete(op)
        elif op in (LE_CREATE_CONN, LE_START_ENC):
            self.le(op, p)
        elif op in (CREATE_CONN, AUTH_REQUESTED, LINK_KEY_REPLY, LINK_KEY_NEG,
                    IO_CAP_REPLY, USER_CONFIRM, USER_CONFIRM_NEG,
                    SET_ENCRYPTION, DISCONNECT):
            self.pairing(op, p)
        elif op == WRITE_INQUIRY_MODE:
            self.complete(op, b"\x12")        # invalid parameters
        else:
            self.complete(op, b"\x01")                 # unknown command

    def inquire(self, seconds):
        """The phone answers a little after the inquiry starts, in the
        form the inquiry mode says - if the mask lets that form through"""
        ph = self.phone
        a, cod = addr_bytes(ph.addr), struct.pack("<I", ph.cod)[:3]
        clock = struct.pack("<H", 0x1234)
        mode = self.inquiry_mode
        if mode == 2 and self.allowed(46):
            eir = ad(0x09, ph.name.encode()) + ad(0x01, b"\x06")
            self.event(0x2F, b"\x01" + a + b"\x01\x00" + cod + clock +
                       struct.pack("<b", ph.rssi) + eir + bytes(240 - len(eir)),
                       later=0.5)
        elif mode == 1 and self.allowed(33):
            self.event(0x22, b"\x01" + a + b"\x01\x00" + cod + clock +
                       struct.pack("<b", ph.rssi), later=0.5)
        elif self.allowed(1):
            self.event(0x02, b"\x01" + a + b"\x01\x00\x00" + cod + clock,
                       later=0.5)
        if self.allowed(0):
            # (a test's inquiry need not take the whole of its length)
            self.event(0x01, b"\0", later=min(seconds, 2.0))

    def advertise(self):
        """The watch advertises with no name; its scan response has it.
        Nothing reaches the host without the LE meta event in its mask."""
        if not (self.allowed(61) and self.le_mask >> 1 & 1):
            return
        for k, w in enumerate((self.watch, self.keyboard)):
            a = addr_bytes(w.addr)
            adv = ad(0x01, b"\x06") + ad(0xFF, b"\x4c\x00\x10\x05")
            report = lambda kind, data, a=a, w=w: (
                b"\x02\x01" + bytes([kind, 1]) + a + bytes([len(data)]) +
                data + struct.pack("<b", w.rssi))
            self.event(0x3E, report(0x00, adv), later=0.3 + 0.1 * k)
            if self.scan_active:
                self.event(0x3E, report(0x04, ad(0x09, w.name.encode())),
                           later=0.35 + 0.1 * k)

    def pairing(self, op, p):
        """The phone's side of a connection and of Secure Simple Pairing,
        as the controller tells its host: every event behind the mask bit
        the specification gives it (bit = event code - 1)"""
        ph = addr_bytes(self.phone.addr)
        h = struct.pack("<H", HANDLE)
        if op == CREATE_CONN:
            if len(p) != 13:
                return self.status(op, 0x12)
            self.status(op)
            if p[:6] == ph and self.allowed(2):
                self.connected = True
                self.event(0x03, b"\0" + h + ph + b"\x01\x00", later=0.3)
        elif op == AUTH_REQUESTED:
            self.status(op, 0 if len(p) == 2 and p == h else 0x02)
            if p == h and self.allowed(22):
                self.event(0x17, ph, later=0.1)          # Link Key Request
        elif op == LINK_KEY_REPLY and len(p) == 22:
            self.complete(op, b"\0" + p[:6])
            ok = self.link_key is not None and p[6:] == self.link_key
            if self.allowed(5):
                self.event(0x06, (b"\0" if ok else b"\x06") + h, later=0.1)
        elif op == LINK_KEY_NEG and len(p) == 6:
            self.complete(op, b"\0" + p)
            if self.ssp and self.allowed(48):          # IO Capability Request
                self.event(0x31, ph, later=0.1)
        elif op == IO_CAP_REPLY and len(p) == 9:
            self.complete(op, b"\0" + p[:6])
            self.number = random.randint(0, 999999)
            if self.allowed(49):     # the phone: a display and yes/no, MITM
                self.event(0x32, ph + b"\x01\x00\x03", later=0.1)
            if self.allowed(50):     # User Confirmation Request
                self.event(0x33, ph + struct.pack("<I", self.number),
                           later=0.2)
        elif op == USER_CONFIRM and len(p) == 6:
            self.complete(op, b"\0" + p)
            self.link_key = os.urandom(16)
            self.pairings += 1
            if self.allowed(53):
                self.event(0x36, b"\0" + ph, later=0.1)
            if self.allowed(23):     # authenticated, P-192
                self.event(0x18, ph + self.link_key + b"\x05", later=0.15)
            if self.allowed(5):
                self.event(0x06, b"\0" + h, later=0.2)
        elif op == USER_CONFIRM_NEG and len(p) == 6:
            self.complete(op, b"\0" + p)
            if self.allowed(53):
                self.event(0x36, b"\x05" + ph, later=0.1)
            if self.allowed(5):
                self.event(0x06, b"\x05" + h, later=0.2)
        elif op == SET_ENCRYPTION and len(p) == 3:
            self.status(op)
            if self.allowed(7):
                self.event(0x08, b"\0" + h + bytes([p[2]]), later=0.1)
        elif op == DISCONNECT and len(p) == 3:
            self.status(op)
            self.connected = False
            if self.allowed(4):
                self.event(0x05, b"\0" + p[:2] + b"\x16", later=0.1)
        else:
            self.complete(op, b"\x12")

    # --- LE: a connection, and the device's side of legacy pairing ----------
    #
    # The Security Manager Protocol (Core 5.4 vol 3 part H) as a watch or
    # a keyboard runs it, written here from the specification with
    # OpenSSL's AES - not from the driver: c1 and s1 (2.2.3, 2.2.4), the
    # confirm values checked, the short term key, and over the encrypted
    # link the long term key and identity handed over.

    @staticmethod
    def e(k, p):
        from cryptography.hazmat.primitives.ciphers import (Cipher,
                                                            algorithms, modes)
        enc = Cipher(algorithms.AES(k[::-1]), modes.ECB()).encryptor()
        return enc.update(p[::-1])[::-1]       # little-endian in and out

    def c1(self, k, r, preq, pres, iat, rat, ia, ra):
        p1 = bytes([iat, rat]) + preq + pres
        p2 = ra + ia + bytes(4)
        x = self.e(k, bytes(a ^ b for a, b in zip(r, p1)))
        return self.e(k, bytes(a ^ b for a, b in zip(x, p2)))

    def s1(self, k, r1, r2):
        return self.e(k, r2[:8] + r1[:8])

    def le_device(self, addr):
        for d in (self.watch, self.keyboard):
            if addr_bytes(d.addr) == addr:
                return d
        return None

    def le(self, op, p):
        if op == LE_CREATE_CONN:
            if len(p) != 25:
                return self.status(op, 0x12)
            self.status(op)
            d = self.le_device(p[6:12])
            if d and self.allowed(61) and self.le_mask & 1:
                self.peer = d
                self.smp = {"tk": bytes(16)}
                self.event(0x3E, b"\x01\x00" + struct.pack("<H", LE_HANDLE) +
                           b"\x00\x01" + p[6:12] +
                           struct.pack("<HHHB", 0x28, 0, 0x1F4, 0), later=0.3)
        elif op == LE_START_ENC and len(p) == 28:
            self.status(op)
            ltk, ediv, rand = p[12:28], p[10:12], p[2:10]
            d = getattr(self, "peer", None)
            ok = d is not None and (
                ltk == self.smp.get("stk") or
                self.le_keys.get(d.addr) == (ltk, ediv, rand))
            if self.allowed(7):
                self.event(0x08, (b"\0" if ok else b"\x06") +
                           struct.pack("<H", LE_HANDLE) +
                           (b"\x01" if ok else b"\x00"), later=0.1)
            if ok and ltk == self.smp.get("stk"):
                # paired: the keys, over the encrypted link
                k = (os.urandom(16), os.urandom(2), os.urandom(8))
                self.le_keys[d.addr] = k
                self.pairings += 1
                self.smp_send(b"\x06" + k[0], 0.2)
                self.smp_send(b"\x07" + k[1] + k[2], 0.25)
                self.smp_send(b"\x08" + os.urandom(16), 0.3)
                self.smp_send(b"\x09\x01" + addr_bytes(d.addr), 0.35)

    def bulk_out(self, ep, data):
        """ACL data from the host: an L2CAP frame in one piece"""
        if len(data) < 8:
            return
        handle, n = struct.unpack("<HH", data[:4])
        ln, cid = struct.unpack("<HH", data[4:8])
        if handle & 0x0FFF == LE_HANDLE and cid == 6:
            self.smp_rx(bytes(data[8:8 + ln]))

    def smp_send(self, msg, later=0.05):
        pkt = struct.pack("<HH", LE_HANDLE | 0x2000, 4 + len(msg)) + \
            struct.pack("<HH", len(msg), 6) + msg
        threading.Timer(later, lambda: self.redir and
                        self.redir.bulk_in(ACL_IN, pkt)).start()

    def smp_rx(self, m):
        self.smp_log.append(m)
        d, st = self.peer, self.smp
        ia, ra = addr_bytes(self.address), addr_bytes(d.addr)
        if m[0] == 0x01 and len(m) == 7:         # Pairing Request
            st["preq"] = m
            st["pres"] = bytes([0x02, d.io_cap, 0, 0x01, 16, 0,
                                m[6] & 0x03])
            self.smp_send(st["pres"])
        elif m[0] == 0x03 and len(m) == 17:      # the host's confirm
            st["mconfirm"] = m[1:]
            threading.Thread(target=self.smp_confirm, daemon=True).start()
        elif m[0] == 0x04 and len(m) == 17:      # the host's random
            mrand = m[1:]
            want = self.c1(st["tk"], mrand, st["preq"], st["pres"], 0, 1,
                           ia, ra)
            if want != st["mconfirm"]:
                self.smp_send(b"\x05\x04")      # confirm value failed
                return
            self.smp_send(b"\x04" + st["srand"])
            st["stk"] = self.s1(st["tk"], st["srand"], mrand)

    def smp_confirm(self):
        """The device's confirm: at once, or - a keyboard - once the
        person has typed the digits shown on the host"""
        d, st = self.peer, self.smp
        if d.io_cap == 0x02:
            end = time.time() + 60
            while self.typed is None and time.time() < end:
                time.sleep(0.1)
            if self.typed is None:
                return
            st["tk"] = struct.pack("<I", self.typed) + bytes(12)
        st["srand"] = os.urandom(16)
        self.smp_send(b"\x03" + self.c1(st["tk"], st["srand"], st["preq"],
                                         st["pres"], 0, 1,
                                         addr_bytes(self.address),
                                         addr_bytes(d.addr)))

    # --- for the test ------------------------------------------------------

    def sent(self, op):
        """The parameters of every command of this opcode the host sent"""
        return [p for o, p in self.commands if o == op]
