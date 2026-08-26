/* Sprawdzanie sciezki pakietow w jadrze na uszkodzonych danych
 *
 * net.c i tcp.c rozbieraja ramki przychodzace z sieci - czyli dane,
 * ktore moze podac ktokolwiek. Blad w tym miejscu nie konczy sie
 * smiercia procesu, tylko padnieciem calego systemu, wiec warto
 * przepuscic ten kod przez sanitizery na hoscie.
 *
 * Zrodla jadra sa wciagane wprost; zaslepki w kstub/include udaja
 * sprzet, scheduler i wypisywanie.
 *
 * Uzycie: kernel_harness <plik z ramka>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "tcp.h"

/* net.c i tcp.c kompilujemy osobno (kstub/net.c i kstub/tcp.c to
   dowiazania do zrodel jadra, obok katalogu z zaslepkami) */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uzycie: %s <plik z ramka>\n", argv[0]);
        return 2;
    }

    static unsigned char scratch[65536];
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    size_t len = fread(scratch, 1, sizeof(scratch), f);
    fclose(f);

    /* Bufor dokladnie tej dlugosci - inaczej odczyt za koncem ramki
       zmiescilby sie w zapasie i przeszedlby niezauwazony */
    unsigned char *frame = malloc(len ? len : 1);
    memcpy(frame, scratch, len);

    /* System z adresem, zeby pakiety nie odpadaly na pierwszym warunku */
    net_set_config(IP4(10, 0, 2, 15), IP4(255, 255, 255, 0),
                   IP4(10, 0, 2, 2), IP4(10, 0, 2, 3));

    /* Gniazda i polaczenia, zeby ruch mial gdzie trafic */
    net_udp_open(53, -1);
    net_udp_open(7777, -1);
    tcp_listen(5555, -1);

    net_rx(frame, (uint16_t)len);
    net_rx(frame, (uint16_t)len);      /* drugi raz: stan sie zmienil */

    free(frame);
    return 0;
}
