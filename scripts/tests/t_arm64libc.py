#!/usr/bin/env python3
"""ThreadOS smoke test: musl on the ARM64 kernel.

The C library the PC's programs are built against - musl, unchanged,
with the layer that turns its Linux calls into ThreadOS's
(libc/overlay) - built for aarch64 (make kernelarm64) and run on the
ARM64 kernel, which takes the PC's call numbers (kernelarm64/proc.c).
The programs are the PC's own tests, libc/tests/libctest.c and
pthreadtest.c, unchanged in what they print. Each is built a second
time on this machine against glibc, run with the same arguments, and
the outputs have to agree line for line (lines starting with '~' are
the machine's own and are read on their own terms):

  libctest memory sans.ttf: printf with every format, strtod, the
  math library, strings, setjmp, the heap - 3 MiB blocks, which musl
  maps, freed and asked for again, realloc and calloc - a file of the
  initrd read through stdio and by its descriptor (stat, lseek, pread),
  one that is not there, nanosleep against the monotonic clock, gmtime,
  atexit and main's return value; and the date from the PL031 clock

  pthreadtest with every test that needs neither a disk to write nor a
  second process: a mutex and atomics under contention, joins, thread-
  local storage, a queue on condition variables, semaphores, once,
  barriers, rwlocks, forty detached threads and three hundred created
  and joined one after another, a timed wait - on one core, where only
  the timer moves them, and on four, where they run at once (a '~'
  line says on how many cores sched_getcpu saw them)

  pthreadtest endall: exit(5) in a thread ends the whole program - with
  one thread computing at EL0 and one asleep - with code 5, as glibc's
  build does

and after each run every page the program had is free again, its
threads' included.

Needs qemu-system-aarch64 and `make kernelarm64` (which needs the cross
compiler, g++-aarch64-linux-gnu, for musl).
"""

import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, ARM64_INITRD, ARM64_READY

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

THREAD_TESTS = ["mutex", "join", "tls", "queue", "sem", "sync", "lifetime",
                "timed", "spread"]


def host_build(work, name, *flags):
    exe = os.path.join(work, name)
    src = os.path.join(ROOT, "libc", "tests", name + ".c")
    if subprocess.run(["gcc", "-O2", "-w", *flags, "-o", exe, src]).returncode:
        return None
    return exe


def host_run(exe, args, cwd):
    p = subprocess.run([exe, *args], cwd=cwd, capture_output=True, text=True,
                       timeout=120)
    return p.stdout.splitlines() + p.stderr.splitlines(), p.returncode


def arm_run(name, args, cores):
    """The program on the ARM64 kernel: its lines, its code and what the
    kernel said of the pages"""
    said = arm64_boot(cores=cores, mib=512, initrd=ARM64_INITRD, timeout=240,
                      append="run=" + ",".join([name] + args))
    start = re.search(r"^\[proc\] %s started, pid (\d+)$" % name, said, re.M)
    end = re.search(r"^\[proc\] %s \(pid \d+\) ended with code (-?\d+)$"
                    % name, said, re.M)
    pages = re.search(r"pages free: (\d+) before the programs, (\d+) after",
                      said)
    body = said[start.end():end.start()] if start and end else ""
    lines = [l for l in body.split("\n") if l]
    return (lines, int(end.group(1)) if end else None,
            pages.group(0) if pages else "nothing said",
            bool(pages) and pages.group(1) == pages.group(2),
            ARM64_READY in said)


def same(want, got):
    """Every line but the '~' ones, each in its place among the others -
    stderr, unbuffered, may come at another moment on the two sides"""
    plain_w = [l for l in want if not l.startswith("~")]
    plain_g = [l for l in got if not l.startswith("~")]
    diff = [(w, g) for w, g in zip(sorted(plain_w), sorted(plain_g)) if w != g]
    in_order = [l for l in plain_g if not l.startswith("stderr")] == \
               [l for l in plain_w if not l.startswith("stderr")]
    ok = not diff and len(plain_w) == len(plain_g) and in_order
    return ok, (f"{len(plain_g)} lines" if ok else
                f"glibc {len(plain_w)} lines, ThreadOS {len(plain_g)}; "
                f"first differences {diff[:4]}")


def tagged(lines):
    """The '~' lines by their first word ("~cores 4", "~date: ...")"""
    pairs = (l[1:].split(" ", 1) for l in lines
             if l.startswith("~") and " " in l)
    return {k.rstrip(":"): v for k, v in pairs}


def main():
    print("--- musl on the ARM64 kernel ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(ARM64_INITRD):
        print("SKIPPED: no ARM64 initrd (make kernelarm64)")
        return summary()

    work = tempfile.mkdtemp()
    try:
        libctest = host_build(work, "libctest", "-lm")
        pthreadtest = host_build(work, "pthreadtest", "-pthread")
        check("both programs build against glibc on this machine too",
              libctest is not None and pthreadtest is not None)
        if libctest is None or pthreadtest is None:
            return summary()
        # The file the ARM64 side reads from its initrd, beside glibc's run
        run_dir = os.path.join(work, "run")
        os.makedirs(run_dir)
        shutil.copy(os.path.join(ROOT, "fonts", "sans.ttf"),
                    os.path.join(run_dir, "sans.ttf"))

        # --- the C library
        args = ["memory", "sans.ttf"]
        want, want_code = host_run(libctest, args, run_dir)
        got, code, pages, back, ready = arm_run("libctest", args, 1)
        ok, why = same(want, got)
        check(f"libctest prints exactly what glibc's build prints ({why})", ok)
        check("including a file of the initrd, read through stdio and by "
              "its descriptor, the same as glibc reads it",
              any(l.startswith("read: ") and "at the end 1" in l
                  for l in got) and
              any(l.startswith("lseek end ") for l in got))
        check(f"and ends with main's return value, 3, after atexit "
              f"(got {code})", code == want_code == 3 and
              got[-1:] == ["atexit: ran"])
        tags = tagged(got)
        today = datetime.datetime.now(datetime.timezone.utc).date()
        days = {str(today), str(today - datetime.timedelta(days=1))}
        check(f"the date is the clock's ({tags.get('date')})",
              tags.get("date") in days)
        check(f"uname says what it runs on ({tags.get('uname')})",
              tags.get("uname") == "ThreadOS aarch64")
        check(f"every page it had is free again ({pages})", back and ready)

        # --- threads, on one core and on four
        want, want_code = host_run(pthreadtest, THREAD_TESTS, run_dir)
        for cores in (1, 4):
            got, code, pages, back, ready = arm_run("pthreadtest",
                                                    THREAD_TESTS, cores)
            ok, why = same(want, got)
            check(f"{cores} core(s): pthreadtest prints exactly what "
                  f"glibc's build prints ({why})", ok)
            tags = tagged(got)
            ran = re.match(r"on (\d+)", tags.get("ran", ""))
            ran = int(ran.group(1)) if ran else 0
            check(f"{cores} core(s): sysconf counts the cores "
                  f"({tags.get('cores')}), and the computing threads ran "
                  f"on {ran}", tags.get("cores") == str(cores) and
                  (ran == 1 if cores == 1 else ran >= 2))
            # Main returns with a thread asleep it never joined: exit()
            # ends it, and its pages come back with everyone else's
            check(f"{cores} core(s): it ends with 0, every thread gone and "
                  f"every page free again ({pages})",
                  code == want_code == 0 and back and ready)

        # --- exit() in a thread
        want, want_code = host_run(pthreadtest, ["endall"], run_dir)
        for cores in (1, 4):
            got, code, pages, back, ready = arm_run("pthreadtest",
                                                    ["endall"], cores)
            check(f"{cores} core(s): exit(5) in a thread ends the program, "
                  f"a thread computing and one asleep with it: code {code}, "
                  f"glibc's {want_code} ({pages})",
                  code == want_code == 5 and back and ready)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
