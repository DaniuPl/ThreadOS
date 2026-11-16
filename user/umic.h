/* The microphone, as a program sees it. The layout is
   kernel/include/mic_info.h's and has to stay alike. */
#ifndef THREADOS_USER_UMIC_H
#define THREADOS_USER_UMIC_H

#include "uapi.h"

#define MIC_INFO  0
#define MIC_OPEN  1
#define MIC_READ  2
#define MIC_CLOSE 3

struct mic_info {
    int32_t present, recording, owner;
    int32_t rate, channels;
    char name[56];
};

static inline int sys_mic(int op, int64_t a, int64_t b)
{
    return (int)syscall64_3(SYS64_MIC, op, a, b);
}

#endif
