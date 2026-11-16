#ifndef THREADOS_MIC_INFO_H
#define THREADOS_MIC_INFO_H

#include <stdint.h>

/* What a program is told about the microphone (SYS64_MIC). The same
   layout is in user/umic.h, which is what programs include. */
#define MIC_INFO  0
#define MIC_OPEN  1             /* 0, -1 none, -2 something records */
#define MIC_READ  2             /* a2 = buffer, a3 = frames: frames read */
#define MIC_CLOSE 3

struct mic_info {
    int32_t present, recording, owner;
    int32_t rate, channels;     /* 48000, 2: 16-bit samples */
    char name[56];              /* "microphone, socket 18 -> input 08" */
};

#endif
