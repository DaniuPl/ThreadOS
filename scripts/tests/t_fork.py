#!/usr/bin/env python3
"""ProtonOS smoke test: fork, exec and the process tree.

The Unix fork/exec pair is the heaviest part of process management,
because it touches everything at once: the address space, copy on write,
the descriptor table, signals and the scheduler.

Two assertions matter more than the rest here. The first: fork is NOT to
copy memory - the program takes 256 KiB, and the RAM growth right after
the fork is to be of the order of page tables, not of the buffer. The
second: the parent's pattern is to stay untouched also where it was the
KERNEL that wrote in the child, that is, in a buffer filled in by a
system call - that is looked after by the WP bit in the CR0 register and
it is easy to miss.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, pmm_used


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_for("ProtonOS shell")
        q.wait_for("[net] DHCP", timeout=20)
        time.sleep(0.5)

        # 'run forktest': fork() with copy on write. The program takes
        # 256 KiB, forks and checks that the child got the parent's
        # pattern and that its own writes stayed with it.
        q.type_line("clear")
        q.type_line("mem")
        used_before_fk = pmm_used(q.screen())
        q.type_line("run forktest")
        screen = q.wait_for("[forktest] child: the pattern was inherited",
                            timeout=30)
        check("fork: the child comes back from a syscall with the parent's memory",
              "[forktest] child: the pattern was inherited" in screen)
        m = re.search(r"RAM growth after the fork: (\d+) KiB", screen)
        check("fork does not copy the process's memory"
              + (f" (256 KiB of buffer, growth {m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) < 64)
        screen = q.wait_for("[forktest] parent: the pattern is untouched",
                            timeout=30)
        check("the child's writes are not seen by the parent (private copies of pages)",
              "[forktest] parent: the pattern is untouched" in screen)
        check("a kernel write in the child hit a private copy of the page too",
              "parent: the pattern is untouched" in screen)
        check("the parent takes the child's exit code through wait",
              "child code=7" in screen)

        # The other half of the Unix pair: the child turns itself into
        # another program through exec, and the parent waits for the same
        # pid.
        screen = q.wait_for("[forktest] the parent waited out pid", timeout=30)
        check("exec swaps the process image in place",
              "[forktest] after exec: another program, the same pid" in screen)
        check("after the exec the process is named after the new program",
              "process \'echo\' has ended (code 0)" in screen)
        check("the parent's wait hits the same pid after the exec",
              "waited out pid" in screen)
        q.wait_for("process \'forktest\' has ended (code 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_fk = pmm_used(q.screen())
        check(f"no frames leaked after the fork"
              f" ({used_before_fk} -> {used_after_fk})",
              used_after_fk is not None and used_after_fk == used_before_fk)

        # 'run ptree': the process tree. Three children finish in a
        # different order than they were created in, and the parent
        # collects them through waitany.
        q.type_line("clear")
        q.type_line("mem")
        used_before_pt = pmm_used(q.screen())
        q.type_line("run ptree")
        screen = q.wait_for("[ptree] every child collected", timeout=30)
        m = re.findall(r"a child finished: (\d+) with code (\d+)", screen)
        check("wait for any child gives them back in the order they finish"
              + (f" (pidy {[p for p, _ in m]})" if m else ""),
              len(m) == 3 and [int(p) for p, _ in m] == sorted(
                  (int(p) for p, _ in m), reverse=True))
        check("the exit code matches the child that returned it",
              [c for _, c in m] == ["22", "21", "20"])
        screen = q.wait_for("[ptree] the orphan was taken in", timeout=30)
        check("the orphan ends up in the grandparent's care (code 99 reaches it)",
              "with code 99" in screen)
        q.wait_for("process \'ptree\' has ended (code 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_pt = pmm_used(q.screen())
        check(f"no frames leaked after the process tree"
              f" ({used_before_pt} -> {used_after_pt})",
              used_after_pt is not None and used_after_pt == used_before_pt)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
