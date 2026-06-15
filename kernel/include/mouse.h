#ifndef PROTONOS_MOUSE_H
#define PROTONOS_MOUSE_H

/* Sterownik myszy PS/2 (IRQ12). Pozycja akumulowana z ruchow wzglednych,
   przycinana do zakresu ekranu (domyslnie 320x200 - gotowe pod tryb
   graficzny). Stan czytany przez mouse_get. */
void mouse_init(void);
void mouse_get(int *x, int *y, int *buttons);

#endif
