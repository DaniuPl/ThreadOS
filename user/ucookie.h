/* Cookies for ring 3 HTTP clients - a jar kept in a file
 *
 * What a site asks to be remembered (Set-Cookie) goes into the jar and
 * comes back to it on every later request (Cookie) - that is all a
 * sign-in needs: the session a login page hands out, sent with each
 * page after it. The rules followed are the ones that decide which
 * cookie goes where (RFC 6265):
 *   - a cookie belongs to the host that set it, or with Domain= to
 *     that domain and the hosts under it - never to a domain the host
 *     is not in;
 *   - it goes only to paths under its Path= (by default, the directory
 *     of the page that set it);
 *   - Secure ones only over https;
 *   - Max-Age=0 or an Expires= in the past removes one, which is how
 *     a site signs you out.
 * Other expiry dates are not kept: a cookie lives until it is replaced
 * or removed. The jar is a text file, a cookie a line:
 *     host path secure domain name value
 * - cookie names and values hold no spaces, so a space separates.
 *
 * No system calls in here: the program reads and writes the file.
 */

#ifndef THREADOS_UCOOKIE_H
#define THREADOS_UCOOKIE_H

#include <stdint.h>

#define CK_MAX   48
#define CK_HOST  64
#define CK_PATH  96
#define CK_NAME  64
#define CK_VALUE 384

struct cookie {
    char host[CK_HOST];          /* the host, or with 'domain' the domain */
    char path[CK_PATH];
    char name[CK_NAME];
    char value[CK_VALUE];
    uint8_t secure, domain;
};

struct cookie_jar {
    int n;
    struct cookie c[CK_MAX];
};

static inline int ck_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline int ck_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static inline int ck_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* Copies at most n bytes of src (or up to its end), always ending dst */
static inline void ck_copy(char *dst, int max, const char *src, int n)
{
    int i = 0;
    while (i < n && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Is 'host' the domain 'd' or a host under it? */
static inline int ck_domain_match(const char *host, const char *d)
{
    int hl = ck_len(host), dl = ck_len(d);
    if (dl == 0 || dl > hl)
        return 0;
    for (int i = 0; i < dl; i++)
        if (ck_lower(host[hl - dl + i]) != ck_lower(d[i]))
            return 0;
    return hl == dl || host[hl - dl - 1] == '.';
}

/* Does a request for 'path' (its query cut off) fall under 'cp'? */
static inline int ck_path_match(const char *path, const char *cp)
{
    int n = ck_len(cp), i = 0;
    while (i < n && path[i] == cp[i])
        i++;
    if (i < n)
        return 0;
    return path[n] == '\0' || path[n] == '?' || path[n] == '/' ||
           cp[n - 1] == '/';
}

/* A space or a control character: what would break the jar's lines */
static inline int ck_bad(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s <= ' ' || *s == 0x7F)
            return 1;
    return 0;
}

static inline int ck_find(struct cookie_jar *jar, const char *host,
                          const char *path, const char *name)
{
    for (int i = 0; i < jar->n; i++)
        if (ck_eq(jar->c[i].host, host) && ck_eq(jar->c[i].path, path) &&
            ck_eq(jar->c[i].name, name))
            return i;
    return -1;
}

static inline void ck_remove(struct cookie_jar *jar, int i)
{
    for (; i + 1 < jar->n; i++)
        jar->c[i] = jar->c[i + 1];
    jar->n--;
}

/* Does an attribute 'a' (n bytes, "Name=..." or "Name") have this name?
   Returns where its value starts, or 0. */
static inline const char *ck_attr(const char *a, int n, const char *name)
{
    int i = 0;
    while (name[i]) {
        if (i >= n || ck_lower(a[i]) != ck_lower(name[i]))
            return 0;
        i++;
    }
    while (i < n && a[i] == ' ')
        i++;
    if (i == n)
        return a + i;            /* a flag, with no value */
    if (a[i] != '=')
        return 0;
    i++;
    while (i < n && a[i] == ' ')
        i++;
    return a + i;
}

/* One Set-Cookie header's value, from a response to host/path. Returns
   1 when the jar changed. */
static inline int ck_set(struct cookie_jar *jar, const char *v,
                         const char *host, const char *path, int tls)
{
    struct cookie c;
    int i = 0, n;

    while (v[i] == ' ')
        i++;
    n = i;
    while (v[n] && v[n] != '=' && v[n] != ';')
        n++;
    if (v[n] != '=' || n == i)
        return 0;                /* no name, or no value: not a cookie */
    int ne = n;
    while (ne > i && v[ne - 1] == ' ')
        ne--;
    ck_copy(c.name, sizeof(c.name), v + i, ne - i);
    i = n + 1;
    n = i;
    while (v[n] && v[n] != ';')
        n++;
    int ve = n;
    while (ve > i && v[ve - 1] == ' ')
        ve--;
    ck_copy(c.value, sizeof(c.value), v + i, ve - i);
    /* The value must not break the jar's lines */
    for (int k = 0; c.value[k]; k++)
        if ((unsigned char)c.value[k] <= ' ' || c.value[k] == 0x7F)
            c.value[k] = '_';

    ck_copy(c.host, sizeof(c.host), host, CK_HOST);
    for (int k = 0; c.host[k]; k++)
        c.host[k] = (char)ck_lower(c.host[k]);
    c.domain = 0;
    c.secure = 0;
    /* The default path: the directory of the page that set it */
    int last = 0;
    for (int k = 0; path[k] && path[k] != '?'; k++)
        if (path[k] == '/')
            last = k;
    if (last == 0)
        ck_copy(c.path, sizeof(c.path), "/", 1);
    else
        ck_copy(c.path, sizeof(c.path), path, last);
    int gone = 0;

    while (v[n] == ';') {
        i = n + 1;
        while (v[i] == ' ')
            i++;
        n = i;
        while (v[n] && v[n] != ';')
            n++;
        int an = n - i;
        while (an > 0 && v[i + an - 1] == ' ')
            an--;
        const char *a = v + i, *val;
        int vn;
        if ((val = ck_attr(a, an, "domain")) != 0) {
            vn = an - (int)(val - a);
            while (vn > 0 && *val == '.')
                val++, vn--;
            char d[CK_HOST];
            ck_copy(d, sizeof(d), val, vn);
            /* Only the host's own domain - and not a bare top level
               one, which every site under it would share */
            int dot = 0;
            for (int k = 0; d[k]; k++)
                dot |= d[k] == '.';
            if (!ck_domain_match(c.host, d) || (!dot && !ck_eq(c.host, d)))
                return 0;
            for (int k = 0; d[k]; k++)
                d[k] = (char)ck_lower(d[k]);
            ck_copy(c.host, sizeof(c.host), d, CK_HOST);
            c.domain = 1;
        } else if ((val = ck_attr(a, an, "path")) != 0) {
            vn = an - (int)(val - a);
            if (vn > 0 && val[0] == '/')
                ck_copy(c.path, sizeof(c.path), val, vn);
        } else if ((val = ck_attr(a, an, "max-age")) != 0) {
            vn = an - (int)(val - a);
            int neg = vn > 0 && val[0] == '-';
            long age = 0;
            for (int k = neg; k < vn && val[k] >= '0' && val[k] <= '9'; k++)
                age = age * 10 + (val[k] - '0');
            if (neg || age == 0)
                gone = 1;
        } else if ((val = ck_attr(a, an, "expires")) != 0) {
            /* "Thu, 01 Jan 1970 00:00:00 GMT": a year before any this
               system has run in means "delete" */
            vn = an - (int)(val - a);
            for (int k = 0; k + 4 <= vn; k++) {
                int y = 0, digits = 0;
                while (digits < 4 && val[k + digits] >= '0' &&
                       val[k + digits] <= '9')
                    y = y * 10 + (val[k + digits++] - '0');
                if (digits == 4 && (k + 4 == vn || val[k + 4] < '0' ||
                                    val[k + 4] > '9')) {
                    if (y < 2020)
                        gone = 1;
                    break;
                }
                k += digits;
            }
        } else if (ck_attr(a, an, "secure")) {
            c.secure = 1;
        }
    }
    if (c.secure && !tls)
        return 0;                /* only an https page may set a Secure one */
    /* A name or a path with a space in it: not one this jar can keep
       (nor one a well-made site sends) */
    if (ck_bad(c.name) || ck_bad(c.path) || ck_bad(c.host))
        return 0;

    int at = ck_find(jar, c.host, c.path, c.name);
    if (gone) {
        if (at < 0)
            return 0;
        ck_remove(jar, at);
        return 1;
    }
    if (at < 0) {
        if (jar->n == CK_MAX)
            ck_remove(jar, 0);   /* the oldest makes room */
        at = jar->n++;
    }
    jar->c[at] = c;
    return 1;
}

/* The Cookie header's value for a request: "a=1; b=2", the longer
   paths first, as RFC 6265 asks. Returns its length (0: none). */
static inline int ck_header(struct cookie_jar *jar, const char *host,
                            const char *path, int tls, char *out, int max)
{
    int o = 0;
    char lhost[CK_HOST];
    ck_copy(lhost, sizeof(lhost), host, CK_HOST);
    for (int k = 0; lhost[k]; k++)
        lhost[k] = (char)ck_lower(lhost[k]);
    out[0] = '\0';
    for (int len = CK_PATH; len > 0; len--) {
        for (int i = 0; i < jar->n; i++) {
            struct cookie *c = &jar->c[i];
            if (ck_len(c->path) != len || (c->secure && !tls))
                continue;
            if (c->domain ? !ck_domain_match(lhost, c->host)
                          : !ck_eq(lhost, c->host))
                continue;
            if (!ck_path_match(path, c->path))
                continue;
            int need = ck_len(c->name) + 1 + ck_len(c->value) + (o ? 2 : 0);
            if (o + need >= max)
                return o;
            if (o) {
                out[o++] = ';';
                out[o++] = ' ';
            }
            for (int k = 0; c->name[k]; k++)
                out[o++] = c->name[k];
            out[o++] = '=';
            for (int k = 0; c->value[k]; k++)
                out[o++] = c->value[k];
            out[o] = '\0';
        }
    }
    return o;
}

/* The next space-separated field of a jar line */
static inline int ck_field(const char *s, int *at, int end, char *dst,
                           int max)
{
    int i = *at;
    while (i < end && s[i] == ' ')
        i++;
    int b = i;
    while (i < end && s[i] != ' ')
        i++;
    *at = i;
    ck_copy(dst, max, s + b, i - b);
    return i > b;
}

static inline void ck_load(struct cookie_jar *jar, const char *s, int len)
{
    jar->n = 0;
    for (int p = 0; p < len && jar->n < CK_MAX;) {
        int e = p;
        while (e < len && s[e] != '\n')
            e++;
        struct cookie *c = &jar->c[jar->n];
        char sec[4], dom[4];
        int at = p;
        if (ck_field(s, &at, e, c->host, CK_HOST) &&
            ck_field(s, &at, e, c->path, CK_PATH) &&
            ck_field(s, &at, e, sec, sizeof(sec)) &&
            ck_field(s, &at, e, dom, sizeof(dom)) &&
            ck_field(s, &at, e, c->name, CK_NAME)) {
            ck_field(s, &at, e, c->value, CK_VALUE);
            c->secure = sec[0] == '1';
            c->domain = dom[0] == '1';
            jar->n++;
        }
        p = e + 1;
    }
}

/* The jar as text; returns its length */
static inline int ck_save(const struct cookie_jar *jar, char *out, int max)
{
    int o = 0;
    for (int i = 0; i < jar->n; i++) {
        const struct cookie *c = &jar->c[i];
        const char *parts[6] = { c->host, c->path, c->secure ? "1" : "0",
                                 c->domain ? "1" : "0", c->name, c->value };
        int need = 0;
        for (int k = 0; k < 6; k++)
            need += ck_len(parts[k]) + 1;
        if (o + need >= max)
            break;
        for (int k = 0; k < 6; k++) {
            for (int j = 0; parts[k][j]; j++)
                out[o++] = parts[k][j];
            out[o++] = k == 5 ? '\n' : ' ';
        }
    }
    return o;
}

#endif
