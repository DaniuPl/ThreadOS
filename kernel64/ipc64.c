/* Unix socket pairs and shared memory with descriptors (ipc64.h)
 *
 * A socket pair is two ends, each with a queue of messages waiting to
 * be read there. A message is bytes and, with them, copies of
 * descriptors: each holds what it stands for (task64.c's hold) from
 * the moment it is sent until it is put into the reader's table, so a
 * pipe or a memfd travelling through a socket stays alive on the way
 * even when the sender closes its own number at once. SOCK_SEQPACKET
 * keeps each message whole; SOCK_STREAM hands bytes over as they come,
 * though never across a message that carries descriptors, so those
 * arrive with the bytes they were sent with.
 *
 * Waiting is sleeping, not polling: a reader with nothing to read, a
 * writer whose peer's queue is full and a poll with nothing ready file
 * themselves on the ends they wait for and sleep until a change there
 * wakes them (task64_block and task64_wake, as the futexes do). The
 * pipes' ten-millisecond naps would have cost a page of WebKit a few
 * seconds in messages alone.
 *
 * A memfd is a list of frames, not one contiguous block: shared memory
 * for a picture is megabytes, and contiguous megabytes are what a
 * machine that has been running a while does not have. Mapping one
 * puts its frames into the mapping area, each with one more owner, as
 * pages that fork shares rather than copies (PG_FILE); unmapping takes
 * the owner back. The memfd itself holds one more, so the memory lives
 * while either a descriptor or a mapping does.
 *
 * A pipe is a pair too, a stream used one way: its read end only
 * reads and its write end only writes (pipe64.c), and so it sleeps,
 * polls and holds a quarter of a megabyte like a socket.
 *
 * Everything here is under one lock. User memory is never touched
 * while holding it - a page fault there would need the disk - so the
 * bytes cross into a kernel buffer first, and out of one after.
 */

#include <stddef.h>
#include "ipc64.h"
#include "elf64.h"
#include "task64.h"
#include "paging64.h"
#include "proc64.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/string.h"

#define ENDS       1024              /* 512 pairs: sockets and pipes */
#define QUEUE_MAX  (256 * 1024)      /* bytes waiting at one end */
#define WAITERS    8                 /* tasks asleep on one end */
#define WAIT_MAX   64                /* ends one IPC_WAIT watches */
#define MEMFDS     256
#define MEMFD_PAGES (256u * 1024 * 1024 / 4096)   /* 256 MiB each */

struct umsg {
    struct umsg *next;
    uint32_t len, off;
    int nfds;
    struct fd_entry fds[IPC_FDS_MAX];
    uint8_t data[];
};

struct end {
    int used;
    int refs;                        /* descriptors holding it */
    int peer;                        /* the other end; -1 once it is closed */
    int seqpacket;
    int shut_rd, shut_wr;
    struct umsg *head, *tail;
    uint32_t queued;                 /* bytes waiting here */
    struct task64 *waiting[WAITERS];
};

struct mfd {
    int used, refs;
    uint32_t pages;                  /* frames held: the size rounded up */
    uint64_t size;                   /* bytes, as fstat says */
    uint32_t *frames;
};

static struct end ends[ENDS];
static struct mfd mfds[MEMFDS];
static struct spinlock lock = SPINLOCK_FREE;

/* --- waiting ----------------------------------------------------------- */

static int add_waiter(struct end *e, struct task64 *t)
{
    for (int i = 0; i < WAITERS; i++)
        if (e->waiting[i] == t)
            return 1;
    for (int i = 0; i < WAITERS; i++)
        if (!e->waiting[i]) {
            e->waiting[i] = t;
            return 1;
        }
    return 0;
}

static void del_waiter(struct end *e, struct task64 *t)
{
    for (int i = 0; i < WAITERS; i++)
        if (e->waiting[i] == t)
            e->waiting[i] = 0;
}

static void wake_all(struct end *e)
{
    for (int i = 0; i < WAITERS; i++)
        if (e->waiting[i])
            task64_wake(e->waiting[i]);
}

/* Entered with the lock held (and 'flags' from taking it); sleeps until
   a change at one of the ends, the time ('ticks', 0 = none) or a
   signal, and comes back without the lock. With no room to file
   itself everywhere it naps a tick instead: slower, still correct. */
static void sleep_on(struct end **es, int n, irqflags_t flags,
                     uint32_t ticks)
{
    struct task64 *me = task64_self();
    int filed = 1;
    for (int i = 0; i < n; i++)
        filed &= add_waiter(es[i], me);
    task64_block_prepare();
    spin_unlock_irq(&lock, flags);
    task64_block(filed ? ticks : 1);
    flags = spin_lock_irq(&lock);
    for (int i = 0; i < n; i++)
        del_waiter(es[i], me);
    spin_unlock_irq(&lock, flags);
}

/* --- messages ------------------------------------------------------------ */

/* A message's descriptors let go - outside the lock, since letting go
   of a socket end takes it */
static void msg_free(struct umsg *m)
{
    for (int i = 0; i < m->nfds; i++)
        task64_fd_drop(&m->fds[i]);
    kfree(m);
}

/* The end behind a descriptor, for 'op': a socket's for anything, a
   pipe's read end only to read (IPC_RECV) and its write end only to
   write (IPC_SEND), both to be polled and waited on */
static int end_for(int fd, int op)
{
    int kind = task64_fd_kind(fd);
    if (kind != FDK_USOCK && kind != FDK_POTOK_R && kind != FDK_POTOK_W)
        return -1;
    if ((kind == FDK_POTOK_R && (op == IPC_SEND || op == IPC_SHUTDOWN)) ||
        (kind == FDK_POTOK_W && (op == IPC_RECV || op == IPC_SHUTDOWN)))
        return -1;
    int e = task64_fd_slot(fd);
    return e >= 0 && e < ENDS ? e : -1;
}

/* Bytes and descriptors from ring 3 into a message: 'buf' and 'fdn'
   are the program's */
static struct umsg *msg_make(const void *buf, uint32_t len, const int *fdn,
                             int nfds, long *err)
{
    struct umsg *m = kmalloc(sizeof(*m) + len);
    if (!m) {
        *err = -IPC_ENOMEM;
        return 0;
    }
    m->next = 0;
    m->len = len;
    m->off = 0;
    m->nfds = 0;
    if (len)
        memcpy(m->data, buf, len);
    for (int i = 0; i < nfds; i++) {
        if (task64_fd_copy(fdn[i], &m->fds[m->nfds]) < 0) {
            msg_free(m);
            *err = -IPC_EBADF;
            return 0;
        }
        m->nfds++;
    }
    return m;
}

static long send_msg(int me, struct umsg *m, int dontwait)
{
    /* No bytes on a stream is nothing at all - queued, it would read
       as the end of the data. (Linux sends no descriptors with it
       either.) A seqpacket's empty message is a message. */
    if (!m->len && !ends[me].seqpacket) {
        msg_free(m);
        return 0;
    }
    for (;;) {
        irqflags_t flags = spin_lock_irq(&lock);
        struct end *e = &ends[me];
        if (!e->used || e->shut_wr || e->peer < 0 ||
            ends[e->peer].shut_rd) {
            spin_unlock_irq(&lock, flags);
            msg_free(m);
            return -IPC_EPIPE;
        }
        struct end *p = &ends[e->peer];
        /* a message that is bigger than the whole queue still goes,
           once the queue is empty - otherwise it never would */
        if (!p->queued || p->queued + m->len <= QUEUE_MAX) {
            if (p->tail)
                p->tail->next = m;
            else
                p->head = m;
            p->tail = m;
            p->queued += m->len;
            uint32_t len = m->len;
            wake_all(p);
            spin_unlock_irq(&lock, flags);
            return (long)len;
        }
        if (dontwait) {
            spin_unlock_irq(&lock, flags);
            msg_free(m);
            return -IPC_EAGAIN;
        }
        sleep_on(&p, 1, flags, 0);
        if (task64_signal_pending()) {
            msg_free(m);
            return -IPC_EINTR;
        }
    }
}

/* One receive into ring 3's 'buf'; descriptors into 'fdn' (room for
   'maxfds'), how many through '*nfds', how many found no room through
   '*lost'. A stream hands over what one message has; a seqpacket the
   whole message, cut to 'len' - '*whole_len' says how long it was, for
   MSG_TRUNC. */
static long recv_msg(int me, void *buf, uint32_t len, int *fdn, int maxfds,
                     int *nfds, int *lost, uint32_t *whole_len, int flags_in)
{
    *nfds = 0;
    *lost = 0;
    *whole_len = 0;
    for (;;) {
        irqflags_t flags = spin_lock_irq(&lock);
        struct end *e = &ends[me];
        struct umsg *m = e->head;
        if (m) {
            uint32_t avail = m->len - m->off;
            uint32_t take = avail < len ? avail : len;
            *whole_len = e->seqpacket ? avail : take;
            if (flags_in & IPC_PEEK) {
                uint8_t *tmp = take ? kmalloc(take) : 0;
                if (take && !tmp) {
                    spin_unlock_irq(&lock, flags);
                    return -IPC_ENOMEM;
                }
                if (take)
                    memcpy(tmp, m->data + m->off, take);
                spin_unlock_irq(&lock, flags);
                if (take) {
                    memcpy(buf, tmp, take);
                    kfree(tmp);
                }
                return (long)take;
            }
            /* The descriptors go with the message's first bytes */
            struct fd_entry got[IPC_FDS_MAX];
            int ngot = m->nfds;
            for (int i = 0; i < ngot; i++)
                got[i] = m->fds[i];
            m->nfds = 0;
            struct umsg *whole = 0;
            uint8_t *tmp = 0;
            if (e->seqpacket || take == avail) {
                /* the message leaves the queue */
                e->head = m->next;
                if (!e->head)
                    e->tail = 0;
                e->queued -= avail;
                whole = m;
            } else {
                tmp = kmalloc(take);
                if (!tmp) {
                    for (int i = 0; i < ngot; i++)   /* put them back */
                        m->fds[i] = got[i];
                    m->nfds = ngot;
                    spin_unlock_irq(&lock, flags);
                    return -IPC_ENOMEM;
                }
                memcpy(tmp, m->data + m->off, take);
                m->off += take;
                e->queued -= take;
            }
            wake_all(e);                 /* room for a writer */
            spin_unlock_irq(&lock, flags);

            if (whole) {
                if (take)
                    memcpy(buf, whole->data + whole->off, take);
                kfree(whole);            /* its descriptors are in 'got' */
            } else {
                memcpy(buf, tmp, take);
                kfree(tmp);
            }
            for (int i = 0; i < ngot; i++) {
                int fd = i < maxfds ? task64_fd_install(&got[i]) : -1;
                if (i >= maxfds)
                    task64_fd_drop(&got[i]);
                if (fd < 0)
                    (*lost)++;
                else
                    fdn[(*nfds)++] = fd;
            }
            return (long)take;
        }
        if (e->shut_rd || e->peer < 0 || ends[e->peer].shut_wr) {
            spin_unlock_irq(&lock, flags);
            return 0;                    /* the end of the data */
        }
        if (flags_in & IPC_DONTWAIT) {
            spin_unlock_irq(&lock, flags);
            return -IPC_EAGAIN;
        }
        sleep_on(&e, 1, flags, 0);
        if (task64_signal_pending())
            return -IPC_EINTR;
    }
}

/* What an end is ready for. Called with the lock held. */
static int ready(int me)
{
    struct end *e = &ends[me];
    int r = 0;
    if (e->head || e->shut_rd || e->peer < 0 || ends[e->peer].shut_wr)
        r |= IPC_IN;
    if (e->peer >= 0 && !e->shut_wr && ends[e->peer].queued < QUEUE_MAX)
        r |= IPC_OUT;
    if (e->peer < 0)
        r |= IPC_HUP;
    if (e->head)
        r |= IPC_DATA;
    return r;
}

/* What fstat and FIONREAD say of an end: bytes waiting, and its type */
void usock64_info(int me, uint32_t *queued, int *type)
{
    *queued = 0;
    *type = 1;
    if (me < 0 || me >= ENDS)
        return;
    irqflags_t flags = spin_lock_irq(&lock);
    *queued = ends[me].queued;
    *type = ends[me].seqpacket ? 5 : 1;
    spin_unlock_irq(&lock, flags);
}

/* --- holds --------------------------------------------------------------- */

void usock64_ref(int me)
{
    if (me < 0 || me >= ENDS)
        return;
    irqflags_t flags = spin_lock_irq(&lock);
    ends[me].refs++;
    spin_unlock_irq(&lock, flags);
}

void usock64_unref(int me)
{
    if (me < 0 || me >= ENDS)
        return;
    struct umsg *left = 0;
    irqflags_t flags = spin_lock_irq(&lock);
    struct end *e = &ends[me];
    if (e->used && --e->refs <= 0) {
        left = e->head;
        e->head = e->tail = 0;
        e->queued = 0;
        if (e->peer >= 0) {
            ends[e->peer].peer = -1;
            wake_all(&ends[e->peer]);    /* it reads its end now */
        }
        wake_all(e);
        e->used = 0;
    }
    spin_unlock_irq(&lock, flags);
    while (left) {
        struct umsg *next = left->next;
        msg_free(left);
        left = next;
    }
}

/* --- shared memory ----------------------------------------------------------- */

void memfd64_ref(int m)
{
    if (m < 0 || m >= MEMFDS)
        return;
    irqflags_t flags = spin_lock_irq(&lock);
    mfds[m].refs++;
    spin_unlock_irq(&lock, flags);
}

void memfd64_unref(int m)
{
    if (m < 0 || m >= MEMFDS)
        return;
    uint32_t *frames = 0, pages = 0;
    irqflags_t flags = spin_lock_irq(&lock);
    struct mfd *f = &mfds[m];
    if (f->used && --f->refs <= 0) {
        frames = f->frames;
        pages = f->pages;
        f->frames = 0;
        f->pages = 0;
        f->size = 0;
        f->used = 0;
    }
    spin_unlock_irq(&lock, flags);
    /* the pages still mapped somewhere keep their own owners */
    for (uint32_t i = 0; i < pages; i++)
        pmm_free_frame(frames[i]);
    if (frames)
        kfree(frames);
}

uint64_t memfd64_size(int m)
{
    if (m < 0 || m >= MEMFDS)
        return 0;
    return mfds[m].size;
}

/* The size in bytes. What a shorter size cuts off inside its last
   page is cleared: grown again, a file reads zeros there, not what
   was written before. */
static void set_size(int m, uint64_t size)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct mfd *f = &mfds[m];
    uint64_t was = f->size;
    f->size = size;
    uint32_t frame = 0;
    if (size < was && (size & 4095) && size / 4096 < f->pages) {
        frame = f->frames[size / 4096];
        pmm_frame_ref(frame);
    }
    spin_unlock_irq(&lock, flags);
    if (frame) {
        memset((uint8_t *)phys_ptr(frame) + (size & 4095), 0,
               4096 - (size & 4095));
        pmm_free_frame(frame);
    }
}

/* ftruncate: the frames to reach 'size' made (zeroed) or let go of. The
   frames are found outside the lock - megabytes of them take a while -
   and the list changes under it. */
int memfd64_resize(int m, uint64_t size)
{
    if (m < 0 || m >= MEMFDS || !mfds[m].used)
        return -IPC_EBADF;
    uint64_t want64 = (size + 4095) / 4096;
    if (want64 > MEMFD_PAGES)
        return -IPC_ENOMEM;
    uint32_t want = (uint32_t)want64;
    uint32_t have = mfds[m].pages;
    if (want == have) {
        set_size(m, size);
        return 0;
    }
    uint32_t *fresh = want ? kmalloc(want * sizeof(uint32_t)) : 0;
    if (want && !fresh)
        return -IPC_ENOMEM;
    for (uint32_t i = have; i < want; i++) {
        uint32_t frame = pmm_alloc_user_frame();
        if (!frame) {
            for (uint32_t k = have; k < i; k++)
                pmm_free_frame(fresh[k]);
            kfree(fresh);
            return -IPC_ENOMEM;
        }
        memset(phys_ptr(frame), 0, 4096);
        fresh[i] = frame;
    }
    irqflags_t flags = spin_lock_irq(&lock);
    struct mfd *f = &mfds[m];
    if (f->pages != have) {
        /* another thread resized it meanwhile: start again from its size */
        spin_unlock_irq(&lock, flags);
        for (uint32_t i = have; i < want; i++)
            pmm_free_frame(fresh[i]);
        if (fresh)
            kfree(fresh);
        return memfd64_resize(m, size);
    }
    uint32_t *old = f->frames;
    uint32_t keep = have < want ? have : want;
    for (uint32_t i = 0; i < keep; i++)
        fresh[i] = old[i];
    uint32_t *drop = old;
    uint32_t from = want, to = have;
    f->frames = fresh;
    f->pages = want;
    spin_unlock_irq(&lock, flags);
    set_size(m, size);
    for (uint32_t i = from; i < to; i++)
        pmm_free_frame(drop[i]);
    if (drop)
        kfree(drop);
    return 0;
}

/* read() and write() on a memfd: it is a file in memory, and some
   programs fill one with write() before they send it (a keyboard map,
   say). A write past the end makes it longer first, as a file grows.
   Each page is held while it is copied, so a resize that drops it
   meanwhile does not free it under the copy. */
long memfd64_rw(int m, uint32_t pos, void *buf, uint32_t len, int write)
{
    if (m < 0 || m >= MEMFDS || !mfds[m].used)
        return -IPC_EBADF;
    if (write && len && (uint64_t)pos + len > memfd64_size(m)) {
        int r = memfd64_resize(m, (uint64_t)pos + len);
        if (r < 0)
            return r;
    }
    if (!write) {
        uint64_t size = memfd64_size(m);
        if (pos >= size)
            return 0;
        if (len > size - pos)
            len = (uint32_t)(size - pos);
    }
    uint32_t done = 0;
    while (done < len) {
        uint64_t at = (uint64_t)pos + done;
        uint32_t page = (uint32_t)(at / 4096), in = (uint32_t)(at % 4096);
        uint32_t part = 4096 - in < len - done ? 4096 - in : len - done;
        irqflags_t flags = spin_lock_irq(&lock);
        uint32_t frame = 0;
        if (page < mfds[m].pages) {
            frame = mfds[m].frames[page];
            pmm_frame_ref(frame);
        }
        spin_unlock_irq(&lock, flags);
        if (!frame)
            break;                       /* the end of it */
        uint8_t *mem = (uint8_t *)phys_ptr(frame) + in;
        if (write)
            memcpy(mem, (const uint8_t *)buf + done, part);
        else
            memcpy((uint8_t *)buf + done, mem, part);
        pmm_free_frame(frame);
        done += part;
    }
    return (long)done;
}

static long memfd_new(void)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int m = -1;
    for (int i = 0; i < MEMFDS; i++)
        if (!mfds[i].used) {
            mfds[i].used = 1;
            mfds[i].refs = 1;
            mfds[i].pages = 0;
            mfds[i].size = 0;
            mfds[i].frames = 0;
            m = i;
            break;
        }
    spin_unlock_irq(&lock, flags);
    if (m < 0)
        return -IPC_ENOMEM;
    int fd = task64_fd_new(FDK_MEMFD, m);
    if (fd < 0) {
        memfd64_unref(m);
        return -IPC_EMFILE;
    }
    return fd;
}

/* The memfd's pages from 'off' at a fresh address of the mapping area */
static long memfd_map(int fd, uint64_t off, uint64_t len)
{
    if (task64_fd_kind(fd) != FDK_MEMFD || (off & 4095) || !len)
        return -IPC_EINVAL;
    int m = task64_fd_slot(fd);
    uint64_t pages = (len + 4095) / 4096;
    uint64_t first = off / 4096;
    uint64_t addr = proc64_mmap(pages * 4096);
    if (!addr)
        return -IPC_ENOMEM;
    for (uint64_t i = 0; i < pages; i++) {
        irqflags_t flags = spin_lock_irq(&lock);
        uint32_t frame = 0;
        if (mfds[m].used && first + i < mfds[m].pages) {
            frame = mfds[m].frames[first + i];
            pmm_frame_ref(frame);
        }
        spin_unlock_irq(&lock, flags);
        if (!frame || paging64_map(addr + i * 4096, frame,
                                   PG_USER | PG_WRITE | PG_FILE | PG_NX) < 0) {
            if (frame)
                pmm_free_frame(frame);
            for (uint64_t k = 0; k < i; k++) {
                uint64_t f = paging64_unmap(addr + k * 4096);
                if (f)
                    pmm_free_frame((uint32_t)f);
            }
            return -IPC_EINVAL;         /* past its end, or no memory */
        }
    }
    return (long)addr;
}

/* munmap of a memfd's pages: each loses this mapping as an owner */
static long memfd_unmap(uint64_t addr, uint64_t len)
{
    if ((addr & 4095) || addr < USER64_MMAP_BASE || len > USER64_MMAP_MAX ||
        addr > USER64_MMAP_MAX - len)
        return -IPC_EINVAL;
    for (uint64_t va = addr; va < addr + len; va += 4096) {
        uint64_t f = paging64_unmap(va);
        if (f)
            pmm_free_frame((uint32_t)f);
    }
    return 0;
}

/* --- the calls ---------------------------------------------------------------- */

/* Two connected ends, each held once */
static int new_pair(int type, int *pa, int *pb)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int a = -1, b = -1;
    for (int i = 0; i < ENDS && b < 0; i++)
        if (!ends[i].used) {
            if (a < 0)
                a = i;
            else
                b = i;
        }
    if (b < 0) {
        spin_unlock_irq(&lock, flags);
        return -IPC_ENOMEM;
    }
    for (int k = 0; k < 2; k++) {
        struct end *e = &ends[k ? b : a];
        memset(e, 0, sizeof(*e));
        e->used = 1;
        e->refs = 1;
        e->peer = k ? a : b;
        e->seqpacket = type == 5;
    }
    spin_unlock_irq(&lock, flags);
    *pa = a;
    *pb = b;
    return 0;
}

int usock64_pipe(int *reader, int *writer)
{
    return new_pair(1, reader, writer);
}

static long pair(int type, uint64_t sv)
{
    if (type != 1 && type != 5)
        return -IPC_EINVAL;
    if (!paging64_user_writable(sv, 2 * sizeof(int)))
        return -IPC_EINVAL;
    int a, b;
    if (new_pair(type, &a, &b) < 0)
        return -IPC_ENOMEM;
    int fa = task64_fd_new(FDK_USOCK, a);
    int fb = fa >= 0 ? task64_fd_new(FDK_USOCK, b) : -1;
    if (fb < 0) {
        if (fa >= 0)
            task64_fd_close(fa);
        else
            usock64_unref(a);
        usock64_unref(b);
        return -IPC_EMFILE;
    }
    int *out = (int *)(uintptr_t)sv;
    out[0] = fa;
    out[1] = fb;
    return 0;
}

static long io_call(int op, int fd, uint64_t iop)
{
    int me = end_for(fd, op);
    if (me < 0)
        return -IPC_EBADF;
    if (!paging64_user_writable(iop, sizeof(struct ipc_io)))
        return -IPC_EINVAL;
    struct ipc_io *uio = (struct ipc_io *)(uintptr_t)iop;
    struct ipc_io io = *uio;
    if (io.nfds > IPC_FDS_MAX)
        io.nfds = op == IPC_SEND ? (uint32_t)-1 : IPC_FDS_MAX;
    if (io.nfds == (uint32_t)-1)
        return -IPC_EINVAL;
    if (io.nfds && !(op == IPC_SEND
                     ? paging64_user_range(io.fds, io.nfds * sizeof(int))
                     : paging64_user_writable(io.fds, io.nfds * sizeof(int))))
        return -IPC_EINVAL;
    int fdn[IPC_FDS_MAX];
    if (op == IPC_SEND) {
        if (io.len > IPC_MSG_MAX) {
            if (ends[me].seqpacket)
                return -IPC_EMSGSIZE;
            io.len = IPC_MSG_MAX;       /* a stream takes what it can */
        }
        if (io.len && !paging64_user_range(io.buf, io.len))
            return -IPC_EINVAL;
        for (uint32_t i = 0; i < io.nfds; i++)
            fdn[i] = ((const int *)(uintptr_t)io.fds)[i];
        long err = 0;
        struct umsg *m = msg_make((const void *)(uintptr_t)io.buf, io.len,
                                  fdn, (int)io.nfds, &err);
        if (!m)
            return err;
        return send_msg(me, m, io.flags & IPC_DONTWAIT);
    }
    if (io.len > IPC_MSG_MAX)
        io.len = IPC_MSG_MAX;
    if (io.len && !paging64_user_writable(io.buf, io.len))
        return -IPC_EINVAL;
    int got = 0, lost = 0;
    uint32_t whole = 0;
    long n = recv_msg(me, (void *)(uintptr_t)io.buf, io.len, fdn,
                      (int)io.nfds, &got, &lost, &whole, (int)io.flags);
    for (int i = 0; i < got; i++)
        ((int *)(uintptr_t)io.fds)[i] = fdn[i];
    uio->nfds = (uint32_t)got;
    uio->truncated = (uint32_t)lost;
    uio->len = whole;
    return n;
}

static long wait_many(uint64_t fdsp, uint64_t n, int64_t ms)
{
    if (n > WAIT_MAX || (n && !paging64_user_range(fdsp, n * sizeof(int))))
        return -IPC_EINVAL;
    struct end *es[WAIT_MAX * 2];
    int idx[WAIT_MAX], want[WAIT_MAX];
    int k = 0;
    for (uint64_t i = 0; i < n; i++) {
        int v = ((const int *)(uintptr_t)fdsp)[i];
        int e = end_for(v & 0xFFFF, IPC_WAIT);
        if (e >= 0) {
            idx[k] = e;
            want[k] = (v >> 16) & (IPC_IN | IPC_OUT);
            k++;
        }
    }
    uint32_t ticks = ms < 0 ? 0 : (uint32_t)((ms + 9) / 10);
    int slept = 0;
    for (;;) {
        irqflags_t flags = spin_lock_irq(&lock);
        int any = 0;
        int m = 0;
        for (int i = 0; i < k; i++) {
            if (ready(idx[i]) & (want[i] | IPC_HUP))
                any++;
            es[m++] = &ends[idx[i]];
            if (ends[idx[i]].peer >= 0)
                es[m++] = &ends[ends[idx[i]].peer];
        }
        if (any || !k || ms == 0 || slept) {
            spin_unlock_irq(&lock, flags);
            return any;
        }
        sleep_on(es, m, flags, ms < 0 ? 0 : (ticks ? ticks : 1));
        if (task64_signal_pending())
            return -IPC_EINTR;
        slept = 1;      /* woken or timed out: one more look, then back */
    }
}

long ipc64_call(uint64_t op, uint64_t a, uint64_t b, uint64_t c)
{
    switch (op) {
    case IPC_PAIR:
        return pair((int)a, b);
    case IPC_SEND:
    case IPC_RECV:
        return io_call((int)op, (int)a, b);
    case IPC_POLL: {
        int me = end_for((int)a, IPC_POLL);
        if (me < 0)
            return -IPC_EBADF;
        irqflags_t flags = spin_lock_irq(&lock);
        int r = ready(me);
        spin_unlock_irq(&lock, flags);
        return r;
    }
    case IPC_WAIT:
        return wait_many(a, b, (int64_t)c);
    case IPC_SHUTDOWN: {
        int me = end_for((int)a, IPC_SHUTDOWN);
        if (me < 0)
            return -IPC_EBADF;
        irqflags_t flags = spin_lock_irq(&lock);
        struct end *e = &ends[me];
        if (b == 0 || b == 2)
            e->shut_rd = 1;
        if (b == 1 || b == 2)
            e->shut_wr = 1;
        wake_all(e);
        if (e->peer >= 0)
            wake_all(&ends[e->peer]);
        spin_unlock_irq(&lock, flags);
        return 0;
    }
    case IPC_MEMFD:
        return memfd_new();
    case IPC_MAP:
        return memfd_map((int)a, b, c);
    case IPC_UNMAP:
        return memfd_unmap(a, b);
    case IPC_NULL: {
        /* A program's standard input from nowhere, its output to
           nowhere - and it has to outlive exec as descriptor 0 or 1,
           so it is the kernel's, not a made-up file in libc */
        int fd = task64_fd_new(FDK_NULL, 0);
        return fd < 0 ? -IPC_EMFILE : fd;
    }
    }
    return -IPC_EINVAL;
}

long usock64_read(int me, void *buf, uint32_t len)
{
    int got = 0, lost = 0;
    uint32_t whole;
    int none[1];
    return recv_msg(me, buf, len, none, 0, &got, &lost, &whole, 0);
}

long usock64_write(int me, const void *buf, uint32_t len)
{
    long err = 0;
    if (len > IPC_MSG_MAX)
        len = IPC_MSG_MAX;
    struct umsg *m = msg_make(buf, len, 0, 0, &err);
    if (!m)
        return err;
    return send_msg(me, m, 0);
}
