# Brotli's built-in data (RFC 7932)

Brotli decompression needs three pieces of fixed data, which the format
defines and every decoder carries:

- `dictionary.bin` - the static dictionary, 122,784 bytes of common words
  (RFC 7932, Appendix A). SHA-256
  `20e42eb1b511c21806d4d227d07e5dd06877d8ce7b3a817f378f313653f35c70`.
- `transforms.txt` - the 121 word transforms: a prefix, an operation and
  a suffix each (RFC 7932, Appendix B).
- `context.bin` - the context lookup tables for literals (RFC 7932,
  section 7.1), 2048 bytes: for each of the four context modes, 512.

They are data, not code: ThreadOS's own decoder (`user/ubrotli.h`) is
written from the RFC. The files were dumped from libbrotlicommon 1.1.0
(Debian's libbrotli1), unchanged; `scripts/mkbrotlidata.py` turns them
into a C header at build time.

Copyright 2009, 2010, 2013-2015 by the Brotli Authors. MIT licence:

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
