#!/usr/bin/env python3
"""ThreadOS smoke test: GLib.

GLib is what WebKit's processes are built on - their main loop, their
threads, their objects and signals, how they start each other. It is
built from its release (third_party/, with libffi and PCRE2 under it)
for ThreadOS by its own meson (Makefile, 'Libraries for WebKit's
processes').

libc/tests/glibtest.c is built a second time on this machine against
this machine's GLib and run here; ThreadOS has to print the same lines,
on one core and on four:

  the main loop: idle callbacks before timeouts, timeouts in order, a
  pipe and a socket pair watched while a thread writes into them, a
  callback sent to the loop from another thread

  threads: a queue fed by four, a pool of workers, a condition
  variable, once-only initialization

  GObject: a class of the program's own with properties and a signal
  whose arguments go through libffi; finalization

  data: hash tables, sorting, UTF-8 (Polish), regular expressions with
  named groups (PCRE2), GVariant, key files, dates, SHA-256 and HMAC,
  base64, GIO's streams and files

  processes, as WebKit starts its own: a child spawned with pipes and a
  descriptor handed to it as fd 3, spawning returning at once (exec
  closes what is marked close-on-exec), the child's end seen by the
  main loop (a pidfd); and GSubprocess with a socket at fd 3 and the
  child's standard input from /dev/null

And ThreadOS's own numbers: the loop sleeps until the pipe is written
and wakes at once, and spawning takes well under a second.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPTS = ("thread64>", "thread64#")


def host_glibtest(work, args=()):
    """glibtest built against this machine's GLib and run: its lines"""
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "gio-2.0",
                            "gio-unix-2.0"], capture_output=True, text=True)
    if flags.returncode:
        return None
    exe = os.path.join(work, "glibtest")
    src = os.path.join(ROOT, "libc", "tests", "glibtest.c")
    if subprocess.run(["gcc", "-O2", "-w", "-o", exe, src,
                       *flags.stdout.split()]).returncode:
        return None
    p = subprocess.run([exe, *args], cwd=work, capture_output=True,
                       text=True, timeout=120)
    return [l for l in p.stdout.splitlines() if l], p.returncode


def lines(out):
    """What the program printed: from the kernel's word that it started
    (the shell may still be printing its greeting when the command is
    typed), without the kernel's own lines (a child's end among them)"""
    out = out.encode("latin1").decode("utf-8", "replace")
    start = out.find("Program 'glibtest' from initrd")
    out = out[start:] if start >= 0 else out
    body = []
    for l in out.replace("\r", "").split("\n")[1:]:
        l = l.strip()
        if l and not l.startswith(("[",) + PROMPTS):
            body.append(l)
    return body


def one(cpus, want):
    """'want': the host's lines, or None without a GLib on the host -
    then only ThreadOS's own answers are held to 'yes'"""
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
             cpus=cpus if cpus > 1 else None, append="nodesktop")
    try:
        q.wait_serial("The 64-bit system is ready", timeout=90)
        out = q.command("run glibtest", "[glibtest] done, exit code",
                        timeout=240)
        got = lines(out)
        plain = [l for l in got if not l.startswith("~")]
        if want is not None:
            wanted = [l for l in want if not l.startswith("~")]
            first = next((i for i, (w, g) in enumerate(zip(wanted, plain))
                          if w != g), None)
            where = (f"first difference at line {first + 1}: host "
                     f"{wanted[first]!r}, ThreadOS {plain[first]!r}"
                     if first is not None else
                     f"host {len(wanted)} lines, ThreadOS {len(plain)}")
            check(f"({cpus} core(s)) glibtest prints exactly what it prints "
                  f"against this machine's GLib ({where})", plain == wanted)
        else:
            noes = [l for l in plain if l.endswith(": no")]
            check(f"({cpus} core(s)) glibtest says yes to all its own "
                  f"questions ({noes or 'all yes'}) and gets to 'done'",
                  not noes and plain[-1:] == ["done"])
        check(f"({cpus} core(s)) and ends with code 0",
              "[glibtest] done, exit code 0" in out)
        measured = "\n".join(l for l in got if l.startswith("~"))

        def number(pattern):
            m = re.search(pattern, measured)
            return int(m.group(1)) if m else None
        woke = number(r"~pipe watch woke after (\d+) ms")
        check(f"({cpus} core(s)) the loop woke when the pipe was written, "
              f"not a tick later ({woke} ms after the start, written at 80)",
              woke is not None and woke < 100)
        spawn = number(r"~spawning took (\d+) ms")
        check(f"({cpus} core(s)) spawning a child took {spawn} ms",
              spawn is not None and spawn < 1000)
        version = re.search(r"~GLib (\S+)", measured)
        print(f"   ({cpus} core(s)) GLib {version.group(1) if version else '?'}"
              f"; a callback from a thread woke the loop "
              f"{number(r'~the loop woke: (\d+) ms')} ms after the send")
    finally:
        q.close()
        shutil.rmtree(work, ignore_errors=True)


def main():
    print("--- GLib ---")
    work = tempfile.mkdtemp()
    try:
        host = host_glibtest(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    want = None
    if host is None:
        print("SKIPPED: no GLib on this machine (pkg-config gio-2.0, "
              "libglib2.0-dev) to compare glibtest with")
    else:
        check("glibtest builds and runs against this machine's GLib",
              host[1] == 0)
        want = host[0]
        check(f"(and ends there with 'done': {want[-1]!r})",
              want[-1] == "done")
    one(1, want)
    one(4, want)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
