#ifndef THREADOS_SMPTHREADS_H
#define THREADOS_SMPTHREADS_H

/* Kernel threads on all cores
 *
 * A secondary core now has everything it needs to run tasks: an
 * exception table, its own timer, its own idle task and a lock on the
 * task list. This trial checks whether it really does run them - and
 * whether it does so IN PARALLEL rather than merely in turns.
 *
 * "The thread computed its share" alone would settle nothing:
 * interleaving on a single core looks exactly the same. What settles
 * it are two things a single core cannot fake - the core number
 * written down by the thread itself while computing, and TIME: the
 * same work run in parallel finishes sooner than one piece after
 * another.
 */

/* Runs the trial and prints the result. Called from the shell of both
   kernels. */
void smpthreads_test(void);

#endif
