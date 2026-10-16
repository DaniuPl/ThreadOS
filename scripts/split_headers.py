#!/usr/bin/env python3
"""Splits a header into declarations and bodies - for the shared library.

In ThreadOS the cryptography, TLS, HTTP and DNS live in headers, as
'static inline' functions. That was fine as long as every program
carried them inside itself: a single source of truth, no intermediate
files, no dependencies in the Makefile.

A shared library, though, needs two different views of the same file:

  --lib   for the library: the same functions, but with external
          linkage, so that they can be FOUND in it by name;
  --decl  for the program: declarations only, with no bodies - the
          program is to contain calls, not copies of the code.

The source stays single and untouched. Instead of breaking fourteen
headers into .c/.h pairs by hand (and then seeing to it that they do not
drift apart), this script does it on every build.

Usage: split_headers.py --lib|--decl file.h > result.h
"""

import re
import sys


def body_end(text, i):
    """The index just past the closing brace of a body starting at
    text[i]=='{'.

    It counts braces, but skips the ones inside strings, characters and
    comments - otherwise a single '}' character in a translation table
    would close the function halfway."""
    assert text[i] == '{'
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            finishing = c
            i += 1
            while i < n and text[i] != finishing:
                i += 2 if text[i] == '\\' else 1
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i = text.find('*/', i + 2)
            if i < 0:
                return n
            i += 2
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                return n
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def process_out(text, mode):
    summary = []
    i = 0
    n = len(text)
    # 'static inline' and 'static' at the start of a line - only such
    # ones are top-level definitions; indentation means it is inside
    # something else
    pattern = re.compile(r'^static(\s+inline)?\s', re.M)

    while i < n:
        m = pattern.search(text, i)
        if not m:
            summary.append(text[i:])
            break
        summary.append(text[i:m.start()])

        # How far this definition reaches: to the brace opening a body, or
        # to the semicolon ending a data declaration.
        #
        # The brace by itself does not settle it: an array with an
        # initializer has one too. What settles it is WHAT stands before
        # it - after the argument parenthesis a function body begins,
        # after an equals sign a data value.
        brace = text.find('{', m.end())
        semicolon = text.find(';', m.end())
        fn = False
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            before = text[m.end():brace].rstrip()
            fn = before.endswith(')')

        if fn:
            end = body_end(text, brace)
            header = text[m.start():brace].rstrip()
            body = text[brace:end]
            if mode == 'lib':
                # The same, only visible from the outside
                summary.append(header[len(m.group(0)) - 1:].lstrip() +
                             '\n' + body)
            else:
                summary.append(header[len(m.group(0)) - 1:].lstrip() + ';')
            i = end
        else:
            # Data: in the library they stay (its functions use them), in
            # the program they disappear along with the bodies that
            # reached for them
            end = semicolon
            if end < 0:
                summary.append(text[m.start():])
                break
            # An array with an initializer: semicolons may be inside the braces
            if brace >= 0 and brace < semicolon:
                end = text.find(';', body_end(text, brace) - 1)
                if end < 0:
                    end = semicolon
            end += 1
            if mode == 'lib':
                summary.append(text[m.start():end])
            i = end

    return ''.join(summary)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--lib', '--decl'):
        sys.stderr.write(__doc__)
        return 2
    mode = 'lib' if sys.argv[1] == '--lib' else 'decl'
    with open(sys.argv[2]) as fh:
        text = fh.read()
    sys.stdout.write(process_out(text, mode))
    return 0


if __name__ == '__main__':
    sys.exit(main())
