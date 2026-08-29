#ifndef PROTONOS_XHCI_H
#define PROTONOS_XHCI_H

/* Kontroler USB (xHCI) - warstwa transportowa
 *
 * Trzeci i ostatni z kontrolerow USB, jedyny na nowszych plytach.
 * Rozni sie od UHCI nie szczegolami, tylko podzialem pracy: tam
 * sterownik ukladal deskryptory przesylek w liscie ramek i sam pilnowal
 * adresow urzadzen; tu rozmawia sie z kontrolerem POLECENIAMI, ktore
 * wklada sie do pierscienia, a odpowiedzi przychodzia drugim
 * pierscieniem - zdarzen. Adresy urzadzen nadaje kontroler.
 *
 * Wolany tylko przez usbhc.c; usbkbd.c nie wie, ze cos takiego jest.
 */

/* Szuka kontrolera po PCI i uruchamia go. 1 = gotowy. */
int xhci_init(void);

/* Ile portow ma kontroler */
int xhci_ports(void);

/* Reset portu, przydzial slotu i nadanie adresu. Zwraca numer slotu
   (1..MAX) albo -1. */
int xhci_attach(int port);

/* Przesylka sterujaca przez punkt koncowy zerowy slotu */
int xhci_control(int slot, const void *setup, void *data, int len, int in);

/* Zaklada punkt koncowy przerwaniowy IN o numerze 'ep' */
int xhci_int_endpoint(int slot, int ep, int maxpacket, int interval);

/* Zaglada do punktu przerwaniowego. >0 = tyle bajtow, 0 = nic
   jeszcze nie przyszlo, -1 = blad. */
int xhci_int_in(int slot, int ep, void *data, int len);

#endif
