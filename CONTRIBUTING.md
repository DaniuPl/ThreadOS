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

The network groups (`net`, `net32`, `tls32`, `tls64`) stand a DNS
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

## Two kernels, one set of sources

Everything that does not genuinely depend on the processor mode lives in
`kernel/` and is **compiled twice**. Only paging, interrupts, context
switching and the syscall entry have separate implementations in
`kernel64/`. When adding a driver, put it in `kernel/` and add it to
both object lists in the `Makefile` - the second compilation is the
cheapest known way to catch a shift, a width or a pointer that happened
to be the right size.

The 32-bit kernel is kept and must keep passing its tests. It is no
longer where new features go, but it is a second opinion on every line
of shared code.

## Before sending a patch

```sh
make            # the 32-bit kernel and its initrd
make kernel64   # the long-mode kernel
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

## Reporting a bug

Please say which kernel (32- or 64-bit), how it was started (the QEMU
command line or the real machine), and attach the serial log - the
system writes almost everything worth knowing there. `hardware` and
`disks` from inside the system are worth more than a description of the
symptom.
