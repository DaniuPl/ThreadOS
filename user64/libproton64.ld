/* Biblioteka wspoldzielona ProtonOS
 *
 * Konsolidowana pod STALYM adresem - tym samym, ktory jadro daje jej
 * w kazdym procesie (USER_LIB_BASE w kernel64/elf64.h). Dzieki
 * temu adresy w jej tablicy symboli sa juz ostateczne i ani ona sama,
 * ani jadro nie maja nic do przeliczania przy ladowaniu.
 *
 * Dane pisemne dostaja wlasne strony, bo tylko one nie moga byc
 * wspolne: biblioteka trzyma w nich swoj stan (ziarno generatora,
 * ustawienia TLS), a stan jednego procesu nie ma prawa byc widoczny
 * w drugim. Kod i stale sa wspolne - i to jest cala oszczednosc.
 */

ENTRY(_nie_startuje)

SECTIONS
{
    . = 0x4F000000;

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .hash   : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rel.dyn  : { *(.rel.*) }
    .rela.dyn : { *(.rela.*) }

    /* Od tego miejsca zaczyna sie to, co da sie zapisac - i wlasnie
       dlatego jest tu granica strony. Segment powyzej jest wspolny dla
       wszystkich procesow, wiec nie moze zawierac ani jednego bajtu,
       ktory ktos moglby zmienic; .dynamic i tablice adresow sa
       zapisywalne, wiec siedza ponizej. */
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    .data   : { *(.data*) *(.got) *(.got.plt) }
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
