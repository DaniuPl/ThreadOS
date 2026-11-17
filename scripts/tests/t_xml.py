#!/usr/bin/env python3
"""ThreadOS smoke test: libxml2.

The XML parser a web engine reads XHTML, SVG and XML documents with,
built from its release for ThreadOS (Makefile, 'Libraries for WebKit's
processes'). libc/tests/xmltest.c parses the documents in
scripts/tests/xml/ and says each tree in a canonical form; this test
works the same form out with another parser, Python's ElementTree
(expat), and holds the two to each other:

  elements, attributes sorted, text and tails, comments and processing
  instructions passed over, CDATA, character references, attribute
  values normalized; namespaces - default, prefixed, a prefix declared
  again, a default undone; a DTD's internal entities, one holding
  markup, and its default attributes; ISO-8859-2 and UTF-16 documents;
  an SVG

  a document that is not well-formed, and one whose entities would
  grow a few hundred bytes into gigabytes ('billion laughs'): an error,
  said as such - where expat refuses too

  XPath: descendants, a predicate on an attribute, a position, a
  count, a namespace prefix, local-name() - against ElementTree's own
  paths for the same nodes

and of xmltest's own: every document, written out by libxml2 and read
again, has the same tree.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary  # noqa: E402

XML_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xml")
PROMPTS = ("thread64>", "thread64#")
DOCS = ["basic.xml", "ns.xml", "dtd.xml", "latin2.xml", "utf16.xml",
        "svg.xml", "broken.xml", "laughs.xml"]

# xmltest's XPath (queries.q, in order) and ElementTree's path for the
# same nodes, from the root; a count is the number of nodes found
ATOM = "http://www.w3.org/2005/Atom"
SVG = "http://www.w3.org/2000/svg"
QUERIES = [
    ("basic.xml", ".//item", False),
    ("basic.xml", ".//item[@id='2']", False),
    ("basic.xml", "./section/item[2]", False),
    ("basic.xml", ".//item", True),
    ("ns.xml", ".//{%s}title" % ATOM, False),
    ("ns.xml", ".//{*}entry", False),
    ("svg.xml", ".//{%s}rect[@width]" % SVG, False),
]


def esc(s):
    """xmltest's put_text: the UTF-8 bytes, quoted and escaped"""
    out = '"'
    for b in (s or "").encode("utf-8"):
        c = chr(b)
        if c == "\\":
            out += "\\\\"
        elif c == '"':
            out += '\\"'
        elif c == "\n":
            out += "\\n"
        elif c == "\t":
            out += "\\t"
        elif c == "\r":
            out += "\\r"
        elif b < 0x20 or b >= 0x7F:
            out += "\\x%02x" % b
        else:
            out += c
    return out + '"'


def split(tag):
    if tag.startswith("{"):
        ns, local = tag[1:].split("}", 1)
        return ns, local
    return "", tag


def dump(e, depth, out):
    ns, local = split(e.tag)
    attrs = []
    for k, v in e.attrib.items():
        ans, aname = split(k)
        attrs.append(("{%s}%s=" % (ans, aname), v))
    attrs.sort(key=lambda a: (a[0] + a[1]).encode("utf-8"))
    line = "  " * depth + "{%s}%s" % (ns, local)
    for name, v in attrs:
        line += " " + name + esc(v)
    line += " text=" + esc(e.text) + " tail=" + esc(e.tail if depth else "")
    out.append(line)
    for c in e:
        dump(c, depth + 1, out)


def expected():
    """The lines xmltest should print, from ElementTree"""
    lines = []
    for name in DOCS:
        try:
            root = ET.parse(os.path.join(XML_DIR, name)).getroot()
        except ET.ParseError:
            lines.append(f"{name}: an error, said as such")
            continue
        lines.append(f"{name}:")
        dump(root, 0, lines)
    roots = {}
    for i, (doc, path, count) in enumerate(QUERIES, 1):
        root = roots.setdefault(doc, ET.parse(os.path.join(XML_DIR, doc))
                                .getroot())
        found = root.findall(path)
        if count:
            lines.append(f"query {i}: number {len(found)}")
            continue
        line = f"query {i}: {len(found)}"
        for e in found:
            line += " %s=%s" % (split(e.tag)[1], esc(e.text))
        lines.append(line)
    good = sum(1 for l in lines if l.endswith(".xml:"))
    lines.append(f"{good} documents written out and read again have the "
                 f"same tree: yes")
    return lines


def run(q, command, timeout=120):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    end = time.time() + timeout
    while time.time() < end and f"[{name}] done, exit code" not in \
            q.serial()[seen:]:
        time.sleep(0.5)
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    start = out.find(f"Program '{name}' from initrd")
    out = out[start:] if start >= 0 else out
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    # (the dump's indentation is part of it: only the line ends go)
    body = [l.rstrip() for l in out.replace("\r", "").split("\n")[1:]
            if l.strip() and not l.strip().startswith(("[",) + PROMPTS)]
    return body, int(m.group(1)) if m else None


def main():
    print("--- libxml2 ---")
    want = expected()
    check("(expat refuses broken.xml and laughs.xml too)",
          "broken.xml: an error, said as such" in want and
          "laughs.xml: an error, said as such" in want)
    files = DOCS + ["queries.q"]
    for name in files:
        with open(os.path.join(XML_DIR, name), "rb") as fh:
            ptest.SERVED["/" + name] = fh.read()
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"))
        try:
            check("the desktop came up", q.wait_desktop())
            q.click_desktop()
            fetched = 0
            for name in files:
                _, c = run(q, f"run httpget 10.0.2.2 {port} /{name} {name}")
                fetched += c == 0
            check(f"the documents reach the disk ({fetched} of {len(files)})",
                  fetched == len(files))
            t0 = time.time()
            got, code = run(q, "run xmltest " + " ".join(files))
            took = time.time() - t0
            plain = [l for l in got if not l.startswith("~")]
            first = next((i for i, (w, g) in enumerate(zip(want, plain))
                          if w != g), None)
            where = (f"first difference at line {first + 1}: ElementTree "
                     f"{want[first]!r}, xmltest {plain[first]!r}"
                     if first is not None else
                     f"ElementTree {len(want)} lines, xmltest {len(plain)}")
            check(f"xmltest's trees and queries are ElementTree's ({where})",
                  plain == want)
            check(f"(and the billion laughs were refused at once: the whole "
                  f"run took {took:.0f} s)", took < 60)
            check(f"xmltest ends with code 0 (got {code})", code == 0)
            for l in got:
                if l.startswith("~"):
                    print("   " + l)
        finally:
            q.close()
    finally:
        stop_http()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
