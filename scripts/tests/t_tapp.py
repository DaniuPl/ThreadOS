#!/usr/bin/env python3
"""ThreadOS smoke test: application packages (.tapp).

A package is an image in the initrd's format with a manifest first and
a signature last (scripts/mktapp.py). The loader hands each one over
as a module named "tapp" (here GRUB, from the disc's /boot/apps; the
installed system's loader in t_appsinst), and the kernel checks it and
takes it: its programs run by name after the initrd's, and one that
asks for a tile in the dock gets one.

Five packages go on the disc, made here:

  hello   signed with a key of the test's, a tile in the dock, a program
  plain   not signed, no tile
  arm     for aarch64 - not a package for this machine
  broken  no manifest
  twin    a second package called hello

and what is checked, from outside:

  the kernel takes hello and plain and says why it passes over the
  other three; 'tapp' lists the two as their manifests say; hello's
  program runs; 'tapp verify' says hello is not signed by the system's
  key, and - once update.key holds the test's key - that it is, that
  plain has no signature and that a damaged copy is not signed: each
  verdict the same as OpenSSL's on the host (mktapp.py --verify);
  'tapp info' prints the manifest mktapp wrote; and on the desktop the
  dock has a tile called Hello, whose click starts hello's program.

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
from ptest import check, summary, make_iso64, no64, DOCK_N, dock_tile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
ECHO = os.path.join(BUILD, "user64", "uecho64.elf")
MKTAPP = os.path.join(ROOT, "scripts", "mktapp.py")
PROMPT = "thread64>"


def mktapp(out, *args):
    return subprocess.run([sys.executable, MKTAPP, out] + list(args),
                          capture_output=True).returncode == 0


def host_verify(path, pub):
    """OpenSSL's verdict on a package, through mktapp.py"""
    r = subprocess.run([sys.executable, MKTAPP, "--verify", path, pub],
                       capture_output=True, text=True)
    return r.stdout.strip()


def run(q, command, end, timeout=60, password=False):
    seen = len(q.serial())
    if password:
        q.sudo(command, end, timeout=timeout)
    else:
        q.type_line(command)
    deadline = time.time() + timeout
    while time.time() < deadline and end not in q.serial()[seen:]:
        time.sleep(0.5)
    time.sleep(0.3)
    return q.serial()[seen:]


def main():
    print()
    print("--- application packages (.tapp) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(ECHO):
        print("SKIPPED: no uecho64 - make")
        return summary()

    work = tempfile.mkdtemp()
    try:
        key = os.path.join(work, "key.pem")
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1",
                        "-genkey", "-noout", "-out", key], check=True,
                       capture_output=True)
        der = subprocess.run(["openssl", "ec", "-in", key, "-pubout",
                              "-outform", "DER"], check=True,
                             capture_output=True).stdout
        pub = der[-65:].hex()
        data = os.path.join(work, "data.bin")
        with open(data, "wb") as fh:
            fh.write(b"package data\n" * 100)

        t = lambda n: os.path.join(work, n + ".tapp")
        made = [
            mktapp(t("hello"), "--id", "hello", "--name", "Hello",
                   "--name-pl", "Witaj", "--version", "2", "--arch",
                   "x86_64", "--exec", "tapphi", "--art", "0", "--dock",
                   "--key", key, "tapphi=" + ECHO, "hello.dat=" + data),
            mktapp(t("plain"), "--id", "plain", "--name", "Plain",
                   "--version", "7", "--arch", "x86_64", "--exec",
                   "tappplain", "tappplain=" + ECHO),
            mktapp(t("arm"), "--id", "arm", "--name", "Arm", "--version",
                   "1", "--arch", "aarch64", "--exec", "tapparm",
                   "tapparm=" + ECHO),
            mktapp(t("twin"), "--id", "hello", "--name", "Twin",
                   "--version", "1", "--arch", "x86_64", "--exec",
                   "tapptwin", "tapptwin=" + ECHO),
        ]
        subprocess.run([sys.executable,
                        os.path.join(ROOT, "scripts", "mkinitrd.py"),
                        t("broken"), "tappbroken=" + ECHO],
                       capture_output=True)
        made.append(os.path.exists(t("broken")))
        check("five packages made by mktapp.py (and one without a "
              "manifest by mkinitrd.py)", all(made))
        bad = bytearray(open(t("hello"), "rb").read())
        bad[len(bad) // 2] ^= 0x01        # inside tapphi's code
        with open(t("bad"), "wb") as fh:
            fh.write(bad)
        host = {n: host_verify(t(n), pub) for n in ("hello", "plain", "bad")}

        SERVED["/update.key"] = pub.encode() + b"\n"
        SERVED["/hello.tapp"] = open(t("hello"), "rb").read()
        SERVED["/bad.tapp"] = bytes(bad)
        port, stop = start_http_server()
        iso = os.path.join(work, "t.iso")
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.truncate(64 << 20)
        try:
            check("the disc with the packages in /boot/apps builds",
                  make_iso64(os.path.join(work, "iso"), iso, graphics=True,
                             tapps=[t(n) for n in ("hello", "plain", "arm",
                                                   "broken", "twin")]))
            q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                     serial=os.path.join(work, "serial.log"),
                     qemu="qemu-system-x86_64", ram_mib=512)
            try:
                q.wait_serial(PROMPT, timeout=120)
                q.wait_serial("[net] DHCP", timeout=60)
                log = q.serial()
                took = re.findall(r"Package (\S+) (\d+): ([^,]+),", log)
                check(f"the kernel takes hello and plain ({took})",
                      took == [("hello", "2", "Hello"),
                               ("plain", "7", "Plain")])
                passed = re.findall(r"a package passed over: ([^\n\r]+)",
                                    log)
                check(f"and passes over the other three, saying why "
                      f"({passed})",
                      sorted(passed) == sorted([
                          "its programs are for another processor",
                          "no manifest that says what it is",
                          "a package of that name is there already"]))

                out = run(q, "run tapp", "[tapp] done")
                check("'tapp' lists them as their manifests say",
                      "hello 2 - Hello / Witaj, " in out and
                      "runs tapphi, in the dock" in out and
                      "plain 7 - Plain, " in out and
                      "runs tappplain\n" in out.replace("\r", "") and
                      "arm " not in out)
                out = run(q, "run tapphi out of a package", "[tapphi] done")
                check("a package's program runs by its name",
                      "out of a package" in out and
                      "[tapphi] done, exit code 0" in out)

                out = run(q, "run tapp verify hello", "[tapp] done")
                check("with the system's own key, hello is not signed by it",
                      "hello: NOT signed by the system's key" in out and
                      "exit code 1" in out)
                out = run(q, "run httpget 10.0.2.2 %d /update.key "
                             "/update.key" % port, "[httpget] done",
                          password=True)
                check("the test's key goes into update.key",
                      "[httpget] done, exit code 0" in out)
                out = run(q, "run tapp verify hello", "[tapp] done", 120)
                check(f"then hello is signed by the system's key, as OpenSSL "
                      f"says ({host['hello']!r})",
                      "hello: signed by the system's key (update.key)" in out
                      and "exit code 0" in out
                      and host["hello"] == "signed by that key")
                out = run(q, "run tapp verify plain", "[tapp] done")
                check(f"plain has no signature, and OpenSSL finds the empty "
                      f"one no signature either ({host['plain']!r})",
                      "plain: has no signature" in out and
                      "exit code 1" in out and
                      host["plain"] == "NOT signed by that key")
                run(q, "run httpget 10.0.2.2 %d /bad.tapp bad.tapp" % port,
                    "[httpget] done")
                out = run(q, "run tapp verify bad.tapp", "[tapp] done", 120)
                check(f"a copy with one bit changed is not signed, as OpenSSL "
                      f"says ({host['bad']!r})",
                      "bad.tapp: NOT signed by the system's key" in out and
                      host["bad"] == "NOT signed by that key")
                run(q, "run httpget 10.0.2.2 %d /hello.tapp hello.tapp"
                    % port, "[httpget] done")
                out = run(q, "run tapp verify hello.tapp", "[tapp] done", 120)
                check("the same package as a file on the disk is signed too",
                      "hello.tapp: signed by the system's key" in out)
                out = run(q, "run tapp info hello.tapp", "[tapp] done")
                check("'tapp info' prints the manifest and the entries",
                      "ThreadOS app" in out and "id hello" in out and
                      "exec tapphi" in out and "dock yes" in out and
                      "hello.dat 1300 B" in out and
                      "signature 64 B" in out)

                # --- the dock
                q.type_line("desktop")
                q.wait_serial("The desktop is starting", timeout=30)
                check("the desktop comes up", q.wait_desktop())
                tile = q.serial()
                check("the desktop gives hello a tile of its own",
                      "[desktop] a tile for the package hello: Hello"
                      in tile and "package plain" not in tile)
                name = q.dock_hint(DOCK_N, n_l=DOCK_N + 1)
                check(f"after the dock's own, called Hello ({name!r})",
                      name == "Hello")
                seen = len(q.serial())
                q.click(*dock_tile(DOCK_N, n_l=DOCK_N + 1, **q._wh({})))
                end = time.time() + 30
                while time.time() < end and "'tapphi' has ended" not in \
                        q.serial()[seen:]:
                    time.sleep(0.5)
                check("and a click on it starts hello's program",
                      "process 'tapphi' has ended (code 0)"
                      in q.serial()[seen:])
            finally:
                q.close()
        finally:
            stop()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
