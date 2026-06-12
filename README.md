# ProtonOS

Własny system operacyjny na PC (x86, 32-bit), pisany od zera w C i asemblerze.
Jądro jest zgodne ze specyfikacją **Multiboot**, więc bootuje je GRUB albo
bezpośrednio QEMU.

## Struktura projektu

```
ProtonOS/
├── boot/
│   └── boot.S          # nagłówek Multiboot, ustawienie stosu, skok do kmain()
├── kernel/
│   ├── kmain.c         # główny punkt wejścia jądra w C
│   ├── vga.c           # sterownik trybu tekstowego VGA (wypisywanie na ekran)
│   ├── string.c        # memset/memcpy/strlen (brak libc w jądrze)
│   └── include/        # nagłówki jądra
├── linker.ld           # skrypt linkera - jądro ładowane pod adres 1 MiB
├── grub.cfg            # konfiguracja GRUB-a dla obrazu ISO
└── Makefile
```

## Wymagania

- `gcc` z obsługą `-m32` (na Ubuntu/Debianie: `sudo apt install gcc-multilib`)
- `make`
- do uruchomienia: `qemu-system-i386` (pakiet `qemu-system-x86`)
- do zbudowania ISO: `grub-mkrescue` + `xorriso` (pakiety `grub-pc-bin`, `xorriso`)

## Budowanie i uruchamianie

```sh
make          # buduje build/protonos.bin
make run      # uruchamia jądro w QEMU (najprostsza droga)
make iso      # buduje bootowalne ISO z GRUB-em
make run-iso  # uruchamia ISO w QEMU
make clean
```

Po uruchomieniu jądro czyści ekran, wypisuje banner i weryfikuje,
że zostało załadowane przez bootloader zgodny z Multiboot.

## Jak to działa

1. **GRUB/QEMU** znajduje w pierwszych 8 KiB obrazu nagłówek Multiboot
   (`boot/boot.S`), ładuje jądro pod adres 1 MiB i skacze do `_start`
   już w 32-bitowym trybie chronionym.
2. `_start` ustawia stos (16 KiB w sekcji `.bss`) i woła `kmain()`,
   przekazując magic Multiboot oraz wskaźnik na strukturę z informacjami
   od bootloadera (m.in. mapę pamięci).
3. `kmain()` inicjalizuje sterownik VGA i wypisuje tekst, pisząc
   bezpośrednio do bufora pod adresem `0xB8000`.

## Plan rozwoju (następne kroki)

- [ ] GDT (Global Descriptor Table) - własne deskryptory segmentów
- [ ] IDT + obsługa wyjątków CPU
- [ ] PIC (remapowanie) i przerwania sprzętowe (IRQ)
- [ ] Sterownik klawiatury (PS/2)
- [ ] Timer (PIT) i licznik czasu
- [ ] Odczyt mapy pamięci z Multiboot
- [ ] Zarządzanie pamięcią fizyczną (alokator ramek)
- [ ] Stronicowanie (paging)
- [ ] Tryb 64-bit / wyższa półka: procesy, scheduler, syscalle
