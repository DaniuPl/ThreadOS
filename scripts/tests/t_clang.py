#!/usr/bin/env python3
"""ThreadOS test: clang compiles and links C programs on ThreadOS.

LLVM's clang and lld, built as one ThreadOS program (Makefile: `make
clang`, apps/clang/threados.cfg), are put on the disk this test boots
from the way they lie on an installed system - /usr/bin/clang beside
/usr/bin/ld.lld, clang's headers and compiler-rt's builtins under
/usr/lib/clang, musl's headers and libraries under /usr/include and
/usr/lib. Then, typed into the shell on ThreadOS:

  clang --version names the release and ThreadOS's target

  clang builds a hello world, and the program it built runs and gets
  its arguments

  it builds scripts/tests/clang/ccprog.c - 128-bit division and
  conversions from compiler-rt, the x87's long double, floating point
  printed to the digit, threads with thread-local storage, qsort, switch
  tables, files - and that program prints exactly what the same source
  built by this machine's gcc against glibc prints

  two source files go into one program in one command

  a mistake in a source is pointed at by file, line and column, and
  clang ends with code 1

and the program it wrote is, read off the disk afterwards, the kind
the kernel loads: a static executable at 0x40000000, without the
relocation sections the loader refuses.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, Ext2Image

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
SDK = os.path.join(BUILD, "clang-sdk")
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "clang")
PROMPT = "thread64>"
CLANG = "/usr/bin/clang"
DONE = "[/usr/bin/clang] done, exit code"
SOURCES = ["cchello.c", "ccprog.c", "ccmain.c", "ccutil.c", "ccbad.c"]


def host_run(sources, work):
    """What the sources print built by this machine's gcc against glibc:
    the independent half of the comparison"""
    exe = os.path.join(work, "host-" + os.path.splitext(sources[0])[0])
    subprocess.run(["gcc", "-O2", "-o", exe] +
                   [os.path.join(SRC, s) for s in sources] +
                   ["-lm", "-lpthread"], check=True)
    out = subprocess.run([exe], cwd=work, check=True, capture_output=True,
                         text=True).stdout
    return [l for l in out.splitlines() if l.strip()]


def make_disk(work):
    tree = os.path.join(work, "tree")
    os.mkdir(tree)
    # cp -a keeps the symbolic links: the compiler is on the disk once,
    # under every name it answers to
    subprocess.run(["cp", "-a", os.path.join(SDK, "usr"), tree], check=True)
    for d in ("tmp", "w"):
        os.mkdir(os.path.join(tree, d))
        os.chmod(os.path.join(tree, d), 0o1777)
    for s in SOURCES:
        shutil.copy(os.path.join(SRC, s), os.path.join(tree, "w", s))
    size = int(subprocess.run(["du", "-sm", tree], check=True,
                              capture_output=True, text=True).stdout.split()[0])
    img = os.path.join(work, "disk.img")
    subprocess.run(["truncate", "-s", "%dM" % (size + 96), img], check=True)
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                    "-L", "ThreadOS", "-d", tree, img], check=True)
    return img


def in_order(want, out):
    """Every line of want in out, one after another"""
    at = 0
    for line in want:
        i = out.find(line, at)
        if i < 0:
            return line
        at = i + len(line)
    return None


def elf_facts(data):
    """(type, entry, names of its sections) of an ELF64 file"""
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return None
    e_type, = struct.unpack_from("<H", data, 16)
    entry, = struct.unpack_from("<Q", data, 24)
    shoff, = struct.unpack_from("<Q", data, 40)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 58)
    sections = []
    for i in range(shnum):
        sh = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        sections.append(sh)
    strtab = sections[shstrndx] if shnum else None
    names = []
    for sh in sections:
        if strtab:
            off = strtab[4] + sh[0]
            names.append(data[off:data.index(b"\0", off)].decode())
    return e_type, entry, [(n, sh[1]) for n, sh in zip(names, sections)]


def compile_on(q, args, timeout=900):
    out = q.command("run %s %s" % (CLANG, args), DONE, timeout=timeout)
    tail = q.wait_serial(PROMPT, timeout=30) if DONE in out else ""
    return (out + tail).replace("\r", "")


def run_on(q, name, args="", timeout=300):
    done = "[%s] done, exit code" % name
    out = q.command(("run %s %s" % (name, args)).strip(), done,
                    timeout=timeout)
    return out.replace("\r", "")


def code_of(out, name):
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    return int(m.group(1)) if m else None


def main():
    print("--- clang on ThreadOS ---")
    if not os.path.exists(os.path.join(SDK, "done.stamp")):
        print("SKIPPED: no " + SDK + " - make clang")
        return summary()

    work = tempfile.mkdtemp()
    try:
        want_prog = host_run(["ccprog.c"], work)
        want_two = host_run(["ccmain.c", "ccutil.c"], work)
        img = make_disk(work)
        q = Qemu(KERNEL, INITRD, img, sata=True, append=TEXT_MODE,
                 ram_mib=2048, serial=os.path.join(work, "s.log"))
        try:
            q.wait_serial(PROMPT, timeout=120)

            out = compile_on(q, "--version", timeout=600)
            check("clang --version runs on ThreadOS and names its release "
                  "(%s)" % (re.search(r"clang version \S+", out) or
                            [None])[0],
                  "clang version 23.1.6" in out and
                  code_of(out, CLANG) == 0)
            check("and the target it builds for by default",
                  "Target: x86_64-unknown-linux-musl" in out)

            q.command("cd /w", PROMPT, timeout=20)
            out = compile_on(q, "-O2 cchello.c -o cchello")
            check("clang builds a hello world on ThreadOS (exit code %s)" %
                  code_of(out, CLANG), code_of(out, CLANG) == 0)
            out = run_on(q, "cchello", "ThreadOS")
            check("and the program it built runs, with its arguments",
                  "hello from clang on ThreadOS: argc=2 argv[1]=ThreadOS"
                  in out and code_of(out, "cchello") == 0)

            out = compile_on(q, "-O2 ccprog.c -o ccprog")
            check("clang builds ccprog.c (exit code %s)" %
                  code_of(out, CLANG), code_of(out, CLANG) == 0)
            out = run_on(q, "ccprog")
            missing = in_order(want_prog, out)
            check("ccprog prints, line for line, what gcc and glibc's build "
                  "of it prints here (%d lines; first missing: %s)" %
                  (len(want_prog), missing),
                  missing is None and code_of(out, "ccprog") == 0)

            out = compile_on(q, "-O2 ccmain.c ccutil.c -o cctwo")
            check("two source files into one program in one command "
                  "(exit code %s)" % code_of(out, CLANG),
                  code_of(out, CLANG) == 0)
            out = run_on(q, "cctwo")
            check("and it prints what the host's build prints (%s)" %
                  want_two, in_order(want_two, out) is None)

            out = compile_on(q, "ccbad.c")
            said = [l for l in out.split("\n") if "error" in l][:2]
            check("a mistake is pointed at by file, line and column (%r)" %
                  said,
                  re.search(r"ccbad\.c:6:\d+: error: use of undeclared "
                            r"identifier 'undeclared_name'", out) is not None)
            check("and clang ends with code 1", code_of(out, CLANG) == 1)
        finally:
            q.close()

        data = Ext2Image(img).read("/w/cchello")
        facts = elf_facts(data) if data else None
        check("the program clang wrote is an executable (ELF type 2) "
              "starting above 0x40000000",
              facts is not None and facts[0] == 2 and
              0x40000000 <= facts[1] < 0x4F000000)
        check("without relocation sections, which the kernel's loader "
              "refuses (%s)" % ([n for n, t in facts[2] if t in (4, 9)]
                                if facts else "-"),
              facts is not None and
              not [n for n, t in facts[2] if t in (4, 9)])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
