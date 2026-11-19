#!/usr/bin/env python3
"""ThreadOS smoke test: Weft remembers its session (64-bit kernel).

Weft keeps what a person would expect a browser to remember - the
cookies a site asked to keep, the history, the bookmarks - in one file
in their home, ~/.weft (apps/webkit/weft.c), and gives the cookies back
to WebKit before the first page of the next run. Whose home it is the
kernel says (SYS64_USER, USER_CTL_WHOAMI). On a disk the first-run
wizard has made an account on (admin), two runs of Weft:

  the first finds nothing to remember, and its home is found; it loads
  a page whose server sets three cookies - one to keep for a day, one
  without an expiry, which ends with the browser as in every browser,
  and one for fifteen seconds - and the page's script sees all three;
  when the page has loaded, and again when Weft ends, it keeps the two
  with an expiry and the page in its history

  the file, read on ThreadOS by the shell's cat, says so: its first
  line, the history's line with the page and its title, the cookie to
  keep with its value, and no cookie that was to end with the browser

  once the short cookie's fifteen seconds are over, the second run
  remembers one cookie and one page, and its first request carries
  exactly that cookie - what the test's web server is sent, the
  independent judge of what WebKit was given back

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import secrets
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64, ROUTES
from t_webkit import wait_line, tell

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
APPS = os.path.join(BUILD, "weft.tapp")
HOME = "/home/admin"          # the account wait_desktop's wizard makes
BRIEF = 15                    # seconds the short cookie lasts

LOGIN = (b'<!doctype html><html><head><meta charset="utf-8"><title>loading'
         b'</title></head><body><script>'
         b'document.title = "login " + document.cookie.split("; ").sort()'
         b'.join(" ");</script></body></html>')
CHECK = (b'<!doctype html><html><head><meta charset="utf-8"><title>check'
         b'</title></head><body></body></html>')


def main():
    print()
    print("--- Weft remembers its session (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(APPS):
        print("SKIPPED: no weft.tapp - make webkit")
        return summary()

    token = secrets.token_hex(8)
    served = {}
    asked = []

    def login(h, body):
        served["login"] = time.time()
        return 200, [("Content-Type", "text/html; charset=utf-8"),
                     ("Set-Cookie", f"sid={token}; Max-Age=86400; Path=/"),
                     ("Set-Cookie", "tmp=gone; Path=/"),
                     ("Set-Cookie", f"brief=1; Max-Age={BRIEF}; Path=/")], \
            LOGIN

    def check_page(h, body):
        asked.append(h.headers.get("Cookie") or "")
        return 200, [("Content-Type", "text/html; charset=utf-8")], CHECK

    ROUTES["/login.html"] = login
    ROUTES["/check.html"] = check_page
    port, stop = start_http_server()
    base = f"http://10.0.2.2:{port}"
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    try:
        check("the disc with the kernel and Weft builds",
              make_iso64(os.path.join(work, "iso"), iso, graphics=True,
                         tapps=[APPS]))
        q = Qemu(KERNEL, INITRD, disk, cdrom=iso,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64", ram_mib=2048)
        try:
            q.wait_serial("thread64>", timeout=120)
            q.wait_serial("[net] DHCP", timeout=60)
            q.type_line("desktop")
            check("the desktop comes up, the wizard's account signed in",
                  q.wait_desktop())
            q.mouse_home()

            # The first run
            seen = len(q.serial())
            q.type_line(f"run weft {base}/login.html")
            line = wait_line(q, seen, "[weft] remembered", 300)
            check(f"Weft finds the home to keep things in, and nothing in "
                  f"it yet ({line})", line == "[weft] remembered 0 cookies, "
                  "0 pages of history, 0 bookmarks")
            title = wait_line(q, seen, "[weft] title login", 300)
            check(f"the page's script sees the three cookies its server set "
                  f"({title})", title == f"[weft] title login brief=1 "
                  f"sid={token} tmp=gone")
            if title is None:
                tell(q, seen)
            kept = wait_line(q, seen, "[weft] kept", 60)
            check(f"loaded, Weft keeps the two with an expiry and the page "
                  f"({kept})", kept == "[weft] kept 2 cookies, 1 pages of "
                  "history, 0 bookmarks")
            seen = len(q.serial())
            q.sendkey("ctrl-w")
            kept = wait_line(q, seen, "[weft] kept", 60)
            done = wait_line(q, seen, "[weft] done, exit code", 60)
            check(f"and once more as it ends, cleanly ({kept}; {done})",
                  kept == "[weft] kept 2 cookies, 1 pages of history, "
                  "0 bookmarks" and done is not None
                  and done.startswith("[weft] done, exit code 0"))

            # The file, as ThreadOS reads it
            q.click_desktop()
            time.sleep(1)
            out = q.command(f"cat {HOME}/.weft", "thread64>",
                            timeout=30).replace("\r", "")
            lines = out.split("\n")
            check(f"~/.weft begins as Weft's ({lines[1:2]})",
                  "ThreadOS Weft 1" in lines)
            check("its history has the page, with its title",
                  any(re.fullmatch(rf"history\t{re.escape(base)}/login\.html"
                                   rf"\tlogin .*sid={token}.*", l)
                      for l in lines))
            check("the cookie to keep is there, with its value",
                  any(re.match(rf"cookie\t10\.0\.2\.2\t/\tsid\t{token}\t\d+\t",
                               l) for l in lines))
            check("and the one that ends with the browser is not",
                  not any(l.startswith("cookie") and "\ttmp\t" in l
                          for l in lines))

            # The second run, once the short cookie has run out
            wait = served.get("login", time.time()) + BRIEF + 3 - time.time()
            if wait > 0:
                time.sleep(wait)
            seen = len(q.serial())
            q.type_line(f"run weft {base}/check.html")
            line = wait_line(q, seen, "[weft] remembered", 300)
            check(f"the next run remembers the one cookie still in date and "
                  f"the page ({line})", line == "[weft] remembered 1 "
                  "cookies, 1 pages of history, 0 bookmarks")
            done = wait_line(q, seen, f"[weft] load finished {base}/check.html",
                             300)
            check(f"and its first request carries exactly that cookie, as "
                  f"the server was sent it ({asked})",
                  done is not None and asked[:1] == [f"sid={token}"])
            if done is None:
                tell(q, seen)
            seen = len(q.serial())
            q.sendkey("ctrl-w")
            kept = wait_line(q, seen, "[weft] kept", 60)
            check(f"ending, it keeps the cookie and both pages ({kept})",
                  kept == "[weft] kept 1 cookies, 2 pages of history, "
                  "0 bookmarks")
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
