"""Wspolna czesc testu dymnego ProtonOS.

Wszystko, co potrzebne, zeby uruchomic maszyne i cos na niej sprawdzic:
klasa Qemu, serwery HTTP i DNS dla testow sieciowych, generowanie
certyfikatow oraz sama funkcja check.

Same sprawdzenia leza w scripts/testy/ - kazda grupa w osobnym pliku,
bo kazda potrzebuje wlasnej maszyny, a te uruchamiaja sie rownolegle.
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


_FONT8X8 = None


def font8x8():
    """Czcionka konsoli wczytana wprost z naglowka jadra: wzorzec
    osmiu bajtow -> znak. Jedno zrodlo prawdy - gdy czcionka sie
    zmieni, testy czytaja nowa."""
    global _FONT8X8
    if _FONT8X8 is None:
        sciezka = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "kernel", "include", "font8x8.h")
        with open(sciezka) as fh:
            zrodlo = fh.read()
        wiersze = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},){7}0x[0-9A-Fa-f]{2})\}",
                             zrodlo)
        _FONT8X8 = {}
        for i, w in enumerate(wiersze):
            wzor = tuple(int(v, 16) for v in w.split(","))
            _FONT8X8.setdefault(wzor, chr(0x20 + i))
    return _FONT8X8


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
        # Limit czasu na odpowiedz. Bez niego odczyt z gniazda czeka
        # w nieskonczonosc: gdy QEMU zginie albo zawiesi sie w srodku
        # polecenia, caly test staje na zawsze zamiast zglosic blad -
        # a nie ma nikogo, kto by go z tego wybudzil.
        sock.settimeout(self.QMP_TIMEOUT)
        self.sock = sock
        self.f = sock.makefile("rwb")
        self.f.readline()  # baner QMP
        self.cmd({"execute": "qmp_capabilities"})

    # Najdluzsze polecenie to zrzut ekranu w wysokiej rozdzielczosci -
    # sekundy wystarcza z ogromnym zapasem
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
            if not raw:                  # gniazdo zamkniete = QEMU zginal
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
        self._ostatnie_piksele = px

        def at(x, y):
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return w, h, at


    def screen_fbcon(self):
        """Tekst konsoli odczytany z BUFORA OBRAZU.

        GRUB, widzac w naglowku jadra zadanie trybu obrazu, zawsze daje
        grafike - pamiec znakow spod 0xB8000 zostaje wtedy pusta i
        screen() nic z niej nie wyczyta. Konsola rysuje sie wowczas sama,
        czcionka 8x8, w bloku 80x25 na srodku ekranu (kernel/fbcon.c).

        Czytamy ja tak, jak czytalby czlowiek: kolor, ktorego na calym
        bloku jest najwiecej, to tlo, reszta to glify - a wzorzec osmiu
        bajtow wyszukujemy w tej samej czcionce, ktora rysowalo jadro.

        Tlo liczymy dla calego bloku, nie dla pojedynczej komorki: sa
        znaki, w ktorych pikseli glifu jest WIECEJ niz tla ('0', 'N',
        'B', 'W'), i liczone osobno wychodzilyby na wspak.
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
            linia = px[o:o + 80 * 8 * 3]
            for i in range(0, len(linia), 3):
                p = linia[i:i + 3]
                ile[p] = ile.get(p, 0) + 1
        if not ile:
            return ""
        tlo = max(ile, key=ile.get)

        font = font8x8()
        wiersze = []
        for row in range(25):
            znaki = []
            for col in range(80):
                wzor = []
                for y in range(8):
                    o = ((y0 + row * 8 + y) * w + x0 + col * 8) * 3
                    linia = px[o:o + 24]
                    b = 0
                    for x in range(8):
                        if linia[x * 3:x * 3 + 3] != tlo:
                            b |= 1 << x
                    wzor.append(b)
                znaki.append(font.get(tuple(wzor), "?"))
            wiersze.append("".join(znaki).rstrip())
        return "\n".join(wiersze).rstrip()

    def czekaj_fbcon(self, tekst, timeout=15):
        """Czeka, az tekst pojawi sie na konsoli rysowanej w buforze
        obrazu. Zwraca caly ekran (takze po przekroczeniu czasu -
        asercja nalezy do wolajacego)."""
        koniec = time.time() + timeout
        ekran = ""
        while time.time() < koniec:
            ekran = self.screen_fbcon()
            if tekst in ekran:
                return ekran
            time.sleep(0.4)
        return ekran

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

    def polecenie(self, cmd, tekst=None, timeout=20):
        """Wpisuje polecenie i oddaje TO, CO PRZYBYLO na porcie po nim.

        Bez odcinania starszej tresci latwo o falszywy alarm: shell
        odbija wpisany wiersz, wiec nazwa pliku jest na porcie takze
        wtedy, gdy polecenie niczego nie znalazlo - a wynik poprzedniego
        polecenia lezy tam do konca biegu."""
        start = len(self.serial())
        self.type_line(cmd)
        if tekst is None:
            time.sleep(0.5)
            return self.serial()[start:]
        koniec = time.time() + timeout
        while time.time() < koniec:
            nowe = self.serial()[start:]
            if tekst in nowe:
                return nowe
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


# --- Zliczanie wynikow ---------------------------------------------
# Kazda grupa dziala we wlasnym procesie i wypisuje swoje wyniki na
# wyjscie. Sumuje je runner - dzieki temu grupy nie musza sie o siebie
# nawzajem troszczyc ani niczego sobie przekazywac.
failures = []


def check(name, cond):
    print(("OK:   " if cond else "BLAD: ") + name)
    if not cond:
        failures.append(name)


def wynik():
    """Konczy proces grupy: 0 = wszystko przeszlo."""
    if failures:
        print(f"NIEPOWODZENIE: {len(failures)} test(y) nie przeszly")
        return 1
    return 0



def zrob_iso64(katalog, iso, grafika=False, tekst=False):
    """Sklada plyte z jadrem 64-bitowym i jego initrd.

    Jadro 64-bitowe nie da sie uruchomic przez -kernel: QEMU wgrywa tak
    tylko obrazy Multiboot 1, a to jest Multiboot 2. Kazdy test musi
    wiec zbudowac plyte GRUB-em, a robil to dotad kazdy na wlasna reke.
    'grafika' wlacza tryb graficzny od loadera (pulpit i konsola na
    buforze obrazu). 'tekst' zada trybu tekstowego: GRUB sam z siebie
    daje bufor obrazu i wtedy konsola rysuje sie w nim zamiast
    w pamieci znakow VGA, ktorej testy szukaja pod 0xB8000.

    Zwraca True, gdy plyta powstala.
    """
    os.makedirs(katalog + "/boot/grub", exist_ok=True)
    shutil.copy(KERNEL64, katalog + "/boot/protonos64.bin")
    mod = ""
    if INITRD64 and os.path.exists(INITRD64):
        shutil.copy(INITRD64, katalog + "/boot/initrd64.img")
        mod = "    module2 /boot/initrd64.img initrd64\n"
    naglowek = ("insmod all_video\nset gfxpayload=keep\n" if grafika
                else "set gfxpayload=text\n" if tekst else "")
    with open(katalog + "/boot/grub/grub.cfg", "w") as fh:
        fh.write(naglowek + "set timeout=0\nset default=0\n"
                 'menuentry "ProtonOS 64" {\n'
                 "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
    return subprocess.run(["grub-mkrescue", "-o", iso, katalog],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def brak64():
    """Powod, dla ktorego testu 64-bitowego nie da sie uruchomic, albo
    None, gdy da sie. Wszystkie grupy 64-bitowe pytaja o to samo."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        return "brak obrazu 64-bitowego (make kernel64)"
    if shutil.which("grub-mkrescue") is None:
        return "brak grub-mkrescue"
    if shutil.which("qemu-system-x86_64") is None:
        return "brak qemu-system-x86_64"
    return None
