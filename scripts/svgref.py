"""SVG pictures drawn by librsvg - the reference the tests compare
ThreadOS's own drawing (user/usvg.h) with.

librsvg is GNOME's renderer, written apart from ours; reached through
ctypes, so nothing needs installing beyond the library itself, which
most Linux systems carry. render() gives None where it is missing.
"""

import ctypes
import ctypes.util


class _Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]


def _libs():
    try:
        rsvg = ctypes.CDLL(ctypes.util.find_library("rsvg-2") or "librsvg-2.so.2")
        cairo = ctypes.CDLL(ctypes.util.find_library("cairo") or "libcairo.so.2")
        gobj = ctypes.CDLL(ctypes.util.find_library("gobject-2.0") or
                           "libgobject-2.0.so.0")
    except OSError:
        return None
    rsvg.rsvg_handle_new_from_data.restype = ctypes.c_void_p
    rsvg.rsvg_handle_new_from_data.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                               ctypes.c_void_p]
    rsvg.rsvg_handle_render_document.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                                 ctypes.POINTER(_Rect), ctypes.c_void_p]
    cairo.cairo_image_surface_create.restype = ctypes.c_void_p
    cairo.cairo_create.restype = ctypes.c_void_p
    cairo.cairo_create.argtypes = [ctypes.c_void_p]
    cairo.cairo_image_surface_get_data.restype = ctypes.POINTER(ctypes.c_ubyte)
    cairo.cairo_image_surface_get_data.argtypes = [ctypes.c_void_p]
    cairo.cairo_image_surface_get_stride.argtypes = [ctypes.c_void_p]
    cairo.cairo_surface_flush.argtypes = [ctypes.c_void_p]
    cairo.cairo_destroy.argtypes = [ctypes.c_void_p]
    cairo.cairo_surface_destroy.argtypes = [ctypes.c_void_p]
    gobj.g_object_unref.argtypes = [ctypes.c_void_p]
    return rsvg, cairo, gobj


_LIBS = _libs()


def available():
    return _LIBS is not None


def render(svg, w, h):
    """The picture drawn into w x h pixels: bytes of RGBA, not
    premultiplied - or None without librsvg"""
    if not _LIBS:
        return None
    rsvg, cairo, gobj = _LIBS
    handle = rsvg.rsvg_handle_new_from_data(svg, len(svg), None)
    if not handle:
        return None
    surface = cairo.cairo_image_surface_create(0, w, h)      # ARGB32
    cr = cairo.cairo_create(surface)
    vp = _Rect(0, 0, w, h)
    rsvg.rsvg_handle_render_document(handle, cr, ctypes.byref(vp), None)
    cairo.cairo_surface_flush(surface)
    data = cairo.cairo_image_surface_get_data(surface)
    stride = cairo.cairo_image_surface_get_stride(surface)
    out = bytearray(w * h * 4)
    for y in range(h):
        row = ctypes.string_at(ctypes.addressof(data.contents) + y * stride, w * 4)
        for x in range(w):
            b, g, r, a = row[x * 4:x * 4 + 4]       # premultiplied, little-endian
            o = (y * w + x) * 4
            if a:
                out[o] = min(255, (r * 255 + a // 2) // a)
                out[o + 1] = min(255, (g * 255 + a // 2) // a)
                out[o + 2] = min(255, (b * 255 + a // 2) // a)
            out[o + 3] = a
    cairo.cairo_destroy(cr)
    cairo.cairo_surface_destroy(surface)
    gobj.g_object_unref(handle)
    return bytes(out)


def on_white(rgba):
    """RGBA laid on white: what a page shows"""
    out = bytearray(len(rgba) // 4 * 3)
    for i in range(len(rgba) // 4):
        a = rgba[i * 4 + 3]
        for k in range(3):
            out[i * 3 + k] = (rgba[i * 4 + k] * a + 255 * (255 - a) + 127) // 255
    return bytes(out)


def compare(a_rgb, b_rgb):
    """How far apart two pictures of the same size are: the mean
    difference per channel, and the share of pixels off by more than a
    quarter of the scale - a shape in the wrong place, not an edge
    shaded a little differently"""
    n = len(a_rgb) // 3
    total = 0
    far = 0
    for i in range(n):
        d = max(abs(a_rgb[i * 3 + k] - b_rgb[i * 3 + k]) for k in range(3))
        total += sum(abs(a_rgb[i * 3 + k] - b_rgb[i * 3 + k]) for k in range(3))
        if d > 64:
            far += 1
    return total / (n * 3), far / n
