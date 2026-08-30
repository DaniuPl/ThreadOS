#ifndef PROTONOS_ZAMEKJADRA_H
#define PROTONOS_ZAMEKJADRA_H

/* Wielki zamek jadra
 *
 * Rdzenie wtorne umieja juz prowadzic watki jadra, ale tylko takie,
 * ktore ktos jawnie na to pozwolil - bo reszta jadra pisana byla dla
 * jednego rdzenia. Zeby wpuscic tam PROGRAMY, trzeba rozstrzygnac ten
 * sam problem dla calego jadra naraz.
 *
 * Odpowiedz jest ta sama, ktora dawaly kiedys wszystkie systemy
 * przechodzace na wiele rdzeni: jeden zamek na cale jadro. Kod
 * uzytkownika chodzi wtedy naprawde rownolegle - i to w nim programy
 * spedzaja wieksza czesc czasu - a kod jadra po staremu, po jednym
 * rdzeniu naraz. To nie jest cel docelowy, tylko poprawna droga do
 * niego: kazdy podsystem, ktory dostanie wlasny zamek, mozna spod
 * tego wyjac, i az do konca system dziala.
 *
 * Zamek jest ZAGNIEZDZALNY. Rdzen, ktory juz go ma, bierze go po raz
 * kolejny bez czekania - inaczej przerwanie, ktore trafia w srodek
 * wywolania systemowego, czekaloby na zamek trzymany przez przerwany
 * wlasnie kod.
 *
 * Zasada, na ktorej to wszystko stoi: zadanie trzyma ten zamek przez
 * caly czas, kiedy wykonuje kod jadra, i oddaje go NA CZAS
 * PRZELACZENIA. Zadania bezczynne nie biora go nigdy - rdzen stojacy
 * na hlt z zamkiem w garsci zatrzymalby caly system.
 */

/* Wchodze w kod jadra (albo juz w nim jestem - wtedy tylko licznik) */
void jadro_wejdz(void);

/* Wychodze; zamek zwalnia sie dopiero przy ostatnim wyjsciu */
void jadro_wyjdz(void);

/* Czy ten rdzen trzyma zamek */
int jadro_trzymam(void);

/* Oddaje zamek CALKOWICIE i zwraca, ile razy byl wziety - do uzycia
   przy przelaczaniu kontekstu, gdzie zadanie musi go puscic, zeby inne
   moglo wejsc do jadra. */
int jadro_zwolnij_wszystko(void);

/* Bierze go z powrotem na te sama glebokosc */
void jadro_przywroc(int glebokosc);

#endif
