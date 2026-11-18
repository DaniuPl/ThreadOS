#ifndef THREADOS_HDA_H
#define THREADOS_HDA_H

#include <stdint.h>

#define HDA_STREAM_MAX_FRAMES 1024

/* Intel HD Audio
 *
 * The sound controller every machine made after about 2005 has, this
 * one included. What it is not is a device one writes samples to.
 * There are two things in the machine and the driver has to arrange
 * both: the CONTROLLER, which moves bytes out of memory by itself, and
 * the CODEC behind it, which is a little graph of widgets - converters,
 * pins, mixers - that has to be walked to find out which of them ends
 * at a speaker.
 *
 * The two are talked to differently. The controller through registers.
 * The codec through a ring of commands and a ring of answers, because
 * the codec sits on a serial link and every question is a packet.
 *
 * We play by handing the controller a list of buffers and letting it
 * read them in a circle; the driver's job afterwards is only to keep
 * filling the half the controller is not reading.
 */

/* Finds a controller, resets it, finds a codec with an output and
   makes a stream ready. 1 = there is sound. */
int hda_init(void);
int hda_present(void);

/* What was found, for the startup message: the codec's maker and the
   nodes the sound goes through. */
const char *hda_name(void);

/* Sixteen-bit signed samples, two channels, 48000 per second - the
   format every codec must support, so the one format worth having.
   Plays them and returns when they have been played. 0 = done. */
int hda_play(const int16_t *samples, uint32_t frames);

/* A stream is filled one DMA half at a time. begin starts silence;
   write waits for the safe half and copies up to HDA_STREAM_MAX_FRAMES;
   end drains the last half before stopping. Only the program that
   began the stream - any of its threads - may write or end it; it is
   let go when the program's first thread ends (hda_stream_abort). */
int hda_stream_begin(void);
int hda_stream_owned(void);
int hda_stream_write(const int16_t *samples, uint32_t frames);
int hda_stream_end(void);
void hda_stream_abort(int task_id);

#define HDA_LEVELS 4
/* Who is playing (a task id, -1 = nobody), and how loud what they
   last handed over is: the peak of each quarter of the block, 0..255.
   For the desktop, which shows it; the samples, not the volume
   setting, so a quiet passage looks quiet at any loudness. */
int hda_stream_who(void);
void hda_stream_levels(uint8_t out[HDA_LEVELS]);

/* A tone of the given frequency and length. The oldest thing a
   computer does with a speaker, and the shortest way to hear whether
   any of the above is true. 0 = played. */
int hda_beep(uint32_t hz, uint32_t ms);

/* --- how loud ---------------------------------------------------------
 *
 * A codec's amplifier is not a number from nothing: every widget says
 * how many steps it has and how big a step is, and asking for more
 * than it has is answered by the command being ignored. So loudness
 * here is a PERCENTAGE of what the widget actually has, and the
 * translation is the driver's business.
 *
 * The steps are decibels, so half the steps is nowhere near half the
 * amplitude. That is not a fault to correct - it is how loudness
 * works, and a mixer that pretended otherwise would feel wrong at
 * both ends.
 */
/* Sound in: the microphone (or a line in) the codec's maker marked,
   recorded at 48 kHz, 16 bits, two channels. One recording at a time;
   start says -1 with no microphone, -2 when one is going; read hands
   out what came since the last read (up to 'frames'), and has to be
   called at least every half second. 'owner' is the program's tgid
   (-1 for the shell), and the program's end stops it (release). */
int hda_mic_present(void);
const char *hda_mic_name(void);
int hda_mic_start(int owner);
int hda_mic_read(int16_t *out, uint32_t frames);
void hda_mic_stop(void);
int hda_mic_who(void);
int hda_mic_on(void);
void hda_mic_release(int tgid);

void hda_set_volume(int percent);        /* 0..100 */
int hda_volume(void);
void hda_set_mute(int on);
int hda_muted(void);
/* How many steps the widget has, for anything that wants to say what
   the percentage is a percentage of. 0 = the codec has no amplifier
   we can move. */
int hda_volume_steps(void);

/* Writes the loudness down, so the next boot is as loud as this one.
   0 = written, -1 = there was nowhere to write it. */
int hda_remember(void);

/* --- Sound, as a program in ring 3 sees it ---------------------------
 *
 * The shape that goes through a syscall, so it lives with the driver
 * rather than with the kernel's list of calls. It must match
 * struct snd_info in user64/compat64.h.
 */
struct snd_info {
    uint32_t present;
    uint32_t volume;             /* 0..100 */
    uint32_t muted;
    uint32_t steps;              /* how many the codec's amplifier has */
};


#endif
