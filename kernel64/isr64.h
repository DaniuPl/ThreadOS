#ifndef PROTONOS_ISR64_H
#define PROTONOS_ISR64_H

#include <stdint.h>

/* The trace left by the last exception that came from ring 3
 *
 * A program that breaks memory protection dies alone - and the kernel
 * carries on working. These functions make it possible to check what
 * exactly happened, after its death.
 */
int isr64_user_fault(void);
uint32_t isr64_user_fault_vector(void);
uint32_t isr64_user_fault_err(void);
uint64_t isr64_user_fault_addr(void);
void isr64_forget_user_fault(void);

/* How many heap pages came into being on the first touch - asking for
   memory costs nothing, payment is for the pages actually used */
uint32_t isr64_heap_pages(void);

/* How many pages the programs' stacks gained (a stack grows on
   demand) */
uint32_t isr64_stack_pages(void);

/* How many pages were read from files mapped into memory */
uint32_t isr64_mmap_pages(void);

#endif
