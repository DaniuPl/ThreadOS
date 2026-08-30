#ifndef PROTONOS_SHELL_H
#define PROTONOS_SHELL_H

/* Registers the shell as the keyboard sink, prints the prompt and
   starts the thread in which commands run (the interrupt itself only
   buffers) */
void shell_init(void);

/* Redraws the prompt after an asynchronous kernel message */
void shell_refresh(void);

#endif
