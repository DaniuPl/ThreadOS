#!/usr/bin/env python3
"""ThreadOS smoke test: curl, over HTTP and HTTPS.

curl 8.23.0 (third_party/, the release archive unchanged) is configured
by its own configure script for musl and built with zlib, and with Mbed
TLS 3.6.7 for TLS; the curl tool runs on ThreadOS over the C library's
sockets. On the other side stand HTTP and HTTPS servers in Python on
the host (OpenSSL's TLS, certificates made by the openssl tool), which
record every request and every handshake, so both ends are checked:

  a plain GET; a 2 MB download into a file, read back off the disk image
  (ptest.TfsImage) and compared by SHA-256; a gzip-encoded answer that
  curl inflates with zlib (--compressed); a redirect followed with -L; a
  POST whose body the server checks; a chunked answer; HEAD (-I); a 404
  with -f, which curl must turn into its exit code 22; a name resolved
  through the system's DNS; -w reporting the status and the size

  HTTPS with a chain the server sends (root, intermediate, server) and
  only the root given to curl: TLS 1.3, and TLS 1.2 from a server that
  offers nothing newer, each seen negotiated by the server itself; 2 MB
  over TLS, compared by SHA-256 off the disk image. And what has to be
  refused, with curl's exit code 60 and no request reaching the server:
  an address the certificate does not name, a root curl was not given,
  a root the system does not trust, and a certificate that expired
  before the guest's clock - while -k, told not to check, does get
  through

  the system's own roots: curl, given none, reads Mozilla's bundle
  where Linux keeps it (/etc/ssl/certs/ca-certificates.crt, made up by
  the C library out of the initrd) - copied out with file://, it is
  third_party/cacert/cacert.pem byte for byte; a root put into
  roots.der at the top of the disk is added to it, in PEM exactly as
  Python's ssl module writes one, and curl then trusts the test's
  server with no --cacert at all

Run on its own or through scripts/run_tests.py.
"""

import gzip
import hashlib
import http.server
import os
import random
import re
import shutil
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_iso64, no64, TfsImage,
                   start_dns_server, DNS_HINT)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = "thread64>"
NAME = "files.threados.test"
BIG = random.Random(11).randbytes(2 * 1024 * 1024)
TEXT = b"".join(b"line %d: the quick brown fox\n" % i for i in range(3000))
REQUESTS = []
HANDSHAKES = []      # (server, TLS version or "failed", path or None)
FILES = {}           # served as they are: the roots curl is given


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if not any(k == "Transfer-Encoding" for k, _ in headers):
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        REQUESTS.append((self.command, self.path, dict(self.headers)))
        if self.path in FILES:
            self.send(200, FILES[self.path])
        elif self.path.split("?")[0] == "/hello.txt":
            self.send(200, b"hello from the host\n",
                      [("Content-Type", "text/plain"), ("X-Test", "seen")])
        elif self.path == "/big.bin":
            self.send(200, BIG, [("Content-Type", "application/octet-stream")])
        elif self.path == "/text.gz":
            if "gzip" in self.headers.get("Accept-Encoding", ""):
                self.send(200, gzip.compress(TEXT),
                          [("Content-Encoding", "gzip")])
            else:
                self.send(406, b"ask for gzip\n")
        elif self.path == "/old":
            self.send(302, b"", [("Location", "/hello.txt")])
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"first part, ", b"second part, ", b"the end\n"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send(404, b"no such thing\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        REQUESTS.append(("POST", self.path, body))
        self.send(200, b"got " + body + b"\n")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class TLSHandler(Handler):
    """The same answers, after a handshake in this connection's thread,
    which records what was agreed - or that nothing was"""

    def setup(self):
        self.agreed = None
        try:
            self.request.settimeout(60)
            self.request.do_handshake()
            self.agreed = self.request.version()
        except (ssl.SSLError, OSError):
            HANDSHAKES.append((self.server.tag, "failed", None))
        super().setup()

    def handle(self):
        if self.agreed:
            super().handle()

    def do_GET(self):
        HANDSHAKES.append((self.server.tag, self.agreed, self.path))
        super().do_GET()


class TLSServer(Server):
    def __init__(self, tag, chain, key, newest=None):
        self.tag = tag
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(chain, key)
        if newest:
            self.ctx.maximum_version = newest
        super().__init__(("0.0.0.0", 0), TLSHandler)
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def get_request(self):
        sock, addr = self.socket.accept()
        return self.ctx.wrap_socket(sock, server_side=True,
                                    do_handshake_on_connect=False), addr


def openssl(*args):
    subprocess.run(["openssl", *args], check=True, capture_output=True,
                   timeout=120)


def make_certs(work):
    """Two roots of our own: one with an intermediate under it, which
    signs the server's certificates (one for ten years, one for a day -
    long gone by the guest's clock, which says 2030), and one that
    signs nothing curl will see"""
    j = lambda name: os.path.join(work, name)
    with open(j("ca.ext"), "w") as fh:
        fh.write("basicConstraints=critical,CA:TRUE\n"
                 "keyUsage=critical,keyCertSign,cRLSign\n")
    with open(j("leaf.ext"), "w") as fh:
        fh.write("basicConstraints=critical,CA:FALSE\n"
                 "subjectAltName=DNS:threados.test\n")
    for root in ("root", "other"):
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", j(root + ".key"), "-out", j(root + ".pem"),
                "-days", "3650", "-subj", f"/CN=ThreadOS curl test {root}",
                "-addext", "basicConstraints=critical,CA:TRUE")
    openssl("req", "-new", "-newkey", "ec", "-pkeyopt",
            "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", j("int.key"),
            "-out", j("int.csr"), "-subj", "/CN=ThreadOS curl test int")
    openssl("x509", "-req", "-in", j("int.csr"), "-CA", j("root.pem"),
            "-CAkey", j("root.key"), "-CAcreateserial", "-out", j("int.pem"),
            "-days", "3650", "-extfile", j("ca.ext"))
    for leaf, days in (("leaf", "3650"), ("old", "1")):
        openssl("req", "-new", "-newkey", "rsa:2048", "-nodes",
                "-keyout", j(leaf + ".key"), "-out", j(leaf + ".csr"),
                "-subj", "/CN=threados.test")
        openssl("x509", "-req", "-in", j(leaf + ".csr"), "-CA", j("int.pem"),
                "-CAkey", j("int.key"), "-CAcreateserial",
                "-out", j(leaf + ".pem"), "-days", days,
                "-extfile", j("leaf.ext"))
        with open(j(leaf + "-chain.pem"), "w") as out:
            out.write(open(j(leaf + ".pem")).read() +
                      open(j("int.pem")).read())
    for root in ("root", "other"):
        with open(j(root + ".pem"), "rb") as fh:
            FILES["/" + root + ".pem"] = fh.read()
    openssl("x509", "-in", j("root.pem"), "-outform", "DER",
            "-out", j("root.der"))
    with open(j("root.der"), "rb") as fh:
        FILES["/root.der"] = fh.read()
    return j


def run(q, command, timeout=120):
    seen = len(q.serial())
    q.type_line(command)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if "[curl] done, exit code" in q.serial()[seen:]:
            break
        time.sleep(0.3)
    out = q.serial()[seen:].replace("\r", "")
    m = re.search(r"\[curl\] done, exit code (-?\d+)", out)
    body = [l for l in out.split("\n")[1:] if l and not l.startswith(
            ("[curl]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    return body, int(m.group(1)) if m else None


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    dns_stop, dns_ok = start_dns_server(only="threados.test")
    if not dns_ok:
        print("SKIPPED: " + DNS_HINT)
        return summary()
    srv = Server(("0.0.0.0", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://10.0.2.2:{port}"
    work = tempfile.mkdtemp()
    servers = []
    try:
        try:
            certs = make_certs(work)
        except (OSError, subprocess.SubprocessError) as e:
            check(f"the openssl tool makes the test's certificates ({e})",
                  False)
            return summary()
        tls13 = TLSServer("1.3", certs("leaf-chain.pem"), certs("leaf.key"))
        tls12 = TLSServer("1.2", certs("leaf-chain.pem"), certs("leaf.key"),
                          newest=ssl.TLSVersion.TLSv1_2)
        expired = TLSServer("expired", certs("old-chain.pem"),
                            certs("old.key"))
        servers = [tls13, tls12, expired]
        h13, h12, hold = (f"https://threados.test:{t.server_address[1]}"
                          for t in servers)
        iso = os.path.join(work, "t.iso")
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.write(b"\0" * DISK_SIZE)
        check("the disc with the kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("] Disk ", timeout=30)
            q.wait_serial("[net] DHCP", timeout=60)
            q.command("dns 10.0.2.2", "DNS server set")

            got, code = run(q, f"run curl -s {base}/hello.txt")
            check(f"a GET prints the body ({got[:2]}, code {code})",
                  got == ["hello from the host"] and code == 0)

            t0 = time.time()
            got, code = run(q, f"run curl -s -o big.bin -w "
                               f"%{{http_code}}:%{{size_download}}\\n "
                               f"{base}/big.bin", 600)
            print(f"    (2 MB took {time.time() - t0:.0f} s)")
            check(f"2 MB into a file, and -w reports 200 and its size ({got})",
                  got == [f"200:{len(BIG)}"] and code == 0)

            got, code = run(q, f"run curl -s --compressed {base}/text.gz")
            check(f"a gzip answer is inflated with zlib ({len(got)} lines; "
                  f"{got[:1]} ... {got[-1:]})",
                  "\n".join(got) + "\n" == TEXT.decode() and code == 0)

            got, code = run(q, f"run curl -s -L {base}/old")
            check(f"-L follows a redirect ({got})",
                  got == ["hello from the host"] and code == 0)

            got, code = run(q, f"run curl -s -d name=threados&kernel=64 "
                               f"{base}/form")
            check(f"a POST: the server got the body, and answers ({got})",
                  got == ["got name=threados&kernel=64"] and
                  ("POST", "/form", b"name=threados&kernel=64") in REQUESTS)

            got, code = run(q, f"run curl -s {base}/chunked")
            check(f"a chunked answer is put back together ({got})",
                  got == ["first part, second part, the end"])

            got, code = run(q, f"run curl -s -I {base}/hello.txt")
            # (on a terminal curl prints the names in bold - the console
            # is one, so the escapes are there, and are not the point)
            got = [re.sub(r"\x1b\[[0-9;]*m", "", l) for l in got]
            check(f"-I shows the headers ({got[:1]})",
                  got[:1] == ["HTTP/1.1 200 OK"] and
                  any(l.lower() == "x-test: seen" for l in got) and
                  ("HEAD", "/hello.txt") in [r[:2] for r in REQUESTS])

            got, code = run(q, f"run curl -s -f {base}/missing")
            check(f"-f turns a 404 into exit code 22 (got {code})", code == 22)

            got, code = run(q, f"run curl -s http://{NAME}:{port}/hello.txt")
            check(f"a name through the system's DNS ({got})",
                  got == ["hello from the host"] and code == 0)

            # --- HTTPS ---
            for root in ("root", "other"):
                _, code = run(q, f"run curl -s -o {root}.pem "
                                 f"{base}/{root}.pem")
                check(f"{root}.pem reaches the ThreadOS disk", code == 0)

            def agreed(path):
                return [h[:2] for h in HANDSHAKES if h[2] == path]

            got, code = run(q, f"run curl -s --cacert root.pem "
                               f"{h13}/hello.txt?tls13")
            check(f"HTTPS: a GET through a chain to the root curl was given "
                  f"({got}, code {code})",
                  got == ["hello from the host"] and code == 0)
            check(f"and the server agreed on TLS 1.3 "
                  f"({agreed('/hello.txt?tls13')})",
                  agreed("/hello.txt?tls13") == [("1.3", "TLSv1.3")])

            got, code = run(q, f"run curl -s --cacert root.pem "
                               f"{h12}/hello.txt?tls12")
            check(f"a server that offers nothing newer than TLS 1.2 is "
                  f"answered in TLS 1.2 ({got}, code {code}, "
                  f"{agreed('/hello.txt?tls12')})",
                  got == ["hello from the host"] and code == 0 and
                  agreed("/hello.txt?tls12") == [("1.2", "TLSv1.2")])

            t0 = time.time()
            got, code = run(q, f"run curl -s --cacert root.pem -o tls.bin -w "
                               f"%{{http_code}}:%{{size_download}}\\n "
                               f"{h13}/big.bin", 600)
            print(f"    (2 MB over TLS took {time.time() - t0:.0f} s)")
            check(f"2 MB over TLS 1.3 into a file ({got})",
                  got == [f"200:{len(BIG)}"] and code == 0)

            port13 = tls13.server_address[1]
            for what, args, path in (
                    ("an address the certificate does not name",
                     f"--cacert root.pem https://10.0.2.2:{port13}"
                     f"/hello.txt?byip", "/hello.txt?byip"),
                    ("a root curl was not given",
                     f"--cacert other.pem {h13}/hello.txt?other",
                     "/hello.txt?other"),
                    ("a root the system does not trust (no --cacert: "
                     "only Mozilla's)", f"{h13}/hello.txt?none",
                     "/hello.txt?none"),
                    ("a certificate that expired before the guest's clock",
                     f"--cacert root.pem {hold}/hello.txt?expired",
                     "/hello.txt?expired")):
                got, code = run(q, f"run curl -s {args}")
                check(f"refused: {what} (exit code {code}, "
                      f"{'a request reached' if agreed(path) else 'nothing reached'}"
                      f" the server)", code == 60 and not agreed(path))

            # The system's roots, as a program finds them
            _, code = run(q, "run curl -s -o bundle.pem "
                             "file:///etc/ssl/certs/ca-certificates.crt")
            check(f"curl reads the system's CA bundle (code {code})",
                  code == 0)
            _, code = run(q, f"run curl -s -o /roots.der {base}/root.der")
            check("the test's root goes into roots.der", code == 0)
            got, code = run(q, f"run curl -s {h13}/hello.txt?system")
            check(f"with the root in roots.der, curl trusts the server "
                  f"with no --cacert ({got}, code {code}, "
                  f"{agreed('/hello.txt?system')})",
                  got == ["hello from the host"] and code == 0 and
                  agreed("/hello.txt?system") == [("1.3", "TLSv1.3")])
            _, code = run(q, "run curl -s -o bundle2.pem "
                             "file:///etc/ssl/certs/ca-certificates.crt")

            got, code = run(q, f"run curl -s -k {hold}/hello.txt?insecure")
            check(f"-k, told not to check, gets through all the same "
                  f"({got}, code {code})",
                  got == ["hello from the host"] and code == 0 and
                  agreed("/hello.txt?insecure") == [("expired", "TLSv1.3")])

            agents = {r[2].get("User-Agent", "") for r in REQUESTS
                      if r[0] == "GET"}
            check(f"every request says it is curl 8.23.0 ({agents})",
                  agents == {"curl/8.23.0"})
        finally:
            q.close()

        img = TfsImage(disk)
        with open(os.path.join(ROOT, "third_party", "cacert",
                               "cacert.pem"), "rb") as fh:
            mozilla = fh.read()
        bundle = img.read("/bundle.pem")
        check(f"the bundle is third_party/cacert/cacert.pem byte for byte "
              f"({len(bundle or b'')} against {len(mozilla)} bytes)",
              bundle == mozilla)
        want = mozilla + ssl.DER_cert_to_PEM_cert(FILES["/root.der"]).encode()
        bundle2 = img.read("/bundle2.pem")
        check("with roots.der, the bundle is Mozilla's and then the "
              "root in PEM, as Python's ssl module writes it "
              f"({len(bundle2 or b'')} against {len(want)} bytes)",
              bundle2 == want)
        for name in ("big.bin", "tls.bin"):
            data = img.read("/" + name)
            check(f"{name} on the disk has the SHA-256 of what was sent",
                  data is not None and hashlib.sha256(data).digest() ==
                  hashlib.sha256(BIG).digest())
    finally:
        for t in servers:
            t.shutdown()
        srv.shutdown()
        dns_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
