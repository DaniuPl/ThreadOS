#!/usr/bin/env python3
"""ThreadOS smoke test: only the first x86-64 processor's instructions.

A release built on GitHub's Ubuntu 26.04 runner stopped a laptop's
installer with "Invalid Opcode": that system's GCC, and its libgcc and
libstdc++, aim at x86-64-v3 by default, so the kernel came out full of
BMI2 and MOVBE and every program full of AVX, whose register state the
kernel never enables. QEMU's default processor has none of them either
- the release's kernel did not even start there - but no test ran on
what the release had built.

So, from outside, on the images given:

  scripts/checkisa.py is not blind: a function compiled for x86-64-v3
  shows AVX and BMI2 to it, the same compiled for the first x86-64
  shows nothing, and a rep bsf (tzcnt) is let through

  the Makefile's ARCH64 wins over a compiler aimed at x86-64-v3:
  the function compiled with -march=x86-64-v3 first and ARCH64 after
  it, as a distribution's default and our flags meet, has none

  the kernel and every program in the initrd (and in weft.tapp, when
  the build has one) hold only the first x86-64's instructions, outside the code that asks the processor
  first (OpenSSL, Mbed TLS's AES-NI, simdutf, RDRAND) - and those
  exceptions are all still in the initrd, so the list does not grow
  stale names

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import checkisa              # noqa: E402
import ptest                 # noqa: E402
from ptest import check, summary  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Vectorizable work and a variable shift: what x86-64-v3 turns into AVX2
# and SHLX/SARX
PROBE = r"""
void scale(float *restrict out, const float *restrict in, float k, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = in[i] * k + 1.0f;
}
unsigned long shift(unsigned long x, unsigned n) { return x << (n & 63); }
"""


def arch64():
    """ARCH64 from the Makefile, as the build uses it"""
    with open(os.path.join(ROOT, "Makefile")) as f:
        for line in f:
            m = re.match(r"^ARCH64\s*:=\s*(.*)$", line)
            if m:
                return m.group(1).split()
    return []


def compile_probe(work, name, flags):
    src = os.path.join(work, "probe.c")
    with open(src, "w") as f:
        f.write(PROBE)
    obj = os.path.join(work, name + ".o")
    subprocess.run(["gcc", "-O3", "-c", *flags, src, "-o", obj], check=True)
    return checkisa.beyond_baseline(obj)


def the_checker(work):
    v3 = compile_probe(work, "v3", ["-march=x86-64-v3"])
    check("a function built for x86-64-v3 shows AVX and BMI2 to the "
          "checker (%s)" % ", ".join(sorted(v3)),
          any(op.startswith("v") for op in v3) and
          any(op in ("shlx", "sarx", "shrx") for op in v3))
    v1 = compile_probe(work, "v1", ["-march=x86-64", "-mtune=generic"])
    check("the same built for the first x86-64 shows nothing (%s)"
          % (", ".join(sorted(v1)) or "nothing"), not v1)
    asm = os.path.join(work, "bsf.s")
    with open(asm, "w") as f:
        f.write("ctz: rep bsf %rdi, %rax\n ret\n")
    subprocess.run(["gcc", "-c", asm, "-o", asm + ".o"], check=True)
    got = checkisa.beyond_baseline(asm + ".o")
    check("rep bsf reads as tzcnt and is let through everywhere",
          set(got) == {"tzcnt"} and "tzcnt" in checkisa.EVERYWHERE)

    flags = arch64()
    check("the Makefile names the processor (ARCH64 = %s)" % " ".join(flags),
          "-march=x86-64" in flags)
    over = compile_probe(work, "over", ["-march=x86-64-v3", *flags])
    check("ARCH64 after a compiler's own -march=x86-64-v3 leaves none of "
          "it (%s)" % (", ".join(sorted(over)) or "nothing"), not over)


def the_images():
    # The applications' image, when this build made one (make webkit)
    apps = os.path.join(os.path.dirname(os.path.abspath(ptest.INITRD)),
                        "weft.tapp")
    apps = apps if os.path.exists(apps) else None
    wrong, excused = checkisa.check(ptest.KERNEL, ptest.INITRD, apps)
    for name, found in wrong:
        print("      %s: %s" % (name, dict(found.most_common(6))))
    programs = [n for n, _ in checkisa.initrd_programs(ptest.INITRD)]
    if apps:
        programs += [n for n, _ in checkisa.initrd_programs(apps)]
    check("the kernel and all %d programs of the initrd%s hold only the "
          "first x86-64's instructions outside code that asks first "
          "(%d files beyond it)"
          % (len(programs), " and weft.tapp" if apps else "",
             len(wrong)), not wrong)
    names = {"kernel"} | set(programs)
    asks = dict(checkisa.ASKS_FIRST,
                **(checkisa.APPS_ASKS_FIRST if apps else {}))
    stale = sorted(set(asks) - names)
    check("every exception names a file that is there (%s)"
          % (", ".join(stale) or "all present"), not stale)
    used = {n for n, _ in excused}
    check("and each of them is needed - it does hold such code (%s)"
          % ", ".join(sorted(used)),
          used == set(asks) - set(stale))


def main():
    print("--- only the first x86-64's instructions ---")
    work = tempfile.mkdtemp()
    try:
        the_checker(work)
        the_images()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
