#ifndef _LINUX_MEMFD_H
#define _LINUX_MEMFD_H

/* <linux/memfd.h> for programs written for Linux: memfd_create()'s
   flags. WebKit's shared memory (WebCore/platform/unix/
   SharedMemoryUnix.cpp) asks for memfds only when this header is
   there, and otherwise names files under /dev/shm, which ThreadOS does
   not have. musl's <sys/mman.h> has the same flags under _GNU_SOURCE,
   hence the guards. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC       0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef MFD_HUGETLB
#define MFD_HUGETLB       0x0004U
#endif
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL   0x0008U
#endif
#ifndef MFD_EXEC
#define MFD_EXEC          0x0010U
#endif

#endif
