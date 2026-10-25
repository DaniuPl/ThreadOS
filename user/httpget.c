/* httpget - an HTTP/HTTPS client in ring 3
 *
 * Usage: run httpget [-k] [-p <file>] <address> [port] [path] [file]
 *   run httpget 10.0.2.2 8000 /                - to the console
 *   run httpget https://example.com / str.htm  - over TLS 1.3, to a file
 *   run httpget -p form.txt 10.0.2.2 8000 /f   - POSTs what form.txt holds
 *
 * The body of a POST comes from a file, not the command line: a form's
 * fields can be longer than an argument may be.
 *
 * It resolves the host name through DNS, copes with 'chunked' encoding
 * and follows redirects (up to three). For https addresses it connects
 * over TLS 1.3 and verifies the certificate: the built-in public CA
 * roots plus whatever happens to lie in a "roots.der" file on disk. A
 * certificate that does not hold up ends the connection - unless we use
 * -k.
 */

#include "uapi.h"
#include "uhttp.h"
#include "ucert.h"

#define MAX_REDIRECTS 3

struct sink_state {
    int fd;                  /* -1 = write to the console */
    uint32_t written;
};

static void body_sink(const char *data, uint32_t len, void *arg)
{
    struct sink_state *st = arg;
    if (st->fd >= 0)
        sys_write(st->fd, data, len);
    else
        sys_write(1, data, len);
    st->written += len;
}

void _start(int argc, char **argv)
{
    int insecure = 0;
    static char post[16384];
    const char *body = 0;
    uint32_t body_len = 0;
    while (argc > 1 && argv[1][0] == '-') {
        if (argv[1][1] == 'k') {
            insecure = 1;                /* like -k in curl */
        } else if (argv[1][1] == 'p' && argc > 2) {
            int fd = sys_open(argv[2], O_READ);
            if (fd < 0) {
                print("[httpget] I cannot read the body from ");
                print(argv[2]);
                print("\n");
                sys_exit(1);
            }
            for (;;) {
                int32_t n = sys_read(fd, post + body_len,
                                     sizeof(post) - body_len);
                if (n <= 0)
                    break;
                body_len += (uint32_t)n;
                if (body_len >= sizeof(post))
                    break;
            }
            sys_close(fd);
            body = post;
            argv++;
            argc--;
        } else {
            break;
        }
        argv++;
        argc--;
    }

    if (argc < 2) {
        print("usage: httpget [-k] [-p <file>] <host|ip> [port] [path] "
              "[file]\n");
        sys_exit(1);
    }

    char host[64];
    char path[512];
    uint32_t port = argc > 2 ? (uint32_t)uatoi(argv[2]) : 0;
    const char *outfile = argc > 4 ? argv[4] : 0;

    /* The host may be bare ("example.com") or a full address */
    uint32_t parsed_port = port;
    int use_tls = 0;
    if (!http_parse_url(argv[1], host, sizeof(host), &parsed_port,
                        path, sizeof(path), &use_tls)) {
        print("[httpget] a bad address\n");
        sys_exit(1);
    }
    if (port)
        parsed_port = port;              /* the port from the argument takes precedence */
    if (argc > 3) {
        int i = 0;
        while (argv[3][i] && i < (int)sizeof(path) - 1) {
            path[i] = argv[3][i];
            i++;
        }
        path[i] = '\0';
    }

    struct sink_state st;
    st.fd = -1;
    st.written = 0;

    if (outfile) {
        st.fd = sys_open(outfile, O_CREATE | O_WRITE | O_TRUNC);
        if (st.fd < 0) {
            print("[httpget] I cannot open the file ");
            print(outfile);
            print("\n");
            sys_exit(1);
        }
    }

    static uint8_t roots[16384];
    if (use_tls) {
        /* The built-in roots are always there; from disk we add our own */
        uint32_t roots_len = cert_load_roots("roots.der", roots, sizeof(roots));
        http_set_roots(roots, roots_len, !insecure);
        if (insecure)
            print("[httpget] WARNING: -k, the certificate will not be checked\n");
    }

    struct http_result res;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        uint32_t ip = http_resolve(host);
        if (!ip) {
            print("[httpget] I do not know the address of ");
            print(host);
            print("\n");
            if (st.fd >= 0)
                sys_close(st.fd);
            sys_exit(1);
        }

        print(use_tls ? "[httpget] TLS 1.3 " : "[httpget] ");
        if (body)
            print("POST ");
        print(host);
        print(" (");
        print_ip(ip);
        print(":");
        print_dec(parsed_port);
        print(")");
        print(path);
        print("\n");

        if (http_request(ip, parsed_port, host, path, use_tls, body,
                         body_len, body_sink, &st, &res) < 0) {
            print("[httpget] could not connect");
            if (use_tls && res.verify_err) {
                print(": ");
                print(cert_error_text(res.verify_err));
            }
            print("\n");
            if (st.fd >= 0)
                sys_close(st.fd);
            sys_exit(1);
        }

        int redirect = (res.status == 301 || res.status == 302 ||
                        res.status == 303 || res.status == 307 ||
                        res.status == 308);
        if (!redirect || !res.location[0])
            break;
        /* After a POST the answer is fetched with a GET - except where
           307 and 308 say to send the same again */
        if (res.status != 307 && res.status != 308)
            body = 0;

        print("[httpget] redirect -> ");
        print(res.location);
        print("\n");
        if (!http_apply_location(res.location, host, sizeof(host),
                                 &parsed_port, path, sizeof(path), &use_tls))
            break;
        st.written = 0;                  /* the body of a redirect does not count */
    }

    if (st.fd >= 0)
        sys_close(st.fd);

    if (use_tls) {
        print(res.verified ? "[httpget] certificate checked (issuer, "
                             "name, dates)\n"
                           : "[httpget] certificate NOT CHECKED - the channel is "
                             "encrypted, but nobody's identity is confirmed\n");
    }

    print("[httpget] HTTP ");
    print_dec((uint32_t)res.status);
    print(res.chunked ? " (chunked), content " : ", content ");
    print_dec(res.body_len);
    print(" B\n");
    sys_exit(res.status == 200 ? 0 : 1);
}
