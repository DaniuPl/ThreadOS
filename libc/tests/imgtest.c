/* imgtest - libpng and libjpeg-turbo on ThreadOS
 *
 * The image decoders a web page's pictures go through. Every file named
 * on the command line is decoded to 8-bit RGBA, and what comes out is
 * said as its size, a hash of every pixel and a few pixels themselves;
 * scripts/tests/t_images.py works the same out with Pillow, another
 * decoder, from the same files, and holds the two to each other:
 *
 *   PNG: RGB, RGBA, a palette with transparency, grey, grey with
 *   alpha, 16 bits a channel (cut to 8 by dropping the low byte),
 *   interlaced; JPEG: baseline and progressive, 4:2:0 and 4:4:4, grey
 *
 *   broken files - cut short, or with a byte changed - end in an error
 *   said as such, not in a crash
 *
 * and, of its own: an image encoded as PNG and decoded again is the
 * same to the last bit, and as JPEG at quality 90 within a few levels.
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <jpeglib.h>

struct image {
    unsigned w, h;
    unsigned char *rgba;
};

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? n : 1);
    *len = fread(buf, 1, n, f);
    fclose(f);
    return buf;
}

/* --- PNG ------------------------------------------------------------------ */

struct reader {
    const unsigned char *data;
    size_t len, at;
};

static void png_from_memory(png_structp png, png_bytep out, png_size_t n)
{
    struct reader *r = png_get_io_ptr(png);
    if (r->at + n > r->len)
        png_error(png, "the file ends here");
    memcpy(out, r->data + r->at, n);
    r->at += n;
}

static void quiet(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;
}

static int decode_png(const unsigned char *data, size_t len, struct image *img)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                             NULL, quiet);
    png_infop info = png_create_info_struct(png);
    struct reader r = { data, len, 0 };
    unsigned char **rows = NULL;
    img->rgba = NULL;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(rows);
        free(img->rgba);
        img->rgba = NULL;
        return -1;
    }
    png_set_read_fn(png, &r, png_from_memory);
    png_read_info(png, info);
    img->w = png_get_image_width(png, info);
    img->h = png_get_image_height(png, info);
    int type = png_get_color_type(png, info);
    /* everything to 8-bit RGBA: palettes and small greys expanded,
       transparency made alpha, 16 bits cut to 8, grey made RGB */
    png_set_expand(png);
    png_set_strip_16(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    img->rgba = malloc((size_t)img->w * img->h * 4);
    rows = malloc(img->h * sizeof(*rows));
    for (unsigned y = 0; y < img->h; y++)
        rows[y] = img->rgba + (size_t)y * img->w * 4;
    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    free(rows);
    return 0;
}

struct writer {
    unsigned char *data;
    size_t len, cap;
};

static void png_to_memory(png_structp png, png_bytep in, png_size_t n)
{
    struct writer *w = png_get_io_ptr(png);
    if (w->len + n > w->cap) {
        w->cap = (w->len + n) * 2;
        w->data = realloc(w->data, w->cap);
    }
    memcpy(w->data + w->len, in, n);
    w->len += n;
}

static void png_flush_nothing(png_structp png)
{
    (void)png;
}

static size_t encode_png(const struct image *img, unsigned char **out)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
                                              NULL, quiet);
    png_infop info = png_create_info_struct(png);
    struct writer w = { NULL, 0, 0 };
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(w.data);
        return 0;
    }
    png_set_write_fn(png, &w, png_to_memory, png_flush_nothing);
    png_set_IHDR(png, info, img->w, img->h, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (unsigned y = 0; y < img->h; y++)
        png_write_row(png, img->rgba + (size_t)y * img->w * 4);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    *out = w.data;
    return w.len;
}

/* --- JPEG -------------------------------------------------------------- */

struct jerr {
    struct jpeg_error_mgr base;
    jmp_buf back;
};

static void jpeg_fail(j_common_ptr cinfo)
{
    longjmp(((struct jerr *)cinfo->err)->back, 1);
}

static void jpeg_quiet(j_common_ptr cinfo, int level)
{
    (void)cinfo;
    (void)level;
}

static int decode_jpeg(const unsigned char *data, size_t len, struct image *img)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr err;
    cinfo.err = jpeg_std_error(&err.base);
    err.base.error_exit = jpeg_fail;
    err.base.emit_message = jpeg_quiet;
    img->rgba = NULL;
    unsigned char *row = NULL;
    if (setjmp(err.back)) {
        jpeg_destroy_decompress(&cinfo);
        free(img->rgba);
        free(row);
        img->rgba = NULL;
        return -1;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;     /* grey too: as Pillow's RGBA */
    jpeg_start_decompress(&cinfo);
    img->w = cinfo.output_width;
    img->h = cinfo.output_height;
    img->rgba = malloc((size_t)img->w * img->h * 4);
    row = malloc((size_t)img->w * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned y = cinfo.output_scanline;
        JSAMPROW r = row;
        jpeg_read_scanlines(&cinfo, &r, 1);
        for (unsigned x = 0; x < img->w; x++) {
            unsigned char *p = img->rgba + ((size_t)y * img->w + x) * 4;
            p[0] = row[x * 3];
            p[1] = row[x * 3 + 1];
            p[2] = row[x * 3 + 2];
            p[3] = 0xFF;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row);
    return 0;
}

static size_t encode_jpeg(const struct image *img, int quality,
                          unsigned char **out)
{
    struct jpeg_compress_struct cinfo;
    struct jerr err;
    cinfo.err = jpeg_std_error(&err.base);
    err.base.error_exit = jpeg_fail;
    unsigned long len = 0;
    *out = NULL;
    unsigned char *row = malloc((size_t)img->w * 3);
    if (setjmp(err.back)) {
        jpeg_destroy_compress(&cinfo);
        free(row);
        return 0;
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, out, &len);
    cinfo.image_width = img->w;
    cinfo.image_height = img->h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const unsigned char *p = img->rgba +
                                 (size_t)cinfo.next_scanline * img->w * 4;
        for (unsigned x = 0; x < img->w; x++)
            memcpy(row + x * 3, p + x * 4, 3);
        JSAMPROW r = row;
        jpeg_write_scanlines(&cinfo, &r, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    return len;
}

/* --- what is said of an image -------------------------------------------- */

static uint32_t fnv(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void describe(const char *name, const struct image *img)
{
    size_t n = (size_t)img->w * img->h * 4;
    const unsigned char *first = img->rgba;
    const unsigned char *mid = img->rgba + ((size_t)(img->h / 2) * img->w +
                                            img->w / 2) * 4;
    const unsigned char *last = img->rgba + n - 4;
    printf("%s: %ux%u pixels %08x first %02x%02x%02x%02x middle "
           "%02x%02x%02x%02x last %02x%02x%02x%02x\n", name, img->w, img->h,
           fnv(img->rgba, n), first[0], first[1], first[2], first[3],
           mid[0], mid[1], mid[2], mid[3], last[0], last[1], last[2], last[3]);
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

static void round_trips(void)
{
    struct image img = { 173, 101, NULL };
    img.rgba = malloc(img.w * img.h * 4);
    for (unsigned y = 0; y < img.h; y++)
        for (unsigned x = 0; x < img.w; x++) {
            unsigned char *p = img.rgba + (y * img.w + x) * 4;
            p[0] = (unsigned char)(x * 255 / (img.w - 1));
            p[1] = (unsigned char)(y * 255 / (img.h - 1));
            p[2] = (unsigned char)((x ^ y) & 0xFF);
            p[3] = (unsigned char)(255 - (x + y) % 256);
        }
    unsigned char *png = NULL;
    size_t plen = encode_png(&img, &png);
    struct image back;
    int same = plen && decode_png(png, plen, &back) == 0 &&
               back.w == img.w && back.h == img.h &&
               !memcmp(back.rgba, img.rgba, img.w * img.h * 4);
    printf("an image encoded as PNG and decoded again is the same: %s\n",
           same ? "yes" : "no");
    printf("~the PNG of 173x101 RGBA: %zu bytes\n", plen);
    if (plen && back.rgba)
        free(back.rgba);
    free(png);

    for (unsigned i = 0; i < img.w * img.h; i++)
        img.rgba[i * 4 + 3] = 0xFF;
    unsigned char *jpg = NULL;
    size_t jlen = encode_jpeg(&img, 90, &jpg);
    int close = 0;
    double err = 0;
    if (jlen && decode_jpeg(jpg, jlen, &back) == 0) {
        long sum = 0;
        int worst = 0;
        for (unsigned i = 0; i < img.w * img.h * 4; i++) {
            int d = abs((int)back.rgba[i] - (int)img.rgba[i]);
            sum += d;
            if (d > worst)
                worst = d;
        }
        err = (double)sum / (img.w * img.h * 3);
        close = err < 3.0;
        printf("~JPEG at quality 90: %zu bytes, mean error %.2f, worst %d\n",
               jlen, err, worst);
        free(back.rgba);
    }
    printf("and as JPEG at quality 90 within a few levels: %s\n",
           close ? "yes" : "no");
    free(jpg);
    free(img.rgba);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        unsigned char *data = slurp(argv[i], &len);
        if (!data) {
            printf("%s: cannot be read\n", argv[i]);
            continue;
        }
        struct image img;
        int r = ends_with(argv[i], ".png") ? decode_png(data, len, &img)
                                           : decode_jpeg(data, len, &img);
        if (r < 0)
            printf("%s: an error, said as such\n", argv[i]);
        else {
            describe(argv[i], &img);
            free(img.rgba);
        }
        free(data);
    }
    round_trips();
    printf("~libpng %s, libjpeg-turbo %d\n", png_get_libpng_ver(NULL),
           LIBJPEG_TURBO_VERSION_NUMBER);
    return 0;
}
