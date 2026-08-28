/* Programy uzytkownika ProtonOS 64
 *
 * Ten sam adres, co w jadrze 32-bitowym (1 GiB), i z tego samego
 * powodu: nizej lezy mapowanie identycznosciowe pamieci fizycznej.
 * USER64_BASE w kernel64/elf64.h musi sie z tym zgadzac.
 *
 * Sekcje sa wyrownane do stron, bo jadro nadaje prawa dostepu na
 * podstawie tego, co mowi ELF, a najmniejsza jednostka praw jest
 * strona. Kod ma wiec wlasne strony (do odczytu i wykonania), a dane
 * wlasne (do zapisu, ale z zakazem wykonywania).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
