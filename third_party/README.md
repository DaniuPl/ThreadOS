# third_party

Other people's code, kept exactly as its authors published it, so that
updating it is replacing the directory. Whatever ThreadOS needs changed
lives elsewhere, as replacements built over a copy (`libc/overlay`), or
as code of our own around it (`apps/js`).

| Directory | What                                   | Version    | Licence | From |
|-----------|----------------------------------------|------------|---------|------|
| `musl/`   | musl, a C library                      | 1.2.5      | MIT (`COPYRIGHT`) | https://musl.libc.org/ |
| `quickjs/`| QuickJS, a JavaScript engine (core)    | 2026-06-04 | MIT (`LICENSE`)   | https://bellard.org/quickjs/ |
| `litehtml/` | litehtml, an HTML/CSS layout engine, with gumbo (an HTML5 parser) inside | git 8848b99 (2026-10-12); gumbo 0.14.1 | BSD-3-Clause (`LICENSE`); gumbo Apache-2.0 (`src/gumbo/LICENSE`) | https://github.com/litehtml/litehtml |
| `ffmpeg-9.0.tar.xz` | FFmpeg demuxers and audio/video decoders | 9.0 | LGPL-2.1-or-later (`COPYING.LGPLv2.1`) | https://ffmpeg.org/releases/ffmpeg-9.0.tar.xz |

Trimmed, not modified:

- **musl**: only the x86_64 and generic architecture directories are
  kept (`arch/`, and the per-architecture subdirectories of `src/`);
  the other architectures' files are never built for ThreadOS.
- **QuickJS**: only the engine's core - `quickjs.c`, `libregexp.c`,
  `libunicode.c`, `dtoa.c`, `cutils.c` and their headers. Not the
  `qjs`/`qjsc` programs, `quickjs-libc` (its host for POSIX systems -
  ThreadOS has its own in `apps/js`), the Intl and Temporal extensions,
  or the tests.
- **litehtml**: `src/`, `include/` and the licences. Not the sample
  containers (cairo, Windows, Haiku), the support code, the build files
  or the documentation; and from gumbo only its sources (not the
  `.gperf` and `tag.in` they were generated from). Two files are built
  from a patched copy, `apps/browser/patches/`, which says what each
  change is and why - the directory here stays exactly upstream's.
- **FFmpeg**: the upstream source archive is kept intact. The 64-bit
  build enables only the MOV/MP4, MP3, WAV, AAC, FLAC, Ogg, Matroska and
  AVI demuxers and the codecs used for those formats. GPL and version 3
  components are disabled; no FFmpeg programs or network protocols are
  built. The player and kernel audio stream interface are our own code.

