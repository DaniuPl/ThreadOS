/* Zamiana HTML na tekst do czytania w oknie
 *
 * Wyciete z przegladarki, zeby dalo sie to sprawdzac osobno: kod dostaje
 * dane prosto z sieci, wiec musi znosic dowolne smieci - urwane
 * znaczniki, brak zamkniec, encje bez srednika, bajty spoza ASCII,
 * atrybuty bez konca. Kazdy odczyt jest ograniczony dlugoscia wejscia,
 * a kazda petla ma warunek, ktory na pewno sie skonczy.
 *
 * Co robi: wycina znaczniki (pomijajac zawartosc <title>, <script> i
 * <style>), sklada biale znaki, rozwija najczestsze encje, zapamietuje
 * zakresy odnosnikow <a href> i lamie tekst na wiersze zadanej
 * szerokosci, na granicy slow.
 */

#ifndef PROTONOS_UHTML_H
#define PROTONOS_UHTML_H

#include "usys.h"

#define HTML_LINKS_MAX 48
#define HTML_LINES_MAX 400
#define HTML_HREF_MAX  96

struct html_link {
    int start, end;              /* zakres w tekscie wynikowym */
    char href[HTML_HREF_MAX];
};

struct html_line {
    int start, len;              /* zakres w tekscie wynikowym */
};

struct html_doc {
    const char *src;             /* wejscie: surowy HTML */
    int src_len;

    char *text;                  /* wyjscie: tekst do rysowania */
    int text_max;
    int text_len;

    struct html_link links[HTML_LINKS_MAX];
    int link_count;

    struct html_line lines[HTML_LINES_MAX];
    int line_count;
};

/* --- pomocnicze, wszystkie w granicach wejscia --- */

static inline char html_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Czy pod indeksem i stoi nazwa znacznika? Po nazwie musi byc bialy
   znak, '>' albo '/' - inaczej "a" pasowaloby do "article". */
static inline int html_tag_is(const struct html_doc *d, int i,
                              const char *name)
{
    int k = 0;
    while (name[k]) {
        if (i + k >= d->src_len)
            return 0;
        if (html_lower(d->src[i + k]) != name[k])
            return 0;
        k++;
    }
    if (i + k >= d->src_len)
        return 1;                /* znacznik urwany na koncu danych */
    char after = d->src[i + k];
    return after == '>' || after == ' ' || after == '\n' || after == '\r' ||
           after == '\t' || after == '/';
}

/* Nazwa atrybutu - po niej moze stac '=' albo bialy znak */
static inline int html_attr_is(const struct html_doc *d, int i,
                               const char *name)
{
    int k = 0;
    while (name[k]) {
        if (i + k >= d->src_len)
            return 0;
        if (html_lower(d->src[i + k]) != name[k])
            return 0;
        k++;
    }
    if (i + k >= d->src_len)
        return 0;
    char after = d->src[i + k];
    return after == '=' || after == ' ';
}

/* Znaczniki blokowe rozdzielaja tekst - bez tego "<h1>Tytul</h1><p>Tresc"
   skleilby sie w "TytulTresc" */
static inline int html_block_tag(const struct html_doc *d, int i)
{
    static const char *tags[] = {
        "p", "br", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul",
        "ol", "tr", "td", "th", "table", "section", "article", "header",
        "footer", "nav", "blockquote", "pre", "hr", 0
    };
    if (i < d->src_len && d->src[i] == '/')
        i++;
    for (int k = 0; tags[k]; k++)
        if (html_tag_is(d, i, tags[k]))
            return 1;
    return 0;
}

/* Wyciaga wartosc href="..." z wnetrza znacznika <a ...> */
static inline void html_grab_href(const struct html_doc *d, int i, char *out,
                                  int max)
{
    out[0] = '\0';

    while (i < d->src_len && d->src[i] != '>') {
        if ((d->src[i] == 'h' || d->src[i] == 'H') &&
            html_attr_is(d, i, "href")) {
            int j = i + 4;
            while (j < d->src_len && (d->src[j] == ' ' || d->src[j] == '='))
                j++;
            char quote = 0;
            if (j < d->src_len && (d->src[j] == '"' || d->src[j] == '\'')) {
                quote = d->src[j];
                j++;
            }
            int n = 0;
            while (j < d->src_len && n < max - 1) {
                char c = d->src[j];
                if (quote ? c == quote : (c == ' ' || c == '>'))
                    break;
                out[n++] = c;
                j++;
            }
            out[n] = '\0';
            return;
        }
        i++;
    }
}

/* --- glowna czesc: HTML -> tekst + odnosniki --- */

static inline void html_extract(struct html_doc *d)
{
    int out = 0, in_tag = 0, space = 1, skip = 0;
    int open_link = -1;

    d->link_count = 0;
    d->text_len = 0;
    d->line_count = 0;
    if (d->text_max > 0)
        d->text[0] = '\0';
    if (!d->src || d->src_len <= 0 || d->text_max <= 1)
        return;

    for (int i = 0; i < d->src_len && out < d->text_max - 1; i++) {
        char c = d->src[i];

        if (c == '<') {
            in_tag = 1;
            if (!skip && html_block_tag(d, i + 1) && !space) {
                d->text[out++] = ' ';
                space = 1;
                if (out >= d->text_max - 1)
                    break;
            }
            if (html_tag_is(d, i + 1, "title") ||
                html_tag_is(d, i + 1, "script") ||
                html_tag_is(d, i + 1, "style")) {
                skip = 1;
            } else if (html_tag_is(d, i + 2, "title") ||
                       html_tag_is(d, i + 2, "script") ||
                       html_tag_is(d, i + 2, "style")) {
                skip = 0;
            } else if (html_tag_is(d, i + 1, "a") &&
                       d->link_count < HTML_LINKS_MAX) {
                html_grab_href(d, i + 2, d->links[d->link_count].href,
                               HTML_HREF_MAX);
                if (d->links[d->link_count].href[0]) {
                    d->links[d->link_count].start = out;
                    open_link = d->link_count;
                }
            } else if (html_tag_is(d, i + 2, "a") && open_link >= 0) {
                d->links[open_link].end = out;
                if (d->links[open_link].end > d->links[open_link].start)
                    d->link_count++;
                open_link = -1;
            }
            continue;
        }
        if (c == '>') {
            in_tag = 0;
            continue;
        }
        if (in_tag || skip)
            continue;

        if (c == '&') {              /* encja: &amp; &nbsp; &#39; ... */
            int j = i + 1, len = 0;
            char ent[10];
            while (j < d->src_len && d->src[j] != ';' && len < 9 &&
                   d->src[j] != ' ' && d->src[j] != '<')
                ent[len++] = d->src[j++];
            ent[len] = '\0';
            if (j < d->src_len && d->src[j] == ';' && len > 0) {
                i = j;               /* zjedz cala encje */
                char rep;
                if (ent[0] == 'a' && ent[1] == 'm')       rep = '&';
                else if (ent[0] == 'l' && ent[1] == 't')  rep = '<';
                else if (ent[0] == 'g' && ent[1] == 't')  rep = '>';
                else if (ent[0] == 'q')                   rep = '"';
                else if (ent[0] == 'n' && ent[1] == 'b')  rep = ' ';
                else                                      rep = '?';
                if (rep == ' ') {
                    if (!space) {
                        d->text[out++] = ' ';
                        space = 1;
                    }
                } else {
                    d->text[out++] = rep;
                    space = 0;
                }
                continue;
            }
        }
        if ((unsigned char)c >= 0x80) {   /* UTF-8: czcionka zna ASCII */
            if (((unsigned char)c & 0xC0) != 0x80) {
                d->text[out++] = '.';
                space = 0;
            }
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!space) {
                d->text[out++] = ' ';
                space = 1;
            }
            continue;
        }
        if (c < 0x20)                /* inne znaki sterujace pomijamy */
            continue;

        d->text[out++] = c;
        space = 0;
    }

    /* odnosnik bez zamkniecia konczy sie na koncu tekstu */
    if (open_link >= 0 && d->link_count < HTML_LINKS_MAX) {
        d->links[open_link].end = out;
        if (d->links[open_link].end > d->links[open_link].start)
            d->link_count++;
    }

    d->text[out] = '\0';
    d->text_len = out;
}

/* --- lamanie tekstu na wiersze (na granicy slow) --- */

static inline void html_layout(struct html_doc *d, int cols)
{
    d->line_count = 0;
    if (cols < 1)
        cols = 1;

    int i = 0;
    while (i < d->text_len && d->line_count < HTML_LINES_MAX) {
        int start = i, col = 0, last_space = -1;

        while (i < d->text_len && col < cols) {
            if (d->text[i] == ' ')
                last_space = i;
            i++;
            col++;
        }
        int end = i;
        if (i < d->text_len && d->text[i] != ' ' && last_space > start)
            end = last_space;        /* zawin na granicy slowa */

        d->lines[d->line_count].start = start;
        d->lines[d->line_count].len = end - start;
        d->line_count++;

        i = end;
        while (i < d->text_len && d->text[i] == ' ')
            i++;                     /* zjedz spacje na zawinieciu */
    }
}

/* Do ktorego odnosnika nalezy znak o indeksie i (-1 = do zadnego)? */
static inline int html_link_at(const struct html_doc *d, int i)
{
    for (int k = 0; k < d->link_count; k++)
        if (i >= d->links[k].start && i < d->links[k].end)
            return k;
    return -1;
}

#endif
