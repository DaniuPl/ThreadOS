/* souptest - libsoup, glib-networking and OpenSSL on ThreadOS
 *
 * How the web engine's network process fetches pages. Given the URLs
 * of scripts/tests/t_soup.py's servers on the host and the CA file
 * their certificates come from, this goes through what a browser
 * asks of HTTP; every body is said as its length and SHA-256, which
 * the test, knowing what its servers sent, works out itself:
 *
 *   HTTP/1.1: a page, a megabyte sent in chunks, a gzip body decoded,
 *   a redirect followed, a cookie set and sent back, a POST whose body
 *   the server checks, a 404, four requests at once on the main loop
 *
 *   HTTPS through glib-networking's OpenSSL backend: TLS 1.3 to a
 *   server whose certificate the given CA signed, its subject and
 *   issuer; the same server refused without that CA, and a server
 *   whose certificate names another address refused for it
 *
 *   HTTP/2 over TLS (nghttp2), when the test has an HTTP/2 server
 *
 *   the public suffix list (libpsl): base domains and suffixes
 *
 *   OpenSSL itself: SHA-256 and HMAC
 *
 * Usage: souptest http://host:port https://host:port ca.pem
 *        [https-wrong-name://host:port] [h2://host:port]
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>

void g_io_openssl_load(GIOModule *module);    /* glib-networking, static */

static const char *http, *https, *ca_file;

static void say_body(const char *what, GBytes *body)
{
    gsize len = 0;
    const guchar *data = body ? g_bytes_get_data(body, &len) : NULL;
    char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                            data ? data : (const guchar *)"",
                                            len);
    printf("%s: %" G_GSIZE_FORMAT " bytes, sha256 %.16s\n", what, len, sha);
    g_free(sha);
}

static GBytes *get(SoupSession *s, const char *base, const char *path,
                   SoupMessage **out)
{
    char *url = g_strconcat(base, path, NULL);
    SoupMessage *m = soup_message_new("GET", url);
    g_free(url);
    GError *err = NULL;
    GBytes *body = soup_session_send_and_read(s, m, NULL, &err);
    if (err) {
        printf("%s: an error (%s)\n", path, err->message);
        g_clear_error(&err);
    }
    if (out)
        *out = m;
    else
        g_object_unref(m);
    return body;
}

/* --- HTTP/1.1 ----------------------------------------------------------- */

static int pending;
static GMainLoop *loop;

static void got_one(GObject *src, GAsyncResult *res, gpointer user)
{
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res,
                                                     NULL);
    int *ok = user;
    gsize len = 0;
    const char *data = body ? g_bytes_get_data(body, &len) : NULL;
    if (data && len >= 4 && !memcmp(data, "part", 4))
        (*ok)++;
    if (body)
        g_bytes_unref(body);
    if (--pending == 0)
        g_main_loop_quit(loop);
}

static void plain_http(void)
{
    SoupSession *s = soup_session_new();
    SoupMessage *m;
    GBytes *b = get(s, http, "/hello", &m);
    printf("/hello: status %u, %s\n", soup_message_get_status(m),
           soup_message_headers_get_content_type(
               soup_message_get_response_headers(m), NULL));
    say_body("/hello", b);
    g_bytes_unref(b);
    g_object_unref(m);

    b = get(s, http, "/big", &m);
    printf("/big: sent %s\n",
           soup_message_headers_get_encoding(
               soup_message_get_response_headers(m)) == SOUP_ENCODING_CHUNKED
               ? "in chunks" : "whole");
    say_body("/big", b);
    g_bytes_unref(b);
    g_object_unref(m);

    b = get(s, http, "/gzip", &m);
    printf("/gzip: came as %s\n", soup_message_headers_get_one(
               soup_message_get_response_headers(m), "Content-Encoding"));
    say_body("/gzip decoded", b);
    g_bytes_unref(b);
    g_object_unref(m);

    b = get(s, http, "/redirect", &m);
    GUri *uri = soup_message_get_uri(m);
    printf("/redirect: ended at %s\n", g_uri_get_path(uri));
    say_body("/redirect", b);
    g_bytes_unref(b);
    g_object_unref(m);

    soup_session_add_feature_by_type(s, SOUP_TYPE_COOKIE_JAR);
    g_bytes_unref(get(s, http, "/set-cookie", NULL));
    b = get(s, http, "/cookies", NULL);
    printf("the cookie came back: %.*s\n", (int)g_bytes_get_size(b),
           (const char *)g_bytes_get_data(b, NULL));
    g_bytes_unref(b);

    GString *post = g_string_new("");
    for (int i = 0; i < 4000; i++)
        g_string_append_printf(post, "line %d of the posted body\n", i);
    char *url = g_strconcat(http, "/echo", NULL);
    m = soup_message_new("POST", url);
    g_free(url);
    GBytes *sent = g_string_free_to_bytes(post);
    soup_message_set_request_body_from_bytes(m, "text/plain", sent);
    b = soup_session_send_and_read(s, m, NULL, NULL);
    say_body("POST /echo, sent", sent);
    printf("and the server says: %.*s\n", (int)g_bytes_get_size(b),
           (const char *)g_bytes_get_data(b, NULL));
    g_bytes_unref(b);
    g_bytes_unref(sent);
    g_object_unref(m);

    b = get(s, http, "/missing", &m);
    printf("/missing: status %u %s\n", soup_message_get_status(m),
           soup_message_get_reason_phrase(m));
    g_bytes_unref(b);
    g_object_unref(m);

    loop = g_main_loop_new(NULL, FALSE);
    int ok = 0;
    for (int i = 1; i <= 4; i++) {
        char *u = g_strdup_printf("%s/part/%d", http, i);
        SoupMessage *pm = soup_message_new("GET", u);
        g_free(u);
        pending++;
        soup_session_send_and_read_async(s, pm, G_PRIORITY_DEFAULT, NULL,
                                         got_one, &ok);
        g_object_unref(pm);
    }
    g_main_loop_run(loop);
    printf("four requests at once on the main loop: %d of 4\n", ok);
    g_main_loop_unref(loop);
    g_object_unref(s);
}

/* --- HTTPS ------------------------------------------------------------------ */

static void name_of(const char *what, GTlsCertificate *cert)
{
    char *subject = cert ? g_tls_certificate_get_subject_name(cert) : NULL;
    char *issuer = cert ? g_tls_certificate_get_issuer_name(cert) : NULL;
    printf("%s: subject %s, issuer %s\n", what, subject ? subject : "-",
           issuer ? issuer : "-");
    g_free(subject);
    g_free(issuer);
}

static void https_with(const char *what, const char *base, int with_ca)
{
    SoupSession *s = soup_session_new();
    if (with_ca) {
        GError *err = NULL;
        GTlsDatabase *db = g_tls_file_database_new(ca_file, &err);
        if (!db) {
            printf("%s: the CA file cannot be read (%s)\n", what,
                   err ? err->message : "?");
            g_clear_error(&err);
            return;
        }
        soup_session_set_tls_database(s, db);
        g_object_unref(db);
    }
    char *url = g_strconcat(base, "/hello", NULL);
    SoupMessage *m = soup_message_new("GET", url);
    g_free(url);
    GError *err = NULL;
    GBytes *b = soup_session_send_and_read(s, m, NULL, &err);
    if (err) {
        GTlsCertificateFlags f = soup_message_get_tls_peer_certificate_errors(m);
        printf("%s: refused (%s%s%s)\n", what,
               f & G_TLS_CERTIFICATE_UNKNOWN_CA ? "unknown CA" : "",
               (f & G_TLS_CERTIFICATE_UNKNOWN_CA) &&
               (f & G_TLS_CERTIFICATE_BAD_IDENTITY) ? ", " : "",
               f & G_TLS_CERTIFICATE_BAD_IDENTITY ? "wrong name" : "");
        g_clear_error(&err);
    } else {
        GTlsProtocolVersion v = soup_message_get_tls_protocol_version(m);
        printf("%s: status %u over %s\n", what, soup_message_get_status(m),
               v == G_TLS_PROTOCOL_VERSION_TLS_1_3 ? "TLS 1.3" :
               v == G_TLS_PROTOCOL_VERSION_TLS_1_2 ? "TLS 1.2" : "other");
        say_body(what, b);
        name_of("its certificate", soup_message_get_tls_peer_certificate(m));
        g_bytes_unref(b);
    }
    g_object_unref(m);
    g_object_unref(s);
}

static void http2(const char *base)
{
    SoupSession *s = soup_session_new();
    GError *err = NULL;
    GTlsDatabase *db = g_tls_file_database_new(ca_file, &err);
    soup_session_set_tls_database(s, db);
    g_object_unref(db);
    char *url = g_strconcat(base, "/h2", NULL);
    SoupMessage *m = soup_message_new("GET", url);
    g_free(url);
    GBytes *b = soup_session_send_and_read(s, m, NULL, &err);
    if (err) {
        printf("HTTP/2: an error (%s)\n", err->message);
        g_clear_error(&err);
    } else {
        printf("HTTP/2: status %u, spoken %s\n", soup_message_get_status(m),
               soup_message_get_http_version(m) == SOUP_HTTP_2_0
                   ? "HTTP/2" : "HTTP/1");
        say_body("HTTP/2 /h2", b);
        g_bytes_unref(b);
    }
    g_object_unref(m);
    g_object_unref(s);
}

/* --- the rest ------------------------------------------------------------- */

static void public_suffixes(void)
{
    const char *hosts[] = { "www.example.co.uk", "a.b.example.com",
                            "shop.example.pl", "foo.bar.blogspot.com",
                            "x.y.githubusercontent.com" };
    for (unsigned i = 0; i < G_N_ELEMENTS(hosts); i++) {
        GError *err = NULL;
        const char *base = soup_tld_get_base_domain(hosts[i], &err);
        printf("the base domain of %s: %s\n", hosts[i], base ? base : "-");
        g_clear_error(&err);
    }
    printf("co.uk is a public suffix: %s; example.co.uk is not: %s\n",
           soup_tld_domain_is_public_suffix("co.uk") ? "yes" : "no",
           soup_tld_domain_is_public_suffix("example.co.uk") ? "no" : "yes");
}

static void openssl(void)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    EVP_Digest("ThreadOS", 8, md, &len, EVP_sha256(), NULL);
    printf("OpenSSL SHA-256 of 'ThreadOS': ");
    for (unsigned i = 0; i < len; i++)
        printf("%02x", md[i]);
    HMAC(EVP_sha256(), "key", 3, (const unsigned char *)"message", 7, md, &len);
    printf("\nOpenSSL HMAC-SHA256: ");
    for (unsigned i = 0; i < len; i++)
        printf("%02x", md[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 4) {
        printf("usage: souptest http://... https://... ca.pem [wrong] [h2]\n");
        return 2;
    }
    http = argv[1];
    https = argv[2];
    /* (a GTlsFileDatabase wants its file by an absolute path) */
    ca_file = g_canonicalize_filename(argv[3], NULL);
    g_io_openssl_load(NULL);
    plain_http();
    https_with("HTTPS with the CA", https, 1);
    https_with("HTTPS without it", https, 0);
    if (argc > 4)
        https_with("HTTPS, a certificate for another address", argv[4], 1);
    if (argc > 5)
        http2(argv[5]);
    public_suffixes();
    openssl();
    printf("~%s, libsoup %u.%u.%u\n", OPENSSL_VERSION_TEXT,
           soup_get_major_version(), soup_get_minor_version(),
           soup_get_micro_version());
    return 0;
}
