/* Opus packets as a page sends them, through libopus: the packets of
 * an Opus stream come from the page's server, and GStreamer's
 * opusparse reads their framing with libopus (opus_packet_parse)
 * before FFmpeg decodes them.
 *
 * The input is a run of packets, each after its length in two bytes
 * (big-endian); its first byte says the channels (1 or 2). Each packet
 * is parsed as opusparse parses it, its frames and samples counted, and
 * decoded by libopus's own decoder - a crash or a sanitizer's report is
 * the finding; a packet libopus turns down is libopus doing its job.
 */

#include <stdint.h>
#include <string.h>
#include <opus.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    int channels = (data[0] & 1) + 1;
    data++, size--;
    int error;
    OpusDecoder *dec = opus_decoder_create(48000, channels, &error);
    if (!dec)
        return 0;
    static opus_int16 pcm[5760 * 2];
    while (size >= 2) {
        size_t n = (size_t)data[0] << 8 | data[1];
        data += 2, size -= 2;
        if (n > size)
            n = size;
        const unsigned char *frames[48];
        opus_int16 sizes[48];
        unsigned char toc;
        int offset;
        opus_packet_parse(data, (opus_int32)n, &toc, frames, sizes, &offset);
        opus_packet_get_nb_frames(data, (opus_int32)n);
        opus_packet_get_nb_samples(data, (opus_int32)n, 48000);
        opus_decode(dec, n ? data : NULL, (opus_int32)n, pcm, 5760, 0);
        data += n, size -= n;
    }
    opus_decoder_destroy(dec);
    return 0;
}
