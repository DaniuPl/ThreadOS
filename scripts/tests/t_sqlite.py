#!/usr/bin/env python3
"""ThreadOS smoke test: SQLite, the first library brought in whole.

SQLite (third_party/, the amalgamation as sqlite.org publishes it) is
built against musl with threads on, and its own command-line shell runs
on the 64-bit kernel. It is a hard client for an operating system: it
reads and writes pages at offsets, keeps a rollback journal and deletes
it, cuts the file back with ftruncate when a transaction is rolled back
or the database is vacuumed, sorts with worker threads (PRAGMA threads)
and checks its own file for damage.

Everything is checked against the same SQLite built on this machine:

  build.sql makes a database - a join over 20000 rows, window
  functions, a sort bigger than the cache on four threads, a rolled
  back transaction that had grown the file, savepoints, a trigger,
  JSON, dates, full-text search, VACUUM - and prints what it asks; the
  output has to be the host's, line for line

  the database file ThreadOS wrote is read off the disk image by an
  independent ext2 reader (Linux's debugfs, ptest.Ext2Image) and has
  to be the host's
  file byte for byte, with no journal left behind and no block of the
  disk taken that no file holds

  check.sql opens it again, and opens the host's file fetched over
  HTTP: both answer as the host does

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64, Ext2Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SQL = os.path.join(ROOT, "scripts", "tests", "sql")
PROMPT = "thread64>"


def makefile_value(name):
    with open(os.path.join(ROOT, "Makefile")) as fh:
        text = fh.read().replace("\\\n", " ")
    return re.search(r"^%s\s*:=\s*(.*)$" % name, text, re.M).group(1).split()


def host_sqlite(work):
    """The same amalgamation with the same options, built for this
    machine"""
    src = os.path.join(work, "src")
    os.makedirs(src)
    zipname = os.path.join(ROOT, makefile_value("SQLITE_ZIP")[0])
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts",
                                                 "unzipflat.py"),
                    zipname, src], check=True)
    exe = os.path.join(work, "sqlite3")
    r = subprocess.run(["gcc", "-O1", "-w", "-pthread",
                        *makefile_value("SQLITE_OPTS"), "-I" + src,
                        os.path.join(src, "shell.c"),
                        os.path.join(src, "sqlite3.c"), "-lm", "-o", exe],
                       cwd=work)
    return exe if r.returncode == 0 else None


def host_run(exe, cwd, script, db):
    p = subprocess.run([exe, "-init", os.path.join(SQL, script), db, ".quit"],
                       cwd=cwd, capture_output=True, text=True, timeout=120,
                       stdin=subprocess.DEVNULL)
    return p.stdout.splitlines(), p.stderr


def run(q, command, timeout):
    """A program from the kernel's shell; what it printed, the exit code"""
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    deadline = time.time() + timeout
    while time.time() < deadline:
        if f"[{name}] done, exit code" in q.serial()[seen:]:
            break
        time.sleep(0.5)
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT,
             "-- Loading resources from"))]
    return body, int(m.group(1)) if m else None


def compare(what, want, got, code):
    first = next((i for i, (w, g) in enumerate(zip(want, got)) if w != g),
                 None)
    where = (f"first difference at line {first + 1}: host {want[first]!r}, "
             f"ThreadOS {got[first]!r}" if first is not None else
             f"host {len(want)} lines, ThreadOS {len(got)}")
    check(f"{what}: the output is the host SQLite's ({where})", got == want)
    if got != want and os.environ.get("SQLITE_DEBUG"):
        print("\n".join(got))
    check(f"{what}: and the shell ends with code 0 (got {code})", code == 0)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    work = tempfile.mkdtemp()
    http_port, http_stop = start_http_server()
    try:
        exe = host_sqlite(work)
        check("the same SQLite builds on this machine", exe is not None)
        if exe is None:
            return summary()
        hrun = os.path.join(work, "run")
        os.makedirs(hrun)
        want_build, err = host_run(exe, hrun, "build.sql", "test.db")
        want_check, _ = host_run(exe, hrun, "check.sql", "test.db")
        with open(os.path.join(hrun, "test.db"), "rb") as fh:
            host_db = fh.read()
        check("(the host's run asks everything build.sql asks: "
              f"{len(want_build)} lines, no errors{': ' + err[:80] if err else ''})",
              len(want_build) > 40 and not err and want_build[-1] == "ok")

        for name in ("build.sql", "check.sql"):
            with open(os.path.join(SQL, name), "rb") as fh:
                SERVED["/" + name] = fh.read()
        SERVED["/host.db"] = host_db

        iso = os.path.join(work, "t.iso")
        slog = os.path.join(work, "serial.log")
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.write(b"\0" * DISK_SIZE)
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64", cpus=4)
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("] Disk ", timeout=30)
            q.wait_serial("[net] DHCP", timeout=40)
            for name in ("build.sql", "check.sql", "host.db"):
                _, code = run(q, f"run httpget 10.0.2.2 {http_port} /{name} "
                              f"{name}", 60)
                check(f"{name} reaches the ThreadOS disk", code == 0)

            t0 = time.time()
            got, code = run(q, "run sqlite3 -init build.sql test.db .quit",
                            600)
            print(f"    (build.sql took {time.time() - t0:.0f} s in QEMU)")
            compare("build.sql", want_build, got, code)
            got, code = run(q, "run sqlite3 -init check.sql test.db .quit",
                            300)
            compare("check.sql on the database ThreadOS wrote",
                    want_check, got, code)
            got, code = run(q, "run sqlite3 -init check.sql host.db .quit",
                            300)
            compare("check.sql on the host's database, fetched over HTTP",
                    want_check, got, code)
        finally:
            q.close()

        # What the kernel left on the disk, seen from outside
        img = Ext2Image(disk)
        mine = img.read("/test.db")
        check("test.db is on the disk", mine is not None)
        if mine is not None:
            first = next((i for i in range(min(len(mine), len(host_db)))
                          if mine[i] != host_db[i]), None)
            check(f"and it is the host's file byte for byte ({len(mine)} B "
                  f"against {len(host_db)} B"
                  f"{', first difference at %d' % first if first is not None else ''})",
                  mine == host_db)
        check("no rollback journal is left behind",
              not img.exists("/test.db-journal"))
        clean, problems = img.fsck()
        check("every block the bitmap marks as taken belongs to a file, "
              "and none past a file's end - e2fsck -fn finds nothing to "
              f"fix ({problems[:3]})", clean)
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
