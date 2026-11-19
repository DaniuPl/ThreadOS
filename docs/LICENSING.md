# Licensing: what goes out, what comes in

A short map for the maintainer and anyone who publishes ThreadOS. It is
not legal advice. Before a commercial release (a device, paid software,
a company), have a lawyer read [CLA.md](../CLA.md), this file and the
licences of `third_party/`.

## What ThreadOS is under

- **ThreadOS's own code:** the MIT licence ([LICENSE](../LICENSE)).
- **Third-party code:** each library under its own licence, recorded in
  [third_party/README.md](../third_party/README.md). They are MIT, BSD,
  Apache-2.0, zlib, ISC, the LGPL and similar. GPL code is not taken
  without the owner's decision, because it would put everything linked
  with it under the GPL.
- **The name and the mark:** not covered by the MIT licence. See
  [TRADEMARKS.md](../TRADEMARKS.md).

## What comes in: contributions

Everyone except the owner accepts the [contributor agreement](../CLA.md)
in their first pull request (a row in
[CLA-SIGNATURES.md](../CLA-SIGNATURES.md), checked by the `cla`
workflow). Contributors keep their copyright. The project gets a licence
broad enough to publish future versions under any terms, including
proprietary ones, and to hand the project to a company. Without it, a
contribution would come in only under the MIT licence (GitHub's terms:
the inbound licence is the outbound one). That would still allow a
closed product, but not a clean chain of rights, a patent licence or a
transfer to a company.

Code an AI assistant writes in a session the owner runs is the owner's
contribution. Code someone else's assistant wrote is that person's, and
they sign.

## What goes out: every binary release

Most libraries in `third_party/` are linked statically into programs of
the initrd (`gmedia` with FFmpeg, `jsc` with JavaScriptCore, `glibtest`
with GLib, `wpedeps` with libgcrypt and its neighbours). So every disc, stick image and update is a binary
distribution of all of them. Their licences ask for three things, and
the build provides them:

1. **The licence texts and notices.** `scripts/mknotices.py` builds
   `LICENSES.txt` from the libraries' own archives: ThreadOS's licence,
   then each component with its version, licence and source, then the
   texts. It is packed into the initrd (so every installed system and
   every update carries it), copied to the root of the ISO, and attached
   to every release. Settings -> About shows how many components it
   lists. A library in the README table without an entry in
   `SOURCES` in `mknotices.py` stops the build.
2. **The source code.** The LGPL lets a binary be passed on if the
   library's source, and the source or object files of the programs
   linked with it, are available from the same place. Every release on
   GitHub has a "Source code" archive of the commit it was built from,
   with the libraries' archives inside. **That only holds while the
   repository is public.** If it ever becomes private, every published
   binary still needs its source available to whoever received it:
   attach a source archive to each release, or send it to anyone who
   asks for three years (the LGPL 2.1's written offer, section 6c).
3. **The right to relink.** A program statically linked with an LGPL
   library must come with what is needed to link it again with a
   changed library. For ThreadOS that is the whole source tree and its
   `Makefile`.

## Rules for later: closed programs and devices

The MIT licence allows closed, paid programs and devices built on
ThreadOS. The LGPL libraries come with conditions that hold however
closed the rest is:

- **A closed program linked with an LGPL library** ships its object
  files, so that it can be linked again with a changed library, or
  loads the library as a shared one. ThreadOS has the beginning of
  that (`libthread.so`, `t_dynlink`), not yet shared libraries in
  general. The terms of a closed
  program must not forbid changing it for one's own use or the reverse
  engineering needed to debug such changes (LGPL 2.1, section 6).
- **Keep FFmpeg and the other libraries at LGPL 2.1** (their
  `--enable-version3` and GPL parts stay off). LGPL 3 and GPL 3 require a
  device that runs them to accept modified versions installed by its
  owner (the "Installation Information"). A locked device, one that only
  boots what its maker signed, cannot carry such code.
- **Firmware** (`third_party/iwlwifi/`) may be passed on only as the
  vendor published it, unchanged.
- **Mozilla's certificate data** (`third_party/cacert/`) is MPL-2.0. It is
  passed on unchanged, with a note on its licence in `LICENSES.txt`.
- **A new library** follows the rules in [CONTRIBUTING.md](../CONTRIBUTING.md):
  a row in the README table, and its licence files named in `SOURCES`
  in `scripts/mknotices.py`.

## The name

"ThreadOS" is the project's unregistered trademark. Before a device or
paid product is sold under it, register it: search the name first
(EUIPO's eSearch and TMview for the EU, the Polish Patent Office for
Poland), in class 9 (software and devices) and class 42 (software
services). Two nearby names make that search more important:
"Thread", the smart-home network standard, whose name Thread Group
registered as a trademark, and "ThreadX", an embedded operating system
whose trademark (class 9, operating system software) the Eclipse
Foundation now holds. A trademark attorney can say whether "ThreadOS"
is far enough from both.
