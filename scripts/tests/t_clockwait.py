#!/usr/bin/env python3
"""ThreadOS smoke test: C++'s timed waits, timed on steady_clock.

libstdc++ was configured for glibc, and for every wait_for, every
try_lock_for and wait_until on steady_clock it calls glibc's
clock-taking waits - pthread_cond_clockwait, pthread_mutex_clocklock,
pthread_rwlock_clockrdlock and clockwrlock - with CLOCK_MONOTONIC and
a steady_clock deadline. musl has none of them; libc/cxx/compat.h
answers them with musl's timed calls, which read the deadline on
CLOCK_REALTIME. On ThreadOS CLOCK_REALTIME is seconds since 1970 and
CLOCK_MONOTONIC ticks since boot, so a deadline passed on unchanged had
gone decades before the wait began. A timed mutex gave up at once. A
condition variable's wait came back at once too, and libstdc++, which
reads steady_clock again and calls an early return a wakeup, said it
had been notified: a worker sleeping in cv.wait_for(lk, 100ms) spun,
and a wait with a predicate looked at it as fast as it could until
its deadline came. compat.h now moves the deadline onto CLOCK_REALTIME
first.

libc/tests/clockwait.cpp waits every way libstdc++ does, and calls the
four functions straight, each wait timed on steady_clock. It is built
on this machine against glibc and run here too: each outcome on
ThreadOS (timeout, woken, taken, ETIMEDOUT, EINVAL) has to be glibc's,
and each time within bounds of glibc's - a 300 ms wait at least 250 ms,
not 0, and short of a second more, which a carry lost between the
nanoseconds and the seconds would add. A wait with a predicate looks
at it twice on glibc, before and after; here it may look once or
twice more (a wait that ends a tick early is made again), but not the
hundreds of thousands of times of one that never sleeps.

The program is fetched onto the disk over HTTP and run from there, as
t_bigprog's is: only this test needs it, and the initrd is memory every
boot spends.

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
from ptest import check, summary, make_iso64, no64

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = "thread64>"
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
ELF = os.path.join(BUILD, "user64", "clockwait.elf")
LINE = re.compile(r"^(.*): (.*) \((-?\d+) ms\)$")
WORKER = "worker in wait_for 100 ms steps for 1 s"
LOOKS = re.compile(r"^(.*) after (\d+) looks$")


def parse(lines):
    """[(what, outcome, ms)] from the program's lines, and whether the
    last of them is "done" """
    got = []
    for l in lines:
        m = LINE.match(l.strip())
        if m:
            got.append((m.group(1), m.group(2), int(m.group(3))))
    return got, bool(lines) and lines[-1].strip() == "done"


def host_output(work):
    """clockwait built and run against this machine's glibc"""
    exe = os.path.join(work, "clockwait")
    src = os.path.join(ROOT, "libc", "tests", "clockwait.cpp")
    if subprocess.run(["g++", "-std=gnu++17", "-O2", "-w", "-pthread",
                       "-o", exe, src]).returncode:
        return None
    p = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    return p.stdout.splitlines(), p.returncode


def wait_after(q, seen, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if text in q.serial()[seen:]:
            break
        time.sleep(0.5)
    return q.serial()[seen:]


def run(q, command, timeout=120):
    """A program from the kernel's shell: the lines it printed after the
    shell's echo of the command, and its exit code"""
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    out = wait_after(q, seen, f"[{name}] done, exit code", timeout)
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    text = out.replace("\r", "")
    cut = text.find(command + "\n")
    lines = (text[cut + len(command) + 1:].split("\n") if cut >= 0
             else text.split("\n")[1:])
    body = [l for l in lines if l and not l.startswith(
            ("[", PROMPT, "ThreadOS 64 - console"))]
    return body, int(m.group(1)) if m else None


def wakeups(outcome):
    m = re.match(r"(\d+) wakeups$", outcome)
    return int(m.group(1)) if m else None


def compare(want, got):
    """ThreadOS's lines against glibc's, one by one"""
    check(f"ThreadOS reports every wait glibc's build does "
          f"({len(got)} of {len(want)}, in the same order)",
          [w[0] for w in want] == [g[0] for g in got])
    for (what, w_out, w_ms), (g_what, g_out, g_ms) in zip(want, got):
        if what != g_what:
            break
        w_looks, g_looks = LOOKS.match(w_out), LOOKS.match(g_out)
        if what == WORKER:
            # The count is the time a wait takes, not a fixed answer:
            # ten on glibc, give or take one - and thousands when every
            # wait returns at once
            n = wakeups(g_out)
            check(f"{what}: {g_out}, glibc {w_out} (between 5 and 15)",
                  n is not None and 5 <= n <= 15)
        elif w_looks:
            check(f"{what}: {g_out}, as glibc's {w_looks.group(1)}",
                  g_looks is not None and
                  g_looks.group(1) == w_looks.group(1))
            check(f"{what}: it slept rather than spun, looking at its "
                  f"predicate {g_looks.group(2) if g_looks else '?'} "
                  f"times (glibc {w_looks.group(2)}; at most 5)",
                  g_looks is not None and int(g_looks.group(2)) <= 5)
        else:
            check(f"{what}: {g_out}, as glibc's {w_out}", g_out == w_out)
        # What was asked for, from glibc's own time: 300 ms, 100 ms, 0
        asked = round(w_ms / 100) * 100
        check(f"{what}: {g_ms} ms (glibc {w_ms}; between {asked - 50} "
              f"and {asked + 950})", asked - 50 <= g_ms <= asked + 950)


def main():
    print()
    print("--- C++'s timed waits on steady_clock (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    check(f"the program is built ({ELF})", os.path.exists(ELF))
    if not os.path.exists(ELF):
        return summary()

    work = tempfile.mkdtemp()
    http_port, http_stop = start_http_server()
    with open(ELF, "rb") as fh:
        SERVED["/clockwait"] = fh.read()
    try:
        host = host_output(work)
        check("clockwait builds and runs against glibc on this machine",
              host is not None and host[1] == 0)
        if host is None:
            return summary()
        want, host_done = parse(host[0])
        outcomes = dict((w[0], LOOKS.sub(r"\1", w[1])) for w in want)
        check("(the comparison is not empty: glibc's build reports every "
              f"wait, {len(want)} of them, and ends with done)",
              len(want) == 19 and host_done and
              outcomes.get("condition_variable wait_for 300 ms") ==
              "timeout" and
              outcomes.get("condition_variable wait_for 5 s, notified at "
                           "100 ms") == "woken" and
              outcomes.get("timed_mutex try_lock_for 300 ms, held") ==
              "not taken")

        iso = os.path.join(work, "t.iso")
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.truncate(DISK_SIZE)
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64", cpus=2)
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            _, code = run(q, f"run httpget 10.0.2.2 {http_port} "
                             "/clockwait clockwait", timeout=120)
            check("the program reaches the ThreadOS disk over HTTP",
                  code == 0)
            lines, code = run(q, "run clockwait", timeout=120)
            got, done = parse(lines)
            check(f"clockwait runs and ends with code 0 after saying done "
                  f"(code {code}, last line {lines[-1:]})",
                  code == 0 and done)
            compare(want, got)
        finally:
            q.close()
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
