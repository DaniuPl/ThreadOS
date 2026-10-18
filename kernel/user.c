/* Who is at the machine - the admin's password and what it guards
 *
 * The password is not kept in the settings file as itself but as a
 * number computed from it. Which is worth being honest about: this is
 * a hash of eight bytes, not a function made for passwords. It stops
 * the password being READ out of a file that anybody may look into. It
 * would not stop somebody who took the disk away and set about the
 * number with a dictionary - and nothing here pretends otherwise.
 *
 * What it does buy is the thing the machine needs: 'set name X' and
 * writing to the system's own files ask who is doing it, and the
 * answer has to be earned once rather than assumed forever.
 */

#include "include/user.h"
#include "include/config.h"
#include "include/string.h"
#include "include/pit.h"

/* FNV-1a over a salt and the password. The salt makes the same
   password on two machines a different number - and, more to the
   point, makes the number in the file useless anywhere else. */
static uint64_t hash(const char *password)
{
    static const char salt[] = "ThreadOS:";
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char *p = salt; *p; p++) {
        h ^= (uint64_t)(uint8_t)*p;
        h *= 0x100000001b3ULL;
    }
    for (const char *p = password; *p; p++) {
        h ^= (uint64_t)(uint8_t)*p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void to_text(uint64_t v, char *out)
{
    char tmp[24];
    int n = 0, j = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    while (n)
        out[j++] = tmp[--n];
    out[j] = 0;
}

static uint64_t from_text(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

int user_is_system_file(const char *name)
{
    static const char *files[] = { "system.cfg", "swap.sys", "icons.pos",
                                   "update.key" };   /* who signs updates */
    for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++)
        if (!strcmp(name, files[i]))
            return 1;
    return 0;
}

/* Guessing costs time. Three wrong answers and the machine says no for
   a few seconds without even looking - a password of six characters is
   otherwise a short afternoon for a program that can ask as fast as it
   likes. */
static int wrong;
static uint32_t locked_until;

int user_password_ok(const char *password)
{
    uint32_t now = pit_ticks();
    if (locked_until && now < locked_until)
        return 0;

    const char *stored = config_get("password");
    if (!stored || !*stored)
        return 0;

    if (hash(password) == from_text(stored)) {
        wrong = 0;
        locked_until = 0;
        return 1;
    }

    if (++wrong >= 3) {
        wrong = 0;
        locked_until = now + 500;       /* about five seconds at 100 Hz */
    }
    return 0;
}

int user_set_password(const char *password)
{
    char text[24];
    to_text(hash(password), text);
    return config_set("password", text);
}

static int shell_level = USER_PLAIN;

int user_shell_level(void)        { return shell_level; }
void user_shell_set(int level)    { shell_level = level; }
