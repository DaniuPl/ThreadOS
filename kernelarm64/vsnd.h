#ifndef THREADOS_ARM64_VSND_H
#define THREADOS_ARM64_VSND_H

#include <stdint.h>
#include "virtio.h"

/* vsnd.c: the phone's speaker and microphone, virtio-sound - 48 kHz,
   16-bit, two channels each way, as the PC's sound */

/* The sound device among the devices found: 0, or -1 when none */
int vsnd_init(struct virtio_dev *devs, int n);
int vsnd_present(void);
int vsnd_mic_present(void);

/* The loudness applied to what is played: 0..100 (-1 keeps), muted
   (-1 keeps) */
void vsnd_volume(int volume, int mute);
void vsnd_get(int *volume, int *mute);

/* Playing: one owner at a time (0, -1 no speaker, -2 taken); write
   waits for room and takes all n frames (or what went before the
   device stopped answering); end plays what is queued out */
int vsnd_begin(int owner);
int vsnd_owner(void);
int vsnd_write(const int16_t *frames, uint32_t n);
int vsnd_end(void);

/* Recording, the same way round: read waits for the first frames and
   returns those there are, up to n */
int vsnd_mic_start(int owner);
int vsnd_mic_who(void);
int vsnd_mic_read(int16_t *frames, uint32_t n);
void vsnd_mic_stop(void);

#endif
