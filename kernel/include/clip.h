#ifndef PROTONOS_CLIP_H
#define PROTONOS_CLIP_H

#include <stdint.h>

/* The clipboard - the one place text waits between two windows
 *
 * Copying and pasting is the plainest thing two programs do together,
 * and it is the one thing they cannot do by themselves: neither of
 * them may touch the other's memory, and neither of them knows the
 * other exists. So the text waits somewhere that belongs to both -
 * which means the kernel.
 *
 * It is text and nothing else. No formats, no owners, no negotiating
 * what a selection means: a few hundred bytes that one program put
 * there and another takes. That is enough for a name typed in a
 * terminal and pasted into a settings window, and everything past that
 * would be a protocol nobody here needs yet.
 */

#define CLIP_MAX 512

/* Puts text in (a copy - the caller's buffer is its own business).
   Returns how much was taken; a longer text is cut, because a
   clipboard that refuses is worse than one that holds less. */
uint32_t clip_set(const char *text, uint32_t len);

/* Takes what is there into 'out' (at most 'max' bytes, always
   terminated). Returns the length. */
uint32_t clip_get(char *out, uint32_t max);

/* How much is waiting - so that a program can say 'nothing to paste'
   without asking for it first. */
uint32_t clip_len(void);

#endif
