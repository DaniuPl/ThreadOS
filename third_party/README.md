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
| `zlib-1.4.tar.xz` | zlib, deflate compression | 1.4 | zlib (`LICENSE` in the archive) | https://zlib.net/zlib-1.4.tar.xz (SHA-256 `4f11c512053d7bab8d9e9b89bde3575add3e4c50a383b42ef6cb837035fc5a7c`, as zlib.net lists it; the GitHub release is the same file) |
| `curl-8.23.0.tar.xz` | curl and libcurl, a URL transfer library | 8.23.0 | curl (MIT-style, `COPYING` in the archive) | https://curl.se/download/curl-8.23.0.tar.xz (SHA-256 `8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5`; the GitHub release is the same file) |
| `mbedtls-3.6.7.tar.bz2` | Mbed TLS, TLS 1.2 and 1.3, X.509 and the cryptography under them | 3.6.7 (LTS) | Apache-2.0, taken under that one of its two licences (Apache-2.0 OR GPL-2.0-or-later, `LICENSE` in the archive) | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 (SHA-256 `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`, as the release page lists it) |
| `freetype-2.15.0.tar.xz` | FreeType, a font engine | 2.15.0 | FreeType License (BSD-style, `LICENSE.TXT` and `docs/FTL.TXT` in the archive) | https://download.savannah.gnu.org/releases/freetype/freetype-2.15.0.tar.xz (SHA-256 `93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5`; the SourceForge copy is the same file) |
| `harfbuzz/` | HarfBuzz, a text shaping engine | 15.1.0 | Old MIT (`COPYING`) | https://github.com/harfbuzz/harfbuzz/releases/download/15.1.0/harfbuzz-15.1.0.tar.xz (SHA-256 `0fc7c565443a5f4be7da65ac5ee5bd737d9b158dc908708b7689a0a5796b0c8d`) |
| `sqlite-amalgamation-3540100.zip` | SQLite, an SQL database engine, and its command-line shell | 3.54.1 | Public domain (the header of `sqlite3.c`) | https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip (SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, as sqlite.org lists it) |

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
- **zlib**: the archive is kept intact; the build compiles the library's
  fifteen C files from it, with the `zconf.h` it ships.
- **curl**: the archive is kept intact and configured by its own
  `configure` for musl (`CURL_*` in the `Makefile`): HTTP and file
  transfers with zlib, TLS through Mbed TLS, no IPv6, no Unix sockets,
  the resolver in the calling thread, and no built-in CA bundle (roots
  are given with `--cacert`). Only libcurl and the `curl` tool are
  built.
- **Mbed TLS**: the archive is kept intact; its library is built by its
  own `library/Makefile` with the default `mbedtls_config.h` (`MBEDTLS_*`
  in the `Makefile`), and its `programs/test/selftest.c` becomes
  `mbedtest`. It reads `/dev/urandom` for entropy - outside glibc it
  does not use getrandom() - which musl's overlay makes up from the
  kernel's generator.
- **FreeType**: the archive is kept intact. The build compiles the
  modules `apps/fonts/ftmodule.h` names (TrueType, CFF, the hinters, the
  anti-aliasing renderer, WOFF through zlib), not FreeType's own list.
- **HarfBuzz**: only `src/`, `COPYING` and `README.md` of the release are
  kept: the archive is 20 MB, most of it test fonts, documentation and
  tools. The sources are unchanged; the build compiles `src/harfbuzz.cc`.
- **SQLite**: the amalgamation archive is kept intact and unpacked into
  the build directory (`scripts/unzipflat.py`). It is built thread-safe,
  without loadable extensions and without memory-mapped I/O, with the
  math functions and FTS5 (`SQLITE_OPTS` in the `Makefile`). The shell
  is SQLite's own `shell.c`.
- **FFmpeg**: the upstream source archive is kept intact. The 64-bit
  build enables only the MOV/MP4, MP3, WAV, AAC, FLAC, Ogg, Matroska and
  AVI demuxers and the codecs used for those formats. GPL and version 3
  components are disabled; no FFmpeg programs or network protocols are
  built. The player and kernel audio stream interface are our own code.

