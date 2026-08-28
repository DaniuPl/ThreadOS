#ifndef PROTONOS_ISR64_H
#define PROTONOS_ISR64_H

#include <stdint.h>

/* Slad po ostatnim wyjatku, ktory przyszedl z pierscienia 3
 *
 * Program, ktory zlamie ochrone pamieci, ginie sam - a jadro pracuje
 * dalej. Te funkcje pozwalaja sprawdzic, co dokladnie sie stalo, juz
 * po jego smierci.
 */
int isr64_user_fault(void);
uint32_t isr64_user_fault_vector(void);
uint32_t isr64_user_fault_err(void);
uint64_t isr64_user_fault_addr(void);
void isr64_forget_user_fault(void);

/* Ile stron sterty powstalo przy pierwszym dotknieciu - prosba
   o pamiec nie kosztuje nic, placi sie za strony naprawde uzyte */
uint32_t isr64_heap_pages(void);

#endif
