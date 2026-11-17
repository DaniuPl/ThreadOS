#!/usr/bin/env python3
"""ThreadOS smoke test: libsoup, glib-networking, OpenSSL, nghttp2,
libpsl.

How the web engine's network process will fetch pages: libsoup over
GLib's sockets, TLS through glib-networking's OpenSSL backend, HTTP/2
through nghttp2, cookie domains through libpsl's public suffix list
(Makefile, 'The network'). This test stands up
servers on the host - HTTP, HTTPS with certificates from a CA it makes,
and HTTP/2 over TLS when Python's h2 package is there - and runs
libc/tests/souptest.c against them; every line souptest prints is
worked out here independently and has to match:

  HTTP/1.1: a page and its type, a megabyte sent in chunks, a gzip body
  decoded, a redirect followed, a cookie set and sent back, a POST the
  server checks (its length and SHA-256), a 404, four requests at once

  HTTPS: TLS 1.3 to a server whose certificate the test's CA signed,
  the certificate's subject and issuer; refused without that CA
  (unknown CA), and refused for a certificate made out to another
  address (wrong name)

  HTTP/2 (nghttp2), spoken with an HTTP/2-only server

  base domains of five hosts, by this file's own reading of the public
  suffix list libpsl was built with - the same rules, applied by other
  code

  SHA-256 and HMAC from OpenSSL, against Python's hashlib and hmac

Run on its own or through scripts/run_tests.py.
"""

import gzip
import hashlib
import hmac
import http.server
import os
import re
import shutil
import socket
import socketserver
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPTS = ("thread64>", "thread64#")
GUEST_HOST = "10.0.2.2"           # the host, as QEMU's user network shows it

HELLO = "Hello from the host, ThreadOS! Zażółć gęślą jaźń.\n".encode()
BIG = bytes((i * 7 + i // 997) & 0xFF for i in range(1 << 20))
GZIP_TEXT = b"A body that travels compressed. " * 300
POSTED = "".join("line %d of the posted body\n" % i for i in range(4000)).encode()
H2_BODY = b"spoken over HTTP/2 " * 50
HOSTS = ["www.example.co.uk", "a.b.example.com", "shop.example.pl",
         "foo.bar.blogspot.com", "x.y.githubusercontent.com"]


def short(data):
    return hashlib.sha256(data).hexdigest()[:16]


def body_line(what, data):
    return f"{what}: {len(data)} bytes, sha256 {short(data)}"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def send(self, code, body, ctype="text/plain", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if p == "/hello":
            self.send(200, HELLO, "text/plain; charset=utf-8")
        elif p == "/big":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for at in range(0, len(BIG), 50000):
                part = BIG[at:at + 50000]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif p == "/gzip":
            self.send(200, gzip.compress(GZIP_TEXT), "text/plain",
                      [("Content-Encoding", "gzip")])
        elif p == "/redirect":
            self.send(302, b"moved", "text/plain", [("Location", "/final")])
        elif p == "/final":
            self.send(200, b"the end of the redirect\n")
        elif p == "/set-cookie":
            self.send(200, b"set", "text/plain",
                      [("Set-Cookie", "flavour=oatmeal; Path=/")])
        elif p == "/cookies":
            self.send(200, (self.headers.get("Cookie") or "none").encode())
        elif p.startswith("/part/"):
            self.send(200, ("part " + p[6:]).encode())
        else:
            self.send(404, b"no such thing")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(n)
        self.send(200, f"{len(data)} {short(data)}".encode())

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def openssl(*args, cwd):
    subprocess.run(["openssl", *args], cwd=cwd, check=True,
                   capture_output=True, timeout=120)


def make_pki(d):
    """A CA, and two server certificates it signs: one for the address
    the guest reaches the host by, one for an address that is not it"""
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
            "ca.key", "-out", "ca.pem", "-days", "3650", "-subj",
            "/CN=ThreadOS Test CA/O=ThreadOS", "-addext",
            "basicConstraints=critical,CA:true", cwd=d)
    for name, ip in (("good", GUEST_HOST), ("wrong", "10.9.9.9")):
        openssl("req", "-newkey", "ec", "-pkeyopt",
                "ec_paramgen_curve:prime256v1", "-nodes", "-keyout",
                f"{name}.key", "-out", f"{name}.csr", "-subj",
                f"/CN=server {name}/O=ThreadOS", cwd=d)
        with open(os.path.join(d, f"{name}.ext"), "w") as fh:
            fh.write(f"subjectAltName=IP:{ip}\n")
        openssl("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem",
                "-CAkey", "ca.key", "-CAcreateserial", "-out",
                f"{name}.pem", "-days", "3650", "-extfile", f"{name}.ext",
                cwd=d)


def serve(handler, tls=None):
    srv = Server(("0.0.0.0", 0), handler)
    if tls:
        srv.socket = tls.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def tls_context(d, name, alpn=None):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(os.path.join(d, f"{name}.pem"),
                        os.path.join(d, f"{name}.key"))
    if alpn:
        ctx.set_alpn_protocols(alpn)
    return ctx


def h2_server(ctx):
    """HTTP/2 alone, over TLS with ALPN, with Python's h2 package (the
    framing done there, not by nghttp2): or None without it"""
    try:
        import h2.config
        import h2.connection
        import h2.events
    except ImportError:
        return None
    lsock = socket.socket()
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("0.0.0.0", 0))
    lsock.listen(8)

    def one(raw):
        try:
            conn = ctx.wrap_socket(raw, server_side=True)
            if conn.selected_alpn_protocol() != "h2":
                conn.close()
                return
            h2c = h2.connection.H2Connection(
                h2.config.H2Configuration(client_side=False))
            h2c.initiate_connection()
            conn.sendall(h2c.data_to_send())
            while True:
                data = conn.recv(65535)
                if not data:
                    break
                for ev in h2c.receive_data(data):
                    if isinstance(ev, h2.events.RequestReceived):
                        h2c.send_headers(ev.stream_id, [
                            (":status", "200"),
                            ("content-type", "text/plain"),
                            ("content-length", str(len(H2_BODY)))])
                        h2c.send_data(ev.stream_id, H2_BODY, end_stream=True)
                conn.sendall(h2c.data_to_send())
        except (OSError, ssl.SSLError):
            pass

    def accept():
        while True:
            try:
                raw, _ = lsock.accept()
            except OSError:
                return
            threading.Thread(target=one, args=(raw,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return lsock


def public_suffix_rules():
    """The rules libpsl was built with, out of its archive"""
    with open(os.path.join(ROOT, "Makefile")) as fh:
        tar = next(l.split(":=")[1].strip() for l in fh
                   if l.startswith("LIBPSL_TAR "))
    with tarfile.open(os.path.join(ROOT, tar)) as tf:
        member = next(m for m in tf.getmembers()
                      if m.name.endswith("list/public_suffix_list.dat"))
        text = tf.extractfile(member).read().decode()
    rules = set()
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("//"):
            rules.add(line.split()[0])
    return rules


def base_domain(host, rules):
    """The public suffix algorithm (publicsuffix.org/list): the longest
    matching rule wins, an exception rule over a wildcard, '*' when
    nothing matches; the registrable domain is one label more"""
    labels = host.split(".")
    best = 1
    for i in range(len(labels)):
        cand = ".".join(labels[i:])
        wild = ".".join(["*"] + labels[i + 1:])
        if "!" + cand in rules:
            best = max(best, len(labels) - i - 1)
        elif cand in rules or (i + 1 < len(labels) and wild in rules):
            best = max(best, len(labels) - i)
    return ".".join(labels[-best - 1:]) if best < len(labels) else None


def expected(d, rules, with_h2):
    out = ["/hello: status 200, text/plain", body_line("/hello", HELLO),
           "/big: sent in chunks", body_line("/big", BIG),
           "/gzip: came as gzip", body_line("/gzip decoded", GZIP_TEXT),
           "/redirect: ended at /final",
           body_line("/redirect", b"the end of the redirect\n"),
           "the cookie came back: flavour=oatmeal",
           body_line("POST /echo, sent", POSTED),
           f"and the server says: {len(POSTED)} {short(POSTED)}",
           "/missing: status 404 Not Found",
           "four requests at once on the main loop: 4 of 4",
           "HTTPS with the CA: status 200 over TLS 1.3",
           body_line("HTTPS with the CA", HELLO)]
    subj = subprocess.run(["openssl", "x509", "-in", "good.pem", "-noout",
                           "-subject", "-issuer", "-nameopt", "RFC2253"],
                          cwd=d, capture_output=True, text=True).stdout
    # RFC 2253 writes a name's parts last first; glib-networking says
    # them in the certificate's order
    names = {k: ",".join(reversed(v.split(","))) for k, v in
             (l.split("=", 1) for l in subj.strip().splitlines())}
    out.append(f"its certificate: subject {names['subject']}, "
               f"issuer {names['issuer']}")
    out += ["HTTPS without it: refused (unknown CA)",
            "HTTPS, a certificate for another address: refused (wrong name)"]
    if with_h2:
        out += ["HTTP/2: status 200, spoken HTTP/2",
                body_line("HTTP/2 /h2", H2_BODY)]
    for h in HOSTS:
        out.append(f"the base domain of {h}: {base_domain(h, rules) or '-'}")
    out.append("co.uk is a public suffix: yes; example.co.uk is not: yes")
    out.append("OpenSSL SHA-256 of 'ThreadOS': " +
               hashlib.sha256(b"ThreadOS").hexdigest())
    out.append("OpenSSL HMAC-SHA256: " +
               hmac.new(b"key", b"message", hashlib.sha256).hexdigest())
    return out


def run(q, command, timeout=180):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    end = time.time() + timeout
    while time.time() < end and f"[{name}] done, exit code" not in \
            q.serial()[seen:]:
        time.sleep(0.5)
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    start = out.find(f"Program '{name}' from initrd")
    out = out[start:] if start >= 0 else out
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    body = [l.strip() for l in out.replace("\r", "").split("\n")[1:]
            if l.strip() and not l.strip().startswith(("[",) + PROMPTS)]
    return body, int(m.group(1)) if m else None


def main():
    print("--- libsoup, glib-networking, OpenSSL, nghttp2, libpsl ---")
    d = tempfile.mkdtemp()
    servers = []
    try:
        make_pki(d)
        with open(os.path.join(d, "ca.pem"), "rb") as fh:
            ptest.SERVED["/ca.pem"] = fh.read()
        plain = serve(Handler)
        good = serve(Handler, tls_context(d, "good"))
        wrong = serve(Handler, tls_context(d, "wrong"))
        servers += [plain, good, wrong]
        h2 = h2_server(tls_context(d, "good", ["h2"]))
        if h2 is None:
            print("SKIPPED: no h2 package here for an HTTP/2 server "
                  "(pip install h2) - HTTP/2 goes unchecked")
        want = expected(d, public_suffix_rules(), h2 is not None)

        work = tempfile.mkdtemp(dir=d)
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.truncate(16 * 1024 * 1024)
        port, stop_http = start_http_server()
        try:
            q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
                     ram_mib=512)
            try:
                check("the desktop came up", q.wait_desktop())
                q.click_desktop()
                _, c = run(q, f"run httpget {GUEST_HOST} {port} /ca.pem ca.pem")
                check("the test's CA reaches the disk", c == 0)
                cmd = (f"run souptest http://{GUEST_HOST}:"
                       f"{plain.server_address[1]} https://{GUEST_HOST}:"
                       f"{good.server_address[1]} ca.pem https://{GUEST_HOST}:"
                       f"{wrong.server_address[1]}")
                if h2 is not None:
                    cmd += f" https://{GUEST_HOST}:{h2.getsockname()[1]}"
                got, code = run(q, cmd, timeout=300)
                plain_lines = [l for l in got if not l.startswith("~")]
                first = next((i for i, (w, g) in enumerate(zip(want,
                              plain_lines)) if w != g), None)
                where = (f"first difference at line {first + 1}: want "
                         f"{want[first]!r}, souptest {plain_lines[first]!r}"
                         if first is not None else
                         f"want {len(want)} lines, souptest "
                         f"{len(plain_lines)}")
                check(f"souptest says what the servers sent and what "
                      f"Python works out ({where})", plain_lines == want)
                check(f"souptest ends with code 0 (got {code})", code == 0)
                for l in got:
                    if l.startswith("~"):
                        print("   " + l)
            finally:
                q.close()
        finally:
            stop_http()
    finally:
        for s in servers:
            s.shutdown()
        shutil.rmtree(d, ignore_errors=True)
    return summary()


sys.exit(main())
