#!/usr/bin/env python3
"""ThreadOS smoke test: a C library (musl) and JavaScript (QuickJS),
64-bit kernel.

Up to now every ThreadOS program carried what little of a C library it
needed with it. musl is the real thing - stdio, malloc, strtod, printf
with every format, files, errno, fork and wait - built unchanged, with
only its system calls translated into ThreadOS's (libc/). On top of it
runs the first program nobody wrote for ThreadOS: Fabrice Bellard's
QuickJS, a whole JavaScript engine.

Both are checked from outside:

  libctest is plain portable C. It is built a second time on this
  machine against its own C library (glibc), run here, and the two
  outputs have to agree line for line - printf's rounding, strtod,
  the heap, files (truncating, appending, seeking from the end,
  renaming), errno and its messages, fork and an exit code

  js runs scripts whose output Node.js computes on this machine, and
  has to print exactly the same: number formatting, Unicode strings,
  regular expressions, JSON, sorting, classes, closures, generators,
  promises and timers in Node's order, console.log's formatting of
  objects and arrays - and the console takes lines typed on the
  keyboard

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


def host_libctest(work):
    """libctest built and run against this machine's own C library"""
    exe = os.path.join(work, "libctest")
    src = os.path.join(ROOT, "libc", "tests", "libctest.c")
    if subprocess.run(["gcc", "-O2", "-w", "-o", exe, src, "-lm"]).returncode:
        return None
    run = os.path.join(work, "run")
    os.makedirs(run)
    p = subprocess.run([exe, "arg1"], cwd=run, capture_output=True, text=True)
    return p.stdout.splitlines() + p.stderr.splitlines(), p.returncode


JS_DIR = os.path.join(ROOT, "scripts", "tests", "js")


def node(path):
    """What Node.js prints for a script, or None without Node.js"""
    if not shutil.which("node"):
        return None
    p = subprocess.run(["node", path], capture_output=True, text=True,
                       timeout=60)
    # (split at newlines only: U+2028 is a line break to Python, and
    # just a character to a terminal)
    return [l for l in p.stdout.split("\n") if l]


def wait_after(q, seen, text, timeout):
    """What arrived on the port after 'seen', once it holds the text -
    the same program's earlier end is further back in the log"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = q.serial()[seen:]
        if text in out:
            break
        time.sleep(0.5)
    return q.serial()[seen:]


def fetch(q, port, name):
    """A file from the test's HTTP server onto the ThreadOS disk"""
    _, code = run(q, f"run httpget 10.0.2.2 {port} /{name} {name}")
    return code == 0


def run(q, command, timeout=60):
    """A program from the kernel's shell; what it printed, the exit code"""
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    out = wait_after(q, seen, f"[{name}] done, exit code", timeout)
    # (the port's log is read as bytes; what programs print is UTF-8)
    out = out.encode("latin1").decode("utf-8", "replace")
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    # The echo of the command itself, and the kernel's line at the end
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    if os.environ.get("LIBC_DEBUG"):
        print("\n".join(body))
    return body, int(m.group(1)) if m else None


def libc(q, work):
    host = host_libctest(work)
    check("libctest builds and runs on this machine too", host is not None)
    if host is None:
        return
    want, want_code = host
    got, code = run(q, "run libctest arg1", timeout=90)
    plain_w = [l for l in want if not l.startswith("~")]
    plain_g = [l for l in got if not l.startswith("~")]
    # The two sides print stderr at different moments (it is not
    # buffered, stdout is) - so the comparison is of the lines, each
    # in its place among the others of its kind
    diff = [(w, g) for w, g in zip(sorted(plain_w), sorted(plain_g)) if w != g]
    check("libctest prints exactly what glibc's build prints, line for line "
          f"({len(plain_g)} lines; first differences: {diff[:6]})",
          not diff and len(plain_w) == len(plain_g))
    check("in the same order", [l for l in plain_g if not l.startswith("stderr")]
          == [l for l in plain_w if not l.startswith("stderr")])
    check(f"and ends with main's return value, 3 (got {code})",
          code == want_code == 3)
    tagged = {l.split(":")[0]: l.split(":", 1)[1].strip()
              for l in got if l.startswith("~")}
    check(f"the date comes from the machine's clock ({tagged.get('~date')})",
          tagged.get("~date") == RTC_DATE)
    check(f"uname says ThreadOS ({tagged.get('~uname')})",
          tagged.get("~uname") == "ThreadOS x86_64")
    check("standard output on the console is a terminal",
          tagged.get("~isatty") == "1")


def js_against_node(q, port, name, timeout=240):
    want = node(os.path.join(JS_DIR, name))
    if want is None:
        print(f"SKIPPED: no Node.js to compare {name} with")
        return
    check(f"{name} reaches the ThreadOS disk", fetch(q, port, name))
    got, code = run(q, f"run js {name}", timeout=timeout)
    first = next((i for i, (w, g) in enumerate(zip(want, got)) if w != g),
                 None)
    where = (f"first difference at line {first + 1}: node {want[first]!r}, "
             f"ThreadOS {got[first]!r}" if first is not None else
             f"node {len(want)} lines, ThreadOS {len(got)}")
    check(f"js {name} prints exactly what Node.js prints ({where})",
          got == want)
    check(f"and ends with code 0 (got {code})", code == 0)
    return got


def javascript(q, port):
    lang = js_against_node(q, port, "lang.js")
    if lang:
        check("(the comparison is not empty: the script printed "
              f"{len(lang)} lines, among them a sieve's count of primes)",
              len(lang) > 60 and any(l.startswith("2262 ") for l in lang))
    js_against_node(q, port, "async.js")

    # Files on the ThreadOS disk, and what the kernel itself sees in
    # them afterwards
    check("files.js reaches the disk", fetch(q, port, "files.js"))
    got, code = run(q, "run js files.js one two", timeout=90)
    text = "Zażółć gęślą jaźń\nline two\n"
    check("writeFile writes the UTF-8 bytes and says how many "
          f"({[l for l in got if l.startswith('written')]})",
          f"written {len(text.encode())}" in got)
    check("readFile gives the same string back",
          f"read back the same true {len(text)}" in got)
    check("JSON through a file", "json sum 10" in got)
    check("a missing file is an exception with the C library's reason",
          "missing: TypeError readFile 'nope.txt': No such file or directory"
          in got)
    check("the script's arguments, its own name first (as in QuickJS's qjs)",
          "args [ 'files.js', 'one', 'two' ]" in got)
    check(f"exit(4) is the program's exit code (got {code})", code == 4)
    seen = len(q.serial())
    q.type_line("cat pl.txt")
    time.sleep(3)
    raw = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    check("the kernel's own 'cat' shows the file the script wrote",
          "Zażółć gęślą jaźń" in raw and "line two" in raw)

    # One line of code from the command line; an exception nobody
    # caught is exit code 1
    got, code = run(q, "run js -e console.log([6*7, 'x'.repeat(3)])")
    check(f"js -e runs the rest of the line ({got[:1]})",
          "[ 42, 'xxx' ]" in got and code == 0)
    got, code = run(q, "run js -e null.x")
    check(f"an uncaught exception is reported, exit code 1 ({got[:1]}, {code})",
          any(l.startswith("Uncaught TypeError") for l in got) and code == 1)


def console(q):
    """js with no file is a console, typed into from the keyboard"""
    seen = len(q.serial())
    q.type_line("run js")
    wait_after(q, seen, "Type \".help\"", 40)
    answers = []
    for line, want in [("1 + 2 * 3", "7"),
                       ("let xs = [3, 1, 2]", "undefined"),
                       ("xs.sort()", "[ 1, 2, 3 ]"),
                       ("'gęś'.toUpperCase()", "'GĘŚ'"),
                       ("xs.map(v => v * 10)", "[ 10, 20, 30 ]"),
                       ("Math.sqrt(2)", "1.4142135623730951")]:
        mark = len(q.serial())
        if "ę" in line:
            # (AltGr is how the keyboard makes Polish letters)
            q.type_line("'g", enter=False)
            q.sendkey("alt_r-e")
            q.sendkey("alt_r-s")
            q.type_line("'.toUpperCase()")
        else:
            q.type_line(line)
        time.sleep(1.5)
        out = q.serial()[mark:].encode("latin1").decode("utf-8", "replace")
        answers.append((line, want, want in out.replace("\r", "").split("\n")))
    bad = [a for a in answers if not a[2]]
    check(f"the console answers each line typed ({len(answers) - len(bad)} of "
          f"{len(answers)}; wrong: {bad})", not bad)
    mark = len(q.serial())
    q.type_line("nosuch")
    time.sleep(1.5)
    check("a mistake is reported and the console goes on",
          "Uncaught ReferenceError" in q.serial()[mark:])
    q.type_line(".exit")
    out = wait_after(q, seen, "[js] done, exit code", 30)
    check("and .exit leaves it", "[js] done, exit code 0" in out)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    http_port, http_stop = start_http_server()
    for name in ("lang.js", "async.js", "files.js"):
        with open(os.path.join(JS_DIR, name), "rb") as fh:
            SERVED["/" + name] = fh.read()
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    slog = os.path.join(work, "serial.log")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("] Disk ", timeout=30)
            libc(q, work)
            q.wait_serial("[net] DHCP", timeout=40)
            javascript(q, http_port)
            console(q)
        finally:
            q.close()
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
