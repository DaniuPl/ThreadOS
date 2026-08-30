/* Programy uzytkownika ProtonOS korzystajace z biblioteki wspoldzielonej
 *
 * Ten sam adres poczatkowy, co user/user.ld - roznica jest w tym, co
 * jeszcze trzeba w programie umiescic. Program dynamiczny nie niesie
 * kodu biblioteki; niesie tablice skokow (.plt) i tablice adresow
 * (.got.plt), ktore jadro wypelnia przy ladowaniu, oraz opis tego, co
 * ma tam trafic (.dynsym, .dynstr, .rel.plt).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text     : { *(.text*) }
    .plt      : { *(.plt) *(.iplt) }
    .rodata   : { *(.rodata*) }
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rel.plt  : { *(.rel.plt) *(.rel.dyn) }
    .rela.plt : { *(.rela.plt) *(.rela.dyn) }
    .dynamic  : { *(.dynamic) }

    .data     : { *(.data*) }
    .got      : { *(.got) }
    .got.plt  : { *(.got.plt) }
    .bss      : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.interp) }
}
