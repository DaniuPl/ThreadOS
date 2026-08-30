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
  --decl  dla programu: same zapowiedzi, bez cial - program ma
          zawierac wywolania, a nie kopie kodu.

Zrodlo zostaje jedno i nietkniete. Zamiast rozbijac czternascie
naglowkow na pary .c/.h recznie (i pilnowac potem, zeby nie rozjechaly
sie miedzy soba), robi to ten skrypt przy kazdym budowaniu.

Uzycie: split_headers.py --lib|--decl plik.h > wynik.h
"""

import re
import sys


def body_end(text, i):
    """Indeks tuz za klamra zamykajaca cialo zaczynajace sie na tekst[i]=='{'.

    Liczy klamry, ale omija te w napisach, znakach i komentarzach -
    inaczej pojedynczy znak '}' w tablicy tlumaczen zamknalby funkcje
    w polowie."""
    assert text[i] == '{'
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            finishing = c
            i += 1
            while i < n and text[i] != finishing:
                i += 2 if text[i] == '\\' else 1
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i = text.find('*/', i + 2)
            if i < 0:
                return n
            i += 2
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                return n
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def process_out(text, mode):
    summary = []
    i = 0
    n = len(text)
    # 'static inline' i 'static' na poczatku wiersza - tylko takie sa
    # definicjami najwyzszego poziomu; wciecie znaczy, ze to w srodku
    # czegos innego
    pattern = re.compile(r'^static(\s+inline)?\s', re.M)

    while i < n:
        m = pattern.search(text, i)
        if not m:
            summary.append(text[i:])
            break
        summary.append(text[i:m.start()])

        # Dokad siega ta definicja: do klamry otwierajacej cialo albo
        # do srednika konczacego deklaracje danych.
        #
        # Sama klamra nie rozstrzyga: tablica z inicjatorem tez ja ma.
        # Rozstrzyga to, CO przed nia stoi - po nawiasie argumentow
        # zaczyna sie cialo funkcji, po znaku rownosci wartosc danych.
        brace = text.find('{', m.end())
        semicolon = text.find(';', m.end())
        fn = False
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            before = text[m.end():brace].rstrip()
            fn = before.endswith(')')

        if fn:
            end = body_end(text, brace)
            header = text[m.start():brace].rstrip()
            body = text[brace:end]
            if mode == 'lib':
                # To samo, tylko widoczne z zewnatrz
                summary.append(header[len(m.group(0)) - 1:].lstrip() +
                             '\n' + body)
            else:
                summary.append(header[len(m.group(0)) - 1:].lstrip() + ';')
            i = end
        else:
            # Dane: w bibliotece zostaja (uzywaja ich jej funkcje),
            # w programie znikaja razem z cialami, ktore po nie siegaly
            end = semicolon
            if end < 0:
                summary.append(text[m.start():])
                break
            # Tablica z inicjatorem: sredniki moga byc w srodku klamr
            if brace >= 0 and brace < semicolon:
                end = text.find(';', body_end(text, brace) - 1)
                if end < 0:
                    end = semicolon
            end += 1
            if mode == 'lib':
                summary.append(text[m.start():end])
            i = end

    return ''.join(summary)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--lib', '--decl'):
        sys.stderr.write(__doc__)
        return 2
    mode = 'lib' if sys.argv[1] == '--lib' else 'decl'
    with open(sys.argv[2]) as fh:
        text = fh.read()
    sys.stdout.write(process_out(text, mode))
    return 0


if __name__ == '__main__':
    sys.exit(main())
