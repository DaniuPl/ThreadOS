# Contributing to ThreadOS

Thank you for looking. (AI coding assistants: [AGENTS.md](AGENTS.md) has
the same rules in a shorter form, with the commands.) This file is short on ceremony and long on the
two or three rules that actually shape this codebase, because those are
the ones a patch gets sent back for.

## The one rule that matters: every feature brings a test

There is no code in this system that is only checked by reading it. A
feature without a test in `scripts/tests/` is not finished, and the
review will say so.

A test here is not a unit test with mocks. It **starts a real kernel in
QEMU** and looks at what happened: the screen through QMP screenshots
(its text read back glyph by glyph, in the lettering the system draws in), the serial port,
injected keystrokes and mouse clicks, the bytes on a disk image.
Nothing is stubbed, so a failing assertion means the system
misbehaved - not that a stub drifted from the thing it stood for.

**Assertions must be non-vacuous, and verified from outside where
anything outside exists.** That is the difference between a test and a
decoration:

- the FAT driver is checked against `mtools` - somebody else's
  implementation writes a file and ours has to read it, and then the
  other way round;
- the GPT writer is checked by a partition-table reader in Python that
  validates both CRCs and the backup copy;
- AES, GCM and CCM are checked against OpenSSL **in both directions**:
  our ciphertext must match theirs, and a file they encrypted must
  decrypt here - and a single flipped bit of the tag must be refused;
- the sound driver is checked by measuring the WAV file QEMU wrote:
  441 Hz for 200 ms has to come out as 441 Hz for 200 ms;
- an installed disk is booted again, on its own, by the firmware.

If a thing cannot be checked from outside, say so in the test's own
words. `bt test` ends by printing that it proves the reading of packets
and not the conversation with an adapter, because QEMU has no Bluetooth
device to emulate. An honest boundary is worth more than a green tick
that means nothing.

### Adding a test group

1. write `scripts/tests/t_<name>.py`, modelled on the closest existing
   one - the file starts with a docstring saying **what is being
   checked and why that is the interesting part**;
2. add `"<name>"` to `GROUPS` in `scripts/run_tests.py`;
3. run `make test T=<name>` until it passes, then `make test` to be
   sure the group plays well with the others running in parallel.

The network groups (`net`, `net64`, `socket`, `tls64`) stand a DNS
server of their own on **port 53** of the host: that is where QEMU's
user network sends the guest's questions, and there is no way to remap
it. Binding it needs root - so either run the suite as root, or once:

```sh
sudo sysctl -w net.ipv4.ip_unprivileged_port_start=53
```

Without it those groups say what they are giving up and why, rather
than failing; the assertions are simply not run, which is worth knowing
before concluding that everything is fine.

Tests run four machines at once on a loaded host. **Never write
`time.sleep(3)` and then read the screen once** - look again until the
thing you are waiting for has happened, and give up only then. Half the
flaky failures this project ever had were exactly that.

## Language: English everywhere the code can be read

This is deliberate, and it is not negotiable in a patch:

- **identifiers, file names, code comments and commit messages:
  English** - the code is read by people who do not speak Polish;
- **what the running system says: English too.** `printk`, shell
  messages, window labels, the installer, the names of the commands
  themselves. The system was Polish until it went out into the open,
  and then a system nobody outside can read stopped being worth the
  saving;
- **test assertion descriptions: English**, because they are read next
  to the system's own output;
- **documentation: English**, the development journal
  ([docs/JOURNAL.md](docs/JOURNAL.md)) included.

The one place other languages appear is the keyboard: the Polish layout
(`kernel/kbdmap.c`) and the test that types its letters.

## Comments explain WHY

The code says what it does. A comment that repeats it is noise. What is
worth writing down is the reason: which bit of a register lies where and
why reading it the other way round looks like a working driver, why a
value is checked twice, what breaks if the order changes. Several
comments in this codebase are a paragraph long because the fault they
describe cost a day to find; that is the right length for such a
comment.

## One kernel for the PC, and an ARM64 one beside it

ThreadOS is a 64-bit system. Everything that does not depend on the
processor mode lives in `kernel/`; paging, interrupts, context switching
and the syscall entry live in `kernel64/`. When adding a driver, put it
in `kernel/` and add it to `SHARED64` in the `Makefile`.

There used to be a 32-bit kernel built from the same `kernel/` files. It
was removed in November 2026 (see the journal). Do not bring back 32-bit
code paths or `#ifdef __x86_64__` branches for a 32-bit build.

A few files of `kernel/` (`string.c`, `heap.c`, and the `io.h` and
`spinlock.h` headers) are built for the ARM64 kernel as well, and a
change to them has to build and pass there too.

## Libraries: use them, bring them in properly

ThreadOS does not have to write its own version of everything. A
well-tested library is usually better than a quick rewrite, and new
libraries are welcome. Bringing one in means:

- **a pinned upstream release in `third_party/<name>/`** (or its release
  archive), built from source by the `Makefile`. Nothing is downloaded
  at build time, and no prebuilt binaries are committed (device
  firmware, which exists only as its vendor's binary, is the exception);
- **the code stays exactly as upstream published it.** ThreadOS changes
  go into `libc/overlay/`, a patch directory next to the code that uses
  the library (like `apps/browser/patches/`), or wrapper code of our own,
  so updating a library still means replacing its directory;
- **a row in `third_party/README.md`** with the version, licence, source
  and anything that was trimmed, and **its licence files named in
  `SOURCES` in `scripts/mknotices.py`**. That script puts every
  library's licence into `LICENSES.txt`, which every disc and update
  carries, and it stops the build when a library in the table has no
  entry there;
- **a licence that allows redistribution with an MIT system:** MIT, BSD,
  Apache-2.0, zlib, ISC, LGPL and similar. GPL code would change the
  licence of everything linked with it, so ask before adding it;
- **a test, like any other feature:** a QEMU group that uses the library
  on ThreadOS. A library that parses untrusted input also gets a harness
  in `make fuzz`.

Programs link libraries against `libc/` musl. Code that goes into the
kernel must be freestanding (no libc) and build with the kernel's flags.

## Your contribution's licence: the contributor agreement

Before your first pull request is merged, you accept the
[contributor agreement](CLA.md): add a row with your GitHub login to
[CLA-SIGNATURES.md](CLA-SIGNATURES.md) in that pull request. The `cla`
check on every pull request looks for it.

The agreement is short; read it. You keep the copyright in what you
wrote. The project gets a permanent licence to publish your work under
whatever licence ThreadOS is under, now or later, and you promise the
work is yours to give. The reason is that ThreadOS may one day ship on
devices of its own, and that needs a clean record of who gave which
rights. Every version published under the MIT licence stays MIT, your
work in it too. [docs/LICENSING.md](docs/LICENSING.md) explains what
the project hands out and what that asks of it. The name and the mark
are covered separately, in [TRADEMARKS.md](TRADEMARKS.md).

## Before sending a patch

```sh
make            # the kernel and its initrd
make test       # the whole suite - it has to be green
```

The suite takes about twenty minutes and starts a hundred virtual
machines. If a group fails, run it alone (`make test T=<group>`) before
concluding anything: a failure under full load that passes alone is
usually a test that assumes how long something takes.

## Commit messages

One line saying what changed, then a paragraph or two saying **why** -
and, when something went wrong on the way, what it was. The journal
(`docs/JOURNAL.md`) gets the longer version of that story for anything
worth remembering.

## Talking to people

Questions that are not bug reports, ideas, and "it boots on my laptop" (or
"it does not") are welcome on [Discord](https://discord.gg/AyaEyTSG6). Decisions that change the code
still end up written down in an issue or a pull request, where they can be
found later.

## Reporting a bug

Please say which kernel (x86-64 or ARM64), how it was started (the QEMU
command line or the real machine), and attach the serial log - the
system writes almost everything worth knowing there. `hardware` and
`disks` from inside the system are worth more than a description of the
symptom.
