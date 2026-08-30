#ifndef PROTONOS_DYNLINK_H
#define PROTONOS_DYNLINK_H

#include <stdint.h>

/* Biblioteka wspoldzielona i wiazanie symboli przy ladowaniu
 *
 * Do tej pory kazdy program niosl wlasna kopie wszystkiego, czego
 * uzywal. Przy kryptografii i TLS-ie znaczylo to po kilkadziesiat
 * kilobajtow tego samego kodu w kazdym z kilku programow - raz na
 * dysku, raz w initrd i raz w pamieci, osobno dla kazdego procesu.
 *
 * Biblioteka wspoldzielona rozwiazuje to tak, jak rozwiazuje sie to
 * wszedzie: kod lezy raz, pod stalym adresem, ten sam we wszystkich
 * przestrzeniach adresowych - te same ramki fizyczne. Program nie ma
 * go w sobie; ma tylko tablice pustych miejsc (GOT), ktore jadro
 * wypelnia przy ladowaniu adresami z biblioteki.
 *
 * Wiazemy WSZYSTKO od razu, przy starcie programu, a nie przy
 * pierwszym wywolaniu. Leniwe wiazanie wymagaloby, zeby czesc
 * konsolidatora siedziala w przestrzeni procesu i byla wolana
 * z zaslepki PLT - a to caly osobny mechanizm po to, zeby oszczedzic
 * mikrosekundy przy starcie.
 *
 * Ten plik nie wie nic o stronicowaniu. Wyszukiwanie symboli i chodzenie
 * po relokacjach jest identyczne w obu jadrach; rozne jest tylko to,
 * KTO i DOKAD zapisze wynik - i to zostaje po stronie wolajacego.
 */

#ifdef __x86_64__
typedef uint64_t dyn_addr;
#else
typedef uint32_t dyn_addr;
#endif

/* Wczytuje biblioteke z initrd (albo z dysku). Wolane raz, przy
   rozruchu. 1 = jest i da sie jej uzyc. */
int dyn_init(void);

/* Czy biblioteka jest zaladowana */
int dyn_present(void);

/* Nazwa pliku biblioteki - do komunikatow */
const char *dyn_name(void);

/* Ile stron zajmuje biblioteka i ile z nich jest wspoldzielonych */
uint32_t dyn_pages(void);

/* Prawa segmentu biblioteki - te same, ktore podaje sam ELF */
#define DYN_WRITE   1
#define DYN_EXEC   2

/* Segmenty biblioteki do zamapowania w przestrzeni procesu.
   Zwraca 0, gdy segmentu o tym numerze nie ma. */
int dyn_segment(int i, dyn_addr *vaddr, uint32_t *memsz, uint32_t *filesz,
                int *right, const uint8_t **data);

/* Ramka fizyczna strony biblioteki tylko do odczytu (wspolna dla
   wszystkich procesow). 0 = nie ma takiej strony. */
uint32_t dyn_frame(dyn_addr vaddr);

/* Czy obraz programu w ogole potrzebuje biblioteki (ma relokacje) */
int dyn_is_dynamic(const uint8_t *image, uint32_t size);

/* Przechodzi relokacje programu i dla kazdej wola 'zapisz' z adresem
   miejsca w przestrzeni procesu i wartoscia, ktora ma tam trafic.
   Zwraca 0, gdy wszystko sie rozwiazalo, albo -1. */
int dyn_relocate(const uint8_t *image, uint32_t size, void *ctx,
                int (*store)(void *ctx, dyn_addr addr, dyn_addr value));

#endif
