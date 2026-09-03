"""The shared part of the ProtonOS smoke test.

Everything needed to start the machine and check something on it: the
Qemu class, the HTTP and DNS servers for the network tests, certificate
generation and the check function itself.

The checks themselves live in scripts/tests/ - every group in its own
file, because every one needs its own machine, and those start in
parallel.
"""

import http.server
import json
import os
import re
import shutil
import socket
import socketserver
import subprocess
import threading
import sys
import tempfile
import time

KERNEL = sys.argv[1] if len(sys.argv) > 1 else "build/protonos.bin"
INITRD = sys.argv[2] if len(sys.argv) > 2 else None
KERNEL64 = sys.argv[3] if len(sys.argv) > 3 else None
INITRD64 = sys.argv[4] if len(sys.argv) > 4 else None
VGA_ADDR = 0xB8000
VGA_BYTES = 80 * 25 * 2
VIDEO_W = 320
VIDEO_H = 200

# The geometry of the desktop menu (kernel/desktop.c): the entries grow
# upwards from the taskbar, so every new program shifts the previous
# ones. We keep it in one place, so that adding an application does not
# break the click tests.
# The system starts straight into the desktop; the console tests force
# text mode with a kernel command line.
TEXT_MODE = "nodesktop"

# The disk image under PFS3 (the kernel formats an empty disk on the
# first start)
DISK_SIZE = 16 * 1024 * 1024

# The guest's clock: fixed, but later than the issue date of the
# certificates generated during the test
RTC_BASE = "2030-06-13T12:34:56"
RTC_DATE = "2030-06-13"

MENU_ITEMS = ["guiapp", "gclock", "gfiles", "gweb", "gedit", "gtasks"]
MENU_ROW = 16
MENU_TASKBAR_Y = 462   # video_h() - TB_H (kernel/desktop.c)


def menu_y(name):
    """The centre of the menu entry with the given name (screen
    coordinates 640x480)."""
    idx = MENU_ITEMS.index(name)
    return MENU_TASKBAR_Y - (len(MENU_ITEMS) - idx) * MENU_ROW + MENU_ROW // 2


PAGE = b"<html>ProtonOS TCP dziala</html>\n"
# A page with a link and enough text that it has to be scrolled
LINK_PAGE = (b"<html><body><h1>Strona pierwsza</h1>"
             + b"<p>Wiersz wypelniajacy strone numer jeden.</p>" * 20
             + b'<p>Przejdz do <a href="/druga">drugiej strony</a>.</p>'
             + b"</body></html>\n")
LINK_PAGE2 = (b"<html><body><h1>Strona druga</h1>"
              b"<p>Odnosnik zadzialal.</p></body></html>\n")
# Above the old ceiling of 268 KiB per file - it reaches into the
# doubly indirect block
HUGE_PAGE = b"<p>ProtonOS PFS3: blok posredni, potem podwojnie posredni.</p>\n" * 6500
# A page well above the old PFS limit (16 KiB per file) - it checks
# block allocation, the indirect block and TCP throughput
BIG_PAGE = b"<p>ProtonOS PFS3: blok po bloku.</p>\n" * 3000


def make_bmp(w=64, h=48):
    """A 24-bit BMP with four quarters: red, green, blue and white.
    Those four are in the screen's palette exactly, so a picture drawn
    from this file has to come out in exactly those colours - which is
    what tells a working decoder from a decoder that draws something."""
    stride = (w * 3 + 3) & ~3
    pixels = bytearray()
    for y in range(h):                      # BMP rows run bottom-up
        row = bytearray()
        for x in range(w):
            top = (h - 1 - y) < h // 2
            left = x < w // 2
            if top and left:
                bgr = (0, 0, 255)           # red, as BGR
            elif top:
                bgr = (0, 255, 0)           # green
            elif left:
                bgr = (255, 0, 0)           # blue
            else:
                bgr = (255, 255, 255)       # white
            row += bytes(bgr)
        row += b"\0" * (stride - len(row))
        pixels += row

    dib = bytes([40, 0, 0, 0]) + w.to_bytes(4, "little") \
        + h.to_bytes(4, "little") + (1).to_bytes(2, "little") \
        + (24).to_bytes(2, "little") + (0).to_bytes(4, "little") \
        + len(pixels).to_bytes(4, "little") + b"\0" * 16
    head = b"BM" + (14 + len(dib) + len(pixels)).to_bytes(4, "little") \
        + b"\0\0\0\0" + (14 + len(dib)).to_bytes(4, "little")
    return head + dib + bytes(pixels)


BMP_IMAGE = make_bmp()


_FONT8X8 = None


def font8x8():
    """The console font read straight from the kernel's header: a
    pattern of eight bytes -> a character. A single source of truth -
    when the font changes, the tests read the new one."""
    global _FONT8X8
    if _FONT8X8 is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "kernel", "include", "font8x8.h")
        with open(path) as fh:
            src = fh.read()
        rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},){7}0x[0-9A-Fa-f]{2})\}",
                             src)
        _FONT8X8 = {}
        for i, w in enumerate(rows):
            pattern = tuple(int(v, 16) for v in w.split(","))
            _FONT8X8.setdefault(pattern, chr(0x20 + i))
    return _FONT8X8


def start_dns_server(answer_ip=(10, 0, 2, 2)):
    """A very simple DNS server on the host: to every question about an A
    record it answers with the same address. Thanks to that the path
    'name -> address' can be checked with no network outside at all."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", 53))
    stop = threading.Event()

    def serve():
        sock.settimeout(0.5)
        while not stop.is_set():
            try:
                data, addr = sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 12:
                continue
            # a question ends with a zero name byte + QTYPE + QCLASS
            i = 12
            while i < len(data) and data[i]:
                i += data[i] + 1
            qend = i + 5
            header = data[:2] + b"\x81\x80" + data[4:6] + b"\x00\x01" \
                     + b"\x00\x00\x00\x00"
            answer = (b"\xc0\x0c"            # a pointer to the name from the question
                      + b"\x00\x01\x00\x01"  # type A, class IN
                      + b"\x00\x00\x00\x3c"  # TTL 60 s
                      + b"\x00\x04" + bytes(answer_ip))
            sock.sendto(header + data[12:qend] + answer, addr)
        sock.close()

    threading.Thread(target=serve, daemon=True).start()
    return stop.set


CERT_DER = None          # the HTTPS server's certificate (for the trust store)
CERT_DER_EC = None       # the same, but on the P-256 curve
CERT_DER_P384 = None     # and on the P-384 curve (a SHA-384 signature)


def make_test_cert(dirpath, ec=False, p384=False):
    """A self-signed certificate for protonos.test: a SubjectAltName with
    the name and a wildcard, the CA flag (so it can serve as a trust
    root too) and a ten-year validity, so that it covers the guest's
    fixed clock. We do it with the openssl tool - it is everywhere TLS
    is tested."""
    import subprocess

    tag = "p384" if p384 else ("ec" if ec else "rsa")
    certfile = os.path.join(dirpath, "cert-%s.pem" % tag)
    keyfile = os.path.join(dirpath, "key-%s.pem" % tag)
    derfile = os.path.join(dirpath, "cert-%s.der" % tag)

    if p384:
        keyargs = ["-newkey", "ec", "-pkeyopt",
                   "ec_paramgen_curve:secp384r1", "-sha384"]
    elif ec:
        keyargs = ["-newkey", "ec", "-pkeyopt",
                   "ec_paramgen_curve:prime256v1"]
    else:
        keyargs = ["-newkey", "rsa:2048"]

    subprocess.run(
        ["openssl", "req", "-x509"] + keyargs + ["-keyout", keyfile,
         "-out", certfile, "-days", "3650", "-nodes",
         "-subj", "/CN=protonos.test",
         "-addext", "subjectAltName=DNS:protonos.test,DNS:*.protonos.test"],
        check=True, capture_output=True, timeout=120)
    subprocess.run(
        ["openssl", "x509", "-in", certfile, "-outform", "DER", "-out", derfile],
        check=True, capture_output=True, timeout=60)

    with open(derfile, "rb") as f:
        return certfile, keyfile, f.read()


def make_chain_certs(dirpath):
    """A real chain: root -> intermediate -> the server's certificate.
    We trust only the root, so ProtonOS has to walk the whole path of
    signatures rather than merely compare one certificate."""
    import subprocess

    def run(args, **kw):
        subprocess.run(args, check=True, capture_output=True, timeout=120, **kw)

    j = lambda name: os.path.join(dirpath, name)

    with open(j("ca.ext"), "w") as f:
        f.write("basicConstraints=critical,CA:TRUE\n"
                "keyUsage=critical,keyCertSign,cRLSign\n")
    with open(j("leaf.ext"), "w") as f:
        f.write("basicConstraints=critical,CA:FALSE\n"
                "subjectAltName=DNS:protonos.test\n")

    # the root
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("root.key"), "-out", j("root.pem"), "-days", "3650",
         "-subj", "/CN=ProtonOS Test Root",
         "-addext", "basicConstraints=critical,CA:TRUE"])

    # the intermediate signed by the root
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("int.key"), "-out", j("int.csr"),
         "-subj", "/CN=ProtonOS Test Intermediate"])
    run(["openssl", "x509", "-req", "-in", j("int.csr"),
         "-CA", j("root.pem"), "-CAkey", j("root.key"), "-CAcreateserial",
         "-out", j("int.pem"), "-days", "3650", "-extfile", j("ca.ext")])

    # the server's certificate signed by the intermediate
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("leaf.key"), "-out", j("leaf.csr"),
         "-subj", "/CN=protonos.test"])
    run(["openssl", "x509", "-req", "-in", j("leaf.csr"),
         "-CA", j("int.pem"), "-CAkey", j("int.key"), "-CAcreateserial",
         "-out", j("leaf.pem"), "-days", "3650", "-extfile", j("leaf.ext")])

    # the server sends its certificate together with the intermediate
    with open(j("chain.pem"), "w") as out:
        out.write(open(j("leaf.pem")).read())
        out.write(open(j("int.pem")).read())

    run(["openssl", "x509", "-in", j("root.pem"), "-outform", "DER",
         "-out", j("root.der")])
    with open(j("root.der"), "rb") as f:
        return j("chain.pem"), j("leaf.key"), f.read()


CERT_DER_ROOT = None     # the root of the chain (for the trust store)


def start_https_chain_server():
    """An HTTPS server presenting a two-level chain."""
    global CERT_DER_ROOT
    import ssl

    tmpdir = tempfile.mkdtemp(prefix="protonos-chain-")
    try:
        chainfile, keyfile, root_der = make_chain_certs(tmpdir)
    except BaseException:
        return None, lambda: None
    CERT_DER_ROOT = root_der

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        def log_message(self, *args):
            pass

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(chainfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv.shutdown


def start_https_server(ec=False, p384=False):
    """An HTTPS server (TLS 1.3) on the host with a certificate generated
    on the fly - with an RSA key or on the P-256 curve. Thanks to that
    the test needs nothing from outside.
    Returns (port, stop), or (None, stop) when openssl is missing."""
    global CERT_DER, CERT_DER_EC, CERT_DER_P384
    import ssl

    tmpdir = tempfile.mkdtemp(prefix="protonos-tls-")
    try:
        certfile, keyfile, der = make_test_cert(tmpdir, ec, p384)
    except BaseException:                # a missing openssl is no reason to
        return None, lambda: None        # bring the whole test down
    if p384:
        CERT_DER_P384 = der
    elif ec:
        CERT_DER_EC = der
    else:
        CERT_DER = der

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        def log_message(self, *args):
            pass

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv.shutdown


def start_http_server():
    """An HTTP server on the host - the guest reaches it through the slirp
    gateway (10.0.2.2), so the TCP test needs no network outside."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            if self.path.startswith("/stara"):      # a redirect
                self.send_response(302)
                self.send_header("Location", "/kawalki")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            if self.path.startswith("/kawalki"):    # chunked encoding
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for i in range(0, len(PAGE), 7):    # deliberately small pieces
                    part = PAGE[i:i + 7]
                    self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
                return

            if self.path.startswith("/ogromna"):
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(HUGE_PAGE)))
                self.end_headers()
                self.wfile.write(HUGE_PAGE)
                return

            if self.path.startswith("/linki") or self.path.startswith("/druga"):
                body = (LINK_PAGE2 if self.path.startswith("/druga")
                        else LINK_PAGE)
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path.endswith(".bmp"):
                self.send_response(200)
                self.send_header("Content-Type", "image/bmp")
                self.send_header("Content-Length", str(len(BMP_IMAGE)))
                self.end_headers()
                self.wfile.write(BMP_IMAGE)
                return

            if self.path.startswith("/rootca") and CERT_DER_ROOT:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER_ROOT)))
                self.end_headers()
                self.wfile.write(CERT_DER_ROOT)
                return

            if self.path.startswith("/p384cert") and CERT_DER_P384:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER_P384)))
                self.end_headers()
                self.wfile.write(CERT_DER_P384)
                return

            if self.path.startswith("/eccert") and CERT_DER_EC:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER_EC)))
                self.end_headers()
                self.wfile.write(CERT_DER_EC)
                return

            if self.path.endswith(".der") and CERT_DER:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER)))
                self.end_headers()
                self.wfile.write(CERT_DER)
                return

            body = BIG_PAGE if self.path.startswith("/duza") else PAGE
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv.shutdown


class Qemu:
    def __init__(self, kernel, initrd=None, disk=None, hostfwd_port=None,
                 append=None, ram_mib=None, sata=False, nvme=False,
                 usbkbd=False,
                 cdrom=None, serial=None, qemu="qemu-system-i386",
                 uefi=None, nic="rtl8139", cpus=None):
        self.qmp_path = tempfile.mktemp(suffix=".qmp")
        self.dump_path = tempfile.mktemp(suffix=".vga")
        # The 64-bit kernel needs a processor with long mode, and
        # qemu-system-i386 does not pretend to be one
        cmd = [qemu,
               "-display", "none", "-no-reboot",
               # A fixed RTC date -> a deterministic test of the 'date' command.
               # It has to be later than the moment the test certificates
               # were issued, otherwise TLS will rightly reject them as
               # "not yet valid".
               "-rtc", "base=" + RTC_BASE,
               # The network card in user mode: the gateway 10.0.2.2
               # answers ARP and ICMP echo, so 'ping' can be tested with
               # no network on the host at all
               "-netdev",
               "user,id=n0" + (f",hostfwd=tcp::{hostfwd_port}-:{hostfwd_port}"
                               if hostfwd_port else ""),
               # The network card: RTL8139 by default, but the system
               # knows e1000 too - and that is the only difference in the
               # QEMU invocation, because which card sits in the socket
               # is decided by the netdev layer, not by the network code
               "-device", nic + ",netdev=n0",
               "-qmp", f"unix:{self.qmp_path},server,nowait"]
        if ram_mib:
            cmd += ["-m", str(ram_mib)]
        if cpus:
            # More than one core. The firmware lists them in an ACPI
            # table, and the system wakes them with an INIT-SIPI signal
            cmd += ["-smp", str(cpus)]
        if usbkbd == "xhci":
            # An xHCI controller: the only one newer boards have. It is
            # the same keyboard, only on a different bus - and whether
            # the system sees it the same way is for the test to settle,
            # not for an assumption
            cmd += ["-device", "qemu-xhci,id=xhci",
                    "-device", "usb-kbd,bus=xhci.0"]
        elif usbkbd:
            # A USB keyboard instead of PS/2 - that is what every
            # machine newer than roughly 2010 looks like
            cmd += ["-usb", "-device", "usb-kbd"]
        self.serial_path = serial
        if serial:
            # When the console draws on the framebuffer, there is nothing
            # left to read in the VGA character buffer - we collect the
            # text from the serial port
            cmd += ["-serial", "file:" + serial]
        if uefi:
            # Instead of a BIOS, flash memory with UEFI firmware (OVMF):
            # the code separately (read-only) and separately the
            # variables the firmware writes - hence a copy of the
            # variables file for every run
            code, nvram = uefi
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,"
                              "file=" + code,
                    "-drive", "if=pflash,format=raw,unit=1,file=" + nvram]
        if cdrom:
            # Booting as from a real board: the kernel is loaded by GRUB, not by
# QEMU
            cmd += ["-cdrom", cdrom, "-boot", "d"]
        else:
            cmd += ["-kernel", kernel]
            if append:
                cmd += ["-append", append]
            if initrd:
                cmd += ["-initrd", initrd]
        if disk and nvme:
            # An NVMe disk: with no controller in between - the device
            # sits straight on PCI Express
            cmd += ["-drive", f"id=nvm0,file={disk},if=none,format=raw",
                    "-device", "nvme,drive=nvm0,serial=protonos"]
        elif disk and sata:
            # A disk behind an AHCI controller - that is what every newer board
# looks like
            cmd += ["-device", "ich9-ahci,id=ahci",
                    "-drive", f"id=sata0,file={disk},if=none,format=raw",
                    "-device", "ide-hd,drive=sata0,bus=ahci.0"]
        elif disk:
            cmd += ["-hda", disk]
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        # Wait until QEMU puts up the QMP socket (the start is sometimes slow)
        sock = socket.socket(socket.AF_UNIX)
        deadline = time.time() + 10
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU zakonczyl sie przedwczesnie:\n"
                                   + self.proc.stderr.read().decode())
            try:
                sock.connect(self.qmp_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        # A time limit on an answer. Without it a read from the socket
        # waits forever: when QEMU dies or hangs in the middle of a
        # command, the whole test stops for good instead of reporting an
        # error - and there is nobody to wake it from that.
        sock.settimeout(self.QMP_TIMEOUT)
        self.sock = sock
        self.f = sock.makefile("rwb")
        self.f.readline()  # the QMP banner
        self.cmd({"execute": "qmp_capabilities"})

    # The longest command is a screenshot at high resolution -
    # seconds are enough with enormous room to spare
    QMP_TIMEOUT = 60

    def cmd(self, msg):
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            try:
                raw = self.f.readline()
            except (socket.timeout, TimeoutError):
                raise RuntimeError(
                    f"QMP nie odpowiedzialo w {self.QMP_TIMEOUT} s na: {msg}")
            if not raw:                  # a closed socket = QEMU died
                raise RuntimeError(f"QEMU rozlaczyl sie podczas: {msg}")
            resp = json.loads(raw)
            if "return" in resp or "error" in resp:
                if "error" in resp:
                    raise RuntimeError(f"Blad QMP: {resp}")
                return resp

    def hmp(self, command):
        return self.cmd({"execute": "human-monitor-command",
                         "arguments": {"command-line": command}})

    def screen(self):
        self.hmp(f'pmemsave {VGA_ADDR} {VGA_BYTES} "{self.dump_path}"')
        raw = open(self.dump_path, "rb").read()
        lines = []
        for y in range(25):
            chars = raw[y * 160:(y + 1) * 160:2]
            lines.append(chars.decode("cp437", "replace").rstrip())
        return "\n".join(lines).rstrip()

    def wait_for(self, text, timeout=30):
        """Waits until the given text appears on the VGA screen.

        Thirty seconds, not fifteen: a full run starts four machines at
        once, and four kernels booting on one host take longer than one
        does. The wait is for a THING TO HAPPEN, so waiting longer
        weakens nothing - it only stops a busy host from being reported
        as a broken kernel."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            screen = self.screen()
            if text in screen:
                return screen
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' nie pojawilo sie na ekranie")

    DESK_BG = (0, 128, 160)          # the desktop's background (teal)

    def wait_desktop(self, timeout=25):
        """Waits until the compositor draws the desktop. In graphics mode
        the VGA text buffer (0xB8000) stops being a window onto what can
        be seen, so we look for readiness in pixels rather than in
        text."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                w, _, at = self.screen_raw()
                if w >= 640 and all(abs(at(600, 300)[i] - self.DESK_BG[i]) <= 40
                                    for i in range(3)):
                    return True
            except Exception:
                pass
            time.sleep(0.3)
        raise RuntimeError("timeout: pulpit sie nie pojawil")

    def sendkey(self, key):
        self.hmp(f"sendkey {key}")
        time.sleep(0.1)

    def mouse_move(self, dx, dy):
        """A PS/2 mouse carries only small deltas in one packet, so we
        split larger jumps into steps - otherwise the cursor lands
        somewhere other than where the test wanted."""
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
            dx -= sx
            dy -= sy
            time.sleep(0.05)
        time.sleep(0.15)

    def mouse_home(self):
        """Pushes the cursor into the top left corner (the compositor holds
        it within the screen's bounds) and zeroes the tracked position -
        a calibration before clicking on particular places."""
        self.mouse_move(-800, -600)
        self.mx, self.my = 0, 0

    def mouse_button(self, down, button="left"):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": button, "down": down}}]}})
        time.sleep(0.2)

    # Tracking the cursor's position (the desktop starts in the middle of
# 320x200)
    mx, my = 160, 100

    def mouse_goto(self, tx, ty):
        self.mouse_move(tx - self.mx, ty - self.my)
        self.mx, self.my = tx, ty

    def mouse_click(self):
        self.mouse_button(True)
        self.mouse_button(False)

    def mouse_right_click(self):
        self.mouse_button(True, "right")
        self.mouse_button(False, "right")

    def screen_pixels(self):
        """A screenshot (QMP screendump) -> (scale_x, scale_y, a pixel
        function in framebuffer coordinates 320x200). QEMU doubles mode
        13h when displaying it, hence the scaling."""
        ppm = tempfile.mktemp(suffix=".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        idx, fields = 2, []
        while len(fields) < 3:
            while data[idx] in b" \t\n\r":
                idx += 1
            s = idx
            while data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[s:idx]))
        idx += 1
        w, h, _ = fields
        px = data[idx:]
        sx, sy = w // VIDEO_W, h // VIDEO_H

        def at(fx, fy):
            x, y = fx * sx + 1, fy * sy + 1
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return sx, sy, at

    def wait_pixel(self, x, y, want, tol=40, timeout=4):
        """Waits until the pixel (x,y) reaches the colour 'want' (with a
        tolerance). It removes flakiness: the hi-res compositor refreshes
        more slowly than events arrive. Returns True/False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, _, at = self.screen_raw()
            p = at(x, y)
            if all(abs(p[i] - want[i]) <= tol for i in range(3)):
                return True
            time.sleep(0.2)
        return False

    def screen_raw(self):
        """A screenshot in native coordinates (with no scaling) -
        (width, height, a pixel function). For the VESA modes."""
        ppm = tempfile.mktemp(suffix=".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        idx, fields = 2, []
        while len(fields) < 3:
            while data[idx] in b" \t\n\r":
                idx += 1
            s = idx
            while data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[s:idx]))
        idx += 1
        w, h, _ = fields
        px = data[idx:]
        self._ostatnie_piksele = px

        def at(x, y):
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return w, h, at


    def screen_fbcon(self):
        """The console text read from the FRAMEBUFFER.

        GRUB, seeing a request for a video mode in the kernel's header,
        always gives graphics - the character memory at 0xB8000 then
        stays empty and screen() reads nothing out of it. The console
        draws itself then, in an 8x8 font, in an 80x25 block in the
        middle of the screen (kernel/fbcon.c).

        We read it the way a human would: the colour most common across
        the whole block is the background, the rest are glyphs -
        and we look up the pattern of eight bytes in the same font the
        kernel drew with.

        We compute the background for the whole block, not for a single
        cell: there are characters in which there are MORE glyph pixels
        than background ones ('0', 'N', 'B', 'W'), and computed
        separately they would come out inverted.
        """
        w, h, _ = self.screen_raw()
        px = self._ostatnie_piksele
        x0 = (w - 80 * 8) // 2
        y0 = (h - 25 * 8) // 2
        if x0 < 0 or y0 < 0:
            return ""

        ile = {}
        for y in range(25 * 8):
            o = ((y0 + y) * w + x0) * 3
            line_text = px[o:o + 80 * 8 * 3]
            for i in range(0, len(line_text), 3):
                p = line_text[i:i + 3]
                ile[p] = ile.get(p, 0) + 1
        if not ile:
            return ""
        tlo = max(ile, key=ile.get)

        font = font8x8()
        rows = []
        for row in range(25):
            chars = []
            for col in range(80):
                pattern = []
                for y in range(8):
                    o = ((y0 + row * 8 + y) * w + x0 + col * 8) * 3
                    line_text = px[o:o + 24]
                    b = 0
                    for x in range(8):
                        if line_text[x * 3:x * 3 + 3] != tlo:
                            b |= 1 << x
                    pattern.append(b)
                chars.append(font.get(tuple(pattern), "?"))
            rows.append("".join(chars).rstrip())
        return "\n".join(rows).rstrip()

    def wait_fbcon(self, text, timeout=15):
        """Waits until the text appears on the console drawn in the
        framebuffer. Returns the whole screen (also after the time runs
        out - the assertion belongs to the caller)."""
        end = time.time() + timeout
        screen_text = ""
        while time.time() < end:
            screen_text = self.screen_fbcon()
            if text in screen_text:
                return screen_text
            time.sleep(0.4)
        return screen_text

    # What a character is called on the way to QEMU. Anything with a
    # name of its own is here; a capital letter is handled in
    # type_line, because there are twenty-six of those and one rule.
    KEYNAMES = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot",
                ":": "shift-semicolon", ";": "semicolon",
                "/": "slash", ">": "shift-dot", "<": "shift-comma",
                "|": "shift-backslash", "&": "shift-7",
                "#": "shift-3", "!": "shift-1", "@": "shift-2",
                "$": "shift-4", "%": "shift-5", "^": "shift-6",
                "*": "shift-8", "(": "shift-9", ")": "shift-0",
                "_": "shift-minus", "+": "shift-equal", "=": "equal",
                "?": "shift-slash", "'": "apostrophe",
                '"': "shift-apostrophe", ",": "comma",
                "[": "bracket_left", "]": "bracket_right",
                "{": "shift-bracket_left", "}": "shift-bracket_right",
                "\\": "backslash", "`": "grave_accent",
                "~": "shift-grave_accent"}

    def type_line(self, text, enter=True):
        """Types the text, and Enter after it unless told otherwise -
        an editor is a place where what is typed does not end with
        pressing Enter."""
        for ch in text:
            if ch.isupper():
                # QEMU has no key called 'T' - a capital is the shift
                # key and the letter, and typing one without saying so
                # simply loses it
                self.sendkey("shift-" + ch.lower())
                continue
            self.sendkey(self.KEYNAMES.get(ch, ch))
        if enter:
            self.sendkey("ret")
        time.sleep(0.3)

    def serial(self):
        """Everything the kernel wrote to the serial port."""
        if not self.serial_path or not os.path.exists(self.serial_path):
            return ""
        with open(self.serial_path, "rb") as fh:
            return fh.read().decode("latin1")

    def command(self, cmd, text=None, timeout=20):
        """Types a command and hands back WHAT ARRIVED on the port after it.

        Without cutting off the older content a false alarm is easy: the
        shell echoes the typed line, so the file name is on the port even
        when the command found nothing - and the previous command's
        result lies there until the end of the run."""
        start = len(self.serial())
        self.type_line(cmd)
        if text is None:
            time.sleep(0.5)
            return self.serial()[start:]
        end = time.time() + timeout
        while time.time() < end:
            fresh = self.serial()[start:]
            if text in fresh:
                return fresh
            time.sleep(0.3)
        return self.serial()[start:]

    def wait_serial(self, text, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            out = self.serial()
            if text in out:
                return out
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' nie pojawilo sie na porcie")

    def wait_exit(self, timeout=20):
        """Waits until QEMU ends by itself (a power off)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return True
            time.sleep(0.3)
        return False

    def close(self):
        self.proc.kill()
        for p in (self.qmp_path, self.dump_path):
            if os.path.exists(p):
                os.unlink(p)


# --- Counting the results -------------------------------------------
# Every group runs in its own process and prints its results to the
# output. The runner adds them up - thanks to that the groups do not
# have to care about one another or pass anything between themselves.
failures = []


def pmm_used(screen_text):
    """How many frames are taken according to the last PMM line on the
    'mem' screen."""
    m = re.findall(r"PMM:\s+(\d+)/", screen_text)
    return int(m[-1]) if m else None


def check(name, cond):
    print(("OK:   " if cond else "BLAD: ") + name)
    if not cond:
        failures.append(name)


def summary():
    """Ends the group's process: 0 = everything passed."""
    if failures:
        print(f"NIEPOWODZENIE: {len(failures)} test(y) nie przeszly")
        return 1
    return 0



def make_iso64(workdir, iso, graphics=False, text=False):
    """Assembles a disc with the 64-bit kernel and its initrd.

    The 64-bit kernel cannot be started through -kernel: QEMU loads only
    Multiboot 1 images that way, and this is Multiboot 2. So every test
    has to build a disc with GRUB, and until now every one did it on its
    own. 'graphics' turns graphics mode on from the loader (the desktop
    and the console on the framebuffer). 'text' demands text mode: GRUB
    gives a framebuffer of its own accord and the console then draws in
    it instead of in the VGA character memory the tests look for at
    0xB8000.

    Returns True when the disc was created.
    """
    os.makedirs(workdir + "/boot/grub", exist_ok=True)
    shutil.copy(KERNEL64, workdir + "/boot/protonos64.bin")
    mod = ""
    if INITRD64 and os.path.exists(INITRD64):
        shutil.copy(INITRD64, workdir + "/boot/initrd64.img")
        mod = "    module2 /boot/initrd64.img initrd64\n"
    header = ("insmod all_video\nset gfxpayload=keep\n" if graphics
                else "set gfxpayload=text\n" if text else "")
    with open(workdir + "/boot/grub/grub.cfg", "w") as fh:
        fh.write(header + "set timeout=0\nset default=0\n"
                 'menuentry "ProtonOS 64" {\n'
                 "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
    return subprocess.run(["grub-mkrescue", "-o", iso, workdir],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def no64():
    """The reason a 64-bit test cannot be run, or None when it can. Every
    64-bit group asks the same question."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        return "brak obrazu 64-bitowego (make kernel64)"
    if shutil.which("grub-mkrescue") is None:
        return "brak grub-mkrescue"
    if shutil.which("qemu-system-x86_64") is None:
        return "brak qemu-system-x86_64"
    return None
