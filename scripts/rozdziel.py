#!/usr/bin/env python3
"""Rozdziela naglowek na zapowiedzi i tresc - dla biblioteki wspoldzielonej.

Kryptografia, TLS, HTTP i DNS zyja w ProtonOS w naglowkach, jako
funkcje 'static inline'. To bylo dobre, dopoki kazdy program niosl je
w sobie: jedno zrodlo prawdy, zero plikow posrednich, zero zaleznosci
w Makefile.

Biblioteka wspoldzielona potrzebuje jednak dwoch roznych spojrzen na
ten sam plik:

  --lib   dla biblioteki: te same funkcje, ale z zewnetrznym
          wiazaniem, zeby dalo sie je w niej ODNALEZC po nazwie;
  --dekl  dla programu: same zapowiedzi, bez cial - program ma
          zawierac wywolania, a nie kopie kodu.

Zrodlo zostaje jedno i nietkniete. Zamiast rozbijac czternascie
naglowkow na pary .c/.h recznie (i pilnowac potem, zeby nie rozjechaly
sie miedzy soba), robi to ten skrypt przy kazdym budowaniu.

Uzycie: rozdziel.py --lib|--dekl plik.h > wynik.h
"""

import re
import sys


def koniec_ciala(tekst, i):
    """Indeks tuz za klamra zamykajaca cialo zaczynajace sie na tekst[i]=='{'.

    Liczy klamry, ale omija te w napisach, znakach i komentarzach -
    inaczej pojedynczy znak '}' w tablicy tlumaczen zamknalby funkcje
    w polowie."""
    assert tekst[i] == '{'
    glebokosc = 0
    n = len(tekst)
    while i < n:
        c = tekst[i]
        if c == '"' or c == "'":
            konczy = c
            i += 1
            while i < n and tekst[i] != konczy:
                i += 2 if tekst[i] == '\\' else 1
            i += 1
            continue
        if c == '/' and i + 1 < n and tekst[i + 1] == '*':
            i = tekst.find('*/', i + 2)
            if i < 0:
                return n
            i += 2
            continue
        if c == '/' and i + 1 < n and tekst[i + 1] == '/':
            i = tekst.find('\n', i)
            if i < 0:
                return n
            continue
        if c == '{':
            glebokosc += 1
        elif c == '}':
            glebokosc -= 1
            if glebokosc == 0:
                return i + 1
        i += 1
    return n


def przetworz(tekst, tryb):
    wynik = []
    i = 0
    n = len(tekst)
    # 'static inline' i 'static' na poczatku wiersza - tylko takie sa
    # definicjami najwyzszego poziomu; wciecie znaczy, ze to w srodku
    # czegos innego
    wzor = re.compile(r'^static(\s+inline)?\s', re.M)

    while i < n:
        m = wzor.search(tekst, i)
        if not m:
            wynik.append(tekst[i:])
            break
        wynik.append(tekst[i:m.start()])

        # Dokad siega ta definicja: do klamry otwierajacej cialo albo
        # do srednika konczacego deklaracje danych.
        #
        # Sama klamra nie rozstrzyga: tablica z inicjatorem tez ja ma.
        # Rozstrzyga to, CO przed nia stoi - po nawiasie argumentow
        # zaczyna sie cialo funkcji, po znaku rownosci wartosc danych.
        klamra = tekst.find('{', m.end())
        srednik = tekst.find(';', m.end())
        funkcja = False
        if klamra >= 0 and (srednik < 0 or klamra < srednik):
            przed = tekst[m.end():klamra].rstrip()
            funkcja = przed.endswith(')')

        if funkcja:
            koniec = koniec_ciala(tekst, klamra)
            naglowek = tekst[m.start():klamra].rstrip()
            cialo = tekst[klamra:koniec]
            if tryb == 'lib':
                # To samo, tylko widoczne z zewnatrz
                wynik.append(naglowek[len(m.group(0)) - 1:].lstrip() +
                             '\n' + cialo)
            else:
                wynik.append(naglowek[len(m.group(0)) - 1:].lstrip() + ';')
            i = koniec
        else:
            # Dane: w bibliotece zostaja (uzywaja ich jej funkcje),
            # w programie znikaja razem z cialami, ktore po nie siegaly
            koniec = srednik
            if koniec < 0:
                wynik.append(tekst[m.start():])
                break
            # Tablica z inicjatorem: sredniki moga byc w srodku klamr
            if klamra >= 0 and klamra < srednik:
                koniec = tekst.find(';', koniec_ciala(tekst, klamra) - 1)
                if koniec < 0:
                    koniec = srednik
            koniec += 1
            if tryb == 'lib':
                wynik.append(tekst[m.start():koniec])
            i = koniec

    return ''.join(wynik)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--lib', '--dekl'):
        sys.stderr.write(__doc__)
        return 2
    tryb = 'lib' if sys.argv[1] == '--lib' else 'dekl'
    with open(sys.argv[2]) as fh:
        tekst = fh.read()
    sys.stdout.write(przetworz(tekst, tryb))
    return 0


if __name__ == '__main__':
    sys.exit(main())
