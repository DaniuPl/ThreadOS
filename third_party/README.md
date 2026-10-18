# third_party

Other people's code, kept exactly as its authors published it, so that
updating it is replacing the directory. Whatever ThreadOS needs changed
lives elsewhere, as replacements built over a copy (`libc/overlay`), or
as code of our own around it (`apps/js`).

| Directory | What                                   | Version    | Licence | From |
|-----------|----------------------------------------|------------|---------|------|
| `musl/`   | musl, a C library                      | 1.2.5      | MIT (`COPYRIGHT`) | https://musl.libc.org/ |
| `quickjs/`| QuickJS, a JavaScript engine (core)    | 2026-06-04 | MIT (`LICENSE`)   | https://bellard.org/quickjs/ |

Trimmed, not modified:

- **musl**: only the x86_64 and generic architecture directories are
  kept (`arch/`, and the per-architecture subdirectories of `src/`);
  the other architectures' files are never built for ThreadOS.
- **QuickJS**: only the engine's core - `quickjs.c`, `libregexp.c`,
  `libunicode.c`, `dtoa.c`, `cutils.c` and their headers. Not the
  `qjs`/`qjsc` programs, `quickjs-libc` (its host for POSIX systems -
  ThreadOS has its own in `apps/js`), the Intl and Temporal extensions,
  or the tests.
