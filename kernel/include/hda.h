#ifndef PROTONOS_HDA_H
#define PROTONOS_HDA_H

#include <stdint.h>

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

/* A tone of the given frequency and length. The oldest thing a
   computer does with a speaker, and the shortest way to hear whether
   any of the above is true. 0 = played. */
int hda_beep(uint32_t hz, uint32_t ms);

#endif
