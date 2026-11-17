#ifndef THREADOS_ARM64_VIDEO64_H
#define THREADOS_ARM64_VIDEO64_H

/* The ARM64 screen (video.c): QEMU's ramfb at width x height, scale16
   panel pixels to sixteen logical ones (32 = two to one). 0 = shown;
   -1 no ramfb, -2 no memory for it, -3 fw_cfg would not take it.
   Everything else is kernel/include/video.h. */
int video_ramfb_init(int width, int height, int scale16);

#endif
