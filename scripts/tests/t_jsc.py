#!/usr/bin/env python3
"""ThreadOS smoke test: JavaScriptCore, WebKit's JavaScript engine.

The first piece of WebKit on ThreadOS: its engine, built from
WebKitGTK's release by WebKit's own CMake (apps/webkit/), runs as 'jsc',
the engine's shell. What it prints is held to what Node.js prints for
the same files - another engine, V8, on the same ICU:

  an expression from the command line

  core.js, the language: numbers printed shortest-first, strings,
  regular expressions with named groups and Unicode properties,
  classes with private fields, generators, destructuring, typed
  arrays, Proxy, JSON, dates, the errors' kinds, and promises, timers
  and async functions in the order they happen

  intl.js, what rests on ICU: Unicode properties and case,
  normalization, Polish and English collation, numbers, currencies,
  plurals, dates in time zones, relative times, lists, word and
  grapheme segmentation, display names - and the collector, after tens
  of thousands of objects made and dropped, with the kept ones intact

  an exception nobody catches ends the program with an error code and
  says what it was

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
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary  # noqa: E402

JS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "js")
PROMPTS = ("thread64>", "thread64#")    # the kernel's shell, plain and admin
THROWS = (b"function deep(n) {\n"
          b"    if (!n) throw new TypeError('no luck');\n"
          b"    deep(n - 1);\n}\n"
          b"print('before');\ndeep(3);\nprint('never');\n")


def node(path):
    """What Node.js prints for a script, or None without Node.js"""
    if not shutil.which("node"):
        return None
    p = subprocess.run(["node", path], capture_output=True, text=True,
                       timeout=60)
    return [l for l in p.stdout.split("\n") if l]


def wait_after(q, seen, text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if text in q.serial()[seen:]:
            break
        time.sleep(0.5)
    return q.serial()[seen:]


def run(q, command, timeout=60):
    """A program from the kernel's shell: what it printed, its exit code"""
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    out = wait_after(q, seen, f"[{name}] done, exit code", timeout)
    out = out.encode("latin1").decode("utf-8", "replace")
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]") + PROMPTS)]
    return body, int(m.group(1)) if m else None


ICU_MAJOR = "78"                    # third_party/icu


def node_icu():
    """The major version of the ICU Node.js carries, or None"""
    if not shutil.which("node"):
        return None
    p = subprocess.run(["node", "-p", "process.versions.icu"],
                       capture_output=True, text=True, timeout=30)
    return p.stdout.strip().split(".")[0] or None


def against_node(q, name, timeout=300, same_icu=False):
    want = node(os.path.join(JS_DIR, name))
    if want is None:
        print(f"SKIPPED: no Node.js to compare {name} with")
        return None
    # The formats of dates and numbers are ICU's data, which changes
    # between its versions: a Node.js on another ICU is no reference
    if same_icu and node_icu() != ICU_MAJOR:
        print(f"SKIPPED: Node.js has ICU {node_icu()}, not {ICU_MAJOR}, "
              f"to compare {name} with")
        return None
    got, code = run(q, f"run jsc {name}", timeout=timeout)
    first = next((i for i, (w, g) in enumerate(zip(want, got)) if w != g),
                 None)
    where = (f"first difference at line {first + 1}: node {want[first]!r}, "
             f"jsc {got[first]!r}" if first is not None else
             f"node {len(want)} lines, jsc {len(got)}")
    check(f"jsc {name} prints exactly what Node.js prints ({where})",
          got == want)
    check(f"and ends with code 0 (got {code})", code == 0)
    return got


def main():
    for name in ("core.js", "intl.js"):
        with open(os.path.join(JS_DIR, name), "rb") as fh:
            ptest.SERVED["/" + name] = fh.read()
    ptest.SERVED["/throws.js"] = THROWS
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    port, stop_http = start_http_server()
    try:
        # The engine with its ICU data is the biggest program there is
        q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
                 ram_mib=512)
        try:
            check("the desktop came up", q.wait_desktop())
            q.click_desktop()
            got, code = run(q, "run jsc -e print(6*7,typeof(WebAssembly))")
            check(f"jsc evaluates an expression from the command line "
                  f"({got}, code {code})",
                  got == ["42 undefined"] and code == 0)

            fetched = 0
            for name in ("core.js", "intl.js", "throws.js"):
                _, c = run(q, f"run httpget 10.0.2.2 {port} /{name} {name}")
                fetched += c == 0
            check(f"the scripts reach the disk ({fetched} of 3)", fetched == 3)

            core = against_node(q, "core.js")
            if core:
                check(f"(core.js is not a short story: {len(core)} lines, "
                      f"the last the promises' order)", len(core) > 35 and
                      core[-1].startswith("start sync micro timer"))
            intl = against_node(q, "intl.js", same_icu=True)
            if intl:
                check(f"(and intl.js reached the collector's line: "
                      f"{intl[-2]!r})", intl[-2].startswith("60 "))

            got, code = run(q, "run jsc throws.js")
            text = "\n".join(got)
            check(f"an uncaught exception ends it with an error code ({code}) "
                  f"and says what it was ({got[:3]})",
                  code not in (0, None) and "TypeError: no luck" in text and
                  "before" in got and "never" not in got)
        finally:
            q.close()
    finally:
        stop_http()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
