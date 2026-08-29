#ifndef PROTONOS_USBHC_H
#define PROTONOS_USBHC_H

/* Kontroler USB - warstwa wyboru
 *
 * Tak samo jak blk.c wybiera miedzy ATA, AHCI i NVMe, a netdev.c
 * miedzy RTL8139 a e1000: wyzej nikogo nie obchodzi, ktory kontroler
 * siedzi w gniezdzie. Probujemy najpierw xHCI (nowsze plyty nie maja
 * juz nic innego), potem UHCI.
 *
 * Granica biegnie tam, gdzie konczy sie ROZNICA. Reset portu, nadanie
 * urzadzeniu adresu i ustalenie rozmiaru pakietu punktu zerowego
 * wygladaja w obu kontrolerach zupelnie inaczej - w UHCI adres nadaje
 * sterownik zwyklym zapytaniem, w xHCI robi to kontroler przy okazji
 * przydzielania slotu. Wszystko to jest wiec po tej stronie granicy,
 * pod jednym slowem 'podlacz'. Powyzej zostaje juz tylko rozmowa
 * z urzadzeniem, ktora jest ta sama niezaleznie od kontrolera - i to
 * dlatego usbkbd.c nie wie, przez co mowi.
 */

/* Szuka kontrolera i uruchamia go. 1 = gotowy. */
int usbhc_init(void);

/* Nazwa znalezionego kontrolera - do wypisania przy rozruchu */
const char *usbhc_name(void);

/* Ile portow ma kontroler */
int usbhc_ports(void);

/* Podlacza urzadzenie z portu: reset, adres, rozmiar pakietu punktu
   zerowego. Zwraca uchwyt urzadzenia (>= 0) albo -1, gdy na porcie
   niczego nie ma albo nie dalo sie go obudzic. */
int usbhc_attach(int port);

/* Przesylka sterujaca do urzadzenia. 'setup' to 8 bajtow zapytania,
   dane moga isc w obie strony (in=1 czyta). Zwraca liczbe bajtow
   danych albo -1. */
int usbhc_control(int dev, const void *setup, void *data, int len, int in);

/* Zglasza punkt koncowy przerwaniowy do uzycia. xHCI musi go poznac,
   zanim cokolwiek nim posle; UHCI tylko zapamietuje. 0 = gotowe. */
int usbhc_int_endpoint(int dev, int ep, int maxpacket, int interval);

/* Jednorazowe zajrzenie do punktu przerwaniowego. Zwraca liczbe
   bajtow, 0 gdy urzadzenie nie ma nic do powiedzenia, -1 przy bledzie. */
int usbhc_int_in(int dev, int ep, void *data, int len);

#endif
