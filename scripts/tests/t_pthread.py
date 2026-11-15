#!/usr/bin/env python3
"""ThreadOS smoke test: POSIX threads in musl programs, 64-bit kernel.

musl's pthread_create used to answer ENOSYS, so no library that starts
a thread could run on ThreadOS. Now a thread is SYS64_CLONE (on the
library's own stack and thread pointer), waiting is SYS64_FUTEX, and
exit() is SYS64_EXIT_GROUP; threads of a program share its descriptor
table.

What is interesting is what goes wrong when any of that is missing,
and pthreadtest is written to show it in its output: a mutex that does
not exclude loses increments, a futex wake that comes between a
waiter's check and its sleep hangs the queue, a thread's exit that does
not clear its tid hangs pthread_join, thread-local variables that are
not per thread show another thread's value, a descriptor table copied
instead of shared hides a file one thread opened from the others, and
an exit() that ends only one thread leaves the program running.

pthreadtest is plain portable C. It is built a second time on this
machine against glibc and run here: the two outputs have to agree line
for line. It runs on a machine with four cores, so the threads really
run at the same time, and once more on one core, where every wait is a
switch to another thread.

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


def host_output(work):
    """pthreadtest built and run against this machine's glibc"""
    exe = os.path.join(work, "pthreadtest")
    src = os.path.join(ROOT, "libc", "tests", "pthreadtest.c")
    if subprocess.run(["gcc", "-O2", "-w", "-pthread", "-o", exe, src]).returncode:
        return None
    run = os.path.join(work, "run")
    os.makedirs(run, exist_ok=True)
    p = subprocess.run([exe], cwd=run, capture_output=True, text=True,
                       timeout=120)
    return p.stdout.splitlines(), p.returncode


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
    out = q.serial()[seen:]
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    return body, int(m.group(1)) if m else None, out


def tasks_named(q, name):
    """How many tasks 'ps' lists with this name"""
    seen = len(q.serial())
    q.type_line("ps")
    time.sleep(2)
    return sum(1 for l in q.serial()[seen:].split("\n")
               if re.search(r"\b%s\b" % name, l))


def one_machine(work, want, cpus):
    iso = os.path.join(work, f"t{cpus}.iso")
    slog = os.path.join(work, f"serial{cpus}.log")
    disk = os.path.join(work, f"disk{cpus}.img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)
    check(f"the disc with the 64-bit kernel builds ({cpus} core(s))",
          make_iso64(os.path.join(work, f"iso{cpus}"), iso))
    q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk, sata=True,
             qemu="qemu-system-x86_64", cpus=cpus if cpus > 1 else None)
    try:
        q.wait_serial(PROMPT, timeout=90)
        q.wait_serial("] Disk ", timeout=30)
        threads_before = tasks_named(q, "thread")
        got, code, raw = run(q, "run pthreadtest", timeout=240)
        plain_w = [l for l in want if not l.startswith("~")]
        plain_g = [l for l in got if not l.startswith("~")]
        first = next((i for i, (w, g) in enumerate(zip(plain_w, plain_g))
                      if w != g), None)
        where = (f"first difference: glibc {plain_w[first]!r}, ThreadOS "
                 f"{plain_g[first]!r}" if first is not None else
                 f"glibc {len(plain_w)} lines, ThreadOS {len(plain_g)}")
        check(f"{cpus} core(s): pthreadtest prints exactly what glibc's "
              f"build prints ({where})", plain_g == plain_w)
        check(f"{cpus} core(s): and ends with code 0 (got {code})", code == 0)
        print("   ", " ".join(l for l in got if l.startswith("~")))
        tagged = dict(l[1:].split(" ", 1) for l in got
                      if l.startswith("~") and " " in l)
        check(f"{cpus} core(s): sysconf counts the cores "
              f"({tagged.get('cores')})", tagged.get("cores") == str(cpus))
        ran = re.match(r"on (\d+)", tagged.get("ran", ""))
        ran = int(ran.group(1)) if ran else 0
        check(f"{cpus} core(s): four computing threads ran on "
              f"{'more than one core' if cpus > 1 else 'the one core'} "
              f"(sched_getcpu saw {ran})",
              ran >= 2 if cpus > 1 else ran == 1)
        check(f"{cpus} core(s): no thread was killed on the way "
              "(exit(5) in a thread ends its program quietly)",
              "killed" not in raw)
        # Every thread created along the way has ended - including one
        # main never joins, asleep when main returns, which exit() has
        # to stop (and in the child, one spinning in ring 3 and one
        # asleep, which exit(5) in a third thread had to stop)
        check(f"{cpus} core(s): no thread outlives its program "
              f"(ps lists {threads_before} 'thread' task(s) before, "
              f"{tasks_named(q, 'thread')} after)",
              tasks_named(q, "thread") == threads_before)
        # And the kernel's own shell still answers
        seen = len(q.serial())
        q.type_line("echo still here")
        time.sleep(2)
        check(f"{cpus} core(s): the system goes on afterwards",
              "still here" in q.serial()[seen:].split("echo", 1)[-1])
    finally:
        q.close()


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    work = tempfile.mkdtemp()
    try:
        host = host_output(work)
        check("pthreadtest builds and runs against glibc on this machine",
              host is not None and host[1] == 0)
        if host is None:
            return summary()
        want = host[0]
        check("(the comparison is not empty: glibc's build reports every "
              "check it makes)",
              len([l for l in want if not l.startswith("~")]) >= 17 and
              "mutex: 160000 of 160000" in want and
              "exit(5) in a thread: the program ends with 5" in want)
        one_machine(work, want, 4)
        one_machine(work, want, 1)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
