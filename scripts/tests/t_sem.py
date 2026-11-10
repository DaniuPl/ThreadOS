#!/usr/bin/env python3
"""ThreadOS smoke test: counting semaphores and mutexes.

Threads that share memory need a way to wait for one another, and until
this group the system had none - two threads touching the same counter
simply raced. kernel/sem.c gives programs counting semaphores found by
key (a mutex is a semaphore of one), and two programs show what they
are for:

  mutex: two threads bump a shared counter through a deliberately long
  critical section (read, sleep, write). Unprotected, updates are lost -
  and the test insists they ARE lost, because a race that never happens
  would make the second half prove nothing. With the mutex, the count is
  exact.

  prodcons: a producer and a slow consumer on a buffer of four places,
  eight items. The sum is right, the items come out in the order they
  went in, and the buffer filled up - so the producer did have to wait
  on the semaphore, and it is the semaphore that held it.

A waiter sleeps a tick at a time and looks again, so a post is noticed
on the next tick rather than at once - slower, but the answers have to
be exact all the same. Ported from the thread-synchronisation work on
an older branch, which had only a single-core kernel.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

N = 10                        # bumps per thread (user/mutex.c)
ITEMS = 8                     # user/prodcons.c


def both(q):
    out = q.command("run mutex", "[mutex] unprotected", timeout=60)
    m = re.search(r"\[mutex\] unprotected=(\d+) with the mutex=(\d+)", out)
    without, with_ = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    check("the race is real: unprotected, updates are lost (%s of %d)"
          % (without, 2 * N), without is not None and without < 2 * N)
    check("with the mutex the count is exact (%s of %d)"
          % (with_, 2 * N), with_ == 2 * N)

    out = q.command("run prodcons", "[prodcons] taken", timeout=60)
    m = re.search(r"taken=(\d+) sum=(\d+) \(expected (\d+)\) in order=(\w+) "
                  r"buffer full=(\d+)", out)
    check("the consumer took every item (%s of %d)"
          % (m.group(1) if m else "?", ITEMS),
          m is not None and int(m.group(1)) == ITEMS)
    check("nothing lost, nothing taken twice (sum %s, expected %d)"
          % (m.group(2) if m else "?", ITEMS * (ITEMS + 1) // 2),
          m is not None and int(m.group(2)) == ITEMS * (ITEMS + 1) // 2)
    check("the items came out in the order they went in",
          m is not None and m.group(4) == "yes")
    check("the buffer filled, so the producer had to wait (%s times)"
          % (m.group(5) if m else "?"),
          m is not None and int(m.group(5)) > 0)


def main():
    print()
    print("--- semaphores and mutexes ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    check("the disc with the kernel builds", make_iso64(d, iso, text=True))
    q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog)
    try:
        q.wait_serial("thread64>", timeout=120)
        both(q)
        # The shell's own line about each program's end: a program that
        # printed the right numbers and then crashed is not a pass
        log = q.serial()
        check("both programs ended with exit code 0",
              "[mutex] done, exit code 0" in log and
              "[prodcons] done, exit code 0" in log)
    finally:
        q.close()
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
