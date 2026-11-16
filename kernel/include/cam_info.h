#ifndef THREADOS_CAM_INFO_H
#define THREADOS_CAM_INFO_H

#include <stdint.h>

/* What a program is told about the camera, and how it asks for frames
   (SYS64_CAMERA). The same layout is in user/ucam.h, which is what
   programs include: a program cannot reach the kernel's headers, so
   the two have to be kept alike. */

#define CAM_INFO  0             /* a2 = struct cam_info * */
#define CAM_OPEN  1             /* a2 = mode: 0, or what uvc_start says */
#define CAM_FRAME 2             /* a2 = struct cam_frame *: bytes, 0 none newer */
#define CAM_CLOSE 3

#define CAM_MODES 16

struct cam_mode_info {
    uint32_t fourcc;            /* 'MJPG', 'YUY2' */
    uint16_t w, h;
    uint32_t interval;          /* 100 ns */
    int32_t alt;                /* 0 bulk, -1 none carries it, -2 no answer */
};

struct cam_info {
    int32_t present;
    int32_t streaming;          /* the mode streaming, or -1 */
    int32_t owner;              /* the program streaming (tgid), or -1 */
    int32_t nmodes;
    uint32_t frames, dropped, lost;
    char name[16];              /* "0c45:6366" */
    char why[96];               /* with no camera: why */
    struct cam_mode_info modes[CAM_MODES];
};

struct cam_frame {
    uint64_t buf;               /* the program's buffer ... */
    uint32_t cap;               /* ... and its size */
    uint32_t seq;               /* in: the last frame it has; out: this one */
    uint32_t bytes;             /* out: the frame's length */
    uint32_t fourcc;
    uint16_t w, h;
};

#endif
