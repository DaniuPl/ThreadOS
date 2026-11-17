/* libwebp under the sanitizers, the way a web engine uses it
 *
 * A page's pictures are whatever a server sends. libwebp's own fuzzers
 * (tests/fuzzer in its archive) are written for Google's FuzzTest,
 * which is not here; this does what they and WebKit's decoder do, for
 * libFuzzer's entry point (tests/fuzz/onefile.c drives it): the size
 * from the header, the picture decoded whole into RGBA and into YUV,
 * decoded again as it would arrive off the network - in pieces of
 * growing size through the incremental decoder - and the container
 * taken apart: its frames and chunks, and every frame of an animation
 * composed onto the canvas.
 */

#include <stdint.h>
#include <stdlib.h>
#include "webp/decode.h"
#include "webp/demux.h"

/* A size in the header is the file's word: past this the only thing
   decoding would test is malloc */
#define MAX_PIXELS (4096u * 4096u)

static void whole(const uint8_t *data, size_t size)
{
    int w, h;
    if (!WebPGetInfo(data, size, &w, &h) || (uint64_t)w * h > MAX_PIXELS)
        return;
    WebPFree(WebPDecodeRGBA(data, size, &w, &h));
    uint8_t *u, *v;
    int stride, uv_stride;
    WebPFree(WebPDecodeYUV(data, size, &w, &h, &u, &v, &stride, &uv_stride));

    WebPIDecoder *idec = WebPINewRGB(MODE_RGBA, NULL, 0, 0);
    if (!idec)
        return;
    for (size_t at = 0, step = 1; at < size; step = step * 2 + 1) {
        size_t n = size - at < step ? size - at : step;
        VP8StatusCode st = WebPIAppend(idec, data + at, n);
        if (st != VP8_STATUS_OK && st != VP8_STATUS_SUSPENDED)
            break;
        at += n;
    }
    WebPIDelete(idec);
}

static void container(const uint8_t *data, size_t size)
{
    WebPData wd = { data, size };
    WebPDemuxer *dmx = WebPDemux(&wd);
    if (!dmx)
        return;
    WebPIterator it;
    if (WebPDemuxGetFrame(dmx, 1, &it)) {
        while (WebPDemuxNextFrame(&it))
            ;
        WebPDemuxReleaseIterator(&it);
    }
    WebPChunkIterator ci;
    if (WebPDemuxGetChunk(dmx, "ICCP", 1, &ci))
        WebPDemuxReleaseChunkIterator(&ci);
    uint64_t pixels = (uint64_t)WebPDemuxGetI(dmx, WEBP_FF_CANVAS_WIDTH) *
                      WebPDemuxGetI(dmx, WEBP_FF_CANVAS_HEIGHT);
    WebPDemuxDelete(dmx);
    if (pixels > MAX_PIXELS)
        return;

    WebPAnimDecoderOptions opt;
    if (!WebPAnimDecoderOptionsInit(&opt))
        return;
    opt.color_mode = MODE_RGBA;
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
    if (!dec)
        return;
    uint8_t *buf;
    int ts;
    for (int k = 0; k < 64 && WebPAnimDecoderHasMoreFrames(dec) &&
                    WebPAnimDecoderGetNext(dec, &buf, &ts); k++)
        ;
    WebPAnimDecoderDelete(dec);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    whole(data, size);
    container(data, size);
    return 0;
}
