#!/usr/bin/env python3
"""Which of ICU's data items ThreadOS keeps (apps/webkit/icu-data.py).

ICU's data file holds everything for every locale - 32 MB, most of it
conversion tables for old code pages, transliterators and the names of
things in hundreds of languages. JavaScriptCore needs the Unicode
properties, normalization, case mapping, collation and the formats of
numbers, dates and plurals, and ThreadOS speaks English and Polish. So
the data keeps those, in the root locale, English and Polish, and the
tables every locale shares; it drops the code-page converters (UTF-8
and UTF-16 need no table), transliteration, the spoof checker, the
character names and the dictionaries that break Chinese, Japanese,
Thai and the like into words.

Reads 'icupkg -l' output on stdin, prints the items to remove.
"""

import re
import sys

LOCALES = ("root", "en", "en_001", "en_US", "en_US_POSIX", "pl", "pl_PL")
LOCALE = re.compile(r"^[a-z]{2,3}(_[A-Za-z0-9]+)*\.res$")
PER_LOCALE = ("brkitr", "coll", "curr", "lang", "rbnf", "region", "unit",
              "zone")
SHARED = ("res_index.res", "pool.res", "tzdbNames.res", "ucadata.icu")


def keep(item):
    folder, _, name = item.rpartition("/")
    if folder == "translit":
        return False
    if folder == "brkitr" and name.endswith(".dict"):
        return False
    if folder in PER_LOCALE or (not folder and LOCALE.match(name)):
        if name in SHARED or not name.endswith(".res"):
            return True
        return name[:-4] in LOCALES
    if not folder and (name.endswith(".cnv") or name.endswith(".spp") or
                       name in ("unames.icu", "confusables.cfu",
                                "confusablesWholeScript.cfu")):
        return False
    return True


for line in sys.stdin:
    item = line.strip()
    if item and not keep(item):
        print(item)
