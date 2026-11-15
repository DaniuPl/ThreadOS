#!/usr/bin/env python3
"""Unpacks the files of a zip archive into one directory, without the
archive's own directories: unzipflat.py ARCHIVE DIR

For third_party/ archives whose files sit in a versioned directory
(SQLite's amalgamation), so the build can name them without the
version. Python's zipfile, so the build needs no unzip program."""

import os
import sys
import zipfile

archive, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(archive) as z:
    for name in z.namelist():
        if name.endswith("/"):
            continue
        base = os.path.basename(name)
        if not base or base.startswith("."):
            continue
        with open(os.path.join(out, base), "wb") as fh:
            fh.write(z.read(name))
