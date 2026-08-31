/* System settings - what the machine remembers about itself
 *
 * A setting is a line in a text file on the disk: name=value. That is
 * the whole format, and it is plain text on purpose - it can be read
 * with 'cat' and changed with 'write', and when something goes wrong
 * it is obvious what.
 *
 * The set of names is fixed and lives in one table below, together
 * with the default and a line of description. A name that has no place
 * in the system reading it is not a setting - so adding one here is a
 * promise that somewhere there is code that asks for it.
 *
 * The file is read once, at boot, right after the file system comes
 * up. A machine with no disk gets the defaults and works exactly as it
 * did before there were any settings at all.
 */

#include "include/config.h"
#include "include/fs.h"
#include "include/string.h"

#define CONFIG_MAX_FILE 512

struct setting {
    const char *name;
    const char *fallback;
    const char *help;
    char value[CONFIG_VAL_MAX];
};

/* Every one of these is read somewhere. In order: the shell prompt,
   the network, the console and the desktop. */
static struct setting settings[] = {
    { "nazwa",  "proton",   "nazwa maszyny - widac ja w zachecie", { 0 } },
    { "dns",    "0.0.0.0",  "serwer nazw; 0.0.0.0 = ten z DHCP",  { 0 } },
    { "kolor",  "11",       "kolor zachety shella, 0-15",         { 0 } },
    { "tlo",    "0",        "tlo pulpitu: 0 morskie, 1 granat,\n"
                            "2 fiolet, 3 zielen, 4 braz, 5 szarosc", { 0 } },
};

#define NSETTINGS ((int)(sizeof(settings) / sizeof(settings[0])))

static struct setting *find(const char *key)
{
    for (int i = 0; i < NSETTINGS; i++)
        if (!strcmp(settings[i].name, key))
            return &settings[i];
    return (struct setting *)0;
}

/* A value copied with a bound: what does not fit is not truncated but
   refused. A setting cut in half is worse than one that was never
   changed - the first looks like it worked. */
static int put_value(struct setting *s, const char *value, uint32_t len)
{
    if (len >= CONFIG_VAL_MAX)
        return -1;
    for (uint32_t i = 0; i < len; i++)
        s->value[i] = value[i];
    s->value[len] = 0;
    return 0;
}

const char *config_get(const char *key)
{
    struct setting *s = find(key);
    if (!s)
        return (const char *)0;
    return s->value[0] ? s->value : s->fallback;
}

/* "10.0.2.3" -> 0x0A000203, and an ordinary decimal for everything
   else. Anything that is not a number gives the fallback: a setting
   the disk cannot make sense of is a setting that was not set. */
uint32_t config_get_num(const char *key, uint32_t fallback)
{
    const char *v = config_get(key);
    if (!v || !*v)
        return fallback;

    uint32_t parts[4] = { 0, 0, 0, 0 };
    int n = 0, digits = 0;
    for (const char *p = v; ; p++) {
        if (*p >= '0' && *p <= '9') {
            if (parts[n] > 429496729u)     /* it will not fit anyway */
                return fallback;
            parts[n] = parts[n] * 10 + (uint32_t)(*p - '0');
            digits++;
        } else if (*p == '.' && n < 3 && digits) {
            n++;
            digits = 0;
        } else if (*p == 0) {
            break;
        } else {
            return fallback;
        }
    }
    if (!digits)
        return fallback;

    if (n == 3) {                          /* four parts - an address */
        for (int i = 0; i < 4; i++)
            if (parts[i] > 255)
                return fallback;
        return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    }
    if (n)                                 /* two or three parts is neither */
        return fallback;
    return parts[0];
}

int config_count(void)               { return NSETTINGS; }

const char *config_name(int i)
{
    return (i >= 0 && i < NSETTINGS) ? settings[i].name : (const char *)0;
}

const char *config_value(int i)
{
    if (i < 0 || i >= NSETTINGS)
        return (const char *)0;
    return settings[i].value[0] ? settings[i].value : settings[i].fallback;
}

const char *config_help(int i)
{
    return (i >= 0 && i < NSETTINGS) ? settings[i].help : (const char *)0;
}

/* --- the file ------------------------------------------------------- */

void config_load(void)
{
    if (!fs_ready())
        return;

    char buf[CONFIG_MAX_FILE];
    int n = fs_read(CONFIG_FILE, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = 0;

    /* Line by line, and everything that is not 'name=value' is passed
       over in silence. The file comes from a disk, so it may hold
       anything at all - including something that was never written by
       us. */
    int i = 0;
    while (i < n) {
        int start = i;
        while (i < n && buf[i] != '\n')
            i++;
        int end = i;
        if (i < n)
            i++;                            /* step over the newline */

        int eq = -1;
        for (int k = start; k < end; k++)
            if (buf[k] == '=') {
                eq = k;
                break;
            }
        if (eq < 0 || eq == start)
            continue;                       /* no name, or no '=' at all */

        char key[CONFIG_VAL_MAX];
        int klen = eq - start;
        if (klen >= CONFIG_VAL_MAX)
            continue;
        for (int k = 0; k < klen; k++)
            key[k] = buf[start + k];
        key[klen] = 0;

        struct setting *s = find(key);
        if (!s)
            continue;                       /* a name nobody reads */
        put_value(s, buf + eq + 1, (uint32_t)(end - eq - 1));
    }
}

static uint32_t append(char *dst, uint32_t at, const char *src)
{
    while (*src && at < CONFIG_MAX_FILE - 1)
        dst[at++] = *src++;
    return at;
}

int config_set(const char *key, const char *value)
{
    struct setting *s = find(key);
    if (!s)
        return -1;

    uint32_t len = 0;
    while (value[len])
        len++;
    if (put_value(s, value, len) < 0)
        return -1;

    if (!fs_ready())
        return -1;                          /* changed here, but not remembered */

    char out[CONFIG_MAX_FILE];
    uint32_t at = 0;
    for (int i = 0; i < NSETTINGS; i++) {
        at = append(out, at, settings[i].name);
        if (at < CONFIG_MAX_FILE - 1)
            out[at++] = '=';
        at = append(out, at, config_value(i));
        if (at < CONFIG_MAX_FILE - 1)
            out[at++] = '\n';
    }
    /* fs_write says 0 for success, not the number of bytes */
    return fs_write(CONFIG_FILE, out, at) == 0 ? 0 : -1;
}
