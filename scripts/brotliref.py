"""Brotli by libbrotli - the reference the tests compare ThreadOS's own
decoder (user/ubrotli.h) with, and what lets fontTools write and read
WOFF2 fonts for them.

libbrotli is Google's, written apart from ours; reached through ctypes,
so nothing needs installing beyond the libraries themselves, which most
Linux systems carry. install() puts this in as the 'brotli' module
fontTools asks for; available() says whether it can.
"""

import ctypes
import ctypes.util
import sys


def _lib(name):
    try:
        return ctypes.CDLL(ctypes.util.find_library(name) or "lib%s.so.1" % name)
    except OSError:
        return None


_enc, _dec = _lib("brotlienc"), _lib("brotlidec")
if _enc:
    _enc.BrotliEncoderCompress.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_size_t, ctypes.c_char_p,
                                           ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p]
if _dec:
    _dec.BrotliDecoderDecompress.argtypes = [ctypes.c_size_t, ctypes.c_char_p,
                                             ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p]

MODE_GENERIC, MODE_TEXT, MODE_FONT = 0, 1, 2


def available():
    return bool(_enc and _dec)


def compress(data, mode=MODE_GENERIC, quality=11, lgwin=22, lgblock=0):
    data = bytes(data)
    cap = len(data) + (len(data) >> 2) + 1024
    out = ctypes.create_string_buffer(cap)
    n = ctypes.c_size_t(cap)
    if not _enc.BrotliEncoderCompress(quality, lgwin, mode, len(data), data,
                                      ctypes.byref(n), out):
        raise ValueError("brotli: could not compress")
    return out.raw[:n.value]


def decompress(data, size=64 << 20):
    data = bytes(data)
    out = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(size)
    if _dec.BrotliDecoderDecompress(len(data), data, ctypes.byref(n), out) != 1:
        raise ValueError("brotli: a damaged stream")
    return out.raw[:n.value]


def install():
    """As the 'brotli' module, for fontTools' WOFF2. False when the
    libraries are not here."""
    if not available():
        return False
    sys.modules.setdefault("brotli", sys.modules[__name__])
    return True
