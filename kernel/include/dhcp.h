#ifndef PROTONOS_DHCP_H
#define PROTONOS_DHCP_H

/* Klient DHCP: DISCOVER -> OFFER -> REQUEST -> ACK.
   Uruchamiany w osobnym watku, bo czeka na odpowiedzi serwera. */

/* Tworzy watek konfigurujacy siec (wolac po scheduler_init) */
void dhcp_start(void);

/* Pelna wymiana synchronicznie - tylko z kontekstu watku.
   1 = dostalismy adres, 0 = brak serwera / timeout. */
int dhcp_configure(void);

#endif
