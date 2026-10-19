/* Accounts and the privileges they carry through a session. Passwords
   are kept as salted PBKDF2 hashes in a kernel-owned file. */

#include "include/user.h"
#include "include/config.h"
#include "include/fs.h"
#include "include/string.h"
#include "include/pit.h"
#include "include/task.h"
#define THREADOS_UAPI_H
#include "../user/usha1.h"
#undef THREADOS_UAPI_H

#ifdef __x86_64__
#include "../kernel64/task64.h"
#define current_level task64_level
#define current_uid task64_uid
#define set_current_uid task64_set_uid
#define set_current_level task64_set_level
#define set_current_cwd task64_set_cwd
#define set_shell_identity task64_set_identity_name
#define set_shell_level task64_set_level_name
#define kill_uid task64_kill_uid
#else
#define current_level task_level
#define current_uid task_uid
#define set_current_uid task_set_uid
#define set_current_level task_set_level
#define set_current_cwd task_set_cwd
#define set_shell_identity task_set_identity_name
#define set_shell_level task_set_level_name
#define kill_uid task_kill_uid
#endif

#define USER_DB_FILE ".users"
#define USER_DB_MAGIC "THRUSR2"
#define USER_DB_MAGIC_V1 "THRUSR1"
#define USER_PASSWORD_MIN 6
#define USER_HASH_LEGACY 1
#define USER_HASH_PBKDF2 2
#define USER_HASH_ROUNDS 4096

struct account {
    char name[USER_NAME_MAX + 1];
    char display_name[USER_DISPLAY_NAME_MAX + 1];
    uint8_t uid;
    uint8_t level;
    uint8_t hash_kind;
    uint8_t used;
    uint8_t salt[16];
    uint8_t hash[20];
} __attribute__((packed));

struct user_db {
    char magic[8];
    uint8_t next_uid;
    uint8_t reserved[3];
    struct account account[USER_ACCOUNT_MAX];
} __attribute__((packed));

struct account_v1 {
    char name[USER_NAME_MAX + 1];
    uint8_t uid;
    uint8_t level;
    uint8_t hash_kind;
    uint8_t used;
    uint8_t salt[16];
    uint8_t hash[20];
} __attribute__((packed));

struct user_db_v1 {
    char magic[8];
    uint8_t next_uid;
    uint8_t reserved[3];
    struct account_v1 account[USER_ACCOUNT_MAX];
} __attribute__((packed));

static struct user_db db;
static int db_loaded;
static int setup_pending;
static int setup_enforced;
static int shell_level = USER_PLAIN;
static int shell_uid = 127;       /* no account owns this login-screen identity */
static int wrong;
static uint32_t locked_until;
static char current_name[USER_NAME_MAX + 1] = "guest";
static char current_display_name[USER_DISPLAY_NAME_MAX + 1] = "Guest";

static uint64_t legacy_hash(const char *password)
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

static uint64_t legacy_value(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

static int password_length(const char *s)
{
    int n = 0;
    while (n <= USER_PASSWORD_MAX && s[n])
        n++;
    return n;
}

static int account_name_ok(const char *s)
{
    int n = 0;
    if (!s || !*s)
        return 0;
    for (; *s; s++, n++) {
        if (n >= USER_NAME_MAX ||
            !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
            return 0;
    }
    return n > 0;
}

static int display_name_ok(const char *s)
{
    int n = 0;
    if (!s || !*s)
        return 0;
    for (; *s; s++, n++)
        if (n >= USER_DISPLAY_NAME_MAX || *s < 0x20 || *s > 0x7E)
            return 0;
    return s[-1] != ' ';
}

static int account_index(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < USER_ACCOUNT_MAX; i++)
        if (db.account[i].used && !strcmp(db.account[i].name, name))
            return i;
    return -1;
}

static int account_uid(int uid)
{
    for (int i = 0; i < USER_ACCOUNT_MAX; i++)
        if (db.account[i].used && db.account[i].uid == uid)
            return i;
    return -1;
}

static int db_valid(const struct user_db *saved)
{
    if (memcmp(saved->magic, USER_DB_MAGIC, sizeof(USER_DB_MAGIC)) ||
        saved->next_uid == 0 || saved->next_uid > 127)
        return 0;
    int admins = 0;
    int root_admin = 0;
    for (int i = 0; i < USER_ACCOUNT_MAX; i++) {
        const struct account *a = &saved->account[i];
        if (!a->used)
            continue;
        int n = 0;
        while (n <= USER_NAME_MAX && a->name[n]) n++;
        if (n > USER_NAME_MAX || !account_name_ok(a->name) ||
            !display_name_ok(a->display_name) ||
            a->uid >= saved->next_uid ||
            (a->level != USER_ADMIN && a->level != USER_PLAIN) ||
            (a->hash_kind != USER_HASH_LEGACY &&
             a->hash_kind != USER_HASH_PBKDF2))
            return 0;
        admins += a->level == USER_ADMIN;
        if (a->uid == 0 && a->level == USER_ADMIN)
            root_admin = 1;
        for (int j = 0; j < i; j++) {
            const struct account *b = &saved->account[j];
            if (b->used && (a->uid == b->uid || !strcmp(a->name, b->name)))
                return 0;
        }
    }
    return admins > 0 && root_admin;
}

static int db_migrate_v1(const struct user_db_v1 *old, struct user_db *fresh)
{
    if (memcmp(old->magic, USER_DB_MAGIC_V1, sizeof(USER_DB_MAGIC_V1)) ||
        old->next_uid == 0 || old->next_uid > 127)
        return 0;
    memset(fresh, 0, sizeof(*fresh));
    memcpy(fresh->magic, USER_DB_MAGIC, sizeof(USER_DB_MAGIC));
    fresh->next_uid = old->next_uid;
    memcpy(fresh->reserved, old->reserved, sizeof(fresh->reserved));
    int admins = 0, root_admin = 0;
    for (int i = 0; i < USER_ACCOUNT_MAX; i++) {
        const struct account_v1 *src = &old->account[i];
        struct account *dst = &fresh->account[i];
        if (!src->used)
            continue;
        int n = 0;
        while (n <= USER_NAME_MAX && src->name[n])
            n++;
        if (n > USER_NAME_MAX || !account_name_ok(src->name) ||
            src->uid >= old->next_uid ||
            (src->level != USER_ADMIN && src->level != USER_PLAIN) ||
            (src->hash_kind != USER_HASH_LEGACY &&
             src->hash_kind != USER_HASH_PBKDF2))
            return 0;
        memcpy(dst->name, src->name, sizeof(dst->name));
        memcpy(dst->display_name, src->name, n + 1);
        dst->uid = src->uid;
        dst->level = src->level;
        dst->hash_kind = src->hash_kind;
        dst->used = src->used;
        memcpy(dst->salt, src->salt, sizeof(dst->salt));
        memcpy(dst->hash, src->hash, sizeof(dst->hash));
        admins += src->level == USER_ADMIN;
        if (src->uid == 0 && src->level == USER_ADMIN)
            root_admin = 1;
        for (int j = 0; j < i; j++)
            if (fresh->account[j].used &&
                (dst->uid == fresh->account[j].uid ||
                 !strcmp(dst->name, fresh->account[j].name)))
                return 0;
    }
    return admins > 0 && root_admin;
}

static void salt_for(struct account *a)
{
    uint32_t tick = pit_ticks();
    uint32_t mix = 2166136261u ^ ((uint32_t)a->uid << 24);
    for (const char *p = a->name; *p; p++)
        mix = (mix ^ (uint8_t)*p) * 16777619u;
    for (int i = 0; i < (int)sizeof(a->salt); i++) {
        tick = tick * 1664525u + 1013904223u + (uint32_t)i;
        a->salt[i] = (uint8_t)((tick >> ((i & 3) * 8)) ^
                               (mix >> ((i & 3) * 8)));
    }
}

static void set_hash(struct account *a, const char *password)
{
    int n = password_length(password);
    salt_for(a);
    pbkdf2_sha1((const uint8_t *)password, (uint32_t)n,
                a->salt, sizeof(a->salt), USER_HASH_ROUNDS,
                a->hash, sizeof(a->hash));
    a->hash_kind = USER_HASH_PBKDF2;
}

static int hash_matches(const struct account *a, const char *password)
{
    uint8_t got[20];
    if (a->hash_kind == USER_HASH_LEGACY) {
        uint64_t h = legacy_hash(password);
        uint8_t diff = 0;
        for (int i = 0; i < 8; i++)
            diff |= (uint8_t)(a->hash[i] ^ (uint8_t)(h >> (i * 8)));
        return diff == 0;
    }
    if (a->hash_kind != USER_HASH_PBKDF2)
        return 0;
    pbkdf2_sha1((const uint8_t *)password, (uint32_t)password_length(password),
                a->salt, sizeof(a->salt), USER_HASH_ROUNDS, got, sizeof(got));
    uint8_t diff = 0;
    for (int i = 0; i < (int)sizeof(got); i++)
        diff |= got[i] ^ a->hash[i];
    return diff == 0;
}

static int config_set_system(const char *key, const char *value)
{
    int old_level = current_level();
    int old_uid = current_uid();
    set_current_uid(0);
    set_current_level(USER_ADMIN);
    int rc = config_set(key, value);
    set_current_level(old_level);
    set_current_uid(old_uid);
    return rc;
}

static int unlink_system_file(const char *path)
{
    int old_level = current_level();
    int old_uid = current_uid();
    set_current_uid(0);
    set_current_level(USER_ADMIN);
    int rc = fs_unlink(FS_ROOT, path);
    set_current_level(old_level);
    set_current_uid(old_uid);
    return rc;
}

static int db_save(void)
{
    if (!fs_ready())
        return -1;
    /* The account database is root-owned. Changing one's own password is
       allowed from an ordinary session, so make this narrow kernel write
       privileged and restore the caller's credentials before returning. */
    int old_level = current_level();
    int old_uid = current_uid();
    set_current_uid(0);
    set_current_level(USER_ADMIN);
    int slot = fs_open(FS_ROOT, USER_DB_FILE, 1);
    int rc = slot < 0 ||
             fs_write_at(slot, 0, &db, sizeof(db)) != (int)sizeof(db)
                 ? -1 : 0;
    set_current_level(old_level);
    set_current_uid(old_uid);
    if (rc == 0)
        setup_pending = 0;
    return rc;
}

static void forget_legacy_password(void)
{
    /* Old releases kept a fast password hash in the public settings
       file. The account database now owns that credential. */
    (void)config_set_system("password", "");
}

static int ensure_home(const struct account *a)
{
    int slot;
    if (!fs_ready())
        return -1;
    int old_level = current_level();
    int old_uid = current_uid();
    set_current_uid(0);
    set_current_level(USER_ADMIN);
    if (fs_chdir(FS_ROOT, "/home", &slot) < 0 &&
        fs_mkdir(FS_ROOT, "/home") < 0)
        goto fail;
    char path[32] = "/home/";
    int at = 6;
    for (const char *p = a->name; *p && at < (int)sizeof(path) - 1; p++)
        path[at++] = *p;
    path[at] = 0;
    if (fs_chdir(FS_ROOT, path, &slot) < 0) {
        if (fs_mkdir(FS_ROOT, path) < 0)
            goto fail;
        if (fs_chdir(FS_ROOT, path, &slot) < 0)
            goto fail;
    }
    if (fs_set_owner(slot, a->uid) < 0)
        goto fail;
    static const char *const home_dirs[] = {"Desktop", ".Trash"};
    for (int i = 0; i < (int)(sizeof(home_dirs) / sizeof(home_dirs[0])); i++) {
        char child[48];
        int p = 0;
        while (path[p] && p < (int)sizeof(child) - 2) {
            child[p] = path[p];
            p++;
        }
        child[p++] = '/';
        for (int j = 0; home_dirs[i][j] && p < (int)sizeof(child) - 1; j++)
            child[p++] = home_dirs[i][j];
        child[p] = '\0';
        if (fs_chdir(FS_ROOT, child, &slot) < 0) {
            if (fs_mkdir(FS_ROOT, child) < 0 ||
                fs_chdir(FS_ROOT, child, &slot) < 0)
                goto fail;
        }
        if (fs_set_owner(slot, a->uid) < 0)
            goto fail;
    }
    set_current_level(old_level);
    set_current_uid(old_uid);
    return 0;

fail:
    set_current_level(old_level);
    set_current_uid(old_uid);
    return -1;
}

static void default_admin(void)
{
    memset(&db, 0, sizeof(db));
    memcpy(db.magic, USER_DB_MAGIC, sizeof(USER_DB_MAGIC));
    db.next_uid = 1;
    struct account *a = &db.account[0];
    memcpy(a->name, "admin", 6);
    memcpy(a->display_name, "admin", 6);
    a->uid = 0;
    a->level = USER_ADMIN;
    a->hash_kind = USER_HASH_LEGACY;
    a->used = 1;
    uint64_t h = legacy_value(config_get("password"));
    for (int i = 0; i < 8; i++)
        a->hash[i] = (uint8_t)(h >> (i * 8));
}

void user_accounts_load(void)
{
    if (db_loaded)
        return;
    db_loaded = 1;
    default_admin();
    if (!fs_ready())
        return;
    int slot = fs_open(FS_ROOT, USER_DB_FILE, 0);
    if (slot >= 0) {
        struct user_db saved;
        int n = fs_read_at(slot, 0, &saved, sizeof(saved));
        if (n == (int)sizeof(saved) && db_valid(&saved)) {
            db = saved;
            for (int i = 0; i < USER_ACCOUNT_MAX; i++)
                if (db.account[i].used)
                    ensure_home(&db.account[i]);
            forget_legacy_password();
            return;
        }
        struct user_db_v1 old;
        if (n == (int)sizeof(old) &&
            fs_read_at(slot, 0, &old, sizeof(old)) == (int)sizeof(old) &&
            db_migrate_v1(&old, &saved)) {
            db = saved;
            if (db_save() == 0)
                forget_legacy_password();
            for (int i = 0; i < USER_ACCOUNT_MAX; i++)
                if (db.account[i].used)
                    ensure_home(&db.account[i]);
            return;
        }
        /* Never fall back to the bootstrap credential when an account
           database exists but cannot be trusted. */
        memset(&db, 0, sizeof(db));
        return;                         /* do not replace an unreadable database */
    }
    /* fs_open also fails when an entry with this name exists but is a
       directory. Treat that as a damaged account database, not as a fresh
       installation that may accept the legacy bootstrap password. */
    int unexpected_dir;
    if (fs_chdir(FS_ROOT, USER_DB_FILE, &unexpected_dir) == 0) {
        memset(&db, 0, sizeof(db));
        return;
    }
    int legacy_settings = fs_open(FS_ROOT, CONFIG_FILE, 0);
    if (legacy_settings < 0) {
        setup_pending = 1;
        return;
    }
    if (db_save() == 0) {               /* migrate the legacy admin credential */
        forget_legacy_password();
    }
    ensure_home(&db.account[0]);
}

int user_setup_required(void)
{
    user_accounts_load();
    setup_enforced = 1;
    return setup_pending;
}

int user_account_setup(const char *name, const char *display_name,
                       const char *password, const char *theme)
{
    user_accounts_load();
    int plen = password ? password_length(password) : -1;
    if (!setup_pending || !fs_ready() || !account_name_ok(name) ||
        !display_name_ok(display_name) || plen < 0 ||
        (plen && plen < USER_PASSWORD_MIN) || plen > USER_PASSWORD_MAX ||
        !theme || (strcmp(theme, "dark") && strcmp(theme, "light")))
        return -1;
    const char *old_theme = config_get("theme");
    char saved_theme[CONFIG_VAL_MAX];
    int i = 0;
    while (old_theme && old_theme[i] && i < CONFIG_VAL_MAX - 1) {
        saved_theme[i] = old_theme[i];
        i++;
    }
    saved_theme[i] = 0;
    memset(&db, 0, sizeof(db));
    memcpy(db.magic, USER_DB_MAGIC, sizeof(USER_DB_MAGIC));
    db.next_uid = 1;
    struct account *a = &db.account[0];
    memcpy(a->name, name, strlen(name) + 1);
    memcpy(a->display_name, display_name, strlen(display_name) + 1);
    a->uid = 0;
    a->level = USER_ADMIN;
    a->used = 1;
    set_hash(a, password);
    if (ensure_home(a) < 0 || db_save() < 0) {
        (void)unlink_system_file(USER_DB_FILE);
        memset(&db, 0, sizeof(db));
        setup_pending = 1;
        return -1;
    }
    if (config_set_system("theme", theme) < 0) {
        (void)unlink_system_file(USER_DB_FILE);
        (void)config_set_system("theme", saved_theme);
        (void)unlink_system_file(CONFIG_FILE);
        memset(&db, 0, sizeof(db));
        setup_pending = 1;
        return -1;
    }
    forget_legacy_password();
    return user_login(name, password);
}

int user_auto_login(void)
{
    user_accounts_load();
    for (int i = 0; i < USER_ACCOUNT_MAX; i++)
        if (db.account[i].used && hash_matches(&db.account[i], ""))
            return user_login(db.account[i].name, "");
    return -1;
}

int user_current_passwordless(void)
{
    user_accounts_load();
    int i = account_uid(current_uid());
    return i >= 0 && hash_matches(&db.account[i], "");
}

static int attempt_allowed(void)
{
    uint32_t now = pit_ticks();
    if (locked_until && now < locked_until)
        return 0;
    if (locked_until)
        locked_until = 0;
    return 1;
}

static int password_try(const char *name, const char *password, int admin_only)
{
    user_accounts_load();
    if (setup_enforced && setup_pending)
        return -1;
    if (!attempt_allowed())
        return -1;
    int found = -1;
    for (int i = 0; i < USER_ACCOUNT_MAX; i++) {
        struct account *a = &db.account[i];
        if (!a->used || (name && strcmp(name, a->name)) ||
            (admin_only && a->level != USER_ADMIN))
            continue;
        if (hash_matches(a, password)) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        wrong = 0;
        struct account *a = &db.account[found];
        if (a->hash_kind == USER_HASH_LEGACY) {
            set_hash(a, password);
            db_save();
        }
        return found;
    }
    if (++wrong >= 3) {
        wrong = 0;
        locked_until = pit_ticks() + 500;
    }
    return -1;
}

int user_password_ok(const char *password)
{
    return password && password_try((const char *)0, password, 1) >= 0;
}

int user_reauth(const char *password)
{
    if (current_level() != USER_ADMIN || !password)
        return 0;
    user_accounts_load();
    int i = account_uid(current_uid());
    if (i >= 0 && db.account[i].level == USER_ADMIN)
        return user_password_current_ok(password);
    /* A plain account elevated with sudo has no admin credential of its
       own, so a sensitive account change still needs an administrator's. */
    return user_password_ok(password);
}

int user_password_current_ok(const char *password)
{
    user_accounts_load();
    if (!password || !attempt_allowed())
        return 0;
    int i = account_uid(current_uid());
    /* The text-only maintenance shell has no logged-in account. Once its
       admin session is authenticated, passwd can still rotate the admin
       credential instead of pretending the guest has an account. */
    if (i < 0 && current_level() == USER_ADMIN)
        return user_password_ok(password);
    if (i >= 0 && hash_matches(&db.account[i], password)) {
        wrong = 0;
        if (db.account[i].hash_kind == USER_HASH_LEGACY) {
            set_hash(&db.account[i], password);
            db_save();
        }
        return 1;
    }
    if (++wrong >= 3) {
        wrong = 0;
        locked_until = pit_ticks() + 500;
    }
    return 0;
}

int user_login(const char *name, const char *password)
{
    if (!name || !password)
        return -1;
    int i = password_try(name, password, 0);
    if (i < 0)
        return -1;
    struct account *a = &db.account[i];
    /* Login runs in kernel context, whose task UID can differ from the
       account session used to launch ring 3 programs. */
    int old_uid = shell_uid;
    int old_level = shell_level;
    set_current_uid(a->uid);
    set_current_level(a->level);
    char path[32] = "/home/";
    int at = 6;
    for (const char *p = a->name; *p && at < (int)sizeof(path) - 1; p++)
        path[at++] = *p;
    path[at] = 0;
    int home;
    if (fs_chdir(FS_ROOT, path, &home) < 0) {
        set_current_uid(old_uid);
        set_current_level(old_level);
        return -1;
    }
    if (old_uid != a->uid && old_uid != 127)
        kill_uid(old_uid);
    shell_uid = a->uid;
    shell_level = a->level;
    set_current_cwd(home);
    set_shell_identity("shell", a->uid, a->level, home);
    set_shell_identity("main", a->uid, a->level, home);
    set_shell_identity("desktop", a->uid, a->level, home);
    memcpy(current_name, a->name, sizeof(current_name));
    memcpy(current_display_name, a->display_name,
           sizeof(current_display_name));
    return 0;
}

void user_session_guest(void)
{
    shell_uid = 127;
    shell_level = USER_PLAIN;
    memcpy(current_name, "guest", 6);
    memcpy(current_display_name, "Guest", 6);
    set_current_uid(shell_uid);
    set_current_level(USER_PLAIN);
    set_current_cwd(FS_ROOT);
    set_shell_identity("shell", shell_uid, USER_PLAIN, FS_ROOT);
    set_shell_identity("main", shell_uid, USER_PLAIN, FS_ROOT);
    set_shell_identity("desktop", shell_uid, USER_PLAIN, FS_ROOT);
}

int user_account_count(void)
{
    user_accounts_load();
    int n = 0;
    for (int i = 0; i < USER_ACCOUNT_MAX; i++)
        n += !!db.account[i].used;
    return n;
}

int user_account_info(int index, char *name, int name_size,
                      int *uid, int *level)
{
    user_accounts_load();
    for (int i = 0; i < USER_ACCOUNT_MAX; i++) {
        struct account *a = &db.account[i];
        if (!a->used)
            continue;
        if (index--)
            continue;
        if (!name || name_size <= (int)strlen(a->name))
            return -1;
        memcpy(name, a->name, strlen(a->name) + 1);
        if (uid) *uid = a->uid;
        if (level) *level = a->level;
        return 0;
    }
    return -1;
}

int user_account_add(const char *name, const char *password, int level,
                     const char *admin_password)
{
    user_accounts_load();
    if (current_level() != USER_ADMIN || !account_name_ok(name) ||
        !password || password_length(password) < USER_PASSWORD_MIN ||
        password_length(password) > USER_PASSWORD_MAX ||
        (level != USER_ADMIN && level != USER_PLAIN) ||
        account_index(name) >= 0 || db.next_uid >= 127 ||
        !user_reauth(admin_password))
        return -1;
    int ai = -1;
    for (int i = 0; i < USER_ACCOUNT_MAX; i++)
        if (!db.account[i].used) { ai = i; break; }
    if (ai < 0)
        return -1;

    int home_parent;
    if (fs_chdir(FS_ROOT, "/home", &home_parent) < 0 &&
        fs_mkdir(FS_ROOT, "/home") < 0)
        return -1;
    char path[32] = "/home/";
    int at = 6;
    for (const char *p = name; *p; p++) path[at++] = *p;
    path[at] = 0;
    if (fs_mkdir(FS_ROOT, path) < 0)
        return -1;
    int home_slot;
    if (fs_chdir(FS_ROOT, path, &home_slot) < 0) {
        fs_unlink(FS_ROOT, path);
        return -1;
    }

    struct account *a = &db.account[ai];
    memset(a, 0, sizeof(*a));
    memcpy(a->name, name, strlen(name) + 1);
    memcpy(a->display_name, name, strlen(name) + 1);
    a->uid = db.next_uid++;
    a->level = (uint8_t)level;
    a->used = 1;
    set_hash(a, password);
    if (fs_set_owner(home_slot, a->uid) < 0 || db_save() < 0) {
        memset(a, 0, sizeof(*a));
        db.next_uid--;
        fs_unlink(FS_ROOT, path);
        return -1;
    }
    return 0;
}

int user_account_delete(const char *name, const char *admin_password)
{
    user_accounts_load();
    if (current_level() != USER_ADMIN || !name ||
        !user_reauth(admin_password))
        return -1;
    int i = account_index(name);
    if (i < 0 || db.account[i].uid == 0 ||
        db.account[i].uid == current_uid())
        return -1;
    struct account old = db.account[i];
    char home[32] = "/home/";
    char archive[32] = "/home/.deleted-";
    int at = 6;
    for (const char *p = old.name; *p; p++)
        home[at++] = *p;
    home[at] = 0;
    at = 15;
    uint8_t uid = old.uid;
    char digits[4];
    int nd = 0;
    do { digits[nd++] = (char)('0' + uid % 10); uid /= 10; } while (uid);
    while (nd)
        archive[at++] = digits[--nd];
    archive[at] = 0;
    int home_slot;
    int archived = fs_chdir(FS_ROOT, home, &home_slot) == 0;
    if (archived && fs_rename(FS_ROOT, home, archive) < 0)
        return -1;
    memset(&db.account[i], 0, sizeof(db.account[i]));
    if (db_save() < 0) {
        db.account[i] = old;
        if (archived)
            fs_rename(FS_ROOT, archive, home);
        return -1;
    }
    kill_uid(old.uid);
    return 0;
}

int user_account_set_level(const char *name, int level,
                           const char *admin_password)
{
    user_accounts_load();
    if (current_level() != USER_ADMIN || !name ||
        (level != USER_ADMIN && level != USER_PLAIN) ||
        !user_reauth(admin_password))
        return -1;
    int i = account_index(name);
    if (i < 0 || db.account[i].uid == 0 ||
        db.account[i].uid == current_uid())
        return -1;
    int admins = 0;
    for (int j = 0; j < USER_ACCOUNT_MAX; j++)
        if (db.account[j].used && db.account[j].level == USER_ADMIN)
            admins++;
    if (db.account[i].level == USER_ADMIN && level == USER_PLAIN && admins <= 1)
        return -1;
    uint8_t old = db.account[i].level;
    db.account[i].level = (uint8_t)level;
    if (db_save() < 0) {
        db.account[i].level = old;
        return -1;
    }
    if (old != level)
        kill_uid(db.account[i].uid);
    return 0;
}

static int set_account_password(struct account *a, const char *password)
{
    if (!a || !password || password_length(password) < USER_PASSWORD_MIN ||
        password_length(password) > USER_PASSWORD_MAX)
        return -1;
    struct account old = *a;
    set_hash(a, password);
    if (db_save() < 0) {
        *a = old;
        return -1;
    }
    return 0;
}

int user_set_password(const char *password)
{
    user_accounts_load();
    int i = account_uid(current_uid());
    if (i < 0 && current_level() == USER_ADMIN)
        i = account_uid(0);
    return i < 0 ? -1 : set_account_password(&db.account[i], password);
}

int user_set_password_for(const char *name, const char *password,
                          const char *admin_password)
{
    user_accounts_load();
    if (current_level() != USER_ADMIN || !user_reauth(admin_password))
        return -1;
    int i = account_index(name);
    if (i < 0 || set_account_password(&db.account[i], password) < 0)
        return -1;
    if (db.account[i].uid != current_uid())
        kill_uid(db.account[i].uid);
    return 0;
}

int user_is_system_file(const char *name)
{
    static const char *files[] = { "system.cfg", "swap.sys", "icons.pos",
                                   "update.key", USER_DB_FILE };
    for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++)
        if (!strcmp(name, files[i]))
            return 1;
    return 0;
}

int user_shell_level(void)       { return shell_level; }
void user_shell_set(int level)
{
    shell_level = level;
    set_shell_level("shell", level);
}
int user_shell_uid(void)         { return shell_uid; }
void user_shell_set_uid(int uid) { if (uid >= 0) shell_uid = uid; }
const char *user_current_name(void) { return current_name; }
const char *user_current_display_name(void) { return current_display_name; }
