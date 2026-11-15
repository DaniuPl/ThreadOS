#!/usr/bin/env python3
"""ThreadOS smoke test: curl.

curl 8.23.0 (third_party/, the release archive unchanged) is configured
by its own configure script for musl and built with zlib; the curl tool
runs on ThreadOS over the C library's sockets. On the other side stands
an HTTP server in Python on the host, which records every request, so
both ends are checked:

  a plain GET; a 2 MB download into a file, read back off the disk image
  (ptest.TfsImage) and compared by SHA-256; a gzip-encoded answer that
  curl inflates with zlib (--compressed); a redirect followed with -L; a
  POST whose body the server checks; a chunked answer; HEAD (-I); a 404
  with -f, which curl must turn into its exit code 22; a name resolved
  through the system's DNS; -w reporting the status and the size

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
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_iso64, no64, TfsImage,
                   start_dns_server, DNS_HINT)

PROMPT = "thread64>"
NAME = "files.threados.test"
BIG = random.Random(11).randbytes(2 * 1024 * 1024)
TEXT = b"".join(b"line %d: the quick brown fox\n" % i for i in range(3000))
REQUESTS = []


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
        if self.path == "/hello.txt":
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
    try:
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

            agents = {r[2].get("User-Agent", "") for r in REQUESTS
                      if r[0] == "GET"}
            check(f"every request says it is curl 8.23.0 ({agents})",
                  agents == {"curl/8.23.0"})
        finally:
            q.close()

        img = TfsImage(disk)
        data = img.read("/big.bin")
        check("big.bin on the disk has the SHA-256 of what was sent",
              data is not None and
              hashlib.sha256(data).digest() == hashlib.sha256(BIG).digest())
    finally:
        srv.shutdown()
        dns_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
