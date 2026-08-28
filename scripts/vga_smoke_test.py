#!/usr/bin/env python3
"""Test dymny ProtonOS: bootuje jądro w QEMU bez okna i weryfikuje:
  1. boot + komunikaty inicjalizacji (Multiboot, GDT, IDT, PIC, PIT, klawiatura)
  2. obsługę wyjątku breakpoint (int3) przez IDT
  3. inicjalizację pamięci: PMM, stronicowanie, kmalloc selftest
  4. tykanie timera PIT (licznik uptime w rogu ekranu rośnie)
  5. wielozadaniowość: liczniki wątków A/B rosną niezależnie, wątek C
     kończy się i znika z listy 'ps'
  6. shell: echo klawiatury, komendy 'help', 'mem', 'ps' (QMP sendkey)
  7. procesy: 'run hello' (ELF z initrd, syscalle, czyste wyjście,
     brak wycieku ramek), 'run evil' (czyta pamięć jądra - ginie,
     system żyje), 'run brak' (błąd), 'ls'
  8. shell użytkownika (ring 3): 'run sh' przejmuje klawiaturę przez
     sys_read, spawn+wait programów, exit oddaje konsolę jądru
  9. sieć: DHCP (adres z serwera), ARP + ICMP ('ping'), gniazda UDP
     w ring 3 (udpecho + udptest przez pętlę zwrotną), DNS ('nslookup'),
     TCP w obie strony - klient ('httpget' do serwera HTTP na hoście,
     treść ląduje na PFS) i serwer ('tcpserv' + połączenie z hosta)
 10. page fault w ring 0: komenda 'crash' zatrzymuje system z diagnostyką

Użycie: python3 scripts/vga_smoke_test.py [protonos.bin] [initrd.img]
Kod wyjścia 0 = wszystko OK, 1 = błąd.
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

# Geometria menu pulpitu (kernel/desktop.c): wpisy rosna w gore od paska
# zadan, wiec kazdy nowy program przesuwa poprzednie. Trzymamy to w
# jednym miejscu, zeby dodanie aplikacji nie psulo testow klikania.
# System startuje prosto na pulpit; testy konsolowe wymuszaja tryb
# tekstowy linia polecen jadra.
TEXT_MODE = "nodesktop"

# Obraz dysku pod PFS3 (jadro formatuje pusty dysk przy pierwszym starcie)
DISK_SIZE = 16 * 1024 * 1024

# Zegar guesta: ustalony, ale pozniejszy niz data wystawienia
# certyfikatow generowanych w trakcie testu
RTC_BASE = "2030-06-13T12:34:56"
RTC_DATE = "2030-06-13"

MENU_ITEMS = ["guiapp", "gclock", "gfiles", "gweb"]
MENU_ROW = 12
MENU_TASKBAR_Y = 468   # video_h() - TB_H (kernel/desktop.c)


def menu_y(name):
    """Srodek wpisu menu o danej nazwie (wspolrzedne ekranu 640x480)."""
    idx = MENU_ITEMS.index(name)
    return MENU_TASKBAR_Y - (len(MENU_ITEMS) - idx) * MENU_ROW + MENU_ROW // 2


PAGE = b"<html>ProtonOS TCP dziala</html>\n"
# Strona z odnosnikiem i taka ilescia tekstu, ze trzeba przewijac
LINK_PAGE = (b"<html><body><h1>Strona pierwsza</h1>"
             + b"<p>Wiersz wypelniajacy strone numer jeden.</p>" * 20
             + b'<p>Przejdz do <a href="/druga">drugiej strony</a>.</p>'
             + b"</body></html>\n")
LINK_PAGE2 = (b"<html><body><h1>Strona druga</h1>"
              b"<p>Odnosnik zadzialal.</p></body></html>\n")
# Ponad dawny sufit 268 KiB na plik - wchodzi w blok podwojnie posredni
HUGE_PAGE = b"<p>ProtonOS PFS3: blok posredni, potem podwojnie posredni.</p>\n" * 6500
# Strona grubo ponad stary limit PFS (16 KiB na plik) - sprawdza
# przydzial blokow, blok posredni i przepustowosc TCP
BIG_PAGE = b"<p>ProtonOS PFS3: blok po bloku.</p>\n" * 3000


def start_dns_server(answer_ip=(10, 0, 2, 2)):
    """Prosciutki serwer DNS na hoscie: na kazde pytanie o rekord A
    odpowiada tym samym adresem. Dzieki temu sciezka 'nazwa -> adres'
    da sie sprawdzic bez zadnej sieci na zewnatrz."""
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
            # pytanie konczy sie zerowym bajtem nazwy + QTYPE + QCLASS
            i = 12
            while i < len(data) and data[i]:
                i += data[i] + 1
            qend = i + 5
            header = data[:2] + b"\x81\x80" + data[4:6] + b"\x00\x01" \
                     + b"\x00\x00\x00\x00"
            answer = (b"\xc0\x0c"            # wskaznik na nazwe z pytania
                      + b"\x00\x01\x00\x01"  # typ A, klasa IN
                      + b"\x00\x00\x00\x3c"  # TTL 60 s
                      + b"\x00\x04" + bytes(answer_ip))
            sock.sendto(header + data[12:qend] + answer, addr)
        sock.close()

    threading.Thread(target=serve, daemon=True).start()
    return stop.set


CERT_DER = None          # certyfikat serwera HTTPS (do magazynu zaufania)
CERT_DER_EC = None       # to samo, ale na krzywej P-256
CERT_DER_P384 = None     # i na krzywej P-384 (podpis SHA-384)


def make_test_cert(dirpath, ec=False, p384=False):
    """Certyfikat samopodpisany dla protonos.test: SubjectAltName z nazwa
    i wieloznacznikiem, flaga CA (moze wiec sluzyc tez za korzen
    zaufania) i dziesiecioletnia waznosc, zeby obejmowala ustalony zegar
    guesta. Robimy to narzedziem openssl - jest wszedzie tam, gdzie
    testuje sie TLS."""
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
    """Prawdziwy lancuch: korzen -> posrednik -> certyfikat serwera.
    Zaufanie dajemy tylko korzeniowi, wiec ProtonOS musi przejsc cala
    droge podpisow, a nie tylko porownac jeden certyfikat."""
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

    # korzen
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("root.key"), "-out", j("root.pem"), "-days", "3650",
         "-subj", "/CN=ProtonOS Test Root",
         "-addext", "basicConstraints=critical,CA:TRUE"])

    # posrednik podpisany korzeniem
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("int.key"), "-out", j("int.csr"),
         "-subj", "/CN=ProtonOS Test Intermediate"])
    run(["openssl", "x509", "-req", "-in", j("int.csr"),
         "-CA", j("root.pem"), "-CAkey", j("root.key"), "-CAcreateserial",
         "-out", j("int.pem"), "-days", "3650", "-extfile", j("ca.ext")])

    # certyfikat serwera podpisany posrednikiem
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("leaf.key"), "-out", j("leaf.csr"),
         "-subj", "/CN=protonos.test"])
    run(["openssl", "x509", "-req", "-in", j("leaf.csr"),
         "-CA", j("int.pem"), "-CAkey", j("int.key"), "-CAcreateserial",
         "-out", j("leaf.pem"), "-days", "3650", "-extfile", j("leaf.ext")])

    # serwer wysyla certyfikat wraz z posrednikiem
    with open(j("chain.pem"), "w") as out:
        out.write(open(j("leaf.pem")).read())
        out.write(open(j("int.pem")).read())

    run(["openssl", "x509", "-in", j("root.pem"), "-outform", "DER",
         "-out", j("root.der")])
    with open(j("root.der"), "rb") as f:
        return j("chain.pem"), j("leaf.key"), f.read()


CERT_DER_ROOT = None     # korzen lancucha (do magazynu zaufania)


def start_https_chain_server():
    """Serwer HTTPS przedstawiajacy lancuch dwupoziomowy."""
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
    """Serwer HTTPS (TLS 1.3) na hoscie z certyfikatem generowanym w
    locie - z kluczem RSA albo na krzywej P-256. Test nie potrzebuje
    dzieki temu niczego z zewnatrz.
    Zwraca (port, stop) albo (None, stop), gdy brakuje openssl-a."""
    global CERT_DER, CERT_DER_EC, CERT_DER_P384
    import ssl

    tmpdir = tempfile.mkdtemp(prefix="protonos-tls-")
    try:
        certfile, keyfile, der = make_test_cert(tmpdir, ec, p384)
    except BaseException:                # brak openssl-a to nie powod, by
        return None, lambda: None        # wywrocic caly test
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
    """Serwer HTTP na hoscie - guest siega do niego przez brame slirpa
    (10.0.2.2), wiec test TCP nie potrzebuje zadnej sieci na zewnatrz."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            if self.path.startswith("/stara"):      # przekierowanie
                self.send_response(302)
                self.send_header("Location", "/kawalki")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            if self.path.startswith("/kawalki"):    # kodowanie chunked
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for i in range(0, len(PAGE), 7):    # celowo male kawalki
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
                 append=None, ram_mib=None, sata=False, usbkbd=False,
                 cdrom=None, serial=None, qemu="qemu-system-i386",
                 uefi=None):
        self.qmp_path = tempfile.mktemp(suffix=".qmp")
        self.dump_path = tempfile.mktemp(suffix=".vga")
        # Jadro 64-bitowe potrzebuje procesora z trybem dlugim, a
        # qemu-system-i386 takiego nie udaje
        cmd = [qemu,
               "-display", "none", "-no-reboot",
               # Stala data RTC -> deterministyczny test komendy 'date'.
               # Musi byc pozniejsza niz chwila wystawienia certyfikatow
               # testowych, inaczej TLS slusznie je odrzuci jako
               # "jeszcze nie obowiazuje".
               "-rtc", "base=" + RTC_BASE,
               # Karta sieciowa w trybie uzytkownika: brama 10.0.2.2
               # odpowiada na ARP i ICMP echo, wiec 'ping' da sie
               # przetestowac bez zadnej sieci na hoscie
               "-netdev",
               "user,id=n0" + (f",hostfwd=tcp::{hostfwd_port}-:{hostfwd_port}"
                               if hostfwd_port else ""),
               "-device", "rtl8139,netdev=n0",
               "-qmp", f"unix:{self.qmp_path},server,nowait"]
        if ram_mib:
            cmd += ["-m", str(ram_mib)]
        if usbkbd:
            # Klawiatura USB zamiast PS/2 - tak wyglada kazda maszyna
            # nowsza niz mniej wiecej 2010 rok
            cmd += ["-usb", "-device", "usb-kbd"]
        self.serial_path = serial
        if serial:
            # Gdy konsola rysuje sie na buforze obrazu, w buforze znakow
            # VGA nie ma juz nic do czytania - tekst zbieramy z portu
            cmd += ["-serial", "file:" + serial]
        if uefi:
            # Zamiast BIOS-u pamiec flash z firmware UEFI (OVMF): osobno
            # kod (tylko do odczytu) i osobno zmienne, ktore firmware
            # zapisuje - stad kopia pliku ze zmiennymi dla kazdego biegu
            code, nvram = uefi
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,"
                              "file=" + code,
                    "-drive", "if=pflash,format=raw,unit=1,file=" + nvram]
        if cdrom:
            # Rozruch jak z prawdziwej plyty: jadro wgrywa GRUB, a nie QEMU
            cmd += ["-cdrom", cdrom, "-boot", "d"]
        else:
            cmd += ["-kernel", kernel]
            if append:
                cmd += ["-append", append]
            if initrd:
                cmd += ["-initrd", initrd]
        if disk and sata:
            # Dysk za kontrolerem AHCI - tak wyglada kazda nowsza plyta
            cmd += ["-device", "ich9-ahci,id=ahci",
                    "-drive", f"id=sata0,file={disk},if=none,format=raw",
                    "-device", "ide-hd,drive=sata0,bus=ahci.0"]
        elif disk:
            cmd += ["-hda", disk]
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        # Poczekaj, az QEMU wystawi socket QMP (start bywa wolny)
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
        self.f = sock.makefile("rwb")
        self.f.readline()  # baner QMP
        self.cmd({"execute": "qmp_capabilities"})

    def cmd(self, msg):
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            resp = json.loads(self.f.readline())
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

    def wait_for(self, text, timeout=15):
        """Czeka, az podany tekst pojawi sie na ekranie VGA."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            screen = self.screen()
            if text in screen:
                return screen
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' nie pojawilo sie na ekranie")

    DESK_BG = (0, 128, 160)          # tlo pulpitu (teal)

    def wait_desktop(self, timeout=25):
        """Czeka, az kompozytor narysuje pulpit. W trybie graficznym
        bufor tekstowy VGA (0xB8000) przestaje byc oknem na to, co
        widac, wiec gotowosci szukamy w pikselach, nie w napisach."""
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
        """Mysz PS/2 przenosi w jednym pakiecie tylko male delty, wiec
        wieksze skoki dzielimy na kroki - inaczej kursor laduje gdzie
        indziej, niz chcial test."""
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
        """Dosuwa kursor do lewego gornego rogu (kompozytor przytrzymuje
        go w granicach ekranu) i zeruje sledzona pozycje - kalibracja
        przed klikaniem w konkretne miejsca."""
        self.mouse_move(-800, -600)
        self.mx, self.my = 0, 0

    def mouse_button(self, down):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}}]}})
        time.sleep(0.2)

    # Sledzenie pozycji kursora (pulpit startuje na srodku 320x200)
    mx, my = 160, 100

    def mouse_goto(self, tx, ty):
        self.mouse_move(tx - self.mx, ty - self.my)
        self.mx, self.my = tx, ty

    def mouse_click(self):
        self.mouse_button(True)
        self.mouse_button(False)

    def screen_pixels(self):
        """Zrzut ekranu (QMP screendump) -> (skala_x, skala_y, funkcja
        pikseli we wspolrzednych framebuffera 320x200). QEMU podwaja
        tryb 13h przy wyswietlaniu, stad skalowanie."""
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
        """Czeka, az piksel (x,y) osiagnie kolor 'want' (z tolerancja).
        Niweluje flakiness: kompozytor hi-res odswieza wolniej niz
        przychodza zdarzenia. Zwraca True/False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, _, at = self.screen_raw()
            p = at(x, y)
            if all(abs(p[i] - want[i]) <= tol for i in range(3)):
                return True
            time.sleep(0.2)
        return False

    def screen_raw(self):
        """Zrzut ekranu w natywnych wspolrzednych (bez skalowania) -
        (szerokosc, wysokosc, funkcja pikseli). Do trybow VESA."""
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

        def at(x, y):
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return w, h, at

    KEYNAMES = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot",
                ":": "shift-semicolon",
                "/": "slash", ">": "shift-dot", "<": "shift-comma",
                "|": "shift-backslash", "&": "shift-7"}

    def type_line(self, text):
        for ch in text:
            self.sendkey(self.KEYNAMES.get(ch, ch))
        self.sendkey("ret")
        time.sleep(0.3)

    def serial(self):
        """Wszystko, co jadro wypisalo na port szeregowy."""
        if not self.serial_path or not os.path.exists(self.serial_path):
            return ""
        with open(self.serial_path, "rb") as fh:
            return fh.read().decode("latin1")

    def wait_serial(self, text, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            out = self.serial()
            if text in out:
                return out
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' nie pojawilo sie na porcie")

    def wait_exit(self, timeout=20):
        """Czeka, az QEMU sam sie zakonczy (wylaczenie zasilania)."""
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


def main():
    failures = []

    def check(name, cond):
        print(("OK:   " if cond else "BLAD: ") + name)
        if not cond:
            failures.append(name)

    # Swiezy obraz dysku (1 MiB) wspoldzielony przez oba boot-y -
    # jadro sformatuje go przy pierwszym starcie
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        # Czekaj na pelny boot (prompt shella) zamiast zgadywac czas
        screen = q.wait_for("ProtonOS shell")
        print("=== Ekran VGA po boocie ===")
        print(screen)
        print("===========================")

        check("dysk + PFS wykryty i sformatowany",
              "+ PFS gotowy" in screen)
        check("kontroler dysku nazwany przy boocie (tu stary ATA PIO)",
              "Dysk ATA PIO + PFS gotowy" in screen)
        check("mysz PS/2 wykryta przy boocie",
              "Mysz PS/2 gotowa" in screen)
        check("karta sieciowa RTL8139 wykryta przy boocie",
              "Siec: RTL8139" in screen)

        # Adres IP przychodzi z serwera DHCP slirpa juz po starcie shella.
        # Czekamy na komunikat, zanim ekran zacznie sluzyc testom
        # klawiatury - inaczej wpada w srodek pisanej linii.
        lease = q.wait_for("[siec] DHCP", timeout=20)
        # Komunikat sklada sie z kilku printk-ow, wiec chwila na dopisanie
        # reszty linii - inaczej test czyta ja w polowie
        time.sleep(0.5)
        lease = q.screen()
        check("DHCP przyznal adres IP", "przyznano 10.0.2.15" in lease)
        check("DHCP przekazal brame i serwer DNS",
              "brama 10.0.2.2" in lease and "DNS 10.0.2.3" in lease)

        for expected in ["Multiboot OK", "GDT zaladowana", "IDT zaladowana",
                         "PIC przemapowany", "Timer PIT", "Klawiatura PS/2",
                         "Przerwania wlaczone"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("obsluga wyjatku int3 (breakpoint)", "[INT3]" in screen)

        for expected in ["Pamiec fizyczna", "Stronicowanie wlaczone",
                         "kmalloc/kfree dzialaja"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("jadro przezylo init pamieci (doszlo do shella)",
              "ProtonOS shell" in screen)

        # Timer: uptime w rogu ekranu musi urosnac
        def uptime(s):
            m = re.search(r"uptime: (\d+)s", s)
            return int(m.group(1)) if m else None

        t1 = uptime(q.wait_for("uptime: "))
        time.sleep(2)
        t2 = uptime(q.screen())
        check(f"timer PIT tyka (uptime {t1}s -> {t2}s)",
              t1 is not None and t2 is not None and t2 > t1)

        # Wielozadaniowosc: liczniki watkow A i B musza rosnac niezaleznie.
        # Bierzemy OSTATNIE wystapienie - po przewinieciu ekranu wyzej
        # moga wisiec stare, martwe kopie linii z licznikami.
        def counters(s):
            a = re.findall(r"watek-A: (\d+)", s)
            b = re.findall(r"watek-B: (\d+)", s)
            return (int(a[-1]) if a else None, int(b[-1]) if b else None)

        a1, b1 = counters(q.wait_for("watek-A: "))
        time.sleep(1.5)
        a2, b2 = counters(q.screen())
        check(f"watek A liczy ({a1} -> {a2})",
              a1 is not None and a2 is not None and a2 > a1)
        check(f"watek B liczy ({b1} -> {b2})",
              b1 is not None and b2 is not None and b2 > b1)

        # Watek C konczy sie po 2 s
        screen = q.wait_for("watek-C: zakonczony")
        check("watek C zakonczyl prace", True)

        # Klawiatura: wstrzyknij "proton 1" i sprawdz echo
        for key in ["p", "r", "o", "t", "o", "n", "spc", "1"]:
            q.sendkey(key)
        q.sendkey("shift-x")  # test shifta: powinno wyjsc 'X'
        screen = q.screen()
        check("echo klawiatury: 'proton 1'", "> proton 1" in screen)
        check("echo klawiatury z shiftem: 'X'", "> proton 1X" in screen)

        # Dokoncz smieciowa linie enterem i przetestuj komendy shella
        q.sendkey("ret")
        time.sleep(0.3)
        screen = q.screen()
        check("nieznana komenda zgloszona",
              "nieznana komenda: 'proton'" in screen)

        q.type_line("help")
        screen = q.screen()
        check("komenda 'help' wypisuje liste komend",
              "Dostepne komendy" in screen and "reboot" in screen)

        q.type_line("mem")
        screen = q.screen()
        check("komenda 'mem' pokazuje RAM i ramki",
              "RAM wykryty" in screen and "ramek 4 KiB" in screen)
        check("komenda 'mem' pokazuje sterte", "sterta jadra" in screen)
        # Cala wykryta pamiec jest pod zarzadem, nie tylko pierwsze 64 MiB:
        # mapowanie identycznosciowe ciagnie sie az po szczyt RAM-u.
        mm = re.search(r"zarzadzane (\d+) MiB", screen)
        ram = re.search(r"RAM wykryty:\s+(\d+) KiB", screen)
        check("zarzadzana pamiec siega szczytu wykrytego RAM-u"
              + (f" ({mm.group(1)} MiB z {int(ram.group(1)) // 1024} MiB)"
                 if mm and ram else ""),
              mm is not None and ram is not None
              and int(mm.group(1)) >= int(ram.group(1)) // 1024 - 2)

        q.type_line("uptime")
        screen = q.screen()
        check("komenda 'uptime' dziala", "tyknac timera" in screen)

        q.type_line("ps")
        screen = q.screen()
        check("komenda 'ps' pokazuje watki",
              "main" in screen and "demo-A" in screen
              and "demo-B" in screen)
        check("shell jest wlasnym watkiem (nie kontekst przerwania)",
              "shell (biezacy)" in screen)
        check("zombie po watku C zostal sprzatniety",
              "demo-C" not in screen)

        # initrd: 'ls' pokazuje spakowane programy. Plikow jest duzo,
        # wiec wczesne (hello/evil) sie przewijaja - sprawdzamy pozne,
        # ktore zostaja na dole ekranu.
        q.type_line("ls")
        screen = q.screen()
        check("komenda 'ls' pokazuje pliki initrd",
              "gmon" in screen and "shmread" in screen)

        # Wyciek ramek: zmierz uzycie PMM przed i po zyciu procesu
        def pmm_used(s):
            m = re.findall(r"PMM:\s+(\d+)/", s)
            return int(m[-1]) if m else None

        q.type_line("mem")
        used_before = pmm_used(q.screen())

        # 'run hello': proces ELF z wlasna przestrzenia adresowa
        q.type_line("run hello")
        screen = q.wait_for("[hello] czesc, jestem procesem ELF")
        check("proces hello wystartowal z ELF i robi syscall write", True)
        # Przestrzen uzytkownika zaczyna sie od 1 GiB - ponizej jest juz
        # tylko mapowanie identycznosciowe jadra
        check("program uzytkownika mieszka powyzej 1 GiB",
              re.search(r"entry 4[0-9a-f]{7}", screen) is not None)
        q.wait_for("[hello] licze: 3")
        check("syscalle write+sleep dzialaja w petli", True)
        q.wait_for("proces 'hello' zakonczyl sie (kod 0)")
        check("hello zakonczyl sie czysto przez sys_exit", True)

        time.sleep(0.5)  # czas na sprzatniecie zombie
        q.type_line("mem")
        used_after = pmm_used(q.screen())
        check(f"brak wycieku ramek po procesie ({used_before} -> {used_after})",
              used_before is not None and used_after == used_before)

        # 'run echo ...': argumenty (argc/argv) docieraja do procesu
        q.type_line("run echo proton os dziala")
        screen = q.wait_for("proton os dziala")
        check("argumenty argc/argv docieraja do procesu (echo)",
              "proton os dziala" in screen)

        # 'run memtest': sbrk/malloc daje procesowi rosnaca pamiec
        q.type_line("run memtest")
        screen = q.wait_for("[memtest] OK:")
        check("sbrk/malloc dziala (memtest zapisal i zweryfikowal pamiec)",
              "blokow po" in screen and "zweryfikowanych" in screen)
        check("memtest nie zglosil bledu pamieci",
              "BLAD" not in screen.split("[memtest] OK")[0].split("[memtest] start")[-1])

        # Brak wycieku takze po procesie uzywajacym sbrk
        time.sleep(0.5)
        q.type_line("mem")
        used_after_mt = pmm_used(q.screen())
        check(f"brak wycieku ramek po memtest ({used_before} -> {used_after_mt})",
              used_after_mt is not None and used_after_mt == used_before)

        # 'run mmaptest': mmap daje swieza, wyzerowana, zapisywalna pamiec
        q.type_line("run mmaptest")
        screen = q.wait_for("[mmaptest] OK:")
        check("mmap dziala (3 strony zmapowane, zerowe i zapisywalne)",
              "zapisane i zweryfikowane" in screen)
        # i nie wycieka po zakonczeniu procesu (zwolnione z przestrzenia)
        time.sleep(0.5)
        q.type_line("mem")
        used_after_mm = pmm_used(q.screen())
        check(f"brak wycieku ramek po mmaptest ({used_before} -> {used_after_mm})",
              used_after_mm is not None and used_after_mm == used_before)

        # 'run shmtest': pamiec wspoldzielona miedzy procesami. shmtest
        # zapisuje 12345 i uruchamia shmread (osobny proces), ktory czyta
        # te sama pamiec - dowodem jest jego linia z odczytana wartoscia.
        q.type_line("run shmtest")
        screen = q.wait_for("[shmread] wartosc=")
        check("pamiec wspoldzielona: inny proces czyta zapisana wartosc",
              "[shmread] wartosc=12345" in screen)

        # 'run threads': dwa watki w jednym procesie (wspolna pamiec).
        # Kazdy zapisuje inna zmienna globalna; main sumuje -> 33.
        # Swieza baza ramek (shmtest zajal trwaly region wspoldzielony).
        q.type_line("mem")
        used_before_th = pmm_used(q.screen())
        q.type_line("run threads")
        screen = q.wait_for("[threads] watki zapisaly")
        check("watki w procesie dziela pamiec (suma globalnych = 33)",
              "suma=33" in screen)
        # brak wycieku ramek po procesie wielowatkowym (stosy watkow +
        # przestrzen adresowa zwolnione, gdy konczy sie ostatni watek)
        time.sleep(0.5)
        q.type_line("mem")
        used_after_th = pmm_used(q.screen())
        check(f"brak wycieku ramek po watkach ({used_before_th} -> {used_after_th})",
              used_after_th is not None and used_after_th == used_before_th)

        # 'run forktest': fork() z kopiowaniem przy zapisie. Program
        # zajmuje 256 KiB, rozwidla sie i sprawdza, ze dziecko dostalo
        # wzorzec rodzica, a wlasne zapisy zostaly u niego - przy czym
        # tuz po fork pamiec urosla o tablice stron, nie o caly bufor.
        q.type_line("clear")
        q.type_line("mem")
        used_before_fk = pmm_used(q.screen())
        q.type_line("run forktest")
        screen = q.wait_for("[forktest] dziecko: wzorzec odziedziczony",
                            timeout=30)
        check("fork: dziecko wraca z syscalla z pamiecia rodzica", True)
        m = re.search(r"przyrost RAM po fork: (\d+) KiB", screen)
        check("fork nie kopiuje pamieci procesu"
              + (f" (256 KiB bufora, przyrost {m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) < 64)
        screen = q.wait_for("[forktest] rodzic: wzorzec nietkniety",
                            timeout=30)
        check("zapisy dziecka nie widac u rodzica (prywatne kopie stron)",
              True)
        # Ten sam wzorzec zostal nietkniety takze tam, gdzie w dziecku
        # pisalo JADRO (syscall) - o to dba bit WP w CR0.
        check("zapis jadra w dziecku tez trafil w prywatna kopie strony",
              "rodzic: wzorzec nietkniety" in screen)
        check("rodzic odbiera kod wyjscia dziecka przez wait",
              "kod dziecka=7" in screen)
        # Druga polowa pary uniksowej: dziecko podmienia sie w inny
        # program przez exec, a rodzic czeka na ten sam pid.
        screen = q.wait_for("[forktest] rodzic doczekal sie pidu", timeout=30)
        check("exec podmienia obraz procesu w miejscu",
              "[forktest] po exec: inny program, ten sam pid" in screen)
        check("po exec proces nazywa sie jak nowy program",
              "proces 'echo' zakonczyl sie (kod 0)" in screen)
        check("wait rodzica trafia w ten sam pid po exec",
              "doczekal sie pidu" in screen)
        q.wait_for("proces 'forktest' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_fk = pmm_used(q.screen())
        check(f"brak wycieku ramek po fork ({used_before_fk} -> {used_after_fk})",
              used_after_fk is not None and used_after_fk == used_before_fk)

        # 'run lazymem': stronicowanie na zadanie. Prosba o 64 MiB nie
        # kosztuje ani ramki - place sie dopiero za dotkniete strony.
        q.type_line("clear")
        time.sleep(0.6)          # niech zombie po poprzednim tescie zniknie
        q.type_line("mem")
        used_before_lz = pmm_used(q.screen())
        q.type_line("run lazymem")
        screen = q.wait_for("[lazymem] dotkniete", timeout=40)
        m = re.search(r"poproszone 64 MiB, RAM urosl o (\d+) KiB", screen)
        check("prosba o 64 MiB sterty nie kosztuje ani ramki"
              + (f" (+{m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) == 0)
        m = re.search(r"dotkniete 1 MiB, RAM urosl o (\d+) KiB", screen)
        check("place sie dokladnie za dotkniete strony"
              + (f" (+{m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) == 1024)
        screen = q.wait_for("[lazymem] mmap tez daje strony", timeout=30)
        check("syscall urzeczywistnia nietknieta strone zamiast zglaszac blad",
              "syscall urzeczywistnil nietknieta strone" in screen)
        check("mmap tez przydziela strony dopiero przy dotknieciu", True)
        q.wait_for("proces 'lazymem' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_lz = pmm_used(q.screen())
        check(f"brak wycieku ramek po stronicowaniu na zadanie"
              f" ({used_before_lz} -> {used_after_lz})",
              used_after_lz is not None and used_after_lz == used_before_lz)

        # 'run ptree': drzewo procesow. Trzy dzieci koncza sie w innej
        # kolejnosci, niz powstaly, a rodzic zbiera je przez waitany.
        q.type_line("clear")
        q.type_line("mem")
        used_before_pt = pmm_used(q.screen())
        q.type_line("run ptree")
        screen = q.wait_for("[ptree] zebrane wszystkie dzieci", timeout=30)
        m = re.findall(r"skonczylo dziecko (\d+) z kodem (\d+)", screen)
        check("wait na dowolne dziecko oddaje je w kolejnosci konczenia"
              + (f" (pidy {[p for p, _ in m]})" if m else ""),
              len(m) == 3 and [int(p) for p, _ in m] == sorted(
                  (int(p) for p, _ in m), reverse=True))
        check("kod wyjscia pasuje do dziecka, ktore go zwrocilo",
              [c for _, c in m] == ["22", "21", "20"])
        screen = q.wait_for("[ptree] sierota przygarnieta", timeout=30)
        check("sierota trafia pod opieke dziadka (kod 99 dociera do niego)",
              "z kodem 99" in screen)
        q.wait_for("proces 'ptree' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_pt = pmm_used(q.screen())
        check(f"brak wycieku ramek po drzewie procesow"
              f" ({used_before_pt} -> {used_after_pt})",
              used_after_pt is not None and used_after_pt == used_before_pt)

        q.type_line("clear")
        q.type_line("ps")
        screen = q.screen()
        check("ps pokazuje rodzica kazdego watku",
              "RODZIC" in screen and re.search(r"\n  0   -", screen) is not None)

        # 'run sigtest': sygnaly. Najostrzejszy punkt to powrot
        # z procedury obslugi - sygnal przychodzi dokladnie w chwili,
        # gdy jadro odlozylo juz wynik przerwanego syscalla.
        q.type_line("clear")
        q.type_line("mem")
        used_before_sg = pmm_used(q.screen())
        q.type_line("run sigtest")
        screen = q.wait_for("[sigtest] wlasna procedura obslugi", timeout=30)
        check("wlasna procedura obslugi sygnalu dziala w ring 3 i wraca",
              "dostala SIGINT i wrocila" in screen)
        screen = q.wait_for("[sigtest] SIG_IGN", timeout=20)
        check("SIG_IGN gubi sygnal zamiast go doreczac",
              "sygnal przepadl" in screen)
        screen = q.wait_for("[sigtest] rodzic dostal SIGCHLD", timeout=30)
        check("SIGTERM domyslnie konczy proces (kod -15)",
              "domyslnie konczy proces (kod -15)" in screen)
        check("rodzic dostaje SIGCHLD po smierci dziecka", True)
        screen = q.wait_for("[sigtest] SIGKILL nie do przechwycenia",
                            timeout=30)
        check("SIGKILL-a nie da sie przechwycic ani zignorowac", True)

        # Ctrl+C z prawdziwej klawiatury -> SIGINT do procesu, ktory
        # trzyma konsole; czekanie na klawisz przerywa sie w pol drogi
        q.wait_for("[sigtest] czekam na Ctrl+C", timeout=20)
        q.sendkey("ctrl-c")
        screen = q.wait_for("[sigtest] Ctrl+C dotarl", timeout=20)
        check("Ctrl+C dociera do procesu jako SIGINT i przerywa odczyt",
              "dotarl jako SIGINT" in screen)
        q.wait_for("proces 'sigtest' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_sg = pmm_used(q.screen())
        check(f"brak wycieku ramek po sygnalach"
              f" ({used_before_sg} -> {used_after_sg})",
              used_after_sg is not None and used_after_sg == used_before_sg)

        # Shell jadra umie wyslac dowolny sygnal, takze pod nazwe procesu
        q.type_line("clear")
        q.type_line("run spin")
        q.wait_for("[spin] tik", timeout=20)
        q.type_line("kill spin 15")
        screen = q.wait_for("zakonczony przez SIGTERM", timeout=20)
        check("'kill nazwa 15' z shella jadra wysyla SIGTERM",
              "kill: SIGTERM -> pid" in screen)

        # 'run prio': scheduler z priorytetami. Watek normalny wywlaszcza
        # niski -> przez to samo okno czasu naliczy wielokrotnie wiecej.
        q.type_line("run prio")
        screen = q.wait_for("[prio] normalny=", timeout=20)
        m = re.search(r"normalny=(\d+) niski=(\d+)", screen)
        ok = m and int(m.group(1)) > int(m.group(2)) * 5
        check("scheduler z priorytetami: normalny >> niski"
              + (f" ({m.group(1)} vs {m.group(2)})" if m else ""), ok)

        # 'run evil': proba czytania pamieci jadra
        q.type_line("run evil")
        screen = q.wait_for("proces 'evil' ubity")
        check("evil czytal pamiec jadra i zostal ubity", True)
        check("CR2 pokazuje atakowany adres", "siegnal po adres 100000" in screen)
        check("kod po ataku nie wykonal sie",
              "TEGO NIE POWINNO BYC WIDAC" not in screen)

        # 'run' z nieistniejacym plikiem
        # Granica jadro/uzytkownik: proces podaje w syscallach wskazniki
        # na pamiec jadra - kazdy musi wrocic z bledem, a proces ma zyc
        q.type_line("run evil2")
        ev2 = q.wait_for("granica trzyma", timeout=20)
        check("syscalle odrzucaja wskazniki na pamiec jadra",
              "jadro odrzucilo wszystkie proby" in ev2)
        check("evil2 nie wypisal pamieci jadra na konsole",
              "PRZESZLO" not in ev2 and "DZIURA" not in ev2)
        check("proces przezyl odrzucone syscalle (kod 0)",
              "kod 0" in ev2)

        # Kryptografia pod przyszly TLS: wektory z RFC liczone w ring 3
        q.type_line("clear")
        q.type_line("run cryptotest")
        crypto = q.wait_for("[crypto] wszystkie", timeout=20)
        check("wektory SHA-256/HMAC/HKDF/ChaCha20/Poly1305/X25519 z RFC",
              "wszystkie wektory zgadzaja sie" in crypto and
              "BLAD" not in crypto)

        q.type_line("run brak")
        check("run zgasza brak pliku", "brak pliku 'brak'" in q.screen())

        # Siec: ARP + ICMP przez karte RTL8139. Brama slirpa (10.0.2.2)
        # odpowiada na oba, wiec test nie potrzebuje sieci na hoscie.
        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  MAC")
        check("ifconfig pokazuje adres IP", "IP 10.0.2.15" in ifc)
        check("ifconfig pokazuje brame", "brama 10.0.2.2" in ifc)
        check("ifconfig pokazuje serwer DNS z DHCP", "DNS 10.0.2.3" in ifc)

        # Shell jest wlasnym watkiem: 'ping' spi w nim przez cztery sekundy,
        # a komenda wpisana w miedzyczasie czeka w buforze klawiatury
        # i wykonuje sie zaraz po nim. Dawniej shell dzialal w kontekscie
        # przerwania, wiec spac nie mogl i ping szedl do osobnego watku.
        q.type_line("ping 10.0.2.2")
        q.type_line("dns 10.0.2.3")     # pisanie na zapas w trakcie pinga
        pong = q.wait_for("40 B od 10.0.2.2: seq=1", timeout=20)
        check("ping: odpowiedz ICMP echo od bramy", "40 B od 10.0.2.2" in pong)
        stats = q.wait_for("--- statystyki:", timeout=20)
        check("ping: wszystkie 4 pakiety wrocily",
              "wyslano 4, odebrano 4" in stats)
        buffered = q.wait_for("serwer DNS ustawiony na", timeout=20)
        check("znaki wpisane w trakcie komendy nie gina (bufor klawiatury)",
              "serwer DNS ustawiony na 10.0.2.3" in buffered)

        q.type_line("arp")
        check("tablica ARP zna MAC bramy",
              "10.0.2.2" in q.wait_for("10.0.2.2  "))

        # Gniazda UDP w ring 3: dwa procesy gadaja przez petle zwrotna,
        # wiec test nie zalezy od zadnej sieci na zewnatrz
        q.type_line("run udpecho")
        q.wait_for("nasluchuje na porcie 7777", timeout=10)
        check("proces ring 3 otworzyl gniazdo UDP", True)

        q.type_line("run udptest")
        echo = q.wait_for("echo wrocilo", timeout=10)
        check("datagram wrocil do nadawcy przez petle zwrotna",
              "echo wrocilo: proton-udp" in echo)
        check("serwer zobaczyl datagram od klienta",
              "[udpecho] od 10.0.2.15:" in echo)
        check("klient zakonczyl sie bez bledu",
              "proces 'udptest' zakonczyl sie (kod 0)" in echo)

        # Gniazdo ma wlasciciela: po smierci procesu port musi sie zwolnic
        q.type_line("kill udpecho")
        time.sleep(0.5)
        q.type_line("clear")          # czysty ekran: stare "nasluchuje" znika
        q.type_line("run udpecho")
        again = q.wait_for("porcie 7777", timeout=10)
        check("port zwolniony po smierci wlasciciela gniazda",
              "nasluchuje na porcie 7777" in again and
              "nie udalo sie otworzyc gniazda" not in again)
        q.type_line("kill udpecho")

        # DNS z ring 3. Pytamy wlasny serwer stojacy na hoscie (10.0.2.2),
        # wiec test nie zalezy od internetu ani od resolvera maszyny.
        dns_stop = start_dns_server()
        try:
            q.type_line("clear")
            q.type_line("dns 10.0.2.2")
            q.wait_for("serwer DNS ustawiony", timeout=10)
            q.type_line("run nslookup protonos.test")
            dns = q.wait_for("ma adres", timeout=20)
            check("nslookup rozwiazal nazwe przez wskazany serwer DNS",
                  "protonos.test ma adres 10.0.2.2" in dns)
        finally:
            dns_stop()

        # TCP jako klient: po drugiej stronie stoi prawdziwy stos
        # (serwer HTTP w Pythonie na hoscie), a nie nasza wlasna petla
        http_port, http_stop = start_http_server()
        try:
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /" % http_port)
            page = q.wait_for("tresc ", timeout=30)
            check("TCP: handshake i odpowiedz HTTP 200",
                  "[httpget] HTTP 200" in page)
            check("TCP: tresc strony dotarla w calosci",
                  "ProtonOS TCP dziala" in page)
            check("httpget zakonczyl sie bez bledu",
                  "proces 'httpget' zakonczyl sie (kod 0)" in page)

            # Siec -> dysk: tresc HTTP zapisana na PFS i odczytana z niego
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d / strona.htm" % http_port)
            q.wait_for("tresc ", timeout=30)
            q.type_line("cat strona.htm")
            saved = q.wait_for("ProtonOS TCP dziala", timeout=10)
            check("tresc z sieci zapisana na PFS i odczytana z dysku",
                  "<html>ProtonOS TCP dziala</html>" in saved)
            check("do pliku poszla sama tresc, bez naglowkow HTTP",
                  "Content-Length" not in saved)

            # PFS3: plik grubo ponad stary limit 16 KiB na plik
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /duza duza.htm" % http_port)
            big = q.wait_for("tresc ", timeout=90)
            # httpget podaje dlugosc samej tresci (bez naglowkow)
            got = re.search(r"tresc (\d+) B", big)
            check("duza strona pobrana w calosci (%d B tresci)" % len(BIG_PAGE),
                  got is not None and int(got.group(1)) == len(BIG_PAGE))
            q.type_line("fls")
            listing = q.wait_for("duza.htm", timeout=10)
            check("plik ponad stary limit 16 KiB zapisany na PFS",
                  "duza.htm  (%d B)" % len(BIG_PAGE) in listing)
            q.type_line("df")
            usage = q.wait_for("dysk: ", timeout=10)
            check("df pokazuje zajetosc dysku w blokach",
                  "bloki po 1024 B" in usage and "zajete" in usage)

            # Plik ponad dawny sufit 268 KiB: dane trafiaja do bloku
            # podwojnie posredniego, a potem czytamy je z powrotem
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /ogromna ogromna.htm"
                        % http_port)
            huge = q.wait_for("tresc ", timeout=180)
            check("plik ponad 268 KiB pobrany w calosci (%d B)"
                  % len(HUGE_PAGE),
                  "tresc %d B" % len(HUGE_PAGE) in huge)

            q.type_line("fls")
            listing = q.wait_for("ogromna.htm", timeout=15)
            check("zapisany na PFS z pelnym rozmiarem",
                  "ogromna.htm  (%d B)" % len(HUGE_PAGE) in listing)

            q.type_line("run sh")
            q.wait_for("ush:/", timeout=20)
            q.type_line("cat ogromna.htm | wc")
            back = q.wait_for("bajtow=", timeout=180)
            check("odczyt przez blok podwojnie posredni oddaje te same dane",
                  "wierszy=6500 bajtow=%d" % len(HUGE_PAGE) in back)
            q.type_line("exit")
            q.wait_for("ush: pa!", timeout=20)

            # HTTP/1.1: przekierowanie 302 i tresc w kawalkach (chunked) -
            # tak odpowiada wiekszosc prawdziwych serwerow
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /stara" % http_port)
            hop = q.wait_for("tresc ", timeout=30)
            check("klient idzie za przekierowaniem 302",
                  "przekierowanie -> /kawalki" in hop)
            check("tresc w kawalkach (chunked) zlozona poprawnie",
                  "HTTP 200 (chunked), tresc %d B" % len(PAGE) in hop)
            check("tresc po zlozeniu jest cala",
                  "ProtonOS TCP dziala" in hop)
            # HTTPS: pelne uzgodnienie TLS 1.3 z prawdziwym OpenSSL-em po
            # drugiej stronie (X25519 + ChaCha20-Poly1305, sprawdzone Finished)
            https_port, https_stop = start_https_server()
            if https_port is None:
                print("INFO: brak openssl - pomijam testy TLS")
            else:
                dns_stop = start_dns_server()
                try:
                    q.type_line("dns 10.0.2.2")
                    q.wait_for("serwer DNS ustawiony", timeout=10)

                    # 1. Certyfikat samopodpisany, a w systemie tylko
                    #    wbudowane korzenie publicznych CA: polaczenie ma
                    #    zostac zerwane
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    rej = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("obcy certyfikat odrzucony przez wbudowany "
                          "magazyn zaufania",
                          "brak zaufanego wystawcy" in rej)

                    # ... chyba ze swiadomie pominiemy sprawdzanie (-k)
                    q.type_line("clear")
                    q.type_line("run httpget -k https://protonos.test %d /"
                                % https_port)
                    sec = q.wait_for("tresc ", timeout=60)
                    check("TLS 1.3: uzgodnienie z serwerem OpenSSL",
                          "[httpget] TLS 1.3" in sec)
                    check("HTTPS: odpowiedz 200 z pelna trescia",
                          "HTTP 200, tresc %d B" % len(PAGE) in sec)
                    check("HTTPS: tresc odszyfrowana poprawnie",
                          "ProtonOS TCP dziala" in sec)
                    check("-k mowi wprost, ze tozsamosc nie jest sprawdzona",
                          "certyfikat NIESPRAWDZONY" in sec)

                    # 2. Pobranie certyfikatu wystawcy prosto na dysk
                    q.type_line("clear")
                    q.type_line("run httpget 10.0.2.2 %d /cert.der roots.der"
                                % http_port)
                    q.wait_for("tresc ", timeout=30)
                    q.type_line("fls")
                    check("magazyn zaufania zapisany na dysku",
                          "roots.der" in q.wait_for("roots.der", timeout=10))

                    # 3. Z magazynem: pelna weryfikacja lancucha, dat i nazwy
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    ver = q.wait_for("tresc ", timeout=60)
                    if "certyfikat sprawdzony" not in ver:
                        print("--- ekran (diagnostyka) ---")
                        print(ver)
                        print("---------------------------")
                    check("certyfikat sprawdzony wzgledem magazynu zaufania",
                          "certyfikat sprawdzony" in ver)
                    check("tresc pobrana po zweryfikowanym TLS",
                          "HTTP 200, tresc %d B" % len(PAGE) in ver)

                    # 4. Serwer z certyfikatem na krzywej P-256: caly
                    #    lancuch i CertificateVerify ida przez ECDSA
                    ec_port, ec_stop = start_https_server(ec=True)
                    if ec_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d /eccert.der "
                                        "roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % ec_port)
                            ecres = q.wait_for("tresc ", timeout=90)
                            check("certyfikat na krzywej P-256 sprawdzony "
                                  "(ECDSA)",
                                  "certyfikat sprawdzony" in ecres)
                            check("tresc pobrana po TLS z kluczem EC",
                                  "HTTP 200, tresc %d B" % len(PAGE) in ecres)
                        finally:
                            ec_stop()

                        # wroc do magazynu z certyfikatem RSA
                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 4b. Krzywa P-384 (podpisy SHA-384) - taka jest m.in.
                    #     ISRG Root X2
                    p384_port, p384_stop = start_https_server(ec=True,
                                                              p384=True)
                    if p384_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d "
                                        "/p384cert.der roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % p384_port)
                            p3 = q.wait_for("tresc ", timeout=120)
                            check("certyfikat na krzywej P-384 sprawdzony "
                                  "(ECDSA + SHA-384)",
                                  "certyfikat sprawdzony" in p3)
                        finally:
                            p384_stop()

                    # 5. Lancuch dwupoziomowy: ufamy tylko korzeniowi,
                    #    wiec system musi przejsc przez posrednika
                    ch_port, ch_stop = start_https_chain_server()
                    if ch_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d /rootca.der "
                                        "roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % ch_port)
                            chres = q.wait_for("tresc ", timeout=90)
                            check("lancuch przez posrednika domkniety do "
                                  "zaufanego korzenia",
                                  "certyfikat sprawdzony" in chres)
                        finally:
                            ch_stop()

                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 6. Ten sam serwer pod adresem IP - nazwa sie nie zgadza,
                    #    wiec polaczenie ma zostac zerwane
                    q.type_line("clear")
                    q.type_line("run httpget https://10.0.2.2 %d /" % https_port)
                    bad = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("zla nazwa w certyfikacie odrzucona",
                          "certyfikat nie obejmuje tej nazwy" in bad)
                finally:
                    https_stop()
                    dns_stop()

        finally:
            http_stop()

        q.type_line("netstat")
        check("netstat: po zamknieciu nie ma wiszacych polaczen",
              "brak polaczen TCP" in q.wait_for("polacze", timeout=10))

        q.type_line("ifconfig")
        ifc = q.wait_for("RX ")
        # Na ekranie wisi tez wyjscie poprzedniego 'ifconfig' (sprzed
        # pinga) - liczy sie ostatnie, najswiezsze wystapienie
        rx = re.findall(r"RX (\d+) pakietow, TX (\d+) pakietow", ifc)
        check(f"licznik pakietow RX/TX rosnie (ostatni: {rx[-1] if rx else None})",
              bool(rx) and int(rx[-1][0]) > 0 and int(rx[-1][1]) > 0)

        # Mysz PS/2: ruch (delty wzgledne) i przycisk, czytane komenda 'mouse'.
        # Start na srodku 320x200 = (160,100); QEMU przekazuje delty 1:1.
        q.mouse_move(40, 30)
        q.type_line("mouse")
        check("mysz reaguje na ruch (160,100)+(40,30) -> (200,130)",
              "mysz: x=200 y=130" in q.wait_for("mysz: x=200 y=130"))
        q.mouse_move(-80, -50)
        q.mouse_button(True)
        q.type_line("mouse")
        check("mysz: kolejny ruch i wcisniety LPM (120,80 b1)",
              "mysz: x=120 y=80 przyciski=1" in
              q.wait_for("mysz: x=120 y=80 przyciski=1"))
        q.mouse_button(False)

        # Shell uzytkownika: sys_read przejmuje konsole
        q.type_line("run sh")
        q.wait_for("ush - shell uzytkownika")
        q.wait_for("ush:/")  # prompt pokazuje katalog roboczy (root = /)
        check("ush wystartowal w ring 3", True)

        q.type_line("pid")
        check("ush zna swoj pid (sys_getpid)", "moj pid to" in q.wait_for("moj pid to"))

        q.type_line("hello")
        q.wait_for("[hello] koncze sie grzecznie")
        check("ush odpalil hello przez spawn+wait", True)

        q.type_line("nieistnieje")
        check("ush zglasza nieznany program",
              "ush: nie znam 'nieistnieje'" in q.wait_for("ush: nie znam"))

        # Przekierowanie wyjscia: 'echo ... > plik' (dziedziczenie fd 1)
        q.type_line("echo przekierowanie-ok > redir.txt")
        screen = q.wait_for("wyjscie zapisane do 'redir.txt'")
        # echo nie powinno wypisac tekstu na konsole - poszedl do pliku
        idx = screen.rfind("redir.txt")
        check("echo z '>' nie pisze na konsole (wyjscie do pliku)",
              "przekierowanie-ok" not in screen.split("echo przekierowanie-ok")[-1]
              .split("wyjscie zapisane")[0])
        # ...a 'cat' tego pliku pokazuje zapisana tresc
        q.type_line("cat redir.txt")
        check("przekierowane wyjscie trafilo do pliku (cat redir.txt)",
              "przekierowanie-ok" in q.wait_for("przekierowanie-ok"))

        # Potok: 'echo proton-pipe | wc' - dwa procesy rownolegle,
        # echo pisze do potoku, wc czyta do EOF i liczy ('proton-pipe\n'
        # = 11 znakow + nowa linia = 12 bajtow, 1 wiersz)
        q.type_line("echo proton-pipe | wc")
        screen = q.wait_for("wc: wierszy=")
        check("potok laczy dwa procesy (echo | wc): poprawny licznik",
              "wc: wierszy=1 bajtow=12" in screen)

        # seq jako producent wielu wierszy: 'seq 5 | wc' -> 5 wierszy,
        # 10 bajtow (1\n2\n3\n4\n5\n)
        q.type_line("seq 5 | wc")
        check("seq przez potok do wc: 5 wierszy, 10 bajtow",
              "wc: wierszy=5 bajtow=10" in q.wait_for("wc: wierszy=5"))

        # grep filtruje w potoku; wynik mierzymy przez plik + wc, bo
        # samo '7' na ekranie myli sie z echem wpisanej komendy.
        # seq 9 | grep 7 -> tylko '7' (1 wiersz, 2 bajty)
        q.type_line("seq 9 | grep 7 > g.txt")
        time.sleep(0.5)
        q.type_line("cat g.txt | wc")
        check("grep w potoku przepuszcza tylko pasujace wiersze",
              "wc: wierszy=1 bajtow=2" in q.wait_for("wc: wierszy=1 bajtow=2"))

        # head ucina do N wierszy: seq 20 | head 3 -> 3 wiersze, 6 bajtow
        q.type_line("seq 20 | head 3 > h.txt")
        time.sleep(0.5)
        q.type_line("cat h.txt | wc")
        check("head ogranicza potok do N wierszy",
              "wc: wierszy=3 bajtow=6" in q.wait_for("wc: wierszy=3 bajtow=6"))

        # Potok 3-stopniowy: seq 20 | grep 1 | wc. Liczby 1..20 z cyfra
        # '1' to 1,10..19 = 11 wierszy; bajty 2 + 10*3 = 32
        q.type_line("seq 20 | grep 1 | wc")
        check("potok 3-stopniowy (seq | grep | wc)",
              "wc: wierszy=11 bajtow=32" in q.wait_for("wc: wierszy=11"))

        # Potok 4-stopniowy z przekierowaniem: seq 30 | grep 2 | head 3 > p.txt
        # liczby z '2': 2,12,20,...; pierwsze 3 -> "2\n12\n20\n" = 3w/8B
        q.type_line("seq 30 | grep 2 | head 3 > p.txt")
        time.sleep(0.5)
        q.type_line("cat p.txt | wc")
        check("potok 4-stopniowy z przekierowaniem do pliku",
              "wc: wierszy=3 bajtow=8" in q.wait_for("wc: wierszy=3 bajtow=8"))

        # ls/rm jako programy ring 3. 'echo xyz > usuwany' -> "xyz\n" (4 B);
        # ls pokazuje "usuwany  4 B" (rozmiar nie wystepuje w wpisanej
        # komendzie, wiec to jednoznaczny dowod)
        q.type_line("echo xyz > usuwany")
        time.sleep(0.5)
        q.type_line("ls")
        check("ls (ring 3) listuje plik z rozmiarem",
              "usuwany  4 B" in q.wait_for("usuwany  4 B"))
        # po rm plik znika - sprawdzamy deterministycznie przez potok
        q.type_line("rm usuwany")
        time.sleep(0.5)
        q.type_line("ls | grep usuwany | wc")
        check("rm usunal plik (ls juz go nie pokazuje)",
              "wc: wierszy=0 bajtow=0" in q.wait_for("wc: wierszy=0 bajtow=0"))

        # Kody wyjscia: grep bez dopasowan konczy sie kodem 1, a kod
        # ostatniego etapu potoku wraca z wait do shella
        q.type_line("seq 5 | grep 9")
        check("kod wyjscia z potoku (grep bez dopasowan -> kod 1)",
              "ush: [kod 1]" in q.wait_for("ush: [kod 1]"))

        # Proces ubity wyjatkiem CPU zwraca kod -1
        q.type_line("evil")
        check("kod wyjscia procesu ubitego wyjatkiem (-1)",
              "ush: [kod -1]" in q.wait_for("ush: [kod -1]"))

        # RTC: 'date' (ring 3) czyta zegar CMOS; QEMU ma stala baze.
        # Sprawdzamy tylko date (godzina przesunie sie przez czas testu),
        # plus format 'godz:min:sek' (dwie cyfry kazda)
        q.type_line("date")
        screen = q.wait_for(RTC_DATE)
        m = re.search(RTC_DATE + r" (\d\d):(\d\d):(\d\d)", screen)
        check("date (ring 3) czyta RTC: data i format z bazy QEMU",
              m is not None)

        # Dopisywanie '>>': apf = "appendtest\n"(11B) + "zz\n"(3B) = 14B/2w.
        # Gdyby '>>' obcinalo jak '>', bylby tylko "zz\n" (3B/1w).
        q.type_line("echo appendtest > apf")
        time.sleep(0.4)
        q.type_line("echo zz >> apf")
        time.sleep(0.4)
        q.type_line("cat apf | wc")
        check("dopisywanie '>>' nie obcina pliku (14 B, 2 wiersze)",
              "wc: wierszy=2 bajtow=14" in q.wait_for("wc: wierszy=2 bajtow=14"))

        # cp tworzy NIEZALEZNA kopie: dopisanie do kopii (+ "more\n" 5B)
        # daje 19B, nie ruszajac oryginalu
        q.type_line("cp apf cpf")
        time.sleep(0.4)
        q.type_line("echo more >> cpf")
        time.sleep(0.4)
        q.type_line("cat cpf | wc")
        check("cp tworzy niezalezna kopie pliku (19 B, 3 wiersze)",
              "wc: wierszy=3 bajtow=19" in q.wait_for("wc: wierszy=3 bajtow=19"))

        # mv przenosi: zrodlo znika, cel ma tresc
        q.type_line("mv cpf mvf")
        time.sleep(0.4)
        q.type_line("cat cpf")
        check("mv usunal plik zrodlowy",
              "nie moge otworzyc 'cpf'" in q.wait_for("nie moge otworzyc 'cpf'"))
        q.type_line("echo end >> mvf")
        time.sleep(0.4)
        q.type_line("cat mvf | wc")
        check("mv zachowal tresc w pliku docelowym (23 B, 4 wiersze)",
              "wc: wierszy=4 bajtow=23" in q.wait_for("wc: wierszy=4 bajtow=23"))

        # Katalogi: mkdir + cd (prompt pokazuje sciezke roboczy)
        q.type_line("mkdir d1")
        time.sleep(0.4)
        q.type_line("cd d1")
        check("cd wchodzi do podkatalogu (prompt = sciezka robocza)",
              "ush:/d1>" in q.wait_for("ush:/d1>"))
        # plik tworzony w podkatalogu (wzgledem cwd); "hejka\n" = 6 B
        q.type_line("echo hejka > f1")
        time.sleep(0.4)
        q.type_line("cd ..")
        time.sleep(0.4)
        # podkatalog widoczny w katalogu glownym jako 'd1/'
        q.type_line("ls")
        check("ls pokazuje podkatalog ze slashem (d1/)",
              "d1/" in q.wait_for("d1/"))
        # plik z podkatalogu osiagalny sciezka wzgledna; nie ma go w root
        q.type_line("cat d1/f1 | wc")
        check("sciezka wzgledna do pliku w podkatalogu (6 B, 1 wiersz)",
              "wc: wierszy=1 bajtow=6" in q.wait_for("wc: wierszy=1 bajtow=6"))

        # Tlo ('&') + kill: spin dziala w tle, kill go konczy
        q.type_line("spin &")
        sbg = q.wait_for("w tle, pid=")
        pidm = re.search(r"w tle, pid=(\d+)", sbg)
        check("uruchomienie w tle ('&') zwraca pid", pidm is not None)
        time.sleep(1.0)
        q.type_line("kill " + pidm.group(1))
        check("kill konczy proces w tle (komunikat 'zabity')",
              "zabity (kod -9)" in q.wait_for("zabity (kod -9)"))
        # po zabiciu spin nie produkuje juz znacznikow
        base = q.screen().count("[spin] tik")
        time.sleep(1.6)
        check("zabity proces przestal dzialac (brak nowych tikow)",
              q.screen().count("[spin] tik") == base)

        q.type_line("exit")
        q.wait_for("ush: pa!")
        check("ush wyszedl przez sys_exit", True)

        # Konsola wraca do shella jadra
        before = len(re.findall(r"tyknac timera", q.screen()))
        q.type_line("uptime")
        time.sleep(0.3)
        after = len(re.findall(r"tyknac timera", q.screen()))
        check("konsola wrocila do shella jadra po smierci ush",
              after > before or "tyknac timera" in q.screen() and before == 0)

        # System musi zyc dalej: liczniki rosna, shell odpowiada
        a3, _ = counters(q.screen())
        time.sleep(1)
        a4, _ = counters(q.screen())
        check(f"system zyje po ubiciu procesu ({a3} -> {a4})",
              a3 is not None and a4 is not None and a4 > a3)
        q.type_line("uptime")
        check("shell odpowiada po ubiciu procesu",
              "tyknac timera" in q.screen())

        # --- System plikow na dysku (PFS) ---
        # Proces ring 3 pisze i czyta plik syscallami
        q.type_line("run fileio")
        screen = q.wait_for("[fileio] OK")
        check("proces ring3 zapisal i odczytal plik z dysku",
              "odczytano z dysku: plik zapisany przez proces ring3" in screen)

        # Shell jadra: write -> cat (zapis i odczyt w obrebie jednego boota)
        q.type_line("write greeting witaj-z-dysku-pfs")
        check("zapis pliku z shella jadra",
              "zapisano" in q.wait_for("zapisano"))
        q.type_line("cat greeting")
        check("odczyt zapisanego pliku (cat)",
              "witaj-z-dysku-pfs" in q.wait_for("witaj-z-dysku-pfs"))

        # fls pokazuje oba pliki
        q.type_line("fls")
        screen = q.wait_for("Katalog glowny")
        check("fls listuje pliki na dysku",
              "greeting" in screen and "ring3.txt" in screen)

        # cat nieistniejacego pliku
        q.type_line("cat niema")
        check("cat zglasza brak pliku",
              "brak pliku 'niema'" in q.wait_for("brak pliku"))

        # Program 'cat' w ring 3: czyta plik przez fd, pisze na fd 1
        # (zunifikowany model I/O: read pliku -> write na konsole).
        # Czyscimy ekran, by nie zlapac starego echa 'witaj-z-dysku-pfs'.
        q.type_line("clear")
        time.sleep(0.3)
        q.type_line("run cat greeting")
        check("program ring3 'cat' czyta plik przez fd i pisze na fd 1",
              "witaj-z-dysku-pfs" in q.wait_for("witaj-z-dysku-pfs",
                                                timeout=20))

        # Na koncu: crash powinien wywolac page fault z diagnostyka
        q.type_line("crash")
        screen = q.screen()
        check("page fault obsluzony (wyjatek 14)",
              "WYJATEK CPU 14: Page Fault" in screen)
        check("CR2 wskazuje felerny adres",
              "(CR2): e0000000" in screen)
        check("system zatrzymany po wyjatku", "System zatrzymany" in screen)
    finally:
        q.close()

    # --- ProtonOS jako serwer TCP (pasywne otwarcie, osobny boot) ---
    print("--- siec: serwer TCP w ring 3 ---")
    srv_port = 5555
    q_srv = Qemu(KERNEL, INITRD, hostfwd_port=srv_port, append=TEXT_MODE)
    try:
        q_srv.wait_for("ProtonOS shell")
        q_srv.wait_for("[siec] DHCP", timeout=20)
        q_srv.type_line("run tcpserv")
        q_srv.wait_for("nasluchuje na porcie 5555", timeout=15)
        check("ring 3 nasluchuje na porcie TCP", True)

        # Laczy sie prawdziwy stos hosta (przez hostfwd QEMU)
        sock = socket.create_connection(("127.0.0.1", srv_port), timeout=15)
        sock.settimeout(15)
        greeting = sock.recv(200)
        check("host polaczyl sie i dostal powitanie od ProtonOS-a",
              b"ProtonOS TCP gotowy" in greeting)

        sock.sendall(b"czesc z hosta\n")
        echoed = sock.recv(200)
        check("dane z hosta wrocily odbite przez serwer w ring 3",
              b"czesc z hosta" in echoed)
        sock.close()

        seen = q_srv.wait_for("od klienta", timeout=15)
        check("serwer zobaczyl dane klienta",
              "od klienta: czesc z hosta" in seen)
    finally:
        q_srv.close()

    # --- Przegladarka w oknie pulpitu (siec -> GUI -> dysk) ---
    print("--- pulpit: przegladarka (gweb) ---")
    wdisk = tempfile.mktemp(suffix=".img")
    with open(wdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    http_port, http_stop = start_http_server()
    dns_stop = start_dns_server()
    q_web = Qemu(KERNEL, INITRD, wdisk)
    try:
        q_web.wait_desktop()
        time.sleep(2.0)                  # chwila na dzierzawe DHCP

        # Serwer DNS hosta: przegladarka bedzie szukac adresu po nazwie.
        # Komenda idzie do shella jadra - w trybie graficznym slucha on
        # klawiatury, dopoki zadne okno nie ma fokusu.
        q_web.type_line("dns 10.0.2.2")
        time.sleep(0.5)
        time.sleep(1.0)
        q_web.type_line("run gweb")
        time.sleep(1.5)

        def page_white():
            _, _, a = q_web.screen_raw()
            return sum(1 for y in range(100, 250) for x in range(96, 400)
                       if a(x, y) == (255, 255, 255))

        before = page_white()

        # Kalibracja kursora - klikniecia w odnosniki musza trafiac co do
        # piksela, a delty myszy licza sie od biezacego polozenia
        q_web.mouse_home()

        # Klik w okno daje mu fokus klawiatury; potem kursor na bok
        q_web.mouse_goto(150, 120)
        q_web.mouse_click()
        q_web.mouse_goto(400, 350)
        time.sleep(0.3)

        for _ in range(20):              # skasuj domyslny adres
            q_web.sendkey("backspace")
        for ch in "protonos.test:%d/" % http_port:   # adres po nazwie
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")

        shown = False
        for _ in range(25):
            if page_white() > before + 50:
                shown = True
                break
            time.sleep(0.4)
        check("przegladarka rozwiazala nazwe, pobrala strone i "
              "narysowala tekst", shown)

        # --- przewijanie i odnosniki ---
        for _ in range(40):              # nowy adres: strona z odnosnikiem
            q_web.sendkey("backspace")
        for ch in "protonos.test:%d/linki" % http_port:
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")
        time.sleep(5)

        def link_pixels():
            """Odnosniki rysowane sa kolorem 5 z palety (zolty)."""
            _, _, a = q_web.screen_raw()
            return [(x, y) for y in range(180, 260) for x in range(96, 420)
                    if a(x, y) == (255, 255, 0)]

        first_view = page_white()
        q_web.sendkey("pgdn")
        time.sleep(1.0)
        check("PgDn przewija tresc w oknie", page_white() != first_view)

        found = []
        for _ in range(8):               # zjedz do konca, gdzie jest odnosnik
            found = link_pixels()
            if found:
                break
            q_web.sendkey("pgdn")
            time.sleep(0.8)
        check("odnosnik wyrozniony kolorem w tekscie", bool(found))

        if found:
            # Odnosnik potrafi zajmowac dwa wiersze - srodek ciezkosci
            # wszystkich pikseli wypadlby wtedy w przerwie miedzy nimi.
            # Bierzemy najgestszy wiersz i jego srodek.
            rows = {}
            for x, y in found:
                rows.setdefault(y, []).append(x)
            best_y = max(rows, key=lambda y: len(rows[y]))
            xs = sorted(rows[best_y])
            cx, cy = xs[len(xs) // 2], best_y
            q_web.mouse_goto(cx, cy)
            q_web.mouse_click()
            q_web.mouse_goto(560, 400)
            time.sleep(5)
    finally:
        q_web.close()
        http_stop()
        dns_stop()

    # Ten sam dysk w trybie tekstowym: tresc ze swiata przezyla restart
    q_web2 = Qemu(KERNEL, INITRD, wdisk, append=TEXT_MODE)
    try:
        q_web2.wait_for("ProtonOS shell")
        q_web2.type_line("cat strona.htm")
        saved = q_web2.wait_for("Strona druga", timeout=10)
        check("klikniety odnosnik pobral druga strone (widac ja na dysku)",
              "Odnosnik zadzialal" in saved)
        check("strona zapisana przez przegladarke przezyla restart",
              "<html><body><h1>Strona druga</h1>" in saved)
    finally:
        q_web2.close()
        if os.path.exists(wdisk):
            os.unlink(wdisk)

    # --- Persystencja: drugi boot na TYM SAMYM dysku ---
    print("--- drugi boot (ten sam dysk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        screen = q2.wait_for("ProtonOS shell")
        check("drugi boot: dysk wykryty (juz sformatowany, bez ponownego format)",
              "+ PFS gotowy" in screen
              and "sformatowany" not in screen)

        q2.type_line("fls")
        screen = q2.wait_for("Katalog glowny")
        check("pliki przetrwaly restart (fls)",
              "greeting" in screen and "ring3.txt" in screen)

        q2.type_line("cat greeting")
        check("zawartosc pliku przetrwala restart (cat)",
              "witaj-z-dysku-pfs" in q2.wait_for("witaj-z-dysku-pfs"))

        q2.type_line("cat ring3.txt")
        check("plik zapisany przez ring3 przetrwal restart",
              "plik zapisany przez proces ring3" in q2.wait_for("ring3"))

        # Duzy plik z sieci: i-wezel z blokiem posrednim po restarcie
        q2.type_line("fls")
        listing = q2.wait_for("duza.htm", timeout=10)
        check("duzy plik (>100 KiB) przetrwal restart z pelnym rozmiarem",
              "duza.htm  (%d B)" % len(BIG_PAGE) in listing)
        q2.type_line("df")
        usage = q2.wait_for("dysk: ", timeout=10)
        check("zajetosc dysku odtworzona z bitmapy po restarcie",
              "zajete" in usage and "zajete 0 KiB" not in usage)
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    # --- Tryb graficzny (osobny boot: 'gfx' jest jednokierunkowy) ---
    print("--- tryb graficzny (screendump) ---")
    q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q3.wait_for("ProtonOS shell")
        q3.type_line("gfx")
        time.sleep(1.0)
        sx, sy, at = q3.screen_pixels()
        check(f"tryb graficzny aktywny (ekran skalowany {sx}x{sy})",
              sx >= 1 and sy >= 1)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("framebuffer: tlo niebieskie (fb 160,180)",
              near(at(160, 180), (0, 0, 255)))
        check("framebuffer: czerwony prostokat na srodku (fb 160,100)",
              near(at(160, 100), (255, 0, 0)))
        check("framebuffer: zielony prostokat w rogu (fb 30,30)",
              near(at(30, 30), (0, 255, 0)))
        check("framebuffer: zolty prostokat w rogu (fb 290,30)",
              near(at(290, 30), (255, 255, 0)))
    finally:
        q3.close()

    # --- Pulpit graficzny: kursor myszy (osobny boot) ---
    print("--- pulpit: kursor myszy ---")
    q4 = Qemu(KERNEL, INITRD)
    try:
        q4.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Pulpit dziala w wysokiej rozdzielczosci VESA (640x480)
        gw, gh, _ = q4.screen_raw()
        check(f"pulpit w trybie VESA wysokiej rozdzielczosci ({gw}x{gh})",
              gw == 640 and gh == 480)

        # mysz startuje na srodku (160,100); przesun do ~(50,40)
        q4.mouse_move(-110, -60)
        check("kursor rysowany w pozycji myszy (bialy ~50,41)",
              q4.wait_pixel(50, 41, (255, 255, 255)))
        _, _, at = q4.screen_raw()
        check("tlo pulpitu (teal) z dala od kursora, ikon i okien",
              near(at(350, 200), (0, 128, 160)))
        # przesun w inne miejsce - kursor podaza
        q4.mouse_move(150, 80)
        check("kursor podaza za mysza (bialy ~200,121)",
              q4.wait_pixel(200, 121, (255, 255, 255)))
        check("poprzednia pozycja kursora wyczyszczona (teal ~50,41)",
              q4.wait_pixel(50, 41, (0, 128, 160)))

        # Tekst (czcionka 8x8): pasek tytulu z napisem "ProtonOS".
        # Pasek jasnoszary; glif czarny -> komorka litery ma i czarne,
        # i szare piksele (czyli to ksztalt, nie pelny prostokat).
        _, _, at = q4.screen_raw()
        check("pasek tytulu narysowany (jasnoszary, puste miejsce)",
              near(at(200, 4), (192, 192, 192)))
        cell = [at(2 + c, 1 + r) for r in range(8) for c in range(8)]
        blk = sum(1 for p in cell if near(p, (0, 0, 0)))
        gry = sum(1 for p in cell if near(p, (192, 192, 192)))
        check(f"tekst renderowany czcionka 8x8 (glif 'P': {blk} czarnych, "
              f"{gry} szarych)", blk > 5 and gry > 5)
    finally:
        q4.close()

    # --- Pulpit: okna i przeciaganie mysza (osobny boot) ---
    print("--- pulpit: okna + drag ---")
    q5 = Qemu(KERNEL, INITRD)
    try:
        q5.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Okna: win0 (40,60,120x70) i win1 (180,95). Mysz startuje (160,100).
        check("okno 0: pasek tytulu niebieski (80,64)",
              q5.wait_pixel(80, 64, (0, 0, 255)))
        _, _, at = q5.screen_raw()
        check("okno 0: tlo szare (80,100)",
              near(at(80, 100), (224, 224, 224)))
        check("okno 1: pasek tytulu niebieski (220,99)",
              near(at(220, 99), (0, 0, 255)))

        # Przeciaganie okna 0: kursor (160,100) -> pasek win0 (60,64),
        # wcisnij LPM, przesun (+80,+50), pusc. Okno -> (120,110).
        q5.mouse_move(-100, -36)
        q5.mouse_button(True)
        q5.mouse_move(80, 50)
        q5.mouse_button(False)
        # nowy pasek win0 w widocznej (niezaslonietej) czesci: (150,114)
        check("okno 0 przeciagniete na nowa pozycje (pasek ~150,114)",
              q5.wait_pixel(150, 114, (0, 0, 255)))
        _, _, at = q5.screen_raw()
        check("stare miejsce okna 0 wyczyszczone (teal ~60,90)",
              near(at(60, 90), (0, 128, 160)))
        check("okno 1 nieruszone podczas przeciagania (220,99)",
              near(at(220, 99), (0, 0, 255)))
    finally:
        q5.close()

    # --- Pulpit: klikalne kontrolki (przycisk akcji + zamkniecie) ---
    print("--- pulpit: kontrolki (klik) ---")
    q6 = Qemu(KERNEL, INITRD)
    try:
        q6.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 (40,60,120x70): przycisk [x] czerwony w prawym rogu paska
        check("przycisk zamkniecia [x] (czerwony, ~153,63)",
              q6.wait_pixel(153, 63, (255, 0, 0)))
        _, _, at = q6.screen_raw()

        # Licznik przy przycisku 'Klik' (komorka ~94,80). Klikamy 3x.
        cell_before = [at(94 + c, 80 + r) for r in range(8) for c in range(8)]
        q6.mouse_goto(68, 84)            # srodek przycisku 'Klik'
        for _ in range(3):
            q6.mouse_click()
        time.sleep(0.6)
        _, _, at = q6.screen_raw()
        cell_after = [at(94 + c, 80 + r) for r in range(8) for c in range(8)]
        white_after = sum(1 for p in cell_after if near(p, (255, 255, 255), 10))
        check("przycisk akcji reaguje na klik (licznik zmienil cyfre)",
              cell_before != cell_after and white_after > 0)

        # Zamkniecie okna: klik [x] okna 1 (close box ~293,99)
        q6.mouse_goto(293, 99)
        q6.mouse_click()
        check("klik [x] zamyka okno (pasek win1 znika, wraca tlo)",
              q6.wait_pixel(220, 99, (0, 128, 160)))
        _, _, at = q6.screen_raw()
        # okno 0 nadal jest (klik nie ruszyl innych okien)
        check("inne okno pozostaje otwarte po zamknieciu (win0 pasek 80,64)",
              near(at(80, 64), (0, 0, 255)))
    finally:
        q6.close()

    # --- Aplikacja ring 3 z wlasnym oknem (rysowanie przez syscalle) ---
    print("--- pulpit: aplikacja ring 3 ---")
    q7 = Qemu(KERNEL, INITRD)
    try:
        q7.wait_desktop()
        time.sleep(0.8)
        q7.type_line("run guiapp")
        time.sleep(1.2)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # guiapp -> slot 2 (sloty 0/1 to okna jadra): okno (94,54),
        # obszar treści od (95,65). Kwadraty RGB rysowane przez syscalle.
        check("aplikacja ring3 narysowala czerwony kwadrat (101,71)",
              q7.wait_pixel(101, 71, (255, 0, 0)))
        check("aplikacja ring3 narysowala niebieski kwadrat (170,75)",
              q7.wait_pixel(170, 75, (0, 0, 255)))

        # Zdarzenia: klik w cialo okna trafia do aplikacji (win_poll),
        # ktora rysuje bialy znacznik w miejscu klikniecia. Licznik
        # 'klikniec:' przy (175,89) rosnie. Kursor odsuwamy, by nie
        # zaslonil znacznika przy zrzucie.
        cnt_before = [at(175 + c, 89 + r) for r in range(8) for c in range(8)]
        q7.mouse_goto(150, 110)
        q7.mouse_click()
        q7.mouse_goto(60, 140)            # odsun kursor od znacznika
        check("klik dostarczony do aplikacji: znacznik w miejscu (149,109)",
              q7.wait_pixel(149, 109, (255, 255, 255)))
        _, _, at = q7.screen_raw()
        cnt_after = [at(175 + c, 89 + r) for r in range(8) for c in range(8)]
        check("aplikacja zliczyla klikniecie (licznik zmienil cyfre)",
              cnt_before != cnt_after)

        # Klawiatura do okna z fokusem: okno jest sfokusowane (klikniete
        # wyzej). Po wpisaniu znakow pole 'txt:' (~135,103) pokazuje tekst.
        for k in ("a", "b", "c"):
            q7.sendkey(k)

        def txt_white():
            _, _, a = q7.screen_raw()
            return sum(1 for x in range(135, 159) for y in range(103, 111)
                       if near(a(x, y), (255, 255, 255), 10))

        appeared = False
        for _ in range(15):          # poll: tekst pojawia sie po kilku klatkach
            if txt_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("klawiatura trafia do okna z fokusem (tekst pojawil sie)",
              appeared)

        # Klik w pulpit (puste tlo) zdejmuje fokus, by 'kill' trafilo
        # do shella jadra, a nie do okna aplikacji
        q7.mouse_goto(60, 140)
        q7.mouse_click()
        time.sleep(0.2)

        # Zabicie aplikacji: kompozytor sprzata jej okno. Zabijamy po
        # nazwie - pid zalezy od tego, ile watkow wystartowalo wczesniej
        q7.type_line("kill guiapp")
        check("po zabiciu aplikacji jej okno zostaje sprzatniete (170,75 teal)",
              q7.wait_pixel(170, 75, (0, 128, 160)))
    finally:
        q7.close()

    # --- Pasek zadan i menu uruchamiania (osobny boot) ---
    print("--- pulpit: pasek zadan + menu ---")
    q8 = Qemu(KERNEL, INITRD)
    try:
        q8.wait_desktop()

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Pasek zadan na dole (640x480 -> y=468); przycisk 'Menu' x2..42
        check("pasek zadan: przycisk 'Menu' (niebieski, 30,470)",
              q8.wait_pixel(30, 470, (0, 0, 255)))
        _, _, at = q8.screen_raw()
        # przed uruchomieniem aplikacji slot 2 jest pusty (tlo okna jadra)
        check("przed launchem brak okna aplikacji (101,71 nie czerwone)",
              not near(at(101, 71), (255, 0, 0)))

        # Wpisy menu liczone z MENU_ITEMS (patrz menu_y)
        q8.mouse_goto(10, 472)
        q8.mouse_click()
        check("klik 'Menu' rozwija menu (wpis szary)",
              q8.wait_pixel(70, menu_y("guiapp"), (224, 224, 224)))

        # Klik wpisu 'guiapp' (entry0) -> uruchamia aplikacje
        q8.mouse_goto(40, menu_y("guiapp"))
        q8.mouse_click()
        check("wybor z menu uruchamia aplikacje (czerwony kwadrat 101,71)",
              q8.wait_pixel(101, 71, (255, 0, 0)))
        _, _, at = q8.screen_raw()
        check("menu zamyka sie po wyborze (wpis znika)",
              not near(at(70, menu_y("guiapp")), (224, 224, 224)))

        # Lista okien na pasku zadan: przyciski Terminal/Pliki/guiapp.
        # guiapp jest na wierzchu (swiezo uruchomione) - czerwony przy (101,71).
        # Klik przycisku 'Terminal' (slot0, j0, x46..96, y468) wynosi je na
        # wierzch i fokusuje: czerwony guiappa zostaje zakryty oknem jadra.
        q8.mouse_goto(60, 473)
        q8.mouse_click()
        check("klik przycisku na pasku wynosi okno na wierzch (czerwony zakryty)",
              q8.wait_pixel(101, 71, (224, 224, 224), 15))
        check("fokusowane okno podswietlone na pasku zadan (niebieski 90,469)",
              q8.wait_pixel(90, 469, (0, 0, 255)))
    finally:
        q8.close()

    # --- Menedzer plikow GUI: FS -> okno aplikacji (osobny boot z dyskiem) ---
    print("--- pulpit: menedzer plikow (gfiles) ---")
    gdisk = tempfile.mktemp(suffix=".img")
    with open(gdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q9 = Qemu(KERNEL, INITRD, gdisk)
    try:
        q9.wait_desktop()
        q9.type_line("write raport abc")   # plik na dysku (przed pulpitem)
        time.sleep(0.5)
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Menu -> wpis 'gfiles'
        q9.mouse_goto(10, 472)
        q9.mouse_click()
        q9.mouse_goto(40, menu_y("gfiles"))
        q9.mouse_click()
        # gfiles slot2 (94,54), content (95,65); nazwa pliku przy
        # content(8,16) -> screen(103,81). Tekst (bialy) = pliki z PFS.
        def files_row_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(103, 160) for y in range(81, 89)
                       if near(a(x, y), (255, 255, 255), 10))
        appeared = False
        for _ in range(15):
            if files_row_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("menedzer plikow GUI listuje plik z dysku (FS -> okno)",
              appeared)

        # Klik nazwy pliku -> podglad tresci. Wiersz0 przy content(.,16)
        # -> ekran y81, kolejne co 10 px. Klikamy WIERSZ 1: wiersz 0 to
        # 'swap.sys', ktory jadro zaklada przy starcie (i-wezel 0), a nam
        # chodzi o 'raport'. Po kliknieciu panel 'tresc:' (content y102 ->
        # ekran y167) pokazuje zawartosc. Kursor odsuwamy przed zrzutem.
        q9.mouse_goto(115, 93)
        q9.mouse_click()
        q9.mouse_goto(60, 300)

        def preview_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(99, 200) for y in range(167, 175)
                       if near(a(x, y), (255, 255, 255), 10))
        prev = False
        for _ in range(15):
            if preview_white() > 0:
                prev = True
                break
            time.sleep(0.2)
        check("klik pliku w gfiles pokazuje jego tresc (open+read z ring3)",
              prev)
    finally:
        q9.close()
        if os.path.exists(gdisk):
            os.unlink(gdisk)

    # --- Ikony na pulpicie (osobny boot) ---
    print("--- pulpit: ikony ---")
    q10 = Qemu(KERNEL, INITRD)
    try:
        q10.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # ikona 'App' (kafelek 8,20,32x24) pomaranczowa
        check("ikona pulpitu narysowana (pomaranczowy kafelek 20,30)",
              q10.wait_pixel(20, 30, (255, 162, 0)))
        # klik ikony uruchamia aplikacje (guiapp -> czerwony kwadrat)
        q10.mouse_goto(24, 32)
        q10.mouse_click()
        q10.mouse_goto(300, 300)         # odsun kursor
        check("klik ikony uruchamia aplikacje (czerwony kwadrat 101,71)",
              q10.wait_pixel(101, 71, (255, 0, 0)))
    finally:
        q10.close()

    # --- Wskaznik zasobow (gmon) na zywo ---
    print("--- pulpit: monitor zasobow (gmon) ---")
    q11 = Qemu(KERNEL, INITRD)
    try:
        q11.wait_desktop()
        time.sleep(0.8)
        q11.type_line("run gmon")

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # gmon slot2 (94,54), content (95,65). Pasek RAM (content y48)
        # zawiera niebieski fragment uzycia -> ekran ~ (101,116).
        check("monitor zasobow: pasek RAM (niebieski 101,116)",
              q11.wait_pixel(101, 116, (0, 0, 255)))

        # uptime (liczba przy content(96,28) -> ekran (191,93)) rosnie:
        # komorka tekstu zmienia sie w czasie -> zywy monitor
        def up_cell():
            _, _, a = q11.screen_raw()
            return [a(191 + c, 93 + r) for r in range(8) for c in range(24)]
        c1 = up_cell()
        time.sleep(1.6)
        c2 = up_cell()
        check("monitor zasobow zywy: uptime aktualizuje sie w oknie",
              c1 != c2)
    finally:
        q11.close()

    # --- Terminal w oknie GUI (osobny boot z dyskiem) ---
    print("--- pulpit: terminal GUI (gterm) ---")
    tdisk = tempfile.mktemp(suffix=".img")
    with open(tdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q12 = Qemu(KERNEL, INITRD, tdisk)
    try:
        q12.wait_desktop()
        time.sleep(0.8)
        q12.type_line("run gterm")
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # klik w okno terminala (slot2, content 95,65) -> fokus klawiatury
        q12.mouse_goto(150, 120)
        q12.mouse_click()
        q12.mouse_goto(400, 300)
        # wpisz 'seq 3' i Enter -> wyjscie (1,2,3) pojawia sie w oknie
        for ch in "seq 3":
            q12.sendkey({" ": "spc"}.get(ch, ch))
        q12.sendkey("ret")

        # wyjscie rysowane u gory okna (content y4 -> ekran y69)
        def out_white():
            _, _, a = q12.screen_raw()
            return sum(1 for x in range(99, 130) for y in range(69, 99)
                       if near(a(x, y), (255, 255, 255), 10))
        shown = False
        for _ in range(20):
            if out_white() > 0:
                shown = True
                break
            time.sleep(0.2)
        check("terminal GUI uruchamia komende i pokazuje wyjscie w oknie",
              shown)
    finally:
        q12.close()
        if os.path.exists(tdisk):
            os.unlink(tdisk)

    # --- Zwijanie i zmiana rozmiaru okien (osobny boot) ---
    print("--- pulpit: minimalizacja + zmiana rozmiaru ---")
    q13 = Qemu(KERNEL, INITRD)
    try:
        q13.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 "Terminal" (40,60,120x70): pasek tytulu niebieski na starcie
        check("okno 0: pasek tytulu niebieski przed zwinieciem (80,64)",
              q13.wait_pixel(80, 64, (0, 0, 255)))

        # Przycisk zwijania [_] win0: min_x = x+w-2*8-2 = 142, y=61 (8x8).
        # Klik srodka (146,65) zwija okno -> pasek znika, wraca tlo.
        q13.mouse_goto(146, 65)
        q13.mouse_click()
        q13.mouse_goto(400, 300)              # odsun kursor
        check("zwiniete okno znika z pulpitu (pasek 80,64 -> teal)",
              q13.wait_pixel(80, 64, (0, 128, 160)))

        # Okno wciaz na pasku zadan (slot0 'Terminal', x46..96, y468).
        # Klik przywraca je: pasek tytulu niebieski znow widoczny.
        q13.mouse_goto(60, 473)
        q13.mouse_click()
        q13.mouse_goto(400, 300)
        check("klik na pasku zadan przywraca zwiniete okno (80,64 niebieski)",
              q13.wait_pixel(80, 64, (0, 0, 255)))

        # Zmiana rozmiaru: uchwyt win0 w prawym dolnym rogu
        # grip_x = x+w-8 = 152, grip_y = y+h-8 = 122. Przed zmiana punkt
        # (170,140) jest poza oknem (teal). Przeciagamy uchwyt (155,125)
        # o (+60,+40) -> okno rosnie i pokrywa (170,140).
        _, _, at = q13.screen_raw()
        check("przed zmiana rozmiaru punkt poza oknem (170,140 teal)",
              near(at(170, 140), (0, 128, 160)))
        q13.mouse_goto(155, 125)
        q13.mouse_button(True)
        q13.mouse_move(60, 40)
        q13.mouse_button(False)
        q13.mouse_goto(400, 300)
        check("po zmianie rozmiaru okno powiekszone (170,140 szare tlo okna)",
              q13.wait_pixel(170, 140, (224, 224, 224)))
    finally:
        q13.close()

    # --- Zapis pozycji ikon na dysku (dwa boot-y na tym samym dysku) ---
    print("--- pulpit: zapis pozycji ikon na dysku ---")
    idisk = tempfile.mktemp(suffix=".img")
    with open(idisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q14 = Qemu(KERNEL, INITRD, idisk)
    try:
        q14.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # ikona 'App' domyslnie (8,20,32x24) - kafelek pomaranczowy ~ (20,30)
        check("ikona App w domyslnej pozycji (20,30 pomaranczowy)",
              q14.wait_pixel(20, 30, (255, 162, 0)))

        # Przeciagnij ikone: chwyt srodka (24,32), przesun na (416,312) z
        # wcisnietym LPM -> ikona laduje w (400,300). Pusc.
        q14.mouse_goto(24, 32)
        q14.mouse_button(True)
        q14.mouse_goto(416, 312)
        q14.mouse_button(False)
        q14.mouse_goto(100, 250)          # odsun kursor od ikony
        check("ikona przeniesiona na nowa pozycje (410,310 pomaranczowy)",
              q14.wait_pixel(410, 310, (255, 162, 0)))
        check("stara pozycja ikony wyczyszczona (20,30 teal)",
              q14.wait_pixel(20, 30, (0, 128, 160)))
    finally:
        q14.close()

    # Reboot na TYM SAMYM dysku: pozycja ikony przetrwala (icons.pos)
    q15 = Qemu(KERNEL, INITRD, idisk)
    try:
        q15.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("pozycja ikony przetrwala restart (410,310 pomaranczowy)",
              q15.wait_pixel(410, 310, (255, 162, 0)))
        check("domyslna pozycja ikony pusta po restarcie (20,30 teal)",
              q15.wait_pixel(20, 30, (0, 128, 160)))
    finally:
        q15.close()
        if os.path.exists(idisk):
            os.unlink(idisk)

    # --- ACPI: wylaczenie i restart komputera ----------------------
    # Wylaczenie zasilania widac po tym, ze maszyna wirtualna sama
    # konczy dzialanie - to najuczciwszy dowod, jaki mozna miec.
    print("--- ACPI (wylaczanie zasilania) ---")
    q20 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        boot = q20.wait_for("ACPI:", timeout=30)
        check("tablice ACPI znalezione przy starcie",
              re.search(r"ACPI: tablice od '\S+", boot) is not None)
        q20.wait_for("ProtonOS shell", timeout=20)
        q20.type_line("help")
        check("'poweroff' jest w spisie komend",
              "poweroff" in q20.wait_for("Dostepne komendy", timeout=20))
        q20.type_line("poweroff")
        check("'poweroff' faktycznie gasi maszyne", q20.wait_exit(timeout=20))
    finally:
        q20.close()

    # --- Klawiatura USB --------------------------------------------
    # Kontroler USB, rozpoznanie urzadzenia, protokol rozruchowy HID.
    # QEMU kieruje klawisze do urzadzenia USB, wiec to, ze komenda
    # w ogole sie wykonuje, jest dowodem, ze przeszly ta droga.
    print("--- klawiatura USB (HID) ---")
    udisk = tempfile.mktemp(suffix=".img")
    with open(udisk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    q19 = Qemu(KERNEL, INITRD, udisk, append=TEXT_MODE, usbkbd=True)
    try:
        boot = q19.wait_for("Klawiatura USB", timeout=30)
        check("klawiatura USB rozpoznana na magistrali",
              re.search(r"Klawiatura USB \(HID\) na porcie \d+", boot)
              is not None)
        q19.wait_for("ProtonOS shell", timeout=20)
        q19.type_line("clear")
        q19.type_line("uptime")
        out = q19.wait_for("tyknac timera", timeout=20)
        check("komenda wpisana z klawiatury USB wykonuje sie",
              "uptime:" in out)
        # Wielka litera powstaje z modyfikatora w raporcie HID -
        # 'type_line' umie tylko male, wiec skladamy klawisze recznie.
        # Klawiature odpytujemy piecdziesiat razy na sekunde, a QEMU
        # trzyma klawisz okolo stu milisekund - przy zbyt szybkim
        # nadawaniu press i release sasiednich klawiszy zachodza na
        # siebie. Stad wolniejsze tempo i druga proba.
        def type_shift_a():
            for ch in "write":
                q19.sendkey(ch)
                time.sleep(0.15)
            for k in ("spc", "shift-a", "spc", "x", "ret"):
                q19.sendkey(k)
                time.sleep(0.25)

        saved = ""
        for attempt in range(3):
            type_shift_a()
            try:
                saved = q19.wait_for("zapisano", timeout=10)
                if "do 'A'" in saved:
                    break
            except RuntimeError:
                pass
            q19.type_line("clear")
        check("modyfikator Shift z raportu HID daje wielkie litery",
              "do 'A'" in saved)
    finally:
        q19.close()
        if os.path.exists(udisk):
            os.unlink(udisk)

    # --- Dysk za kontrolerem AHCI ----------------------------------
    # Ten sam system plikow i ta sama wymiana stron, tyle ze dane
    # przenosi kontroler przez DMA zamiast procesora slowo po slowie.
    print("--- kontroler AHCI (SATA) ---")
    sdisk = tempfile.mktemp(suffix=".img")
    with open(sdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q18 = Qemu(KERNEL, INITRD, sdisk, append=TEXT_MODE, ram_mib=16, sata=True)
    try:
        boot = q18.wait_for("+ PFS gotowy", timeout=30)
        check("kontroler AHCI wykryty po PCI i wybrany zamiast ATA",
              "Dysk AHCI (SATA, DMA) + PFS gotowy" in boot)
        check("PFS sformatowal dysk przez AHCI",
              "sformatowany" in boot)
        check("plik wymiany zalozony na dysku SATA",
              re.search(r"Wymiana stron: \d+ KiB",
                        q18.wait_for("Wymiana stron:", timeout=20)) is not None)

        q18.wait_for("ProtonOS shell", timeout=20)
        q18.type_line("clear")
        q18.type_line("write ahci proton przez dma")
        q18.wait_for("zapisano", timeout=20)
        q18.type_line("cat ahci")
        back = q18.wait_for("proton przez dma", timeout=20)
        check("zapis i odczyt pliku przez DMA oddaja te same dane",
              "proton przez dma" in back)

        # Wymiana stron przez AHCI: te same 18 MiB na maszynie z 16 MiB
        q18.type_line("clear")
        q18.type_line("run swaptest 18")
        out = q18.wait_for("[swaptest] wszystkie", timeout=120)
        check("wymiana stron przez AHCI oddaje strony bez zmian",
              "stron wrocilo w calosci" in out)
    finally:
        q18.close()
        if os.path.exists(sdisk):
            os.unlink(sdisk)

    # --- Konsole wirtualne -----------------------------------------
    # Alt+F1..F4 przelacza ekran, ale kazda konsola ma wlasna tresc,
    # wlasny shell i wlasnych "swoich" pisarzy: program uruchomiony na
    # drugiej pisze na niej dalej, kiedy patrzymy na pierwsza.
    print("--- konsole wirtualne (Alt+F1..F4) ---")
    q17 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q17.wait_for("ProtonOS shell", timeout=30)
        q17.type_line("clear")
        q17.type_line("uptime")
        first = q17.wait_for("tyknac timera", timeout=20)
        check("pierwsza konsola dziala jak dotad", "uptime:" in first)

        q17.sendkey("alt-f2")
        time.sleep(0.6)
        second = q17.screen()
        check("Alt+F2 pokazuje druga konsole z wlasnym promptem",
              "konsola 2" in second)
        check("tresc pierwszej konsoli nie przecieka na druga",
              "tyknac timera" not in second)

        q17.type_line("run spin")
        q17.wait_for("[spin] tik", timeout=20)
        check("program uruchomiony na drugiej konsoli pisze na niej", True)

        q17.sendkey("alt-f1")
        time.sleep(1.5)
        back = q17.screen()
        check("pierwsza konsola nietknieta po powrocie",
              "tyknac timera" in back)
        check("program z drugiej konsoli nie pisze po pierwszej",
              "[spin] tik" not in back)

        q17.sendkey("alt-f2")
        time.sleep(0.5)
        before = q17.screen().count("[spin] tik")
        time.sleep(1.5)
        after = q17.screen().count("[spin] tik")
        check("program pisal na swoja konsole takze wtedy, gdy jej nie bylo"
              f" widac ({before} -> {after})",
              after != before or before >= 8)
        q17.type_line("kill spin")
        q17.wait_for("zabity (kod -9)", timeout=20)
        check("kill z wlasnej konsoli konczy jej program", True)
    finally:
        q17.close()

    # --- Wymiana stron na dysk -------------------------------------
    # Maszyna z 16 MiB RAM-u i program, ktory bierze 18 MiB: bez wymiany
    # nie ma prawa sie udac. Wzorzec zalezy od numeru strony, wiec strona
    # wczytana z niewlasciwego miejsca od razu sie wysypie.
    print("--- wymiana stron na dysk ---")
    swdisk = tempfile.mktemp(suffix=".img")
    with open(swdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q16 = Qemu(KERNEL, INITRD, swdisk, append=TEXT_MODE, ram_mib=16)
    try:
        boot = q16.wait_for("Wymiana stron:", timeout=30)
        check("plik wymiany zakladany przy starcie",
              re.search(r"Wymiana stron: \d+ KiB na dysku", boot) is not None)
        q16.wait_for("ProtonOS shell", timeout=20)
        q16.type_line("clear")
        q16.type_line("run swaptest 18")
        out = q16.wait_for("[swaptest] wszystkie", timeout=120)
        check("18 MiB zmiescilo sie na maszynie z 16 MiB RAM-u",
              "stron wrocilo w calosci" in out)
        q16.type_line("clear")
        q16.type_line("mem")
        stats = q16.wait_for("wymiana:", timeout=20)
        m = re.search(r"wyrzuconych (\d+), sciagnietych (\d+)", stats)
        check("strony naprawde poszly na dysk i wrocily"
              + (f" (-{m.group(1)} +{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) > 1000
              and int(m.group(2)) > 1000)
        # Program skonczyl sie, wiec jego miejsca w pliku wymiany wrocily
        m = re.search(r"wymiana:\s+(\d+)/(\d+) stron", stats)
        check("miejsca w pliku wymiany zwolnione po smierci procesu"
              + (f" ({m.group(1)}/{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) == 0)
    finally:
        q16.close()
        if os.path.exists(swdisk):
            os.unlink(swdisk)

    # --- Rozruch z obrazu ISO (Multiboot 2) ------------------------
    # Tu jadro wgrywa GRUB, a nie QEMU - a wiec inna postac informacji
    # o maszynie: mapa pamieci, initrd jako modul i wskaznik na tablice
    # ACPI przychodza jako znaczniki Multiboot 2.
    print("--- rozruch z ISO (Multiboot 2) ---")
    if shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue "
              "(apt install grub-pc-bin grub-common xorriso)")
    else:
        isodir = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        try:
            os.makedirs(isodir + "/boot/grub")
            shutil.copy(KERNEL, isodir + "/boot/protonos.bin")
            shutil.copy(INITRD, isodir + "/boot/initrd.img")
            # Wlasne menu: bez odliczania i od razu w tryb tekstowy,
            # zeby dalo sie czytac ekran
            with open(isodir + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("set timeout=0\nset default=0\n"
                         'menuentry "ProtonOS" {\n'
                         "    multiboot2 /boot/protonos.bin nodesktop\n"
                         "    module2 /boot/initrd.img\n}\n")
            made = subprocess.run(["grub-mkrescue", "-o", iso, isodir],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("obraz ISO buduje sie", made)
            if made:
                slog = tempfile.mktemp(suffix=".log")
                q21 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog)
                try:
                    boot = q21.wait_serial("Multiboot OK", timeout=40)
                    check("jadro wgrane przez GRUB-a mowi Multiboot 2",
                          "Multiboot OK (Multiboot 2)" in boot)
                    check("mapa pamieci ze znacznika Multiboot 2",
                          re.search(r"Pamiec fizyczna: \d+ KiB RAM",
                                    q21.wait_serial("Pamiec fizyczna",
                                                    timeout=20)) is not None)
                    check("initrd dotarl jako modul Multiboot 2",
                          re.search(r"initrd: \d+ plikow",
                                    q21.wait_serial("initrd:", timeout=20))
                          is not None)
                    check("wskaznik na tablice ACPI podany przez GRUB-a",
                          "ACPI: tablice od" in q21.wait_serial("ACPI:",
                                                                timeout=20))
                    q21.wait_serial("ProtonOS shell", timeout=30)
                    q21.type_line("uptime")
                    check("system z plyty odpowiada na komendy",
                          "uptime:" in q21.wait_serial("tyknac timera",
                                                       timeout=20))

                    # Konsola rysowana na buforze obrazu od GRUB-a:
                    # tryb tekstowy VGA w ogole tu nie dziala, wiec
                    # kazdy zapalony piksel to znak narysowany przez nas
                    w, h, at = q21.screen_raw()
                    check(f"loader ustawil tryb graficzny ({w}x{h})",
                          w >= 640 and h >= 400)
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"konsola narysowana na buforze obrazu "
                          f"({lit} pikseli)", lit > 3000)
                    green = sum(1 for y in range(y0, y0 + 200)
                                for x in range(x0, x0 + 640)
                                if at(x, y) == (85, 255, 85))
                    check("kolory trybu tekstowego oddane w RGB "
                          f"(jasnozielone [ OK ]: {green})", green > 100)
                    outside = sum(1 for y in range(0, min(40, y0))
                                  for x in range(0, w, 4)
                                  if at(x, y) != (0, 0, 0))
                    check("poza blokiem tekstu ekran czysty", outside == 0)
                finally:
                    q21.close()
                    if os.path.exists(slog):
                        os.unlink(slog)

                # Pulpit na tym samym buforze: video_init_vesa nie ma
                # tu czego ustawiac, bo tryb ustawil juz loader
                with open(isodir + "/boot/grub/grub.cfg", "w") as fh:
                    fh.write("set timeout=0\nset default=0\n"
                             'menuentry "ProtonOS" {\n'
                             "    multiboot2 /boot/protonos.bin\n"
                             "    module2 /boot/initrd.img\n}\n")
                iso2 = tempfile.mktemp(suffix=".iso")
                try:
                    subprocess.run(["grub-mkrescue", "-o", iso2, isodir],
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
                    q22 = Qemu(KERNEL, INITRD, cdrom=iso2)
                    try:
                        time.sleep(22)
                        w, h, at = q22.screen_raw()
                        teal = sum(1 for y in range(0, h, 8)
                                   for x in range(0, w, 8)
                                   if all(abs(at(x, y)[i] - (0, 128, 160)[i])
                                          <= 40 for i in range(3)))
                        total = (h // 8) * (w // 8)
                        check(f"pulpit rysuje sie na buforze od loadera "
                              f"({w}x{h}, {teal}/{total} probek tla)",
                              teal > total * 0.7)
                    finally:
                        q22.close()
                finally:
                    if os.path.exists(iso2):
                        os.unlink(iso2)
        finally:
            shutil.rmtree(isodir, ignore_errors=True)
            if os.path.exists(iso):
                os.unlink(iso)

    # --- Jadro 64-bitowe (tryb dlugi) ------------------------------
    # Ten sam kod, kompilowany drugi raz: wejscie wchodzi w tryb dlugi,
    # a reszta - wypisywanie, konsola na buforze obrazu, odczyt
    # znacznikow - to te same pliki, co w jadrze 32-bitowym.
    print("--- jadro 64-bitowe (tryb dlugi) ---")
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
    elif shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
    elif shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64 (tryb dlugi)")
    else:
        d64 = tempfile.mkdtemp()
        iso64 = tempfile.mktemp(suffix=".iso")
        slog64 = tempfile.mktemp(suffix=".log")
        try:
            os.makedirs(d64 + "/boot/grub")
            shutil.copy(KERNEL64, d64 + "/boot/protonos64.bin")
            mod = ""
            if INITRD64 and os.path.exists(INITRD64):
                shutil.copy(INITRD64, d64 + "/boot/initrd64.img")
                mod = "    module2 /boot/initrd64.img initrd64\n"
            with open(d64 + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("set timeout=0\nset default=0\n"
                         'menuentry "ProtonOS 64" {\n'
                         "    multiboot2 /boot/protonos64.bin\n"
                         + mod + "}\n")
            made = subprocess.run(["grub-mkrescue", "-o", iso64, d64],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("obraz ISO z jadrem 64-bitowym buduje sie", made)
            if made:
                q23 = Qemu(KERNEL, INITRD, cdrom=iso64, serial=slog64,
                           qemu="qemu-system-x86_64")
                try:
                    out = q23.wait_serial("Tryb dlugi:", timeout=40)
                    check("jadro startuje w trybie dlugim",
                          "Jadro uruchomione w trybie dlugim" in out)
                    # LMA ustawia sam procesor - to jego zdanie, nie nasze
                    check("procesor potwierdza tryb dlugi (LMA=1)",
                          "LMA=1" in out)
                    check("wskazniki maja osiem bajtow",
                          "wskaznik ma 8 bajtow" in out)
                    check("mapa pamieci czytana tym samym kodem",
                          re.search(r"Pamiec fizyczna: \d{5,} KiB RAM",
                                    q23.wait_serial("Pamiec fizyczna",
                                                    timeout=20)) is not None)
                    check("bufor obrazu od loadera rozpoznany",
                          re.search(r"Bufor obrazu: \d+x\d+",
                                    q23.wait_serial("Bufor obrazu",
                                                    timeout=20)) is not None)

                    # Pamiec: wlasne tablice zbudowane z ramek od PMM,
                    # a nie te ulozone na sztywno w obrazie jadra
                    mem = q23.wait_serial("Stronicowanie czteropoziomowe",
                                          timeout=20)
                    m = re.search(r"czteropoziomowe: (\d+) MiB, zakaz "
                                  r"wykonywania (\w+)", mem)
                    check("wlasne tablice stron obejmuja cala pamiec"
                          + (f" ({m.group(1)} MiB)" if m else ""),
                          m is not None and int(m.group(1)) >= 100)
                    check("zakaz wykonywania wlaczony (bit NXE)",
                          m is not None and m.group(2) == "wlaczony")
                    # Strona wstawiona osiem gigabajtow ponad obszar
                    # opisany identycznosciowo - zapis i odczyt przez
                    # nia przechodza cala czterostopniowa sciezke
                    hi = q23.wait_serial("Mapowanie 4 KiB", timeout=20)
                    check("mapowanie 4 KiB daleko poza pamiecia dziala",
                          "odczyt 123456789abcdef" in hi)
                    heap = q23.wait_serial("Sterta jadra", timeout=20)
                    check("sterta jadra dziala w trybie dlugim",
                          "kmalloc/kfree dzialaja" in heap
                          and "po zwolnieniu 0 B" in heap)

                    # Przerwania w trybie dlugim: pulapka int3 przechodzi
                    # cala droge - stub, tablice, ramke rejestrow, iretq -
                    # i wraca, nie zatrzymujac systemu
                    trap = q23.wait_serial("[INT3]", timeout=20)
                    check("wyjatek-pulapka obsluzony i wznowiony",
                          re.search(r"\[INT3\] breakpoint pod rip=",
                                    trap) is not None)
                    # A timer musi tykac sam z siebie - to dowod, ze
                    # przerwania sprzetowe dochodza przez nowa tablice

                    # Watki: liczniki musza zgadzac sie z dlugoscia snu
                    # co do jednego. Przez 600 ms watek spiacy po 20 ms
                    # zdazy 30 razy, a ten po 50 ms - 12. Zepsute
                    # przelaczanie kontekstu albo usypianie od razu
                    # wypycha te liczby poza zakres.
                    sch = q23.wait_serial("Watki licza rownolegle",
                                          timeout=25)
                    m = re.search(r"A=(\d+), B=(\d+)", sch)
                    check("watki dzialaja rownolegle, a sen trwa tyle, ile ma"
                          + (f" (A={m.group(1)}, B={m.group(2)})"
                             if m else ""),
                          m is not None and 28 <= int(m.group(1)) <= 32
                          and 11 <= int(m.group(2)) <= 13)
                    zom = q23.wait_serial("Watek konczacy sie", timeout=20)
                    check("zakonczony watek znika z listy",
                          "zrobil swoje" in zom
                          and "zadan na liscie: 4" in zom)

                    # Ring 3: selektor kodu zna tylko procesor, a jego
                    # dwa najnizsze bity mowia, w ktorym pierscieniu
                    # program NAPRAWDE dzialal. Zeby to powiedziec,
                    # musial wejsc do jadra instrukcja syscall.
                    q23.wait_serial("Wywolania systemowe przez instrukcje",
                                    timeout=20)
                    r3 = q23.wait_serial("Program w ring 3:", timeout=25)
                    m = re.search(r"Program w ring 3: (.+?), cs=(\w+), "
                                  r"pierscien (\d)", r3)
                    check("program wszedl do jadra instrukcja syscall",
                          m is not None and m.group(1) == "odezwal sie")
                    check("procesor potwierdza pierscien 3"
                          + (f" (cs={m.group(2)})" if m else ""),
                          m is not None and m.group(3) == "3")

                    # Program z PLIKU: jadro nie widzialo go w czasie
                    # budowy - wszystkiego dowiaduje sie z naglowka ELF
                    ini = q23.wait_serial("initrd:", timeout=25)
                    check("jadro 64-bitowe czyta initrd od loadera",
                          re.search(r"initrd: \d+ plik", ini) is not None)
                    ld = q23.wait_serial("z initrd:", timeout=25)
                    m = re.search(r"Program 'hello64' z initrd: (\d+) B, "
                                  r"wejscie ([0-9a-f]+)", ld)
                    check("ELF64 z initrd zaladowany"
                          + (f" ({m.group(1)} B, wejscie {m.group(2)})"
                             if m else ""),
                          m is not None and int(m.group(1)) > 0
                          # punkt wejscia musi lezec w przestrzeni
                          # uzytkownika, czyli od 1 GiB w gore
                          and int(m.group(2), 16) >= 0x40000000)
                    out64 = q23.wait_serial("[hello64] koncze", timeout=30)
                    check("program z pliku pisze przez wywolanie systemowe",
                          "[hello64] czesc, jestem ELF-em 64-bitowym"
                          in out64)
                    check("program liczy i usypia sam siebie",
                          "[hello64] licze: 1" in out64
                          and "[hello64] licze: 3" in out64)
                    m = re.search(r"uplynelo tyknac: (\d+)", out64)
                    check("sen programu trwa tyle, ile ma"
                          + (f" ({m.group(1)} tyknac)" if m else ""),
                          m is not None and 13 <= int(m.group(1)) <= 25)
                    fin = q23.wait_serial("ELF64 z initrd:", timeout=25)
                    m = re.search(r"pierscien (\d), kod wyjscia (\d+)", fin)
                    check("program z pliku dzialal w ring 3",
                          m is not None and m.group(1) == "3")
                    check("kod wyjscia programu dotarl do jadra"
                          + (f" ({m.group(2)})" if m else ""),
                          m is not None and m.group(2) == "7")

                    # Ochrona pamieci widziana od strony programu.
                    # Bit NX byl dotad tylko ustawiony - tu program
                    # naprawde probuje wykonac swoje dane i ginie.
                    nx = q23.wait_serial("Zakaz wykonywania:", timeout=30)
                    check("program probuje wykonac wlasne dane",
                          "[nx64] i probuje ja wykonac" in nx)
                    check("procesor odmawia wykonania danych (bit NX)",
                          "procesor odmowil wykonania danych" in nx)
                    # 0x15 = strona obecna, dostep z ring 3, pobranie
                    # instrukcji. Sam bit obecnosci wykluczylby zwykly
                    # brak mapowania - to naprawde zadzialal NX.
                    m = re.search(r"Zakaz wykonywania: .*err=([0-9a-f]+)", nx)
                    check("blad strony mowi: pobranie instrukcji z ring 3"
                          + (f" (err={m.group(1)})" if m else ""),
                          m is not None and int(m.group(1), 16) & 0x15 == 0x15)
                    check("winny program zostaje zabity, nie caly system",
                          "zabite: Page Fault" in nx)

                    # Druga droga do pamieci jadra: poprosic o nia samo
                    # jadro. Musi odmowic, bo sprawdza wskaznik.
                    wild = q23.wait_serial("Pamiec jadra:", timeout=30)
                    check("jadro odrzuca wskaznik spoza programu",
                          "nie nalezy do programu" in wild
                          and "[wild64] jadro odmowilo" in wild)
                    check("pamiec jadra niedostepna z ring 3",
                          "niedostepna dla pierscienia 3" in wild)
                    m = re.search(r"Pamiec jadra: .*err=([0-9a-f]+), "
                                  r"adres=([0-9a-f]+)", wild)
                    check("blad strony mowi: odczyt z ring 3 strony jadra"
                          + (f" (err={m.group(1)}, adres={m.group(2)})"
                             if m else ""),
                          m is not None and int(m.group(1), 16) == 0x05
                          and int(m.group(2), 16) == 0x100000)

                    # I to, po co ta cala obsluga istnieje
                    surv = q23.wait_serial("System przezyl", timeout=30)
                    m = re.search(r"licznik (\d+) -> (\d+)", surv)
                    check("system pracuje po smierci obu programow"
                          + (f" (licznik {m.group(1)} -> {m.group(2)})"
                             if m else ""),
                          m is not None
                          and int(m.group(2)) > int(m.group(1)))

                    # Dwa procesy naraz, kazdy we wlasnej przestrzeni.
                    # Ten sam plik, ten sam adres wirtualny, osobna
                    # pamiec - i to wlasnie musi byc widac.
                    sep = q23.wait_serial("Pamiec po procesach", timeout=40)
                    addrs = re.findall(r"proces (\d): adres ([0-9a-f]+), "
                                       r"wpisalem (\d+)", sep)
                    check(f"dwa procesy ruszyly naraz ({len(addrs)})",
                          len(addrs) == 2)
                    check("oba pisza pod TEN SAM adres wirtualny"
                          + (f" ({addrs[0][1]})" if addrs else ""),
                          len(addrs) == 2 and addrs[0][1] == addrs[1][1])
                    check("kazdy wpisal co innego"
                          + (f" ({addrs[0][2]} i {addrs[1][2]})"
                             if len(addrs) == 2 else ""),
                          len(addrs) == 2 and addrs[0][2] != addrs[1][2])
                    kept = re.findall(r"proces \d: nadal (\d+) - moja "
                                      r"pamiec nietknieta", sep)
                    check(f"zaden nie zobaczyl pamieci drugiego "
                          f"({len(kept)}/2)", len(kept) == 2)
                    check("obie kopie skonczyly sie bez bledu",
                          "Dwa procesy naraz: 2 zakonczonych, 0 bledow"
                          in sep)
                    # Przestrzen musi zniknac razem z procesem, inaczej
                    # kazdy uruchomiony program zabiera pamiec na zawsze
                    m = re.search(r"Pamiec po procesach oddana "
                                  r"\((\d+) -> (\d+) ramek\)", sep)
                    check("pamiec po procesach wrocila do puli"
                          + (f" ({m.group(1)} -> {m.group(2)})"
                             if m else ""),
                          m is not None
                          and int(m.group(2)) <= int(m.group(1)))

                    # fork: drugi proces bierze sie juz nie z decyzji
                    # jadra, tylko z decyzji samego programu
                    fk = q23.wait_serial("Pamiec po fork-u", timeout=45)
                    m = re.search(r"rodzic: fork dal numer dziecka (\d+)", fk)
                    check("rodzic dostaje z fork-a numer dziecka"
                          + (f" ({m.group(1)})" if m else ""),
                          m is not None and int(m.group(1)) > 0)
                    check("dziecko dostaje z fork-a zero",
                          "dziecko: fork dal 0" in fk)
                    # Oba wracaja w to samo miejsce z tym samym stanem,
                    # ale pamiec maja juz osobna
                    check("dziecko pisze po swojej kopii (200)",
                          "moja wartosc to 200" in fk)
                    check("zapis dziecka nie dosiega rodzica (nadal 100)",
                          "rodzic: nadal 100 - dziecko dostalo kopie" in fk)
                    check("obie galezie skonczyly sie bez bledu",
                          "Proces powielil sie sam: 2 zakonczonych, "
                          "0 bledow" in fk)
                    m = re.search(r"Pamiec po fork-u oddana "
                                  r"\((\d+) -> (\d+) ramek\)", fk)
                    check("kopia pamieci wrocila do puli"
                          + (f" ({m.group(1)} -> {m.group(2)})"
                             if m else ""),
                          m is not None
                          and int(m.group(2)) <= int(m.group(1)))

                    # A teraz rachunek: fork NIE kopiuje pamieci.
                    # Program ma siedem stron (kod, dane stale, dane,
                    # cztery strony stosu), wiec kopiowanie wprost
                    # kosztowaloby ich siedem plus tablice. Z dzieleniem
                    # stron zostaja same tablice, a kopiuje sie tylko to,
                    # co ktos naprawde zapisal.
                    cow = q23.wait_serial("Kopiowanie przy zapisie",
                                          timeout=30)
                    m = re.search(r"fork wzial (\d+) ramek, "
                                  r"rozdzielono (\d+) stron", cow)
                    check("fork nie kopiuje pamieci programu"
                          + (f" ({m.group(1)} ramek - same tablice)"
                             if m else ""),
                          m is not None and int(m.group(1)) <= 6)
                    check("rozdzielone tylko strony naprawde zapisane"
                          + (f" ({m.group(2)})" if m else ""),
                          m is not None and 1 <= int(m.group(2)) <= 6)

                    ticks = q23.wait_serial("Timer tyka:", timeout=25)
                    m = re.search(r"Timer tyka: (\d+) -> (\d+)", ticks)
                    check("przerwania sprzetowe dochodza (timer tyka"
                          + (f" {m.group(1)} -> {m.group(2)})" if m else ")"),
                          m is not None and int(m.group(2)) - int(m.group(1)) >= 100)
                    # I to samo na ekranie: konsola rysowana wlasnym kodem
                    q23.wait_serial("Szkielet 64-bitowy stoi", timeout=20)
                    w, h, at = q23.screen_raw()
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"konsola 64-bitowa rysuje na buforze obrazu "
                          f"({lit} pikseli)", lit > 2000)
                finally:
                    q23.close()
        finally:
            shutil.rmtree(d64, ignore_errors=True)
            for f in (iso64, slog64):
                if os.path.exists(f):
                    os.unlink(f)

    # --- Rozruch przez UEFI (OVMF) ---------------------------------
    # Nowoczesny sprzet nie ma juz BIOS-u: firmware szuka na plycie
    # katalogu /EFI/BOOT i uruchamia stamtad program EFI. GRUB potrafi
    # zlozyc plyte hybrydowa - z sektorem rozruchowym dla BIOS-u i z
    # partycja EFI obok - i to wlasnie sprawdzamy tutaj, uruchamiajac
    # ten sam obraz jadra pod firmware OVMF zamiast pod SeaBIOS-em.
    #
    # Pod UEFI nie ma tez bufora znakow spod 0xB8000. Jedyne, co
    # dostajemy, to bufor obrazu - i to taki, jaki firmware ma pod reka
    # (OVMF daje 24 bity na piksel, a nie 32). Test sprawdza obie
    # rzeczy: ze jadro w ogole wstalo i ze cos narysowalo na ekranie.
    print()
    print("--- rozruch przez UEFI (OVMF) ---")
    OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
    elif shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
    elif not os.path.isdir("/usr/lib/grub/x86_64-efi"):
        print("POMINIETE: brak GRUB-a dla EFI "
              "(apt install grub-efi-amd64-bin)")
    elif shutil.which("mformat") is None:
        print("POMINIETE: brak mtools (partycji EFI nie ma z czego zlozyc)")
    elif not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("POMINIETE: brak firmware OVMF (apt install ovmf)")
    elif shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
    else:
        due = tempfile.mkdtemp()
        isoue = tempfile.mktemp(suffix=".iso")
        slogue = tempfile.mktemp(suffix=".log")
        nvram = tempfile.mktemp(suffix=".fd")
        try:
            os.makedirs(due + "/boot/grub")
            shutil.copy(KERNEL64, due + "/boot/protonos64.bin")
            mod = ""
            if INITRD64 and os.path.exists(INITRD64):
                shutil.copy(INITRD64, due + "/boot/initrd64.img")
                mod = "    module2 /boot/initrd64.img initrd64\n"
            # all_video + gfxpayload=keep: bez tego GRUB pod UEFI nie
            # umie sam ustawic trybu graficznego i konczy sie bledem
            # "no suitable video mode found"
            with open(due + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("insmod all_video\nset gfxpayload=keep\n"
                         "set timeout=0\nset default=0\n"
                         'menuentry "ProtonOS 64" {\n'
                         "    multiboot2 /boot/protonos64.bin\n"
                         + mod + "}\n")
            made = subprocess.run(["grub-mkrescue", "-o", isoue, due],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("plyta hybrydowa (BIOS + UEFI) buduje sie", made)
            # Partycja EFI musi byc naprawde w srodku, inaczej firmware
            # nie ma czego uruchomic - szukamy jej podpisu w obrazie
            if made:
                with open(isoue, "rb") as fh:
                    head = fh.read(4 * 1024 * 1024)
                check("obraz zawiera partycje EFI z programem rozruchowym",
                      b"EFI PART" in head or b"BOOTX64" in head.upper())
            if made:
                # Firmware zapisuje zmienne, wiec pracuje na kopii
                shutil.copy(OVMF_VARS, nvram)
                q24 = Qemu(KERNEL, INITRD, cdrom=isoue, serial=slogue,
                           qemu="qemu-system-x86_64",
                           uefi=(OVMF_CODE, nvram))
                try:
                    out = q24.wait_serial("Multiboot OK", timeout=60)
                    check("firmware UEFI uruchamia GRUB-a, a GRUB jadro",
                          "Jadro uruchomione w trybie dlugim" in out)
                    check("jadro widzi znaczniki Multiboot 2 od GRUB-a EFI",
                          "Multiboot OK (Multiboot 2)" in out)
                    # Pod UEFI tablice ACPI to jedyny sposob, zeby
                    # dowiedziec sie czegokolwiek o plycie
                    acpi = q24.wait_serial("Wskaznik ACPI", timeout=20)
                    check("loader podaje wskaznik ACPI",
                          re.search(r"Wskaznik ACPI od loadera: [0-9a-f]+",
                                    acpi) is not None)
                    fbl = q24.wait_serial("Bufor obrazu", timeout=20)
                    m = re.search(r"Bufor obrazu: (\d+)x(\d+), (\d+) "
                                  r"bitow na piksel", fbl)
                    check("bufor obrazu od firmware przyjety"
                          + (f" ({m.group(1)}x{m.group(2)}, {m.group(3)} b/px)"
                             if m else ""),
                          m is not None)
                    check("format piksela inny niz 32-bitowy tez dziala",
                          m is not None and m.group(3) in ("24", "32"))
                    mem = q24.wait_serial("Pamiec fizyczna", timeout=20)
                    check("mapa pamieci od GRUB-a EFI ma sens",
                          re.search(r"Pamiec fizyczna: \d{5,} KiB RAM",
                                    mem) is not None)
                    r3 = q24.wait_serial("Program w ring 3:", timeout=40)
                    check("program w ring 3 rusza tak samo jak pod BIOS-em",
                          "pierscien 3" in r3)
                    if INITRD64 and os.path.exists(INITRD64):
                        # Modul od GRUB-a EFI dochodzi ta sama droga
                        # co pod BIOS-em - znacznikiem Multiboota 2
                        elf = q24.wait_serial("ELF64 z initrd:", timeout=40)
                        check("program z initrd dziala takze pod UEFI",
                              "pierscien 3" in elf
                              and "kod wyjscia 7" in elf)
                    q24.wait_serial("Szkielet 64-bitowy stoi", timeout=20)
                    # I to samo na ekranie - narysowane naszym kodem,
                    # piksel po pikselu, bez pomocy firmware
                    w, h, at = q24.screen_raw()
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"konsola rysuje sie na ekranie UEFI "
                          f"({lit} pikseli)", lit > 2000)
                finally:
                    q24.close()
        finally:
            shutil.rmtree(due, ignore_errors=True)
            for f in (isoue, slogue, nvram):
                if os.path.exists(f):
                    os.unlink(f)

    print()
    if failures:
        print(f"NIEPOWODZENIE: {len(failures)} test(y) nie przeszly")
        return 1
    print("WSZYSTKIE TESTY PRZESZLY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
