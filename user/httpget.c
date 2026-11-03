/* httpget - an HTTP/HTTPS client in ring 3
 *
 * Usage: run httpget [-k] [-p <file>] [-y <type>] [-c <jar>] [-o <file>]
 *                    [-t <file>] [-s <file>] <address> [port] [path] [file]
 *        run httpget [-c <jar>] -b <list>
 *   run httpget 10.0.2.2 8000 /                - to the console
 *   run httpget https://example.com / str.htm  - over TLS 1.3, to a file
 *   run httpget -p form.txt 10.0.2.2 8000 /f   - POSTs what form.txt holds
 *
 * The body of a POST comes from a file, not the command line: a form's
 * fields can be longer than an argument may be. With -c it keeps cookies
 * in a jar file (ucookie.h) - read before, written after, and followed
 * through redirects, since a sign-in usually answers with a cookie and a
 * redirect in one response. -o names the output file, so a full address
 * can stand alone, without a port and a path after it. -t writes the
 * answer's Content-Type to a file: a browser needs its charset. -s
 * writes its status code, which a page's script reads (a 404 is an
 * answer too), and -y says what a POST's body is, where it is not a
 * form's fields. -u reads the address itself from a file: an argument
 * holds 256 bytes, and the addresses of a site's style sheets are often
 * longer.
 *
 * -b <list> fetches many: each line of the list is an address, a tab and
 * the file it goes to, and "<file>.st" gets its status code and type.
 * Requests to one server go over one connection, kept between them
 * (keep-alive): a page's pictures cost a TCP and a TLS handshake once,
 * not each. With -c the jar's cookies go with them, but the jar is not
 * written: a browser runs several lists at once, and each writing the
 * jar back would lose the others' changes.
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
#include "ucookie.h"

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

static int cookie_header(struct cookie_jar *jar, const char *host,
                         const char *path, int tls, int64_t now, char *out,
                         int max)
{
    int o = 0;
    const char *pre = "Cookie: ";
    while (*pre)
        out[o++] = *pre++;
    int n = ck_header(jar, host, path, tls, now, out + o, max - o - 3);
    if (!n)
        return 0;
    o += n;
    out[o++] = '\r';
    out[o++] = '\n';
    out[o] = '\0';
    return 1;
}

static int put_dec(char *out, uint32_t v)
{
    char d[12];
    int n = 0, o = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out[o++] = d[--n];
    return o;
}

/* -b: every line of the list "<address>\t<file>", over kept connections */
static void run_batch(const char *list_file, struct cookie_jar *jar,
                      int use_jar, int64_t now)
{
    static char list[32768];
    int fd = sys_open(list_file, O_READ);
    int len = 0;
    int32_t n;
    while (fd >= 0 && len < (int)sizeof(list) - 1 &&
           (n = sys_read(fd, list + len, sizeof(list) - 1 - len)) > 0)
        len += n;
    if (fd >= 0)
        sys_close(fd);
    if (fd < 0) {
        print("[httpget] I cannot read the list ");
        print(list_file);
        print("\n");
        sys_exit(1);
    }
    list[len] = '\0';

    static struct http_conn conn;
    static char host[256], path[2048], cookie_line[4096], st_name[300];
    static char last_host[256];
    static uint8_t roots[16384];
    int roots_loaded = 0;
    uint32_t last_ip = 0, count = 0, ok = 0;
    struct http_result res;
    for (char *line = list; *line;) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char *next = *end ? end + 1 : end;
        *end = '\0';
        char *tab = line;
        while (*tab && *tab != '\t')
            tab++;
        if (!*tab) {
            line = next;
            continue;
        }
        *tab = '\0';
        const char *url = line, *file = tab + 1;
        line = next;
        count++;

        uint32_t port = 0;
        int tls = 0;
        if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path),
                            &tls))
            continue;
        if (tls && !roots_loaded) {
            uint32_t rl = cert_load_roots("roots.der", roots, sizeof(roots));
            http_set_roots(roots, rl, 1);
            roots_loaded = 1;
        }
        struct sink_state st = { sys_open(file, O_CREATE | O_WRITE | O_TRUNC), 0 };
        if (st.fd < 0)
            continue;
        res.status = 0;
        for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
            int k = 0;
            while (host[k] && host[k] == last_host[k])
                k++;
            uint32_t ip = last_ip && host[k] == last_host[k] ? last_ip
                                                              : http_resolve(host);
            if (!ip)
                break;
            last_ip = ip;
            for (k = 0; host[k]; k++)
                last_host[k] = host[k];
            last_host[k] = '\0';
            const char *extra = use_jar && cookie_header(jar, host, path, tls, now,
                                                         cookie_line,
                                                         (int)sizeof(cookie_line))
                                    ? cookie_line : 0;
            if (http_conn_request(&conn, ip, port, host, path, tls, extra,
                                  body_sink, &st, &res) < 0) {
                res.status = 0;
                break;
            }
            int redirect = res.status == 301 || res.status == 302 ||
                           res.status == 303 || res.status == 307 ||
                           res.status == 308;
            if (!redirect || !res.location[0] ||
                !http_apply_location(res.location, host, sizeof(host), &port,
                                     path, sizeof(path), &tls))
                break;
            /* the redirect's own body is not the answer */
            sys_close(st.fd);
            st.fd = sys_open(file, O_CREATE | O_WRITE | O_TRUNC);
            st.written = 0;
        }
        sys_close(st.fd);
        if (res.status == 200)
            ok++;

        /* "<status> <type>" beside it */
        int o = 0;
        for (; file[o] && o < (int)sizeof(st_name) - 4; o++)
            st_name[o] = file[o];
        st_name[o++] = '.';
        st_name[o++] = 's';
        st_name[o++] = 't';
        st_name[o] = '\0';
        static char line_out[200];
        int w = put_dec(line_out, (uint32_t)res.status);
        line_out[w++] = ' ';
        const char *t = res.status ? http_last_type() : "";
        for (int j = 0; t[j] && w < (int)sizeof(line_out) - 1; j++)
            line_out[w++] = t[j];
        int sf = sys_open(st_name, O_CREATE | O_WRITE | O_TRUNC);
        if (sf >= 0) {
            sys_write(sf, line_out, (uint32_t)w);
            sys_close(sf);
        }
    }
    http_conn_close(&conn);
    print("[httpget] batch: ");
    print_dec(ok);
    print(" of ");
    print_dec(count);
    print(" fetched, ");
    print_dec(conn.reused);
    print(" over a kept connection\n");
    sys_exit(ok == count ? 0 : 1);
}

void _start(int argc, char **argv)
{
    int insecure = 0;
    static char post[16384];
    const char *body = 0;
    uint32_t body_len = 0;
    const char *jar_file = 0, *out_opt = 0, *type_file = 0, *status_file = 0;
    const char *batch_file = 0;
    static char url_buf[2048];
    const char *url_arg = 0;
    while (argc > 1 && argv[1][0] == '-') {
        char o = argv[1][1];
        if ((o == 'c' || o == 'o' || o == 't' || o == 's' || o == 'y' ||
             o == 'u' || o == 'b') && argc > 2) {
            if (o == 'u') {
                int fd = sys_open(argv[2], O_READ);
                int32_t n = fd >= 0 ? sys_read(fd, url_buf, sizeof(url_buf) - 1) : -1;
                if (fd >= 0)
                    sys_close(fd);
                if (n <= 0) {
                    print("[httpget] I cannot read the address from ");
                    print(argv[2]);
                    print("\n");
                    sys_exit(1);
                }
                while (n > 0 && (url_buf[n - 1] == '\n' || url_buf[n - 1] == '\r'))
                    n--;
                url_buf[n] = '\0';
                url_arg = url_buf;
            } else if (o == 'c')
                jar_file = argv[2];
            else if (o == 'b')
                batch_file = argv[2];
            else if (o == 'o')
                out_opt = argv[2];
            else if (o == 't')
                type_file = argv[2];
            else if (o == 's')
                status_file = argv[2];
            else
                http_set_post_type(argv[2]);
            argv++;
            argc--;
        } else if (argv[1][1] == 'k') {
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

    if (batch_file) {
        static struct cookie_jar bjar;
        static char btext[24576];
        struct utime bclock;
        sys_time(&bclock);
        int64_t bnow = ck_epoch(bclock.year, bclock.month, bclock.day,
                                bclock.hour, bclock.min, bclock.sec);
        int blen = 0;
        int jf = jar_file ? sys_open(jar_file, O_READ) : -1;
        if (jf >= 0) {
            int32_t n;
            while (blen < (int)sizeof(btext) &&
                   (n = sys_read(jf, btext + blen, sizeof(btext) - blen)) > 0)
                blen += n;
            sys_close(jf);
        }
        ck_load(&bjar, btext, blen);
        run_batch(batch_file, &bjar, jar_file != 0, bnow);
    }

    if (argc < 2 && !url_arg) {
        print("usage: httpget [-k] [-p <file>] [-y <type>] [-c <jar>] "
              "[-o <file>] [-t <file>] [-s <file>] [-u <file>] <host|ip> "
              "[port] [path] [file]\n");
        sys_exit(1);
    }
    const char *address = url_arg ? url_arg : argv[1];
    if (url_arg) {
        argc = 1;                        /* (no port, path or file after it) */
    }

    char host[256];
    static char path[2048];
    uint32_t port = argc > 2 ? (uint32_t)uatoi(argv[2]) : 0;
    const char *outfile = argc > 4 ? argv[4] : out_opt;

    /* The host may be bare ("example.com") or a full address */
    uint32_t parsed_port = port;
    int use_tls = 0;
    if (!http_parse_url(address, host, sizeof(host), &parsed_port,
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

    static struct cookie_jar jar;
    /* The time the cookies' lives are measured by: the clock's */
    struct utime clock;
    sys_time(&clock);
    int64_t now = ck_epoch(clock.year, clock.month, clock.day, clock.hour,
                           clock.min, clock.sec);
    static char jar_text[24576];
    static char cookie_line[4096];
    if (jar_file) {
        int fd = sys_open(jar_file, O_READ);
        int len = 0;
        if (fd >= 0) {
            int32_t n;
            while (len < (int)sizeof(jar_text) &&
                   (n = sys_read(fd, jar_text + len,
                                 sizeof(jar_text) - len)) > 0)
                len += n;
            sys_close(fd);
        }
        ck_load(&jar, jar_text, len);
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

        print(use_tls ? "[httpget] TLS " : "[httpget] ");
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

        const char *extra = 0;
        if (jar_file && cookie_header(&jar, host, path, use_tls, now,
                                      cookie_line, (int)sizeof(cookie_line))) {
            extra = cookie_line;
            print("[httpget] sending cookies\n");
        }
        if (http_request(ip, parsed_port, host, path, use_tls, extra, body,
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

        /* What the response set, for the host and path it came from -
           before a redirect moves them */
        if (jar_file) {
            const char *sc = http_last_cookies();
            static char one[1024];
            while (*sc) {
                int k = 0;
                while (sc[k] && sc[k] != '\n')
                    k++;
                int c = k < (int)sizeof(one) - 1 ? k : (int)sizeof(one) - 1;
                for (int j = 0; j < c; j++)
                    one[j] = sc[j];
                one[c] = '\0';
                if (ck_set(&jar, one, host, path, use_tls, now)) {
                    print("[httpget] cookie kept: ");
                    for (int j = 0; one[j] && one[j] != '='; j++) {
                        char ch[2] = { one[j], 0 };
                        print(ch);
                    }
                    print("\n");
                }
                sc += k;
                if (*sc)
                    sc++;
            }
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

    if (type_file) {
        const char *t = http_last_type();
        int n = 0;
        while (t[n])
            n++;
        int fd = sys_open(type_file, O_CREATE | O_WRITE | O_TRUNC);
        if (fd >= 0) {
            sys_write(fd, t, (uint32_t)n);
            sys_close(fd);
        }
    }

    if (status_file) {
        char digits[12];
        int nd = 0;
        uint32_t v = (uint32_t)res.status;
        do {
            digits[11 - nd++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        int fd = sys_open(status_file, O_CREATE | O_WRITE | O_TRUNC);
        if (fd >= 0) {
            sys_write(fd, digits + 12 - nd, (uint32_t)nd);
            sys_close(fd);
        }
    }

    if (jar_file) {
        ck_expire(&jar, now);
        int len = ck_save(&jar, jar_text, sizeof(jar_text));
        int fd = sys_open(jar_file, O_CREATE | O_WRITE | O_TRUNC);
        if (fd >= 0) {
            sys_write(fd, jar_text, (uint32_t)len);
            sys_close(fd);
        }
    }

    if (use_tls) {
        print(res.tls12 ? "[httpget] TLS 1.2 agreed\n"
                        : "[httpget] TLS 1.3 agreed\n");
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
