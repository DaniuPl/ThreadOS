/* glibtest - GLib on ThreadOS
 *
 * GLib is what WebKit's processes are built on: their main loop, their
 * threads, their objects and signals, their strings and data. This
 * goes through the parts they lean on, with output that does not
 * depend on the machine, so scripts/tests/t_glib.py can build it a
 * second time on the test machine against that machine's GLib and
 * hold the two to each other line for line. Lines that start with '~'
 * are measurements, which only ThreadOS's run is held to.
 *
 *   the main loop: idle callbacks before timeouts, timeouts in the
 *   order of their times, a pipe and a socket pair watched (poll
 *   underneath), a callback sent from another thread waking the loop
 *
 *   threads: a queue four threads feed, a pool of workers, a mutex and
 *   a condition, once-only initialization
 *
 *   GObject: a class of our own with a property and a signal whose
 *   arguments go through the generic marshaller (libffi)
 *
 *   data: hash tables, strings, sorting, UTF-8 and Unicode, regular
 *   expressions (PCRE2), GVariant, key files, dates, checksums, base64,
 *   and GIO's streams and files
 *
 *   processes, the way WebKit starts its own: a child spawned with its
 *   standard input and output as pipes and one more descriptor handed
 *   to it at a number of our choosing, its end reported to the main
 *   loop; and through GSubprocess with a socket pair's end given to the
 *   child as descriptor 3 - spawning has to return at once, not when
 *   the child ends, which needs descriptors closed on exec
 *
 * "glibtest --no-disk" leaves out the one part that writes a file, for
 * a system with no disk to write (the ARM64 kernel so far,
 * scripts/tests/t_arm64libs.py).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <glib.h>
#include <glib-object.h>

static int no_disk;
#include <glib-unix.h>
#include <gio/gio.h>

static gint64 now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

/* --- the main loop ---------------------------------------------------- */

static GString *order;
static GMainLoop *loop;

static gboolean on_timeout(gpointer data)
{
    g_string_append_printf(order, " t%d", GPOINTER_TO_INT(data));
    if (GPOINTER_TO_INT(data) == 150)
        g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_idle(gpointer data)
{
    g_string_append_printf(order, " i%d", GPOINTER_TO_INT(data));
    return G_SOURCE_REMOVE;
}

static int pipe_fds[2];

static gpointer write_later(gpointer data)
{
    g_usleep(80 * 1000);
    if (write(GPOINTER_TO_INT(data), "ping", 4) != 4)
        g_printerr("write failed\n");
    return NULL;
}

static gboolean on_readable(gint fd, GIOCondition cond, gpointer data)
{
    char buf[16] = { 0 };
    gssize n = read(fd, buf, sizeof(buf) - 1);
    g_string_append_printf(order, " %s:%s(%d)%s", (const char *)data, buf,
                           (int)n, cond & G_IO_IN ? "" : "?");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean from_thread_cb(gpointer data)
{
    gint64 *sent = data;
    g_string_append_printf(order, " invoked");
    *sent = now_ms() - *sent;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gpointer invoke_later(gpointer data)
{
    g_usleep(60 * 1000);
    *(gint64 *)data = now_ms();
    g_main_context_invoke(NULL, from_thread_cb, data);
    return NULL;
}

static void main_loop(void)
{
    order = g_string_new("");
    loop = g_main_loop_new(NULL, FALSE);
    /* 50 ms apart: ThreadOS's clock moves in 10 ms ticks, and two
       timeouts due in the same tick run in the order they were added */
    g_timeout_add(150, on_timeout, GINT_TO_POINTER(150));
    g_timeout_add(50, on_timeout, GINT_TO_POINTER(50));
    g_timeout_add(100, on_timeout, GINT_TO_POINTER(100));
    g_idle_add(on_idle, GINT_TO_POINTER(1));
    g_idle_add(on_idle, GINT_TO_POINTER(2));
    g_main_loop_run(loop);
    printf("idle and timeouts ran in this order:%s\n", order->str);

    /* a pipe a thread writes into, watched by the loop */
    g_string_truncate(order, 0);
    if (!g_unix_open_pipe(pipe_fds, O_CLOEXEC | O_NONBLOCK, NULL))
        printf("g_unix_open_pipe failed\n");
    g_unix_fd_add(pipe_fds[0], G_IO_IN, on_readable, "pipe");
    gint64 t0 = now_ms();
    GThread *t = g_thread_new("writer", write_later,
                              GINT_TO_POINTER(pipe_fds[1]));
    g_main_loop_run(loop);
    gint64 took = now_ms() - t0;
    g_thread_join(t);
    printf("a watched pipe, written by a thread:%s\n", order->str);
    printf("the loop slept until then: %s\n",
           took >= 70 && took < 1000 ? "yes" : "no");
    printf("~pipe watch woke after %" G_GINT64_FORMAT " ms (written at 80)\n",
           took);
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    /* the same with a socket pair */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    g_string_truncate(order, 0);
    g_unix_fd_add(sv[1], G_IO_IN, on_readable, "socket");
    t = g_thread_new("writer", write_later, GINT_TO_POINTER(sv[0]));
    g_main_loop_run(loop);
    g_thread_join(t);
    printf("a watched socket pair, written by a thread:%s\n", order->str);
    close(sv[0]);
    close(sv[1]);

    /* a callback handed to the loop from another thread */
    g_string_truncate(order, 0);
    gint64 sent = 0;
    t = g_thread_new("invoker", invoke_later, &sent);
    g_main_loop_run(loop);
    g_thread_join(t);
    printf("a callback sent from a thread ran in the loop:%s\n", order->str);
    printf("soon after it was sent: %s\n", sent < 500 ? "yes" : "no");
    printf("~the loop woke: %" G_GINT64_FORMAT " ms after the send\n", sent);
    g_main_loop_unref(loop);
    g_string_free(order, TRUE);
}

/* --- threads ------------------------------------------------------------ */

static gpointer feed(gpointer q)
{
    for (int i = 1; i <= 250; i++)
        g_async_queue_push(q, GINT_TO_POINTER(i));
    return NULL;
}

static GMutex pool_lock;
static long pool_sum;

static void square(gpointer data, gpointer user)
{
    (void)user;
    long v = GPOINTER_TO_INT(data);
    g_mutex_lock(&pool_lock);
    pool_sum += v * v;
    g_mutex_unlock(&pool_lock);
}

static GMutex m;
static GCond c;
static int ready_flag;

static gpointer signal_ready(gpointer data)
{
    (void)data;
    g_usleep(20 * 1000);
    g_mutex_lock(&m);
    ready_flag = 1;
    g_cond_signal(&c);
    g_mutex_unlock(&m);
    return NULL;
}

static gsize once_value;

static void threads(void)
{
    GAsyncQueue *q = g_async_queue_new();
    GThread *th[4];
    for (int i = 0; i < 4; i++)
        th[i] = g_thread_new("feed", feed, q);
    long sum = 0;
    for (int i = 0; i < 1000; i++)
        sum += GPOINTER_TO_INT(g_async_queue_pop(q));
    for (int i = 0; i < 4; i++)
        g_thread_join(th[i]);
    printf("four threads fed a queue: %ld (want %d)\n", sum, 4 * 250 * 251 / 2);
    g_async_queue_unref(q);

    GThreadPool *pool = g_thread_pool_new(square, NULL, 4, FALSE, NULL);
    for (int i = 1; i <= 200; i++)
        g_thread_pool_push(pool, GINT_TO_POINTER(i), NULL);
    g_thread_pool_free(pool, FALSE, TRUE);
    printf("a pool of four squared 1..200: %ld\n", pool_sum);

    GThread *t = g_thread_new("ready", signal_ready, NULL);
    g_mutex_lock(&m);
    while (!ready_flag)
        g_cond_wait(&c, &m);
    g_mutex_unlock(&m);
    g_thread_join(t);
    printf("a condition variable woke the waiter: %s\n",
           ready_flag ? "yes" : "no");

    for (int i = 0; i < 3; i++)
        if (g_once_init_enter(&once_value))
            g_once_init_leave(&once_value, 42 + i);
    printf("once-only initialization: %" G_GSIZE_FORMAT "\n", once_value);
}

/* --- GObject -------------------------------------------------------------- */

#define TEST_TYPE_COUNTER (test_counter_get_type())
G_DECLARE_FINAL_TYPE(TestCounter, test_counter, TEST, COUNTER, GObject)

struct _TestCounter {
    GObject parent;
    int value;
    char *label;
};

G_DEFINE_TYPE(TestCounter, test_counter, G_TYPE_OBJECT)

enum { PROP_0, PROP_VALUE, PROP_LABEL, N_PROPS };
static GParamSpec *props[N_PROPS];
static guint changed_signal;

static void test_counter_set_property(GObject *o, guint id, const GValue *v,
                                      GParamSpec *ps)
{
    TestCounter *self = TEST_COUNTER(o);
    switch (id) {
    case PROP_VALUE:
        self->value = g_value_get_int(v);
        break;
    case PROP_LABEL:
        g_free(self->label);
        self->label = g_value_dup_string(v);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void test_counter_get_property(GObject *o, guint id, GValue *v,
                                      GParamSpec *ps)
{
    TestCounter *self = TEST_COUNTER(o);
    switch (id) {
    case PROP_VALUE:
        g_value_set_int(v, self->value);
        break;
    case PROP_LABEL:
        g_value_set_string(v, self->label);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void test_counter_finalize(GObject *o)
{
    g_free(TEST_COUNTER(o)->label);
    G_OBJECT_CLASS(test_counter_parent_class)->finalize(o);
}

static void test_counter_class_init(TestCounterClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->set_property = test_counter_set_property;
    oc->get_property = test_counter_get_property;
    oc->finalize = test_counter_finalize;
    props[PROP_VALUE] = g_param_spec_int("value", NULL, NULL, -1000, 1000, 0,
                                         G_PARAM_READWRITE);
    props[PROP_LABEL] = g_param_spec_string("label", NULL, NULL, "none",
                                            G_PARAM_READWRITE);
    g_object_class_install_properties(oc, N_PROPS, props);
    /* no marshaller given: the generic one, through libffi */
    changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_INT, 3, G_TYPE_INT, G_TYPE_STRING,
                                  G_TYPE_DOUBLE);
}

static void test_counter_init(TestCounter *self)
{
    self->label = g_strdup("none");
}

static int on_changed(TestCounter *c, int by, const char *why, double scale,
                      gpointer user)
{
    GString *log = user;
    g_string_append_printf(log, " changed(%d,%s,%.2f)", by, why, scale);
    return c->value + by;
}

static void on_notify(GObject *o, GParamSpec *ps, gpointer user)
{
    (void)o;
    g_string_append_printf(user, " notify::%s", ps->name);
}

static int finalized;

static void on_finalized(gpointer data, GObject *where)
{
    (void)data;
    (void)where;
    finalized = 1;
}

static void objects(void)
{
    GString *log = g_string_new("");
    TestCounter *c = g_object_new(TEST_TYPE_COUNTER, "value", 5,
                                  "label", "first", NULL);
    g_signal_connect(c, "changed", G_CALLBACK(on_changed), log);
    g_signal_connect(c, "notify", G_CALLBACK(on_notify), log);
    g_object_weak_ref(G_OBJECT(c), on_finalized, NULL);
    int result = 0;
    g_signal_emit(c, changed_signal, 0, 7, "seven", 1.5, &result);
    g_object_set(c, "value", 12, "label", "second", NULL);
    int value;
    char *label;
    g_object_get(c, "value", &value, "label", &label, NULL);
    printf("a class of our own: %s, value %d, label %s\n",
           G_OBJECT_TYPE_NAME(c), value, label);
    printf("its signal through the generic marshaller returned %d;"
           " and the log:%s\n", result, log->str);
    g_free(label);
    g_object_unref(c);
    printf("the last reference gone, it was finalized: %s\n",
           finalized ? "yes" : "no");
    g_string_free(log, TRUE);
}

/* --- data ------------------------------------------------------------------ */

static gint by_length(gconstpointer a, gconstpointer b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int d = (int)strlen(x) - (int)strlen(y);
    return d ? d : strcmp(x, y);
}

static gboolean upper_word(const GMatchInfo *mi, GString *out, gpointer user)
{
    (void)user;
    char *w = g_match_info_fetch_named(mi, "word");
    char *up = g_utf8_strup(w, -1);
    g_string_append(out, up);
    g_free(up);
    g_free(w);
    return FALSE;
}

static void data(void)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < 1000; i++)
        g_hash_table_insert(h, g_strdup_printf("key%d", i), GINT_TO_POINTER(i));
    long sum = 0;
    for (int i = 0; i < 1000; i += 7) {
        char k[16];
        snprintf(k, sizeof(k), "key%d", i);
        sum += GPOINTER_TO_INT(g_hash_table_lookup(h, k));
    }
    printf("a hash table of 1000: size %u, every 7th key summed %ld\n",
           g_hash_table_size(h), sum);
    g_hash_table_destroy(h);

    char **parts = g_strsplit("pear,fig,apple,kiwi,banana,date", ",", -1);
    GPtrArray *a = g_ptr_array_new();
    for (char **p = parts; *p; p++)
        g_ptr_array_add(a, *p);
    g_ptr_array_sort(a, by_length);
    g_ptr_array_add(a, NULL);
    char *joined = g_strjoinv(" ", (char **)a->pdata);
    printf("split, sorted by length, joined: %s\n", joined);
    g_free(joined);
    g_ptr_array_free(a, TRUE);
    g_strfreev(parts);

    const char *pl = "Zażółć gęślą jaźń";
    char *up = g_utf8_strup(pl, -1);
    char *fold = g_utf8_casefold(pl, -1);
    char *rev = g_utf8_strreverse(pl, -1);
    printf("UTF-8: %ld characters, %zu bytes; upper %s; reversed %s\n",
           g_utf8_strlen(pl, -1), strlen(pl), up, rev);
    printf("case-folded: %s; valid: %s\n", fold,
           g_utf8_validate(pl, -1, NULL) ? "yes" : "no");
    g_free(up);
    g_free(fold);
    g_free(rev);
    char *nfd = g_utf8_normalize("é", -1, G_NORMALIZE_NFD);
    printf("é decomposed is %ld characters; U+0105 is %s, its upper U+%04X\n",
           g_utf8_strlen(nfd, -1),
           g_unichar_islower(0x105) ? "lower case" : "not lower case",
           g_unichar_toupper(0x105));
    g_free(nfd);

    GError *err = NULL;
    GRegex *re = g_regex_new("(?<word>\\p{L}+)-(?<num>\\d+)", 0, 0, &err);
    GMatchInfo *mi;
    GString *found = g_string_new("");
    g_regex_match(re, "alpha-1 beta-22 żółw-333 x-", 0, &mi);
    while (g_match_info_matches(mi)) {
        char *w = g_match_info_fetch_named(mi, "word");
        char *n = g_match_info_fetch_named(mi, "num");
        g_string_append_printf(found, " %s=%s", w, n);
        g_free(w);
        g_free(n);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    char *replaced = g_regex_replace_eval(re, "alpha-1 żółw-333", -1, 0, 0,
                                          upper_word, NULL, NULL);
    printf("a regular expression with named groups found:%s\n", found->str);
    printf("and replacing with a callback: %s\n", replaced);
    g_free(replaced);
    g_string_free(found, TRUE);
    g_regex_unref(re);

    GVariant *v = g_variant_new_parsed(
        "{'name': <'ThreadOS'>, 'cores': <@u 4>, 'ratio': <1.25>,"
        " 'list': <[1, 2, 3]>, 'pair': <(true, @y 7)>}");
    char *text = g_variant_print(v, TRUE);
    printf("a GVariant: %s\n", text);
    printf("its serialized size: %" G_GSIZE_FORMAT " bytes\n", g_variant_get_size(v));
    g_free(text);
    g_variant_unref(v);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_data(kf, "# settings\n[window]\nwidth=800\n"
                              "title=Hello\n[colours]\nlist=red;green;blue;\n",
                              -1, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_integer(kf, "window", "height", 600);
    gsize n;
    char **colours = g_key_file_get_string_list(kf, "colours", "list", &n, NULL);
    printf("a key file: width %d, %" G_GSIZE_FORMAT " colours, the last %s\n",
           g_key_file_get_integer(kf, "window", "width", NULL), n, colours[n - 1]);
    char *back = g_key_file_to_data(kf, NULL, NULL);
    g_strdelimit(back, "\n", '|');
    printf("written back: %s\n", back);
    g_free(back);
    g_strfreev(colours);
    g_key_file_free(kf);

    GDateTime *dt = g_date_time_new_from_unix_utc(1790000000);
    GDateTime *later = g_date_time_add_days(dt, 100);
    char *s1 = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S, day %j, week %V");
    char *s2 = g_date_time_format_iso8601(later);
    printf("a date: %s; 100 days on: %s\n", s1, s2);
    g_free(s1);
    g_free(s2);
    g_date_time_unref(dt);
    g_date_time_unref(later);

    char *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                              "ThreadOS", -1);
    char *hmac = g_compute_hmac_for_string(G_CHECKSUM_SHA1,
                                           (const guchar *)"key", 3,
                                           "message", -1);
    char *b64 = g_base64_encode((const guchar *)"Zażółć", strlen("Zażółć"));
    printf("SHA-256: %s\n", sha);
    printf("HMAC-SHA1: %s; base64: %s\n", hmac, b64);
    g_free(sha);
    g_free(hmac);
    g_free(b64);
}

/* --- GIO -------------------------------------------------------------------- */

static void gio(void)
{
    GInputStream *mem = g_memory_input_stream_new_from_data(
        "first line\nsecond line\r\nthird", -1, NULL);
    GDataInputStream *lines = g_data_input_stream_new(mem);
    g_data_input_stream_set_newline_type(lines, G_DATA_STREAM_NEWLINE_TYPE_ANY);
    char *line;
    int count = 0;
    GString *seen = g_string_new("");
    while ((line = g_data_input_stream_read_line(lines, NULL, NULL, NULL))) {
        g_string_append_printf(seen, " [%s]", line);
        g_free(line);
        count++;
    }
    printf("lines read from a memory stream (%d):%s\n", count, seen->str);
    g_string_free(seen, TRUE);
    g_object_unref(lines);
    g_object_unref(mem);

    GOutputStream *out = g_memory_output_stream_new_resizable();
    GDataOutputStream *d = g_data_output_stream_new(out);
    g_data_output_stream_set_byte_order(d, G_DATA_STREAM_BYTE_ORDER_BIG_ENDIAN);
    g_data_output_stream_put_uint32(d, 0xDEADBEEF, NULL, NULL);
    g_data_output_stream_put_string(d, "ok", NULL, NULL);
    g_output_stream_close(G_OUTPUT_STREAM(d), NULL, NULL);
    gsize size = g_memory_output_stream_get_data_size(
        G_MEMORY_OUTPUT_STREAM(out));
    const guchar *bytes = g_memory_output_stream_get_data(
        G_MEMORY_OUTPUT_STREAM(out));
    printf("a data stream wrote %" G_GSIZE_FORMAT " bytes: %02x %02x %02x %02x %c%c\n",
           size, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    g_object_unref(d);
    g_object_unref(out);

    if (no_disk)
        return;
    GError *err = NULL;
    const char *body = "written with g_file_set_contents\n";
    gboolean ok = g_file_set_contents("glibtest.txt", body, -1, &err);
    char *read_back = NULL;
    gsize len = 0;
    ok = ok && g_file_get_contents("glibtest.txt", &read_back, &len, &err);
    printf("a file written and read back: %s (%" G_GSIZE_FORMAT " bytes)\n",
           ok && !strcmp(read_back, body) ? "yes" : "no", len);
    g_free(read_back);
    if (err)
        printf("  error: %s\n", err->message);
    GFile *f = g_file_new_for_path("glibtest.txt");
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        0, NULL, NULL);
    printf("GFile says: size %" G_GOFFSET_FORMAT ", a regular file %s\n",
           info ? g_file_info_get_size(info) : -1,
           info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR
               ? "yes" : "no");
    if (info)
        g_object_unref(info);
    g_file_delete(f, NULL, NULL);
    printf("and once deleted it is gone: %s\n",
           g_file_query_exists(f, NULL) ? "no" : "yes");
    g_object_unref(f);
}

/* --- processes -------------------------------------------------------- */

/* The child: a line from standard input comes back in capitals; fd 3
   is told it ran */
static int child(void)
{
    char line[64] = { 0 };
    if (!fgets(line, sizeof(line), stdin))
        return 3;
    char *up = g_ascii_strup(line, -1);
    printf("%s", up);
    fflush(stdout);
    g_free(up);
    const char *note = "the child had fd 3";
    if (write(3, note, strlen(note)) < 0)
        return 4;
    return 7;
}

/* The child of GSubprocess: a question over the socket at fd 3, the
   answer back */
static int socket_child(void)
{
    char buf[64] = { 0 };
    ssize_t n = read(3, buf, sizeof(buf) - 1);
    if (n <= 0)
        return 5;
    char *reply = g_strdup_printf("answer to '%s'", buf);
    ssize_t w = write(3, reply, strlen(reply));
    g_free(reply);
    return w > 0 ? 0 : 6;
}

static int exit_status = -1;

static void child_ended(GPid pid, gint status, gpointer user)
{
    (void)user;
    GError *err = NULL;
    exit_status = g_spawn_check_wait_status(status, &err) ? 0
                  : err && err->domain == G_SPAWN_EXIT_ERROR ? err->code : -2;
    g_clear_error(&err);
    g_spawn_close_pid(pid);
    g_main_loop_quit(loop);
}

static void processes(const char *self)
{
    loop = g_main_loop_new(NULL, FALSE);
    int extra[2];
    if (!g_unix_open_pipe(extra, O_CLOEXEC, NULL))
        printf("g_unix_open_pipe failed\n");
    const char *argv[] = { self, "--child", NULL };
    int source[] = { extra[1] }, target[] = { 3 };
    GPid pid;
    int in_fd = -1, out_fd = -1;
    GError *err = NULL;
    gint64 t0 = now_ms();
    gboolean ok = g_spawn_async_with_pipes_and_fds(
        NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL, -1, -1, -1, source, target, 1, &pid, &in_fd, &out_fd,
        NULL, &err);
    gint64 spawned = now_ms() - t0;
    printf("a child spawned with pipes and fd 3: %s\n", ok ? "yes" : "no");
    if (!ok) {
        printf("  (%s)\n", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    printf("and spawning returned before the child ended: %s\n",
           spawned < 2000 ? "yes" : "no");
    printf("~spawning took %" G_GINT64_FORMAT " ms\n", spawned);
    close(extra[1]);
    if (write(in_fd, "hello child\n", 12) != 12)
        printf("writing to the child failed\n");
    close(in_fd);
    char buf[64] = { 0 };
    ssize_t n = read(out_fd, buf, sizeof(buf) - 1);
    g_strchomp(buf);
    printf("its standard output: %s (%d bytes)\n", buf, (int)n);
    memset(buf, 0, sizeof(buf));
    n = read(extra[0], buf, sizeof(buf) - 1);
    printf("and the pipe it got as fd 3: %s\n", buf);
    close(out_fd);
    close(extra[0]);
    g_child_watch_add(pid, child_ended, NULL);
    g_main_loop_run(loop);
    printf("the main loop saw it end with status %d\n", exit_status);

    /* GSubprocess and a socket pair, as WebKit's process launcher */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv);
    GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_take_fd(l, sv[1], 3);
    t0 = now_ms();
    GSubprocess *sp = g_subprocess_launcher_spawn(l, &err, self,
                                                  "--socket-child", NULL);
    spawned = now_ms() - t0;
    printf("GSubprocess with a socket at fd 3: %s, returned at once: %s\n",
           sp ? "yes" : "no", spawned < 2000 ? "yes" : "no");
    if (!sp) {
        printf("  (%s)\n", err ? err->message : "?");
        g_clear_error(&err);
        g_object_unref(l);
        return;
    }
    const char *q = "how are you";
    if (write(sv[0], q, strlen(q)) < 0)
        printf("writing to the socket failed\n");
    memset(buf, 0, sizeof(buf));
    n = read(sv[0], buf, sizeof(buf) - 1);
    printf("over the socket: %s\n", buf);
    printf("and it ended well: %s\n",
           g_subprocess_wait_check(sp, NULL, NULL) ? "yes" : "no");
    close(sv[0]);
    g_object_unref(sp);
    g_object_unref(l);
    g_main_loop_unref(loop);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--child"))
        return child();
    if (argc > 1 && !strcmp(argv[1], "--socket-child"))
        return socket_child();
    no_disk = argc > 1 && !strcmp(argv[1], "--no-disk");
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("~GLib %u.%u.%u\n", glib_major_version, glib_minor_version,
           glib_micro_version);
    main_loop();
    threads();
    objects();
    data();
    gio();
    processes(argv[0]);
    printf("done\n");
    return 0;
}
