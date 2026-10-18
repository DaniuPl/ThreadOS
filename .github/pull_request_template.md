## What this changes

<!-- One paragraph. Why, rather than what - the diff says what. -->

## How it is proved to work

<!--
Every feature here brings a test in scripts/tests/ that starts a real
kernel in QEMU. Name the group, and say what the assertion actually
looks at - and against what, if there is something outside our own code
to compare with (mtools, OpenSSL, a published test vector, a firmware
that either boots the disk or does not).

If part of it cannot be checked from outside, say which part and why.
An honest boundary is worth more than a green tick that means nothing.
-->

- [ ] `make test` is green (or: the groups that could fail are named below, with why)
- [ ] The new feature has its own group in `scripts/tests/`, registered in `scripts/run_tests.py`
- [ ] Comments, identifiers and everything the system says are in English
- [ ] Anything worth remembering went into `docs/JOURNAL.md`
