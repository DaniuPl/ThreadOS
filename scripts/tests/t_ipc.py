#!/usr/bin/env python3
"""ThreadOS smoke test: processes talking to each other.

What a program built of several processes needs - WebKit is one, with
a process for the window, one per page and one for the network:
pairs of connected Unix sockets that carry descriptors with their
bytes, and shared memory with a descriptor of its own
(kernel64/ipc64.c, libc/overlay/src/threados/unix.c).

libc/tests/ipctest.c is built a second time on this machine against
glibc and run here on Linux; ThreadOS has to print the same lines, on
one core and on four:

  socketpair with SOCK_STREAM and SOCK_SEQPACKET; a stream gathering
  two writes into one read and splitting one write over two reads;
  MSG_PEEK; MSG_DONTWAIT and O_NONBLOCK saying EAGAIN, on an empty
  socket and on a full one; readv and writev; FIONREAD, SO_TYPE, fstat;
  shutdown one way while the other still works

  seqpacket messages kept whole, an empty one included; one longer than
  the room cut and said to be (MSG_TRUNC); writev as one message

  memfd_create, write and read and lseek on it, ftruncate, two shared
  mappings seeing each other's writes, a private one keeping its own,
  munmap of one leaving the other

  SCM_RIGHTS: three descriptors made after a fork - a pipe's end, a
  memfd, another socket's end - sent to the child, which writes into
  the pipe, answers in the shared memory and talks over the socket it
  was sent; two descriptors with room for one (MSG_CTRUNC)

  a megabyte from the child in pieces of every size, intact; poll
  sleeping until its timeout, woken by a write from a thread, and what
  it says once the other end is gone; EOF and EPIPE

  pipes, which are pairs of the same ends now: what poll says of each
  end (a main loop waits on a pipe), O_NONBLOCK and pipe2, EAGAIN on an
  empty and on a full one, a hundred open at once where the whole
  system used to have eight, a megabyte from a child

And ThreadOS's own numbers: a message there and back between two
processes, over a socket pair and over two pipes, takes well under the
10 ms a waiting read or poll used to nap for, and the machine has its
memory back once it is all over.

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


def host_ipctest(work):
    """ipctest built against this machine's glibc and run: its lines"""
    exe = os.path.join(work, "ipctest")
    src = os.path.join(ROOT, "libc", "tests", "ipctest.c")
    if subprocess.run(["gcc", "-O2", "-w", "-o", exe, src, "-lpthread"]).returncode:
        return None
    p = subprocess.run([exe], cwd=work, capture_output=True, text=True,
                       timeout=120)
    return [l for l in p.stdout.splitlines() if l], p.returncode


def lines(out):
    """What the program printed, without the shell's echo and the
    kernel's own lines"""
    body = []
    for l in out.replace("\r", "").split("\n")[1:]:
        l = l.strip()
        if l and not l.startswith(("[", ) + PROMPTS):
            body.append(l)
    return body


def free_kib(text):
    m = re.search(r"\((\d+) KiB free\)", text)
    return int(m.group(1)) if m else None


def one(cpus, want):
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
             cpus=cpus if cpus > 1 else None, append="nodesktop")
    try:
        q.wait_serial("The 64-bit system is ready", timeout=90)
        before = free_kib(q.command("mem", "free)", timeout=20))
        out = q.command("run ipctest", "[ipctest] done, exit code", timeout=180)
        got = lines(out)
        plain = [l for l in got if not l.startswith("~")]
        wanted = [l for l in want if not l.startswith("~")]
        first = next((i for i, (w, g) in enumerate(zip(wanted, plain))
                      if w != g), None)
        where = (f"first difference at line {first + 1}: glibc {wanted[first]!r}, "
                 f"ThreadOS {plain[first]!r}" if first is not None else
                 f"glibc {len(wanted)} lines, ThreadOS {len(plain)}")
        check(f"({cpus} core(s)) ipctest prints exactly what glibc's build "
              f"prints on Linux ({where})", plain == wanted)
        check(f"({cpus} core(s)) and ends with code 0",
              "[ipctest] done, exit code 0" in out)

        tagged = {l.split(":")[0]: l.split(":", 1)[1].strip()
                  for l in got if l.startswith("~") and ":" in l}
        trip = re.match(r"(\d+) us", tagged.get("~round trip", ""))
        check(f"({cpus} core(s)) a message there and back between two "
              f"processes takes under 2 ms ({tagged.get('~round trip')}): "
              f"waking, not a 10 ms nap",
              trip is not None and int(trip.group(1)) < 2000)
        pipe = re.match(r"(\d+) us", tagged.get("~pipe round trip", ""))
        check(f"({cpus} core(s)) and through two pipes too "
              f"({tagged.get('~pipe round trip')})",
              pipe is not None and int(pipe.group(1)) < 2000)
        print(f"   ({cpus} core(s)) poll: {tagged.get('~poll(120 ms) took')}, "
              f"{tagged.get('~woken after')}; a full socket held "
              f"{tagged.get('~queued before it was full')}")

        after = free_kib(q.command("mem", "free)", timeout=20))
        check(f"({cpus} core(s)) the machine has its memory back afterwards: "
              f"messages, memfds and their mappings ({before} -> {after} "
              f"KiB free)",
              before is not None and after is not None and
              abs(before - after) < 256)
    finally:
        q.close()
        shutil.rmtree(work, ignore_errors=True)


def main():
    print("--- processes talking: socket pairs, descriptors, shared memory ---")
    work = tempfile.mkdtemp()
    try:
        host = host_ipctest(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    check("ipctest builds and runs on this machine against glibc too",
          host is not None and host[1] == 0)
    if host is None:
        return summary()
    want = host[0]
    check(f"(and Linux says yes to everything: {want[-1]!r})",
          want[-1] == "everything as expected: yes")
    one(1, want)
    one(4, want)
    return summary()


sys.exit(main())
