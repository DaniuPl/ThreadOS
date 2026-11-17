/* nghttp2 against corrupted HTTP/2 frames
 *
 * Whatever a server sends reaches nghttp2 first when libsoup speaks
 * HTTP/2, and nghttp2's release archive has no fuzzers (they live in
 * its repository). This one is a client that has sent a request and
 * receives the input as the server's frames, and then a server that
 * receives it as a client's - the frames parsed, headers decompressed
 * (HPACK), streams opened and closed, flow control kept.
 */

#include <stdint.h>
#include <string.h>
#include <nghttp2/nghttp2.h>

static nghttp2_ssize discard(nghttp2_session *s, const uint8_t *data,
                             size_t len, int flags, void *user)
{
    (void)s;
    (void)data;
    (void)flags;
    (void)user;
    return (nghttp2_ssize)len;
}

static int on_header(nghttp2_session *s, const nghttp2_frame *f,
                     const uint8_t *name, size_t namelen,
                     const uint8_t *value, size_t valuelen, uint8_t flags,
                     void *user)
{
    (void)s;
    (void)f;
    (void)flags;
    size_t *seen = user;
    *seen += namelen + valuelen + (name && namelen ? name[0] : 0) +
             (value && valuelen ? value[valuelen - 1] : 0);
    return 0;
}

static int on_chunk(nghttp2_session *s, uint8_t flags, int32_t stream,
                    const uint8_t *data, size_t len, void *user)
{
    (void)flags;
    size_t *seen = user;
    *seen += len + (len ? data[0] : 0);
    nghttp2_session_consume(s, stream, len);
    return 0;
}

static void run(int server, const uint8_t *data, size_t size)
{
    nghttp2_session_callbacks *cb;
    nghttp2_session *s;
    size_t seen = 0;
    nghttp2_session_callbacks_new(&cb);
    nghttp2_session_callbacks_set_send_callback2(cb, discard);
    nghttp2_session_callbacks_set_on_header_callback(cb, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, on_chunk);
    if (server)
        nghttp2_session_server_new(&s, cb, &seen);
    else
        nghttp2_session_client_new(&s, cb, &seen);
    nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535 },
    };
    nghttp2_submit_settings(s, NGHTTP2_FLAG_NONE, iv, 2);
    if (!server) {
        const nghttp2_nv nva[] = {
            { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, 0 },
            { (uint8_t *)":path", (uint8_t *)"/", 5, 1, 0 },
            { (uint8_t *)":scheme", (uint8_t *)"https", 7, 5, 0 },
            { (uint8_t *)":authority", (uint8_t *)"example.org", 10, 11, 0 },
        };
        nghttp2_submit_request2(s, NULL, nva, 4, NULL, NULL);
    }
    nghttp2_session_send(s);
    /* in pieces, as a socket would hand them over */
    for (size_t at = 0; at < size; ) {
        size_t part = size - at < 1000 ? size - at : (at % 7) * 97 + 1;
        if (part > size - at)
            part = size - at;
        if (nghttp2_session_mem_recv2(s, data + at, part) < 0)
            break;
        nghttp2_session_send(s);
        at += part;
    }
    nghttp2_session_del(s);
    nghttp2_session_callbacks_del(cb);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run(0, data, size);
    /* a server reads the client's preface first */
    static const uint8_t preface[] = NGHTTP2_CLIENT_MAGIC;
    uint8_t buf[65536 + 24];
    size_t n = size < 65536 ? size : 65536;
    memcpy(buf, preface, 24);
    memcpy(buf + 24, data, n);
    run(1, buf, n + 24);
    return 0;
}
