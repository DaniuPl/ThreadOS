/* gskein: Skein - the music player
 *
 * docs/weave/canvas/Skein.dc.html: the library down the left - Listen
 * now, Albums, Artists, Songs, Playlists, Radio, From Files, each with
 * its dye; an album with a cover woven from its own colours, its songs,
 * Play and Shuffle; and beside them what plays: its cover, the thread
 * it is being spun on (the part played solid, the rest dashed, a knot
 * where it is), the time, before, play or pause, next - and what comes
 * next, knots on a thread. The shuttle carries it outside the window.
 *
 * The library is ~/Music, read with FFmpeg (the copy gmedia uses):
 * songs grouped into albums by their tags - title, artist, album
 * (by its album artist first), date, track - or, where a file has none,
 * by its folder and its name. An album's colours are its picture's (the
 * cover a FLAC, MP3 or Ogg file carries), the three it has most of; an
 * album with no picture gets three of Weave's dyes, by its name.
 * Playlists are the .m3u files in ~/Music; From Files is what sounds in
 * the rest of the home, outside the library.
 *
 * The sound goes out through the system's stream (SYS64_SND_STREAM)
 * from a thread of its own: decoded, made 48000 frames of two channels
 * a second (libswresample), handed over a block at a time - each
 * hand-over waits for the room, and that is what keeps the time.
 * Paused, the stream is let go, so that other programs may sound
 * meanwhile.
 *
 * What it does is said on the serial port, for scripts/tests/t_skein.py:
 *
 *   [skein] drawn W H
 *   [skein] library N songs, M albums, P playlists
 *   [skein] album TITLE by WHO: N songs, FORMATS, woven RRGGBB RRGGBB
 *           RRGGBB from its picture | from its name
 *   [skein] view NAME: WHAT | WHAT ...
 *   [skein] playing K/N TITLE
 *   [skein] played TITLE: F frames, sha256 HEX
 *   [skein] paused at M:SS | [skein] playing again at M:SS
 *   [skein] to S.S s of T.T
 *   [skein] the thread ran out
 *
 * Built against musl, with its threads; FFmpeg's libraries linked in.
 */

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ugui.h"
#include "usha256.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* --- where things are (scripts/tests/t_skein.py reads these) --------- */

#define W1 1400
#define H1 756
#define W0 1000
#define H0 640
#define WIDE 1100             /* narrower than this, the narrow layout */
#define NAV_W 220
#define NAV_W_N 176
#define NAV_TOP 14
#define NAV_SIDE 10
#define NAV_H 34
#define NAV_GAP 2
#define NAV_SPLIT 10          /* Playlists begins a group of its own */
#define NAV_PX 13
#define NAV_DOT 8
#define MAIN_X 30
#define MAIN_Y 26
#define COVER 190
#define COVER_N 150
#define COVER_R 16
#define COVER_GAP 24
#define KIND_PX 12            /* "ALBUM · 2026" */
#define TITLE_PX 44
#define TITLE_PX_N 32
#define BY_PX 15
#define BTN_H 40
#define BTN_PX 14
#define HEAD_H 28
#define HEAD_PX 11
#define ROW_H 38
#define ROW_PX 14
#define NUM_W 40
#define TIME_W 70
#define COL_GAP 12
#define CELL_PX 12
#define VIEW_PX 28            /* a view's heading: Albums, Songs... */
#define VIEW_TOP 86           /* where a view's list begins */
#define GRID_COVER 150
#define GRID_GAP 24
#define LIST_H 52             /* a row of Artists, Playlists */
#define ASIDE_W 360
#define ASIDE_W_N 290
#define AS_PAD 20
#define AS_GAP 16
#define AS_COVER 240
#define AS_COVER_N 190
#define AS_COVER_R 18
#define NOW_PX 18
#define WAVE_H 34
#define WAVE_SEG 24           /* a turn of the thread */
#define CTL_D 58
#define CTL_SMALL 42
#define CTL_GAP 16
#define Q_PX 13
#define Q_STEP 24             /* from one song on the thread to the next */

/* The design's colours in Night (docs/weave/canvas/Skein.dc.html) */
#define N_BG       0x16181E
#define N_NAV      0x17191F
#define N_ASIDE    0x14161B
#define N_EDGE     0x262A33
#define N_TEXT     0xECEDF2
#define N_NAV_FG   0xC9CDD8
#define N_DIM      0x9AA0AE
#define N_HEAD     0x8A90A0
#define N_ROW_FG   0xD6D9E2
#define N_LINE     0x2E323D
#define N_QUIET    0x20232B
#define N_REST     0x3A3F4C
#define MADDER_PINK 0xB23D8C  /* Play, and the shuttle's disc */
#define COCHINEAL  0xD152A6   /* what plays: the bars, the thread */

#define RATE 48000
#define BLOCK 1024            /* frames handed over at once (one DMA half) */
#define AMP_SLOTS 64          /* how loud each 64th of a song was */
#define IO_SIZE 32768

/* --- the library ------------------------------------------------------ */

struct song {
    char path[400];
    char title[120], artist[120], album[120], by[120];
    char fmt[12];
    int track, year;
    long long ms;
    long long mtime;          /* when the file came */
    int album_no;             /* in albums[], -1 outside the library */
    int pic;                  /* it carries a picture */
};
#define SONGS_MAX 4000
/* pointers, so that a song stays where it is while the player reads it
   and the list grows (From Files, a playlist's songs from elsewhere) */
static struct song *songs[SONGS_MAX];
static int nsongs, nlib;      /* the library's are the first nlib */

struct album {
    char title[120], by[120];
    int year;
    int *list, n;             /* its songs, in order */
    long long ms;
    long long newest;         /* when its newest file came */
    char fmts[40];
    uint32_t c[3];            /* the cloth, its rows, its columns */
    int from_pic;
};
#define ALBUMS_MAX 600
static struct album albums[ALBUMS_MAX];
static int nalbums;

struct artist {
    char name[120];
    int albums, songs, first;
};
static struct artist artists[ALBUMS_MAX];
static int nartists;

struct plist {
    char name[120];
    int *list, n;
};
#define PLISTS_MAX 64
static struct plist plists[PLISTS_MAX];
static char plist_path[PLISTS_MAX][400];
static int nplists;

static int *all_songs, *loose, nloose, loose_done;

static char home[256], music[300];

/* The design's dyes, for an album with no picture of its own */
static const uint32_t dyes[7] = {
    0xB23D8C, 0x4A6BE0, 0xC77F0A, 0x1FA3A3, 0x2F9A5C, 0x6A4FD6, 0xC9302C,
};

/* --- the player ------------------------------------------------------- */

static pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcond = PTHREAD_COND_INITIALIZER;
/* FFmpeg was built without threads of its own (--disable-pthreads), so
   what it sets up once for every codec - opening one, probing a file -
   must not happen in two threads at once */
static pthread_mutex_t ff_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    int want;                 /* the song to play now, -1 none */
    unsigned gen;             /* every new order bumps it */
    long long seek;           /* frames to go to, -1 none */
    int paused, quit;
    int cur;                  /* the song playing (or paused in), -1 */
    long long pos, len;       /* frames */
    uint8_t amp[AMP_SLOTS];
    int changed;
    int *queue, nq, qpos;
    char note[160];
} P = { .want = -1, .seek = -1, .cur = -1 };

/* --- the window ------------------------------------------------------- */

static int win = -1, cw, ch;
static struct ud_look L;

static struct {
    uint32_t bg, nav, aside, edge, text, nav_fg, dim, head, row_fg, line;
    uint32_t quiet, rest, play, bars, sel, nav_sel;
} C;

static struct {
    int nav_w, aside_w, cover, as_cover, title_px, wave_w;
    int main_x, main_w, ax;
} G;

enum { V_LISTEN, V_ALBUMS, V_ARTISTS, V_SONGS, V_PLAYLISTS, V_RADIO,
       V_FILES, V_N };
static const char *const nav_name[V_N] = {
    "Listen now", "Albums", "Artists", "Songs", "Playlists", "Radio",
    "From Files"
};
static const uint32_t nav_dye[V_N] = {
    0xD152A6, 0x5B7CFA, 0x2EC4C4, 0xF5A524, 0x46C27A, 0x6A4FD6, 0x4B505E
};
static int view = V_LISTEN;
static int open_album = -1;   /* the album shown, in Albums or Artists */
static int open_artist = -1;  /* Artists: whose albums */
static int open_plist = -1;
static int listen_album = -1; /* Listen now: the album last played */
static int top_row;           /* how far the list is scrolled */

/* What a click lands on, as the last drawing laid it out */
enum { H_NAV, H_PLAY, H_SHUFFLE, H_ROW, H_ALBUM, H_ARTIST, H_PLIST,
       H_BEFORE, H_TOGGLE, H_NEXT, H_WAVE };
struct hit { int x, y, w, h, kind, arg; };
#define HITS_MAX 400
static struct hit hits[HITS_MAX];
static int nhits;
static const int *rows_list;  /* the songs the table on the screen shows */
static int rows_n;

static void blit(int x, int y, int w, int h, const uint32_t *px);

static void hit(int x, int y, int w, int h, int kind, int arg)
{
    if (nhits < HITS_MAX)
        hits[nhits++] = (struct hit){ x, y, w, h, kind, arg };
}

static uint32_t mix(uint32_t a, uint32_t b, int alpha)
{
    uint32_t r = (((a >> 16) & 255) * alpha + ((b >> 16) & 255) * (255 - alpha) + 127) / 255;
    uint32_t g = (((a >> 8) & 255) * alpha + ((b >> 8) & 255) * (255 - alpha) + 127) / 255;
    uint32_t bl = ((a & 255) * alpha + (b & 255) * (255 - alpha) + 127) / 255;
    return r << 16 | g << 8 | bl;
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static void clock_words(long long frames, char *out, size_t cap)
{
    long long s = frames / RATE;
    snprintf(out, cap, "%lld:%02lld", s / 60, s % 60);
}

/* --- reading files with FFmpeg ------------------------------------------ */

struct media {
    FILE *f;
    int fd;
    AVFormatContext *fmt;
    AVIOContext *io;
};

static int io_read(void *o, uint8_t *buf, int n)
{
    ssize_t got = read(((struct media *)o)->fd, buf, (size_t)n);
    return got > 0 ? (int)got : AVERROR_EOF;
}

static int64_t io_seek(void *o, int64_t off, int whence)
{
    int fd = ((struct media *)o)->fd;
    if (whence & AVSEEK_SIZE) {
        struct stat st;
        return fstat(fd, &st) == 0 ? (int64_t)st.st_size : -1;
    }
    whence &= ~AVSEEK_FORCE;
    off_t r = lseek(fd, (off_t)off, whence);
    return r < 0 ? -1 : (int64_t)r;
}

static void media_close(struct media *m)
{
    if (m->fmt)
        avformat_close_input(&m->fmt);
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    if (m->f)
        fclose(m->f);
    m->f = 0;
    m->fd = -1;
}

/* The file opened and its streams found: under ff_lock */
static int media_open(struct media *m, const char *path)
{
    memset(m, 0, sizeof(*m));
    m->f = fopen(path, "rb");
    if (!m->f) {
        m->fd = -1;
        return -1;
    }
    m->fd = fileno(m->f);
    uint8_t *buf = av_malloc(IO_SIZE);
    if (buf)
        m->io = avio_alloc_context(buf, IO_SIZE, 0, m, io_read, 0, io_seek);
    if (!m->io) {
        av_free(buf);
        media_close(m);
        return -1;
    }
    m->fmt = avformat_alloc_context();
    if (!m->fmt) {
        media_close(m);
        return -1;
    }
    m->fmt->pb = m->io;
    m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&m->fmt, path, 0, 0) < 0 ||
        avformat_find_stream_info(m->fmt, 0) < 0) {
        media_close(m);
        return -1;
    }
    return 0;
}

static const char *tag(AVFormatContext *f, AVStream *st, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(f->metadata, key, 0, 0);
    if ((!e || !e->value[0]) && st)
        e = av_dict_get(st->metadata, key, 0, 0);
    return e && e->value[0] ? e->value : 0;
}

/* At most cap - 1 bytes of s, never half a character */
static void copy(char *d, const char *s, size_t cap)
{
    size_t n = 0;
    if (s) {
        n = strlen(s);
        if (n >= cap) {
            n = cap - 1;
            while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
                n--;
        }
        memmove(d, s, n);
    }
    d[n] = 0;
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static const char *ext_of(const char *name)
{
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}

static int audio_name(const char *name)
{
    static const char *const ok[] = { "flac", "mp3", "ogg", "oga", "opus",
                                      "m4a", "aac", "wav", "mka", 0 };
    const char *e = ext_of(name);
    for (int i = 0; ok[i]; i++)
        if (!strcasecmp(e, ok[i]))
            return 1;
    return 0;
}

static int playlist_name(const char *name)
{
    const char *e = ext_of(name);
    return !strcasecmp(e, "m3u") || !strcasecmp(e, "m3u8");
}

/* A file's tags, length and kind - or 0 when it is not sound FFmpeg
   knows */
static struct song *probe(const char *path)
{
    struct media m;
    pthread_mutex_lock(&ff_lock);
    if (media_open(&m, path) < 0) {
        pthread_mutex_unlock(&ff_lock);
        return 0;
    }
    struct song *s = 0;
    int ai = av_find_best_stream(m.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    if (ai >= 0 && (s = calloc(1, sizeof(*s)))) {
        AVStream *st = m.fmt->streams[ai];
        copy(s->path, path, sizeof(s->path));
        const char *t = tag(m.fmt, st, "title");
        if (t) {
            copy(s->title, t, sizeof(s->title));
        } else {
            /* the file's name, without its kind or a leading number */
            const char *b = base_of(path);
            while (*b >= '0' && *b <= '9')
                b++;
            while (*b == ' ' || *b == '-' || *b == '.' || *b == '_')
                b++;
            if (!*b || b[0] == '.')
                b = base_of(path);
            copy(s->title, b, sizeof(s->title));
            char *dot = strrchr(s->title, '.');
            if (dot && dot != s->title)
                *dot = 0;
        }
        copy(s->artist, tag(m.fmt, st, "artist") ? tag(m.fmt, st, "artist")
             : "Unknown artist", sizeof(s->artist));
        const char *by = tag(m.fmt, st, "album_artist");
        copy(s->by, by ? by : s->artist, sizeof(s->by));
        const char *al = tag(m.fmt, st, "album");
        if (al) {
            copy(s->album, al, sizeof(s->album));
        } else {
            /* the folder it is in, unless that is Music itself */
            char dir[400];
            copy(dir, path, sizeof(dir));
            char *sl = strrchr(dir, '/');
            if (sl)
                *sl = 0;
            copy(s->album, strcmp(dir, music) ? base_of(dir) : "Unknown album",
                 sizeof(s->album));
        }
        const char *tr = tag(m.fmt, st, "track");
        s->track = tr ? atoi(tr) : 0;
        const char *date = tag(m.fmt, st, "date");
        if (!date)
            date = tag(m.fmt, st, "year");
        s->year = date && strlen(date) >= 4 ? atoi(date) : 0;
        if (m.fmt->duration != AV_NOPTS_VALUE && m.fmt->duration > 0)
            s->ms = m.fmt->duration / 1000;
        else if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
            s->ms = av_rescale_q(st->duration, st->time_base,
                                 (AVRational){ 1, 1000 });
        enum AVCodecID id = st->codecpar->codec_id;
        const char *k = id == AV_CODEC_ID_FLAC ? "FLAC"
                      : id == AV_CODEC_ID_MP3 ? "MP3"
                      : id == AV_CODEC_ID_VORBIS ? "Vorbis"
                      : id == AV_CODEC_ID_OPUS ? "Opus"
                      : id == AV_CODEC_ID_AAC ? "AAC"
                      : (id >= AV_CODEC_ID_PCM_S16LE &&
                         id < AV_CODEC_ID_ADPCM_IMA_QT) ? "WAV"
                      : avcodec_get_name(id);
        copy(s->fmt, k, sizeof(s->fmt));
        for (unsigned i = 0; i < m.fmt->nb_streams; i++)
            if (m.fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
                s->pic = 1;
        s->album_no = -1;
        struct stat fs;
        if (fstat(m.fd, &fs) == 0)
            s->mtime = (long long)fs.st_mtime;
    }
    media_close(&m);
    pthread_mutex_unlock(&ff_lock);
    return s;
}

/* The three colours a picture has most of, each unlike the ones before
   it: counted in a 48 by 48 copy taken pixel by pixel (so that the
   colours are the picture's own, not blends of neighbours), in boxes of
   16 levels a channel, a box's colour the mean of what fell in it */
static int picture_colours(const char *path, uint32_t c[3])
{
    struct media m;
    int found = 0;
    pthread_mutex_lock(&ff_lock);
    if (media_open(&m, path) < 0) {
        pthread_mutex_unlock(&ff_lock);
        return 0;
    }
    AVStream *st = 0;
    for (unsigned i = 0; i < m.fmt->nb_streams && !st; i++)
        if (m.fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
            st = m.fmt->streams[i];
    const AVCodec *dec = st ? avcodec_find_decoder(st->codecpar->codec_id) : 0;
    AVCodecContext *ctx = dec ? avcodec_alloc_context3(dec) : 0;
    AVFrame *fr = av_frame_alloc();
    static uint8_t rgb[48 * 48 * 3];
    if (ctx && fr && avcodec_parameters_to_context(ctx, st->codecpar) >= 0 &&
        avcodec_open2(ctx, dec, 0) >= 0 &&
        avcodec_send_packet(ctx, &st->attached_pic) >= 0 &&
        avcodec_receive_frame(ctx, fr) >= 0) {
        struct SwsContext *sws = sws_getContext(
            fr->width, fr->height, fr->format, 48, 48, AV_PIX_FMT_RGB24,
            SWS_POINT, 0, 0, 0);
        uint8_t *dst[4] = { rgb, 0, 0, 0 };
        int ls[4] = { 48 * 3, 0, 0, 0 };
        if (sws && sws_scale(sws, (const uint8_t *const *)fr->data,
                             fr->linesize, 0, fr->height, dst, ls) > 0)
            found = 1;
        sws_freeContext(sws);
    }
    av_frame_free(&fr);
    avcodec_free_context(&ctx);
    media_close(&m);
    pthread_mutex_unlock(&ff_lock);
    if (!found)
        return 0;

    static int count[4096];
    static uint32_t sum[4096][3];
    memset(count, 0, sizeof(count));
    memset(sum, 0, sizeof(sum));
    for (int i = 0; i < 48 * 48; i++) {
        const uint8_t *p = rgb + i * 3;
        int b = (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | p[2] >> 4;
        count[b]++;
        sum[b][0] += p[0];
        sum[b][1] += p[1];
        sum[b][2] += p[2];
    }
    int n = 0;
    for (int k = 0; k < 3; k++) {
        int best = -1;
        uint32_t best_c = 0;
        for (int b = 0; b < 4096; b++) {
            if (!count[b] || (best >= 0 && count[b] <= count[best]))
                continue;
            uint32_t col = (sum[b][0] / count[b]) << 16 |
                           (sum[b][1] / count[b]) << 8 | sum[b][2] / count[b];
            int far = 1;
            for (int j = 0; j < n && far; j++) {
                int d = abs((int)(col >> 16) - (int)(c[j] >> 16)) +
                        abs((int)((col >> 8) & 255) - (int)((c[j] >> 8) & 255)) +
                        abs((int)(col & 255) - (int)(c[j] & 255));
                far = d > 120;
            }
            if (far) {
                best = b;
                best_c = col;
            }
        }
        if (best < 0)
            break;
        c[n++] = best_c;
    }
    return n == 3;
}

static void name_colours(const char *a, const char *b, uint32_t c[3])
{
    uint32_t h = fnv(a) ^ (fnv(b) * 31u);
    int i = h % 7, j = (h / 7) % 6, k = (h / 42) % 5;
    int left[7] = { 0, 1, 2, 3, 4, 5, 6 };
    c[0] = dyes[left[i]];
    left[i] = left[6];
    c[1] = dyes[left[j]];
    left[j] = left[5];
    c[2] = dyes[left[k]];
}

/* --- the library, gathered ------------------------------------------------ */

struct names {
    char **v;
    int n, cap;
};

static void names_add(struct names *l, const char *s)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char **v = realloc(l->v, (size_t)cap * sizeof(char *));
        if (!v)
            return;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n] = strdup(s);
    if (l->v[l->n])
        l->n++;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The files under dir, but what begins with a dot (the system's own):
   sound into 'audio', playlists into 'pl' */
static void walk(const char *dir, int depth, const char *skip,
                 struct names *audio, struct names *pl, int max)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && audio->n < max) {
        if (e->d_name[0] == '.')
            continue;
        char p[400];
        if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p))
            continue;
        if (skip && !strcmp(p, skip))
            continue;
        struct stat st;
        if (stat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0)
                walk(p, depth - 1, skip, audio, pl, max);
        } else if (audio_name(e->d_name)) {
            names_add(audio, p);
        } else if (pl && playlist_name(e->d_name)) {
            names_add(pl, p);
        }
    }
    closedir(d);
}

static int add_song(struct song *s)
{
    if (!s || nsongs >= SONGS_MAX) {
        free(s);
        return -1;
    }
    songs[nsongs] = s;
    return nsongs++;
}

static int song_order(const void *a, const void *b)
{
    const struct song *x = songs[*(const int *)a], *y = songs[*(const int *)b];
    int tx = x->track ? x->track : 1 << 20, ty = y->track ? y->track : 1 << 20;
    if (tx != ty)
        return tx < ty ? -1 : 1;
    return strcmp(x->path, y->path);
}

static int album_order(const void *a, const void *b)
{
    const struct album *x = a, *y = b;
    int d = strcasecmp(x->by, y->by);
    return d ? d : strcasecmp(x->title, y->title);
}

static int find_song(const char *path)
{
    for (int i = 0; i < nsongs; i++)
        if (!strcmp(songs[i]->path, path))
            return i;
    return -1;
}

static void read_playlist(const char *path)
{
    if (nplists >= PLISTS_MAX)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    struct plist *p = &plists[nplists];
    memset(p, 0, sizeof(*p));
    copy(p->name, base_of(path), sizeof(p->name));
    char *dot = strrchr(p->name, '.');
    if (dot && dot != p->name)
        *dot = 0;
    char dir[400];
    copy(dir, path, sizeof(dir));
    char *sl = strrchr(dir, '/');
    if (sl)
        *sl = 0;
    int cap = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' '))
            line[--n] = 0;
        const char *s = line;
        if (!strncmp(s, "\xEF\xBB\xBF", 3))
            s += 3;                       /* a byte-order mark */
        if (!*s || *s == '#')
            continue;
        char full[1000];
        if (s[0] == '/')
            snprintf(full, sizeof(full), "%s", s);
        else
            snprintf(full, sizeof(full), "%s/%s", dir,
                     s[0] == '.' && s[1] == '/' ? s + 2 : s);
        if (strlen(full) >= sizeof(songs[0]->path))
            continue;
        int k = find_song(full);
        if (k < 0)
            k = add_song(probe(full));
        if (k < 0)
            continue;
        if (p->n == cap) {
            cap = cap ? cap * 2 : 16;
            int *l = realloc(p->list, (size_t)cap * sizeof(int));
            if (!l)
                break;
            p->list = l;
        }
        p->list[p->n++] = k;
    }
    fclose(f);
    copy(plist_path[nplists], path, sizeof(plist_path[0]));
    nplists++;
}

static void fmt_list(const struct album *a, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < a->n; i++) {
        const char *k = songs[a->list[i]]->fmt;
        if (strstr(out, k))
            continue;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", k);
    }
}

static void gather(void)
{
    struct names audio = { 0 }, pl = { 0 };
    walk(music, 6, 0, &audio, &pl, SONGS_MAX / 2);
    qsort(audio.v, (size_t)audio.n, sizeof(char *), by_name);
    qsort(pl.v, (size_t)pl.n, sizeof(char *), by_name);
    for (int i = 0; i < audio.n; i++)
        add_song(probe(audio.v[i]));
    nlib = nsongs;

    /* albums: the same album by the same people */
    for (int i = 0; i < nlib; i++) {
        struct song *s = songs[i];
        int a = 0;
        while (a < nalbums && (strcasecmp(albums[a].title, s->album) ||
                               strcasecmp(albums[a].by, s->by)))
            a++;
        if (a == nalbums) {
            if (nalbums == ALBUMS_MAX)
                continue;
            memset(&albums[a], 0, sizeof(albums[a]));
            copy(albums[a].title, s->album, sizeof(albums[a].title));
            copy(albums[a].by, s->by, sizeof(albums[a].by));
            nalbums++;
        }
        struct album *al = &albums[a];
        int *l = realloc(al->list, (size_t)(al->n + 1) * sizeof(int));
        if (!l)
            continue;
        al->list = l;
        al->list[al->n++] = i;
        al->ms += s->ms;
        if (s->mtime > al->newest)
            al->newest = s->mtime;
        if (s->year > al->year)
            al->year = s->year;
    }
    qsort(albums, (size_t)nalbums, sizeof(albums[0]), album_order);
    for (int a = 0; a < nalbums; a++) {
        struct album *al = &albums[a];
        qsort(al->list, (size_t)al->n, sizeof(int), song_order);
        fmt_list(al, al->fmts, sizeof(al->fmts));
        for (int i = 0; i < al->n; i++)
            songs[al->list[i]]->album_no = a;
        for (int i = 0; i < al->n && !al->from_pic; i++)
            if (songs[al->list[i]]->pic)
                al->from_pic = picture_colours(songs[al->list[i]]->path, al->c);
        if (!al->from_pic)
            name_colours(al->title, al->by, al->c);
        printf("[skein] album %s by %s: %d songs, %s, woven %06X %06X %06X "
               "from its %s\n", al->title, al->by, al->n, al->fmts,
               al->c[0], al->c[1], al->c[2], al->from_pic ? "picture" : "name");
    }

    /* Listen now: what came last, until something is played */
    for (int a = 0; a < nalbums; a++)
        if (listen_album < 0 || albums[a].newest > albums[listen_album].newest)
            listen_album = a;

    /* the songs as the albums have them */
    all_songs = malloc((size_t)(nlib ? nlib : 1) * sizeof(int));
    int k = 0;
    for (int a = 0; a < nalbums && all_songs; a++)
        for (int i = 0; i < albums[a].n; i++)
            all_songs[k++] = albums[a].list[i];

    /* who made them */
    for (int a = 0; a < nalbums; a++) {
        int r = 0;
        while (r < nartists && strcasecmp(artists[r].name, albums[a].by))
            r++;
        if (r == nartists) {
            copy(artists[r].name, albums[a].by, sizeof(artists[r].name));
            artists[r].first = a;
            nartists++;
        }
        artists[r].albums++;
        artists[r].songs += albums[a].n;
    }

    for (int i = 0; i < pl.n; i++)
        read_playlist(pl.v[i]);
    for (int i = 0; i < audio.n; i++)
        free(audio.v[i]);
    for (int i = 0; i < pl.n; i++)
        free(pl.v[i]);
    free(audio.v);
    free(pl.v);
    printf("[skein] library %d songs, %d albums, %d playlists\n", nlib,
           nalbums, nplists);
    fflush(stdout);
}

/* From Files: what sounds elsewhere in the home, looked for the first
   time it is asked for */
static void gather_loose(void)
{
    if (loose_done)
        return;
    loose_done = 1;
    struct names audio = { 0 };
    walk(home, 4, music, &audio, 0, 400);
    qsort(audio.v, (size_t)audio.n, sizeof(char *), by_name);
    loose = malloc((size_t)(audio.n ? audio.n : 1) * sizeof(int));
    for (int i = 0; i < audio.n; i++) {
        int k = find_song(audio.v[i]);
        if (k < 0)
            k = add_song(probe(audio.v[i]));
        if (k >= 0 && loose)
            loose[nloose++] = k;
        free(audio.v[i]);
    }
    free(audio.v);
}

/* --- the player's thread ---------------------------------------------- */

static void say_locked(const char *what)
{
    printf("[skein] %s\n", what);
    fflush(stdout);
}

/* Frames of 48 kHz stereo waiting to be handed over */
static int16_t *pend;
static int npend, cap_pend;

static int pend_room(int more)
{
    if (npend + more <= cap_pend)
        return 1;
    int cap = cap_pend ? cap_pend : 16384;
    while (cap < npend + more)
        cap *= 2;
    int16_t *p = realloc(pend, (size_t)cap * 4);
    if (!p)
        return 0;
    pend = p;
    cap_pend = cap;
    return 1;
}

/* One decoded frame, made 48 kHz stereo and put after what waits. The
   converter is made for the frame's own format, and made again when a
   file changes its mind (AAC, Opus tell theirs only as they decode). */
static void convert(SwrContext **swr, AVFrame *fr, long long *drop)
{
    if (fr->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&fr->ch_layout, fr->ch_layout.nb_channels);
    static AVChannelLayout in_layout;
    static int in_fmt = -1, in_rate;
    if (*swr && (in_fmt != fr->format || in_rate != fr->sample_rate ||
                 av_channel_layout_compare(&in_layout, &fr->ch_layout)))
        swr_free(swr);
    if (!*swr) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(swr, &stereo, AV_SAMPLE_FMT_S16, RATE,
                                &fr->ch_layout, fr->format, fr->sample_rate,
                                0, 0) < 0 || swr_init(*swr) < 0) {
            swr_free(swr);
            return;
        }
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_copy(&in_layout, &fr->ch_layout);
        in_fmt = fr->format;
        in_rate = fr->sample_rate;
    }
    int room = swr_get_out_samples(*swr, fr->nb_samples);
    if (room <= 0 || !pend_room(room))
        return;
    uint8_t *out[1] = { (uint8_t *)(pend + npend * 2) };
    int n = swr_convert(*swr, out, room, (const uint8_t **)fr->extended_data,
                        fr->nb_samples);
    if (n <= 0)
        return;
    if (*drop > 0) {
        /* after a seek: what lies before the place asked for */
        int k = *drop < n ? (int)*drop : n;
        memmove(pend + npend * 2, pend + (npend + k) * 2, (size_t)(n - k) * 4);
        n -= k;
        *drop -= k;
    }
    npend += n;
}

/* What the stream says when it will not have us: another program is
   sounding */
static void busy(void)
{
    pthread_mutex_lock(&pm);
    snprintf(P.note, sizeof(P.note), "Another program is playing sound");
    say_locked("the sound is another program's");
    pthread_mutex_unlock(&pm);
}

/* One song, from where it is to its end or to the next order: 0 played
   to its end, 1 another order came, -1 it could not be played, -2 the
   sound could not be had */
static int play(int song, unsigned gen, int *stream_on)
{
    const struct song *s = songs[song];
    struct media m = { .f = 0, .fd = -1 };
    AVCodecContext *ctx = 0;
    SwrContext *swr = 0;
    AVPacket *pk = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int ai = -1, rc = -1;
    pthread_mutex_lock(&ff_lock);
    if (pk && fr && media_open(&m, s->path) == 0) {
        ai = av_find_best_stream(m.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
        const AVCodec *dec = ai >= 0
            ? avcodec_find_decoder(m.fmt->streams[ai]->codecpar->codec_id) : 0;
        ctx = dec ? avcodec_alloc_context3(dec) : 0;
        if (ctx && (avcodec_parameters_to_context(
                        ctx, m.fmt->streams[ai]->codecpar) < 0 ||
                    avcodec_open2(ctx, dec, 0) < 0))
            avcodec_free_context(&ctx);
    }
    pthread_mutex_unlock(&ff_lock);
    if (!ctx)
        goto out;
    if (!*stream_on) {
        if (sys_snd_stream(SND_STREAM_BEGIN, 0, 0) < 0) {
            busy();
            rc = -2;
            goto out;
        }
        *stream_on = 1;
    }
    AVRational tb = m.fmt->streams[ai]->time_base;
    struct sha256 h;
    sha256_init(&h);
    long long handed = 0, drop = 0;
    int seeked = 0, eof = 0;
    npend = 0;
    for (;;) {
        pthread_mutex_lock(&pm);
        if (P.gen != gen || P.quit) {
            pthread_mutex_unlock(&pm);
            rc = 1;
            break;
        }
        long long seek = P.seek;
        P.seek = -1;
        if (P.paused) {
            pthread_mutex_unlock(&pm);
            /* let the stream go while nothing sounds */
            sys_snd_stream(SND_STREAM_END, 0, 0);
            *stream_on = 0;
            pthread_mutex_lock(&pm);
            char w[64], c[32];
            clock_words(P.pos, c, sizeof(c));
            snprintf(w, sizeof(w), "paused at %s", c);
            say_locked(w);
            P.changed = 1;
            while (P.paused && P.gen == gen && !P.quit)
                pthread_cond_wait(&pcond, &pm);
            int again = P.gen == gen && !P.quit;
            if (again) {
                snprintf(w, sizeof(w), "playing again at %s", c);
                say_locked(w);
            }
            pthread_mutex_unlock(&pm);
            if (!again) {
                rc = 1;
                break;
            }
            if (sys_snd_stream(SND_STREAM_BEGIN, 0, 0) < 0) {
                busy();
                rc = -2;
                break;
            }
            *stream_on = 1;
            continue;
        }
        pthread_mutex_unlock(&pm);

        if (seek >= 0) {
            int64_t ts = av_rescale_q(seek, (AVRational){ 1, RATE }, tb);
            if (av_seek_frame(m.fmt, ai, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(ctx);
                swr_free(&swr);           /* and what it held */
                npend = 0;
                handed = seek;
                drop = -1;                /* found from the next frame */
                seeked = 1;
                eof = 0;
                pthread_mutex_lock(&pm);
                P.pos = seek;
                P.changed = 1;
                pthread_mutex_unlock(&pm);
            }
        }

        /* a block to hand over, when there is one */
        if (npend >= BLOCK || (eof && npend > 0)) {
            int n = npend >= BLOCK ? BLOCK : npend;
            if (sys_snd_stream(SND_STREAM_WRITE, pend, (uint32_t)n) < 0) {
                *stream_on = 0;
                busy();
                rc = -2;
                break;
            }
            sha256_update(&h, pend, (uint32_t)n * 4);
            int peak = 0;
            for (int i = 0; i < n * 2; i++) {
                int v = pend[i] < 0 ? -pend[i] : pend[i];
                if (v > peak)
                    peak = v;
            }
            memmove(pend, pend + n * 2, (size_t)(npend - n) * 4);
            npend -= n;
            handed += n;
            pthread_mutex_lock(&pm);
            /* what is out of the speaker: a block is still on its way */
            P.pos = handed > BLOCK ? handed - BLOCK : 0;
            if (P.len > 0) {
                long long slot = handed * AMP_SLOTS / P.len;
                if (slot >= AMP_SLOTS)
                    slot = AMP_SLOTS - 1;
                int a = peak * 255 / 32767;
                if (a > P.amp[slot])
                    P.amp[slot] = (uint8_t)a;
            }
            pthread_mutex_unlock(&pm);
            continue;
        }
        if (eof) {
            rc = 0;
            break;
        }

        int got = av_read_frame(m.fmt, pk);
        if (got >= 0 && pk->stream_index != ai) {
            av_packet_unref(pk);
            continue;
        }
        avcodec_send_packet(ctx, got >= 0 ? pk : 0);
        if (got >= 0)
            av_packet_unref(pk);
        while (avcodec_receive_frame(ctx, fr) >= 0) {
            if (drop < 0) {
                /* the first frame after a seek: from its time, how much
                   of it comes before the place asked for */
                long long at = fr->best_effort_timestamp != AV_NOPTS_VALUE
                    ? av_rescale_q(fr->best_effort_timestamp, tb,
                                   (AVRational){ 1, RATE }) : handed;
                drop = handed > at ? handed - at : 0;
            }
            convert(&swr, fr, &drop);
            av_frame_unref(fr);
        }
        if (got < 0) {
            /* the end: what the decoder and the converter still hold */
            if (swr && pend_room(4096)) {
                uint8_t *o[1] = { (uint8_t *)(pend + npend * 2) };
                int n = swr_convert(swr, o, 4096, 0, 0);
                if (n > 0)
                    npend += n;
            }
            eof = 1;
        }
    }
    if (rc == 0) {
        uint8_t d[32];
        sha256_final(&h, d);
        char hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(hex + i * 2, 3, "%02x", d[i]);
        if (seeked)
            printf("[skein] played %s, after a seek\n", s->title);
        else
            printf("[skein] played %s: %lld frames, sha256 %s\n", s->title,
                   handed, hex);
        fflush(stdout);
    }
out:
    swr_free(&swr);
    avcodec_free_context(&ctx);
    if (m.fmt || m.f)
        media_close(&m);
    av_packet_free(&pk);
    av_frame_free(&fr);
    if (rc == -1) {
        pthread_mutex_lock(&pm);
        snprintf(P.note, sizeof(P.note), "Skein cannot play \"%.100s\"",
                 s->title);
        pthread_mutex_unlock(&pm);
    }
    return rc;
}

static void *player(void *arg)
{
    (void)arg;
    int stream_on = 0;
    pthread_mutex_lock(&pm);
    for (;;) {
        while (!P.quit && P.want < 0) {
            if (stream_on) {
                pthread_mutex_unlock(&pm);
                sys_snd_stream(SND_STREAM_END, 0, 0);
                pthread_mutex_lock(&pm);
                stream_on = 0;
            }
            pthread_cond_wait(&pcond, &pm);
        }
        if (P.quit)
            break;
        int song = P.want;
        unsigned gen = P.gen;
        P.cur = song;
        P.pos = 0;
        P.len = songs[song]->ms * RATE / 1000;
        P.note[0] = 0;
        memset(P.amp, 0, sizeof(P.amp));
        P.changed = 1;
        char w[200];
        snprintf(w, sizeof(w), "playing %d/%d %s", P.qpos + 1, P.nq,
                 songs[song]->title);
        say_locked(w);
        pthread_mutex_unlock(&pm);
        int r = play(song, gen, &stream_on);
        pthread_mutex_lock(&pm);
        if (P.gen != gen)
            continue;                     /* a new order: P.want has it */
        if (r == -2 || P.qpos + 1 >= P.nq) {
            /* the end of the thread, or no sound to be had */
            P.want = -1;
            P.cur = -1;
            P.paused = 0;
            if (r != -2)
                say_locked("the thread ran out");
        } else {
            P.qpos++;
            P.want = P.queue[P.qpos];
            P.gen++;
        }
        P.changed = 1;
    }
    pthread_mutex_unlock(&pm);
    if (stream_on)
        sys_snd_stream(SND_STREAM_END, 0, 0);
    return 0;
}

/* --- orders, from the window --------------------------------------------- */

static void play_list(const int *list, int n, int at)
{
    if (n <= 0 || at < 0 || at >= n)
        return;
    int *q = malloc((size_t)n * sizeof(int));
    if (!q)
        return;
    memcpy(q, list, (size_t)n * sizeof(int));
    pthread_mutex_lock(&pm);
    free(P.queue);
    P.queue = q;
    P.nq = n;
    P.qpos = at;
    P.want = q[at];
    P.gen++;
    P.paused = 0;
    P.seek = -1;
    pthread_cond_signal(&pcond);
    pthread_mutex_unlock(&pm);
    int a = songs[q[at]]->album_no;
    if (a >= 0)
        listen_album = a;
}

static void shuffle(const int *list, int n)
{
    int *l = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!l)
        return;
    memcpy(l, list, (size_t)n * sizeof(int));
    unsigned r = (unsigned)sys_ticks() * 2654435761u + 1;
    for (int i = n - 1; i > 0; i--) {
        r = r * 1103515245u + 12345u;
        int j = (int)((r >> 8) % (unsigned)(i + 1));
        int t = l[i];
        l[i] = l[j];
        l[j] = t;
    }
    play_list(l, n, 0);
    free(l);
}

static void toggle(void)
{
    pthread_mutex_lock(&pm);
    if (P.cur >= 0) {
        P.paused = !P.paused;
        pthread_cond_signal(&pcond);
    }
    pthread_mutex_unlock(&pm);
}

static void set_paused(int on)
{
    pthread_mutex_lock(&pm);
    if (P.cur >= 0 && P.paused != on) {
        P.paused = on;
        pthread_cond_signal(&pcond);
    }
    pthread_mutex_unlock(&pm);
}

/* Before: the song from its start, or - near its start already - the
   one before it; next: the one after */
static void step(int d)
{
    pthread_mutex_lock(&pm);
    if (P.cur >= 0 && d < 0 && P.pos > 3 * RATE) {
        P.seek = 0;
        P.paused = 0;
    } else if (P.qpos + d >= 0 && P.qpos + d < P.nq) {
        P.qpos += d;
        P.want = P.queue[P.qpos];
        P.gen++;
        P.paused = 0;
        P.seek = -1;
    }
    pthread_cond_signal(&pcond);
    pthread_mutex_unlock(&pm);
}

static void seek_to(int part, int whole)
{
    pthread_mutex_lock(&pm);
    if (P.cur >= 0 && P.len > 0 && whole > 0) {
        P.seek = P.len * part / whole;
        printf("[skein] to %.1f s of %.1f\n", (double)P.seek / RATE,
               (double)P.len / RATE);
        fflush(stdout);
        pthread_cond_signal(&pcond);
    }
    pthread_mutex_unlock(&pm);
}

/* --- drawing ----------------------------------------------------------- */

static int weave(void)
{
    return L.style == UD_STYLE_WEAVE;
}

static void colours(void)
{
    ud_look(&L);
    if (weave() && L.dark) {
        C.bg = N_BG;
        C.nav = N_NAV;
        C.aside = N_ASIDE;
        C.edge = N_EDGE;
        C.text = N_TEXT;
        C.nav_fg = N_NAV_FG;
        C.dim = N_DIM;
        C.head = N_HEAD;
        C.row_fg = N_ROW_FG;
        C.line = N_LINE;
        C.quiet = N_QUIET;
        C.rest = N_REST;
        C.play = MADDER_PINK;
        C.bars = COCHINEAL;
    } else {
        /* Linen, and the styles that are not Weave: the theme's own
           surfaces, the music's colour where the theme has one */
        C.bg = L.bg;
        C.nav = mix(L.text, L.bg, 7);
        C.aside = mix(L.text, L.bg, 10);
        C.edge = L.line;
        C.text = L.text;
        C.nav_fg = mix(L.text, L.bg, 215);
        C.dim = L.dim;
        C.head = L.dim;
        C.row_fg = L.text;
        C.line = L.line;
        C.quiet = L.field;
        C.rest = mix(L.dim, L.bg, 120);
        C.play = weave() ? L.dye[UD_COCHINEAL] : MADDER_PINK;
        C.bars = C.play;
    }
    C.sel = mix(COCHINEAL, C.bg, 36);       /* rgba(209,82,166,0.14) */
    C.nav_sel = mix(COCHINEAL, C.nav, 36);
}

static void lay(void)
{
    int wide = cw >= WIDE;
    G.nav_w = wide ? NAV_W : NAV_W_N;
    G.aside_w = wide ? ASIDE_W : ASIDE_W_N;
    G.cover = wide ? COVER : COVER_N;
    G.as_cover = wide ? AS_COVER : AS_COVER_N;
    G.title_px = wide ? TITLE_PX : TITLE_PX_N;
    G.wave_w = G.aside_w - 2 * AS_PAD;
    G.main_x = G.nav_w + 1 + MAIN_X;
    G.ax = cw - G.aside_w;
    G.main_w = G.ax - 1 - MAIN_X - G.main_x;
}

/* Text with letters set apart by 'em' hundredths of its size - the
   design's small capitals */
static int spaced(int x, int y, const char *s, uint32_t rgb, int font,
                  int px, int em)
{
    char one[8];
    while (*s) {
        int n = 1;
        unsigned char c = (unsigned char)*s;
        if (c >= 0xF0)
            n = 4;
        else if (c >= 0xE0)
            n = 3;
        else if (c >= 0xC0)
            n = 2;
        memcpy(one, s, (size_t)n);
        one[n] = 0;
        ud_text(win, x, y, one, rgb, font, px);
        x += ud_width(one, font, px) + (px * em + 50) / 100;
        s += n;
    }
    return x;
}

static int spaced_w(const char *s, int font, int px, int em)
{
    int w = 0, n = 0;
    char one[2] = { 0, 0 };
    for (; *s; s++, n++) {
        one[0] = *s;
        w += ud_width(one, font, px) + (px * em + 50) / 100;
    }
    return n ? w - (px * em + 50) / 100 : 0;
}

/* Cut to fit w, with an ellipsis */
static const char *fit(const char *s, int font, int px, int w, char *out,
                       size_t cap)
{
    if (ud_width(s, font, px) <= w)
        return s;
    size_t n = strlen(s);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
        snprintf(out, cap, "%.*s\xE2\x80\xA6", (int)n, s);
        if (ud_width(out, font, px) <= w)
            return out;
    }
    return "";
}

/* A dashed line, a pixel high: three on, three off, as a browser draws
   a dashed border */
static void dashed(int x, int y, int w, uint32_t rgb)
{
    for (int i = 0; i < w; i += 6)
        ud_fill(win, x + i, y, i + 3 <= w ? 3 : w - i, 1, rgb);
}

/* How far (x, y) lies outside a rounded rectangle, negative inside */
static float rr_dist(float x, float y, float rx, float ry, float w, float h,
                     float r)
{
    float hx = w / 2, hy = h / 2;
    float qx = (x - rx - hx) < 0 ? -(x - rx - hx) : (x - rx - hx);
    float qy = (y - ry - hy) < 0 ? -(y - ry - hy) : (y - ry - hy);
    qx -= hx - r;
    qy -= hy - r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    float d = sqrtf(ox * ox + oy * oy);
    return d + (in < 0 ? in : 0) - r;
}

/* A cover woven from the album's colours, as the design weaves it: the
   cloth c[0], columns 'band' wide every 'period' of c[2] at 0.55 over
   it, rows of c[1] at 0.75 over those, counted from its foot - and,
   for the album's own, its shadow in the cloth's colour under it */
static void draw_cover(int x, int y, int s, int r, const uint32_t c[3],
                       int band, int period, int shadow, uint32_t under)
{
    int mx = shadow ? 30 : 0, mt = shadow ? 6 : 0, mb = shadow ? 48 : 0;
    int bw = s + 2 * mx, bh = s + mt + mb;
    uint32_t *px = malloc((size_t)bw * (size_t)bh * 4);
    if (!px)
        return;
    for (int j = 0; j < bh; j++) {
        for (int i = 0; i < bw; i++) {
            float X = (float)(i - mx) + 0.5f, Y = (float)(j - mt) + 0.5f;
            uint32_t col = under;
            if (shadow) {
                /* 0 20px 50px rgba(cloth, 0.35) */
                float d = rr_dist(X, Y, 0, 20, (float)s, (float)s, (float)r);
                float t = (d + 25) / 50;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float a = 0.35f * (1 - t * t * (3 - 2 * t));
                col = mix(c[0], col, (int)(a * 255 + 0.5f));
            }
            float cov = 0.5f - rr_dist(X, Y, 0, 0, (float)s, (float)s,
                                       (float)r);
            if (cov > 0) {
                int xi = i - mx, yi = j - mt;
                uint32_t w = c[0];
                if (xi >= 0 && xi % period < band)
                    w = mix(c[2], w, 140);        /* 0.55 */
                if (yi >= 0 && yi < s && (s - 1 - yi) % period < band)
                    w = mix(c[1], w, 191);        /* 0.75 */
                col = cov >= 1 ? w : mix(w, col, (int)(cov * 255 + 0.5f));
            }
            px[j * bw + i] = col;
        }
    }
    blit(x - mx, y - mt, bw, bh, px);
    free(px);
}

/* A block of colours into the window, in the pieces of at most 1024
   pixels the system takes at once */
static void blit(int x, int y, int w, int h, const uint32_t *px)
{
    if (w <= 0 || w > 1024)
        return;
    int rows = 1024 / w;
    for (int r = 0; r < h; r += rows)
        win_blit32(win, x, y + r, w, h - r < rows ? h - r : rows,
                   px + (size_t)r * (size_t)w);
}

static void song_colours(int song, uint32_t c[3])
{
    int a = songs[song]->album_no;
    if (a >= 0)
        memcpy(c, albums[a].c, 3 * sizeof(uint32_t));
    else
        name_colours(songs[song]->title, songs[song]->artist, c);
}

/* A triangle pointing right (or left), filled a column at a time */
static void tri(int x, int y, int w, int h, int left, uint32_t rgb)
{
    for (int i = 0; i < w; i++) {
        int k = left ? w - 1 - i : i;
        int half = (h * (w - k) + w) / (2 * w);
        ud_fill(win, x + i, y + h / 2 - half, 1, 2 * half, rgb);
    }
}

static void bars(int x, int y, uint32_t rgb)
{
    static const int hh[3] = { 8, 14, 10 };
    for (int i = 0; i < 3; i++)
        ud_fill(win, x + i * 5, y + 14 - hh[i], 3, hh[i], rgb);
}

/* --- the nav --- */

static int nav_top(int i)
{
    return NAV_TOP + i * (NAV_H + NAV_GAP) + (i >= V_PLAYLISTS ? NAV_SPLIT : 0);
}

static void draw_nav(void)
{
    ud_fill(win, 0, 0, G.nav_w, ch, C.nav);
    ud_fill(win, G.nav_w, 0, 1, ch, C.edge);
    for (int i = 0; i < V_N; i++) {
        int x = NAV_SIDE, y = nav_top(i), w = G.nav_w - 2 * NAV_SIDE;
        int on = i == view;
        if (on)
            ud_round(win, x, y, w, NAV_H, 8, C.nav_sel);
        ud_disc(win, x + 10, y + (NAV_H - NAV_DOT) / 2, NAV_DOT, nav_dye[i]);
        ud_text(win, x + 10 + NAV_DOT + 10, ud_baseline(y, NAV_H, NAV_PX),
                nav_name[i], on ? (L.dark ? 0xFFFFFF : C.text) : C.nav_fg,
                on ? UD_BOLD : UD_SANS, NAV_PX);
        hit(x, y, w, NAV_H, H_NAV, i);
    }
}

/* --- the table of songs --- */

static int song_now(void)
{
    pthread_mutex_lock(&pm);
    int c = P.cur;
    pthread_mutex_unlock(&pm);
    return c;
}

static void draw_table(int x, int y, int w, const int *list, int n,
                       int numbered, int with_artist)
{
    int cur = song_now();
    char b[200], cut[200];
    spaced(x + COL_GAP, ud_baseline(y, HEAD_H, HEAD_PX), "#", C.head, UD_BOLD,
           HEAD_PX, 8);
    spaced(x + COL_GAP + NUM_W + COL_GAP, ud_baseline(y, HEAD_H, HEAD_PX),
           "TITLE", C.head, UD_BOLD, HEAD_PX, 8);
    spaced(x + w - COL_GAP - spaced_w("TIME", UD_BOLD, HEAD_PX, 8),
           ud_baseline(y, HEAD_H, HEAD_PX), "TIME", C.head, UD_BOLD,
           HEAD_PX, 8);
    dashed(x, y + HEAD_H - 1, w, C.line);
    rows_list = list;
    rows_n = n;
    int ry = y + HEAD_H;
    if (top_row > n - 1)
        top_row = n > 0 ? n - 1 : 0;
    for (int i = top_row; i < n && ry + ROW_H <= ch - 8; i++, ry += ROW_H) {
        const struct song *s = songs[list[i]];
        int now = list[i] == cur;
        if (now)
            ud_round(win, x, ry, w, ROW_H, 8, C.sel);
        if (now) {
            bars(x + COL_GAP, ry + (ROW_H - 14) / 2, C.bars);
        } else {
            snprintf(b, sizeof(b), "%d", numbered || !s->track ? i + 1
                                                               : s->track);
            ud_text(win, x + COL_GAP, ud_baseline(ry, ROW_H, CELL_PX), b,
                    C.head, UD_SANS, CELL_PX);
        }
        int tx = x + COL_GAP + NUM_W + COL_GAP;
        int room = w - (tx - x) - COL_GAP - TIME_W - COL_GAP;
        uint32_t fg = now ? (L.dark ? 0xFFFFFF : C.text) : C.row_fg;
        int font = now ? UD_BOLD : UD_SANS;
        const char *t = fit(s->title, font, ROW_PX, room, cut, sizeof(cut));
        ud_text(win, tx, ud_baseline(ry, ROW_H, ROW_PX), t, fg, font, ROW_PX);
        if (with_artist) {
            int used = ud_width(t, font, ROW_PX) + 12;
            if (room - used > 40)
                ud_text(win, tx + used, ud_baseline(ry, ROW_H, ROW_PX),
                        fit(s->artist, UD_SANS, 13, room - used, cut,
                            sizeof(cut)), C.dim, UD_SANS, 13);
        }
        clock_words(s->ms * RATE / 1000, b, sizeof(b));
        ud_text_right(win, x + w - COL_GAP, ud_baseline(ry, ROW_H, CELL_PX), b,
                      C.dim, UD_MONO, CELL_PX);
        hit(x, ry, w, ROW_H, H_ROW, i);
    }
}

/* --- the views --- */

static void heading(const char *title, const char *sub)
{
    ud_text(win, G.main_x, MAIN_Y + VIEW_PX, title, C.text, UD_BOLD, VIEW_PX);
    if (sub)
        ud_text(win, G.main_x, MAIN_Y + VIEW_PX + 24, sub, C.dim, UD_SANS, 13);
}

static void length_words(long long ms, char *out, size_t cap)
{
    long long s = (ms + 500) / 1000;
    if (s < 60)
        snprintf(out, cap, "%lld sec", s);
    else
        snprintf(out, cap, "%lld min", (s + 30) / 60);
}

static int album_playing(int a)
{
    int c = song_now();
    return c >= 0 && songs[c]->album_no == a;
}

static void draw_album(int a)
{
    const struct album *al = &albums[a];
    int x = G.main_x, y = MAIN_Y, s = G.cover;
    draw_cover(x, y, s, COVER_R, al->c, 6, 14, 1, C.bg);
    int tx = x + s + COVER_GAP, tw = G.main_x + G.main_w - tx;
    int top = y + s - (105 + G.title_px);
    char b[260], cut[260];
    if (al->year)
        snprintf(b, sizeof(b), "ALBUM \xC2\xB7 %d", al->year);
    else
        snprintf(b, sizeof(b), "ALBUM");
    spaced(tx, top + 12, b, C.dim, UD_BOLD, KIND_PX, 10);
    int title_top = top + 23;
    ud_text(win, tx, title_top + G.title_px * 864 / 1000,
            fit(al->title, UD_BOLD, G.title_px, tw, cut, sizeof(cut)), C.text,
            UD_BOLD, G.title_px);
    int by_top = title_top + G.title_px + 8;
    char len[32];
    length_words(al->ms, len, sizeof(len));
    snprintf(b, sizeof(b), "%s \xC2\xB7 %d song%s, %s \xC2\xB7 %s", al->by,
             al->n, al->n == 1 ? "" : "s", len, al->fmts);
    ud_text(win, tx, by_top + 15, fit(b, UD_SANS, BY_PX, tw, cut, sizeof(cut)),
            C.nav_fg, UD_SANS, BY_PX);
    int by = by_top + 18 + 8 + 8;
    int paused;
    pthread_mutex_lock(&pm);
    paused = P.paused;
    pthread_mutex_unlock(&pm);
    const char *label = album_playing(a) && !paused ? "Pause" : "Play";
    int pw = ud_width(label, UD_BOLD, BTN_PX) + 40;
    ud_round(win, tx, by, pw, BTN_H, BTN_H / 2, C.play);
    ud_text_mid(win, tx, by, pw, BTN_H, label, 0xFFFFFF, UD_BOLD, BTN_PX);
    hit(tx, by, pw, BTN_H, H_PLAY, a);
    int sx = tx + pw + 10, sw = ud_width("Shuffle", UD_BOLD, BTN_PX) + 32;
    ud_round(win, sx, by, sw, BTN_H, BTN_H / 2, C.line);
    ud_round(win, sx + 1, by + 1, sw - 2, BTN_H - 2, BTN_H / 2 - 1, C.quiet);
    ud_text_mid(win, sx, by, sw, BTN_H, "Shuffle", C.text, UD_BOLD, BTN_PX);
    hit(sx, by, sw, BTN_H, H_SHUFFLE, a);
    draw_table(x, y + s + 20, G.main_w, al->list, al->n, 0, 0);
}

static void draw_grid(int only_by)
{
    int x0 = G.main_x, y = VIEW_TOP;
    int cols = (G.main_w + GRID_GAP) / (GRID_COVER + GRID_GAP);
    if (cols < 1)
        cols = 1;
    char cut[200];
    int k = 0;
    for (int a = 0; a < nalbums; a++) {
        if (only_by >= 0 && strcasecmp(albums[a].by, artists[only_by].name))
            continue;
        int cx = x0 + (k % cols) * (GRID_COVER + GRID_GAP);
        int cy = y + (k / cols - top_row) * (GRID_COVER + 64);
        k++;
        if (cy < VIEW_TOP || cy + GRID_COVER + 44 > ch)
            continue;
        draw_cover(cx, cy, GRID_COVER, 12, albums[a].c, 5, 11, 0, C.bg);
        ud_text(win, cx, cy + GRID_COVER + 20,
                fit(albums[a].title, UD_BOLD, 13, GRID_COVER, cut, sizeof(cut)),
                C.text, UD_BOLD, 13);
        ud_text(win, cx, cy + GRID_COVER + 38,
                fit(albums[a].by, UD_SANS, 12, GRID_COVER, cut, sizeof(cut)),
                C.dim, UD_SANS, 12);
        hit(cx, cy, GRID_COVER, GRID_COVER + 44, H_ALBUM, a);
    }
}

static void draw_rows(int kind)
{
    int x = G.main_x, w = G.main_w, y = VIEW_TOP;
    int n = kind == H_ARTIST ? nartists : nplists;
    char b[200], cut[200];
    for (int i = top_row; i < n && y + LIST_H <= ch - 8; i++, y += LIST_H) {
        uint32_t dot;
        const char *name;
        if (kind == H_ARTIST) {
            dot = albums[artists[i].first].c[0];
            name = artists[i].name;
            snprintf(b, sizeof(b), "%d album%s, %d song%s", artists[i].albums,
                     artists[i].albums == 1 ? "" : "s", artists[i].songs,
                     artists[i].songs == 1 ? "" : "s");
        } else {
            dot = nav_dye[V_PLAYLISTS];
            name = plists[i].name;
            snprintf(b, sizeof(b), "%d song%s", plists[i].n,
                     plists[i].n == 1 ? "" : "s");
        }
        ud_disc(win, x, y + (LIST_H - 30) / 2, 30, dot);
        ud_text(win, x + 44, y + 22, fit(name, UD_BOLD, 14, w - 44, cut,
                                         sizeof(cut)), C.text, UD_BOLD, 14);
        ud_text(win, x + 44, y + 40, b, C.dim, UD_SANS, 12);
        if (i + 1 < n)
            ud_fill(win, x + 44, y + LIST_H - 1, w - 44, 1, C.edge);
        hit(x, y, w, LIST_H, kind, i);
    }
}

static void draw_main(void)
{
    char b[120];
    switch (view) {
    case V_LISTEN: {
        int a = listen_album >= 0 ? listen_album : nalbums ? 0 : -1;
        if (a >= 0)
            draw_album(a);
        else
            heading("Listen now", "No music yet: put songs in the Music "
                    "folder of your home, and Skein finds them.");
        break;
    }
    case V_ALBUMS:
        if (open_album >= 0) {
            draw_album(open_album);
            break;
        }
        snprintf(b, sizeof(b), "%d album%s", nalbums, nalbums == 1 ? "" : "s");
        heading("Albums", b);
        draw_grid(-1);
        break;
    case V_ARTISTS:
        if (open_album >= 0) {
            draw_album(open_album);
        } else if (open_artist >= 0) {
            snprintf(b, sizeof(b), "%d album%s", artists[open_artist].albums,
                     artists[open_artist].albums == 1 ? "" : "s");
            heading(artists[open_artist].name, b);
            draw_grid(open_artist);
        } else {
            snprintf(b, sizeof(b), "%d artist%s", nartists,
                     nartists == 1 ? "" : "s");
            heading("Artists", b);
            draw_rows(H_ARTIST);
        }
        break;
    case V_SONGS:
        snprintf(b, sizeof(b), "%d song%s", nlib, nlib == 1 ? "" : "s");
        heading("Songs", b);
        draw_table(G.main_x, VIEW_TOP, G.main_w, all_songs, nlib, 1, 1);
        break;
    case V_PLAYLISTS:
        if (open_plist >= 0) {
            const struct plist *p = &plists[open_plist];
            snprintf(b, sizeof(b), "Playlist \xC2\xB7 %d song%s", p->n,
                     p->n == 1 ? "" : "s");
            heading(p->name, b);
            draw_table(G.main_x, VIEW_TOP, G.main_w, p->list, p->n, 1, 1);
        } else {
            heading("Playlists", nplists ? "The .m3u files in your Music "
                    "folder" : "No playlists: an .m3u file in the Music "
                    "folder is one.");
            draw_rows(H_PLIST);
        }
        break;
    case V_RADIO:
        heading("Radio", "Skein does not play radio stations yet: only the "
                "music on this computer.");
        break;
    case V_FILES:
        heading("From Files", nloose ? "Music elsewhere in your home, outside "
                "the Music folder" : "No music elsewhere in your home.");
        draw_table(G.main_x, VIEW_TOP, G.main_w, loose, nloose, 1, 1);
        break;
    }
}

/* --- what plays --- */

struct snap {
    int cur, paused, qpos, nq;
    long long pos, len;
    uint8_t amp[AMP_SLOTS];
    int next[3], nnext;
    char note[160];
};

static void snapshot(struct snap *s)
{
    pthread_mutex_lock(&pm);
    s->cur = P.cur;
    s->paused = P.paused;
    s->pos = P.pos;
    s->len = P.len;
    s->qpos = P.qpos;
    s->nq = P.nq;
    memcpy(s->amp, P.amp, sizeof(s->amp));
    s->nnext = 0;
    for (int k = P.qpos + 1; k < P.nq && s->nnext < 3; k++)
        s->next[s->nnext++] = P.queue[k];
    copy(s->note, P.note, sizeof(s->note));
    pthread_mutex_unlock(&pm);
    if (s->pos > s->len && s->len > 0)
        s->pos = s->len;
}

static int wave_top(void)
{
    return AS_PAD + 15 + AS_GAP + G.as_cover + AS_GAP + 40 + AS_GAP;
}

/* Where the knot is on the thread */
static int knot_x(const struct snap *s)
{
    if (s->cur < 0 || s->len <= 0)
        return 0;
    return (int)(s->pos * G.wave_w / s->len);
}

/* The bit of the curve from t0 to t1 of a cubic, by de Casteljau */
static void cubic_part(const float p[8], float t0, float t1, float out[8])
{
    float q[8];
    /* first the part [t0, 1] */
    float a[8];
    memcpy(a, p, sizeof(a));
    if (t0 > 0) {
        float x01 = a[0] + (a[2] - a[0]) * t0, y01 = a[1] + (a[3] - a[1]) * t0;
        float x12 = a[2] + (a[4] - a[2]) * t0, y12 = a[3] + (a[5] - a[3]) * t0;
        float x23 = a[4] + (a[6] - a[4]) * t0, y23 = a[5] + (a[7] - a[5]) * t0;
        float xa = x01 + (x12 - x01) * t0, ya = y01 + (y12 - y01) * t0;
        float xb = x12 + (x23 - x12) * t0, yb = y12 + (y23 - y12) * t0;
        float xm = xa + (xb - xa) * t0, ym = ya + (yb - ya) * t0;
        float n[8] = { xm, ym, xb, yb, x23, y23, a[6], a[7] };
        memcpy(a, n, sizeof(a));
        t1 = (t1 - t0) / (1 - t0);
    }
    /* then its first t1 */
    float x01 = a[0] + (a[2] - a[0]) * t1, y01 = a[1] + (a[3] - a[1]) * t1;
    float x12 = a[2] + (a[4] - a[2]) * t1, y12 = a[3] + (a[5] - a[3]) * t1;
    float x23 = a[4] + (a[6] - a[4]) * t1, y23 = a[5] + (a[7] - a[5]) * t1;
    float xa = x01 + (x12 - x01) * t1, ya = y01 + (y12 - y01) * t1;
    float xb = x12 + (x23 - x12) * t1, yb = y12 + (y23 - y12) * t1;
    float xm = xa + (xb - xa) * t1, ym = ya + (yb - ya) * t1;
    q[0] = a[0]; q[1] = a[1]; q[2] = x01; q[3] = y01;
    q[4] = xa; q[5] = ya; q[6] = xm; q[7] = ym;
    memcpy(out, q, sizeof(q));
}

static void cubic_at(const float p[8], float t, float *x, float *y)
{
    float u = 1 - t;
    *x = u * u * u * p[0] + 3 * u * u * t * p[2] + 3 * u * t * t * p[4] +
         t * t * t * p[6];
    *y = u * u * u * p[1] + 3 * u * u * t * p[3] + 3 * u * t * t * p[5] +
         t * t * t * p[7];
}

/* The waveform as a thread being spun: a turn every WAVE_SEG pixels,
   each turn as high as that part of the song was loud - the part played
   solid in the music's colour, what is still to come dashed and quiet
   (its loudness not yet known), and a white knot where it is */
static void draw_wave(const struct snap *s)
{
    int x = G.ax + AS_PAD, y = wave_top(), w = G.wave_w, mid = y + WAVE_H / 2;
    int kx = knot_x(s);
    int nseg = (w + WAVE_SEG - 1) / WAVE_SEG;
    float prev = 11;                    /* the design's first turn */
    float knot_y = (float)mid;
    float dash_left = 3;
    int dash_on = 1;
    for (int k = 0; k < nseg; k++) {
        int x0 = k * WAVE_SEG, x1 = x0 + WAVE_SEG < w ? x0 + WAVE_SEG : w;
        int lo = x0 * AMP_SLOTS / w, hi = x1 * AMP_SLOTS / w;
        int a = 0;
        for (int i = lo; i < hi && i < AMP_SLOTS; i++)
            if (s->amp[i] > a)
                a = s->amp[i];
        float amp = x1 <= kx && s->cur >= 0 ? 5 + a * 10.0f / 255
                                            : 8 + (float)((k * 37 + 11) % 5);
        if (x1 - x0 < WAVE_SEG)
            amp = 1;                    /* the thread's last bit lies flat */
        float d = (k % 2 ? -amp : amp);
        float span = (float)(x1 - x0);
        float p[8] = { (float)(x + x0), (float)mid,
                       (float)(x + x0) + span / 3, (float)mid - prev,
                       (float)(x + x0) + span * 2 / 3, (float)mid + d,
                       (float)(x + x1), (float)mid };
        prev = d;
        /* x along a turn is linear in t, so the knot's t is its x's */
        float tk = s->cur >= 0 ? (float)(kx - x0) / span : 0;
        tk = tk < 0 ? 0 : tk > 1 ? 1 : tk;
        if (tk > 0) {
            float q[8];
            cubic_part(p, 0, tk, q);
            ud_thread(win, (int)q[0], (int)q[1], (int)(q[2] + 0.5f),
                      (int)(q[3] + 0.5f), (int)(q[4] + 0.5f),
                      (int)(q[5] + 0.5f), (int)(q[6] + 0.5f),
                      (int)(q[7] + 0.5f), 48, C.bars, 255);
            if (tk < 1) {
                float yy;
                float xx;
                cubic_at(p, tk, &xx, &yy);
                knot_y = yy;
            }
        }
        if (tk < 1) {
            /* the rest: dashes three long, four apart, along the curve */
            float lx, ly;
            cubic_at(p, tk, &lx, &ly);
            float sx = lx, sy = ly;
            int steps = 24;
            for (int i = 1; i <= steps; i++) {
                float t = tk + (1 - tk) * i / steps, nx, ny;
                cubic_at(p, t, &nx, &ny);
                float dx = nx - lx, dy = ny - ly, seg = dx * dx + dy * dy;
                float len = seg > 0 ? seg : 0;
                if (len > 0) {
                    float r = len;
                    for (int j = 0; j < 8; j++)
                        r = 0.5f * (r + len / r);
                    len = r;
                }
                dash_left -= len;
                if (dash_left <= 0 || i == steps) {
                    if (dash_on)
                        ud_line(win, (int)(sx + 0.5f), (int)(sy + 0.5f),
                                (int)(nx + 0.5f), (int)(ny + 0.5f), 32,
                                C.rest);
                    if (dash_left <= 0) {
                        dash_on = !dash_on;
                        dash_left = dash_on ? 3 : 4;
                    }
                    sx = nx;
                    sy = ny;
                }
                lx = nx;
                ly = ny;
            }
        }
    }
    if (s->cur >= 0)
        ud_disc(win, x + kx - 6, (int)(knot_y + 0.5f) - 6, 12, 0xFFFFFF);
    hit(x, y, w, WAVE_H, H_WAVE, 0);

    /* the time, and how long it is */
    char a[32], b[32];
    clock_words(s->cur >= 0 ? s->pos : 0, a, sizeof(a));
    clock_words(s->cur >= 0 ? s->len : 0, b, sizeof(b));
    int ty = y + WAVE_H + AS_GAP - 10 + 12;
    ud_text(win, x, ty, a, C.dim, UD_MONO, CELL_PX);
    ud_text_right(win, x + w, ty, b, C.dim, UD_MONO, CELL_PX);
}

static void draw_aside(const struct snap *s)
{
    int x0 = G.ax, w = G.aside_w;
    char cut[200];
    ud_fill(win, x0, 0, 1, ch, C.edge);
    ud_fill(win, x0 + 1, 0, w - 1, ch, C.aside);
    int x = x0 + AS_PAD, y = AS_PAD;
    spaced(x, y + 12, "PLAYING", C.dim, UD_BOLD, KIND_PX, 8);
    y += 15 + AS_GAP;
    int cx = x0 + (w - G.as_cover) / 2;
    if (s->cur >= 0) {
        uint32_t c[3];
        song_colours(s->cur, c);
        draw_cover(cx, y, G.as_cover, AS_COVER_R, c, 8, 18, 0, C.aside);
    } else {
        ud_round(win, cx, y, G.as_cover, G.as_cover, AS_COVER_R, C.line);
        ud_round(win, cx + 1, y + 1, G.as_cover - 2, G.as_cover - 2,
                 AS_COVER_R - 1, C.aside);
    }
    y += G.as_cover + AS_GAP;
    const char *title = s->cur >= 0 ? songs[s->cur]->title : "Nothing plays";
    const char *who = s->cur >= 0 ? songs[s->cur]->artist
                    : s->note[0] ? s->note : "Choose a song, or Play an album";
    ud_text(win, x, y + 17, fit(title, UD_BOLD, NOW_PX, w - 2 * AS_PAD, cut,
                                sizeof(cut)), C.text, UD_BOLD, NOW_PX);
    ud_text(win, x, y + 37, fit(who, UD_SANS, 13, w - 2 * AS_PAD, cut,
                                sizeof(cut)), C.dim, UD_SANS, 13);
    draw_wave(s);
    y = wave_top() + WAVE_H + AS_GAP - 10 + 15 + AS_GAP;

    /* before, play or pause, next */
    int mid = x0 + w / 2, py = y;
    int bx = mid - CTL_D / 2 - CTL_GAP - CTL_SMALL, nx = mid + CTL_D / 2 + CTL_GAP;
    int sy = py + (CTL_D - CTL_SMALL) / 2;
    for (int k = 0; k < 2; k++) {
        int ox = k ? nx : bx;
        ud_disc(win, ox, sy, CTL_SMALL, C.line);
        ud_disc(win, ox + 1, sy + 1, CTL_SMALL - 2, C.aside);
        int gx = ox + (CTL_SMALL - 16) / 2, gy = sy + (CTL_SMALL - 16) / 2;
        /* a bar and a triangle, 16 across: M6 5h2v14H6zM20 5v14L9 12z */
        if (!k) {
            ud_fill(win, gx + 4, gy + 3, 2, 10, C.text);
            tri(gx + 6, gy + 3, 7, 10, 1, C.text);
        } else {
            ud_fill(win, gx + 11, gy + 3, 2, 10, C.text);
            tri(gx + 3, gy + 3, 7, 10, 0, C.text);
        }
        hit(ox, sy, CTL_SMALL, CTL_SMALL, k ? H_NEXT : H_BEFORE, 0);
    }
    ud_disc(win, mid - CTL_D / 2, py, CTL_D, C.play);
    int gx = mid - 10, gy = py + CTL_D / 2 - 10;
    if (s->cur >= 0 && !s->paused) {
        ud_round(win, gx + 5, gy + 4, 4, 12, 1, 0xFFFFFF);
        ud_round(win, gx + 11, gy + 4, 4, 12, 1, 0xFFFFFF);
    } else {
        tri(gx + 6, gy + 3, 11, 14, 0, 0xFFFFFF);
    }
    hit(mid - CTL_D / 2, py, CTL_D, CTL_D, H_TOGGLE, 0);
    y = py + CTL_D + AS_GAP;

    /* next on the thread */
    dashed(x, y, w - 2 * AS_PAD, C.line);
    spaced(x, y + 12 + 12, "NEXT ON THE THREAD", C.dim, UD_BOLD, KIND_PX, 8);
    int iy = y + 12 + 15 + 8;
    if (s->nnext) {
        int last = iy + (s->nnext - 1) * Q_STEP;
        ud_fill(win, x + 5, y + 40, 2, last + 16 - 6 - (y + 40), C.rest);
        for (int k = 0; k < s->nnext; k++) {
            int ky = iy + k * Q_STEP;
            ud_disc(win, x, ky + 4, 10, C.bars);
            ud_disc(win, x + 2, ky + 6, 6, C.aside);
            ud_text(win, x + 20, ky + 13, fit(songs[s->next[k]]->title, UD_SANS,
                                              Q_PX, w - 2 * AS_PAD - 20, cut,
                                              sizeof(cut)),
                    C.text, UD_SANS, Q_PX);
        }
    } else {
        ud_text(win, x + 20, iy + 13, "Nothing after this", C.dim, UD_SANS,
                Q_PX);
    }
}

static struct snap shown;

static void draw(void)
{
    colours();
    lay();
    nhits = 0;
    snapshot(&shown);
    ud_hold(win, 1);
    ud_fill(win, 0, 0, cw, ch, C.bg);
    draw_nav();
    draw_main();
    draw_aside(&shown);
    ud_hold(win, 0);
}

/* Only the thread and the time, as the song goes on */
static void draw_progress(void)
{
    snapshot(&shown);
    int x = G.ax + 1, y = wave_top() - 8;
    ud_hold(win, 1);
    ud_fill(win, x, y, G.aside_w - 1, WAVE_H + 8 + AS_GAP + 8, C.aside);
    /* the wave's hit is already there from the last whole drawing */
    int keep = nhits;
    draw_wave(&shown);
    nhits = keep;
    ud_hold(win, 0);
}

/* --- clicks --------------------------------------------------------------- */

static void say_view(void)
{
    char b[600];
    size_t n = 0;
    b[0] = 0;
    switch (view) {
    case V_ALBUMS:
        for (int a = 0; a < nalbums && n < sizeof(b) - 130; a++)
            n += (size_t)snprintf(b + n, sizeof(b) - n, "%s%s", a ? " | " : "",
                                  albums[a].title);
        break;
    case V_ARTISTS:
        for (int a = 0; a < nartists && n < sizeof(b) - 130; a++)
            n += (size_t)snprintf(b + n, sizeof(b) - n, "%s%s", a ? " | " : "",
                                  artists[a].name);
        break;
    case V_SONGS:
        snprintf(b, sizeof(b), "%d songs", nlib);
        break;
    case V_LISTEN:
        if (listen_album >= 0)
            snprintf(b, sizeof(b), "%s", albums[listen_album].title);
        break;
    case V_PLAYLISTS:
        for (int a = 0; a < nplists && n < sizeof(b) - 130; a++)
            n += (size_t)snprintf(b + n, sizeof(b) - n, "%s%s (%d)",
                                  a ? " | " : "", plists[a].name, plists[a].n);
        break;
    case V_FILES:
        for (int a = 0; a < nloose && n < sizeof(b) - 130; a++)
            n += (size_t)snprintf(b + n, sizeof(b) - n, "%s%s", a ? " | " : "",
                                  songs[loose[a]]->title);
        break;
    default:
        break;
    }
    printf("[skein] view %s: %s\n", nav_name[view], b);
    fflush(stdout);
}

static void click(int x, int y)
{
    for (int i = nhits - 1; i >= 0; i--) {
        const struct hit *h = &hits[i];
        if (x < h->x || x >= h->x + h->w || y < h->y || y >= h->y + h->h)
            continue;
        switch (h->kind) {
        case H_NAV:
            view = h->arg;
            open_album = open_artist = open_plist = -1;
            top_row = 0;
            if (view == V_FILES)
                gather_loose();
            say_view();
            break;
        case H_PLAY:
            if (album_playing(h->arg))
                toggle();
            else
                play_list(albums[h->arg].list, albums[h->arg].n, 0);
            break;
        case H_SHUFFLE:
            shuffle(albums[h->arg].list, albums[h->arg].n);
            break;
        case H_ROW:
            play_list(rows_list, rows_n, h->arg);
            break;
        case H_ALBUM:
            open_album = h->arg;
            top_row = 0;
            printf("[skein] opened %s\n", albums[h->arg].title);
            fflush(stdout);
            break;
        case H_ARTIST:
            open_artist = h->arg;
            top_row = 0;
            break;
        case H_PLIST:
            open_plist = h->arg;
            top_row = 0;
            printf("[skein] playlist %s: %d songs\n", plists[h->arg].name,
                   plists[h->arg].n);
            fflush(stdout);
            break;
        case H_BEFORE:
            step(-1);
            break;
        case H_NEXT:
            step(1);
            break;
        case H_TOGGLE:
            if (song_now() >= 0) {
                toggle();
            } else if (listen_album >= 0 || nalbums) {
                int a = listen_album >= 0 ? listen_album : 0;
                play_list(albums[a].list, albums[a].n, 0);
            }
            break;
        case H_WAVE:
            seek_to(x - h->x, h->w);
            break;
        }
        return;
    }
}

int main(int argc, char **argv)
{
    if (!getcwd(home, sizeof(home)))
        strcpy(home, "/");
    snprintf(music, sizeof(music), "%s/Music", home);
    static const int sz[3][2] = { { W1, H1 }, { W0, H0 }, { 760, 520 } };
    for (int i = 0; i < 3 && win < 0; i++) {
        win = (int)win_create(sz[i][0], sz[i][1], "Skein");
        cw = sz[i][0];
        ch = sz[i][1];
    }
    if (win < 0)
        return 1;
    win_truecolour(win);
    colours();
    lay();
    ud_fill(win, 0, 0, cw, ch, C.bg);
    gather();
    /* a file handed over to play (Files' Open): it, and its album */
    if (argc > 1) {
        int k = find_song(argv[1]);
        if (k < 0)
            k = add_song(probe(argv[1]));
        if (k >= 0) {
            int a = songs[k]->album_no;
            if (a >= 0) {
                int at = 0;
                while (at < albums[a].n && albums[a].list[at] != k)
                    at++;
                play_list(albums[a].list, albums[a].n, at);
            } else {
                play_list(&k, 1, 0);
            }
        }
    }
    pthread_t th;
    if (pthread_create(&th, 0, player, 0) != 0)
        return 1;
    say_view();
    draw();
    printf("[skein] drawn %d %d\n", cw, ch);
    fflush(stdout);

    int told_cur = -2, told_paused = -1, drawn_sec = -1, drawn_knot = -1;
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE) {
                pthread_mutex_lock(&pm);
                P.quit = 1;
                pthread_cond_signal(&pcond);
                pthread_mutex_unlock(&pm);
                pthread_join(th, 0);
                now_playing(win, NOW_NONE, "");
                return 0;
            }
            if (ev.type == WEV_SIZE && ev.x > 0 && ev.y > 0) {
                cw = ev.x;
                ch = ev.y;
                redraw = 1;
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
                redraw = 1;
            } else if (ev.type == WEV_SCROLL) {
                top_row -= ev.val;
                if (top_row < 0)
                    top_row = 0;
                redraw = 1;
            } else if (ev.type == WEV_KEY && ev.val == ' ') {
                toggle();
            } else if (ev.type == WEV_MEDIA) {
                set_paused(ev.val == WEV_MEDIA_PAUSE);
            } else if (ev.type == WEV_LOOK) {
                redraw = 1;
            }
        }
        struct snap s;
        snapshot(&s);
        pthread_mutex_lock(&pm);
        if (P.changed) {
            P.changed = 0;
            redraw = 1;
        }
        pthread_mutex_unlock(&pm);
        if (s.cur != told_cur || s.paused != told_paused) {
            /* the shuttle: what plays, by the song's name */
            if (s.cur >= 0)
                now_playing(win, (s.paused ? NOW_PAUSED : NOW_PLAYING) |
                            NOW_NAMED, songs[s.cur]->title);
            else if (told_cur >= 0)
                now_playing(win, NOW_NONE, "");
            told_cur = s.cur;
            told_paused = s.paused;
            redraw = 1;
        }
        int sec = s.cur >= 0 ? (int)(s.pos / RATE) : -1, kx = knot_x(&s);
        if (redraw) {
            draw();
            drawn_sec = sec;
            drawn_knot = kx;
        } else if (sec != drawn_sec || kx != drawn_knot) {
            draw_progress();
            drawn_sec = sec;
            drawn_knot = kx;
        }
        sys_sleep(30);
    }
}
