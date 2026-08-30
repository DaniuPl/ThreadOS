#ifndef PROTONOS_SMPMEM_H
#define PROTONOS_SMPMEM_H

/* A trial of the locks: shared kernel structures under fire from every core
 *
 * Until now secondary cores could only compute on what they were
 * given - the entire state shared between them was one number and one
 * lock. For them to ever run tasks, they must first be able to call
 * into the kernel safely; and the kernel must survive it.
 *
 * This trial takes the two structures everything else reaches for -
 * the physical frame map and the kernel heap - and makes every core
 * allocate from them and hand back thousands of times at once. The
 * check is not "did it crash": every allocation gets a pattern that
 * depends on who took it and is read back immediately after. A frame
 * handed to two cores at once has two writers and the pattern does not
 * match.
 */

/* Runs the trial and prints the result. Called from the shell of both
   kernels. */
void smpmem_test(void);

#endif
