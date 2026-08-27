#ifndef PROTONOS_UHCI_H
#define PROTONOS_UHCI_H

#include <stdint.h>

/* Kontroler USB (UHCI) - warstwa transportowa
 *
 * Umie tyle, ile potrzeba, zeby dogadac sie z urzadzeniem: przesylki
 * sterujace (zapytania o deskryptory, ustawienia) i przerwaniowe
 * (dane naplywajace z klawiatury). Nie ma tu nic o klawiaturach -
 * co z tych bajtow wynika, rozstrzyga usbkbd.c.
 *
 * UHCI to najstarszy z trzech kontrolerow USB i najprostszy: sterujemy
 * nim przez porty I/O, a opisy przesylek uklada sie w liscie ramek.
 * Nowsze plyty maja tylko xHCI - ale caly kod rozpoznawania urzadzen
 * i tlumaczenia raportow HID jest od kontrolera niezalezny i przy
 * dokladaniu xHCI zostaje bez zmian.
 */

/* Szuka kontrolera po PCI i uruchamia go. 1 = gotowy. */
int uhci_init(void);

/* Ile portow ma kontroler i czy na danym cos wisi */
int uhci_ports(void);
int uhci_port_reset(int port);          /* 1 = urzadzenie gotowe do rozmowy */
int uhci_port_lowspeed(int port);

/* Przesylka sterujaca. 'setup' to 8 bajtow zapytania, dane moga isc
   w obie strony (in=1 czyta). Zwraca liczbe bajtow danych albo -1. */
int uhci_control(int addr, int lowspeed, int maxpacket,
                 const void *setup, void *data, int len, int in);

/* Jednorazowy odczyt z punktu koncowego przerwaniowego. Zwraca liczbe
   bajtow, 0 gdy urzadzenie nie ma nic do powiedzenia, -1 przy bledzie. */
int uhci_interrupt_in(int addr, int lowspeed, int endpoint, int maxpacket,
                      void *data, int len, int *toggle);

#endif
