/* What this machine actually is
 *
 * Written for a first boot on hardware nobody has tried yet, and it
 * lives in a file of its own rather than in a shell because both
 * shells need it and the machine it describes is the same machine
 * either way. On a laptop this is the first thing one types, and it
 * is the difference between "it does not work" and knowing which of
 * five things is missing.
 *
 * Nothing here writes anything. Every line answers a question one asks
 * while standing in front of a machine that will not do something.
 */

#include "include/hwinfo.h"
#include "include/printk.h"
#include "include/video.h"
#include "include/mouse.h"
#include "include/keyboard.h"
#include "include/usbhc.h"
#include "include/usbkbd.h"
#include "include/usbmouse.h"
#include "include/blk.h"
#include "include/part.h"
#include "include/pci.h"
#include "include/hda.h"
#include "include/string.h"

/* What this machine actually is.
 *
 * Written for a first boot on hardware nobody has tried yet. Every
 * line here answers a question one asks while standing in front of a
 * laptop that will not do something: what did the loader give us for
 * a screen, is there anything on the PS/2 ports, did the USB bus have
 * a keyboard or a pointer on it, and - when the answer to the pointer
 * is no - what else is in the machine that it might be hiding behind.
 *
 * The PCI listing is the last of those. A touchpad that is not on the
 * PS/2 port is usually on an I2C controller, and an I2C controller is
 * a device of the serial-bus class sitting in plain sight here. We do
 * not speak that bus; being able to SAY that is the difference between
 * a mystery and a known gap. */
static const char *pci_class_name(uint32_t class)
{
    switch (class >> 24) {
    case 0x01: return "dysk";
    case 0x02: return "siec";
    case 0x03: return "grafika";
    case 0x04: return "dzwiek/wideo";
    case 0x06: return "mostek";
    case 0x08: return "kontroler systemowy";
    case 0x09: return "wejscie (mysz, tablet)";
    case 0x0C: return ((class >> 16) & 0xFF) == 0x03 ? "USB" : "magistrala szeregowa";
    case 0x11: return "przetwarzanie sygnalu";
    default:   return "inne";
    }
}

void hwinfo_print(void)
{
    printk("ekran:    %dx%d, %s, skala %d, bufor %s\n",
           video_w(), video_h(),
           video_true_colour() ? "kolory" : "paleta 256", video_scale(),
           video_have_loader_lfb() ? "od loadera" : "ustawiony przez jadro");

    if (!ps2_controller_present())
        printk("PS/2:     BRAK kontrolera 8042 - ani klawiatury, ani "
               "myszy na tych portach\n");
    else
        printk("PS/2:     kontroler 8042 jest, mysz %s\n",
               mouse_present() ? (mouse_has_wheel() ? "tak (z kolkiem)"
                                                    : "tak (bez kolka)")
                               : "NIE");
    printk("USB:      %s, klawiatura %s, mysz %s\n",
           usbhc_name(), usbkbd_present() ? "tak" : "nie",
           usbmouse_present() ? "tak" : "nie");

    /* The disk, and what is already on it. A machine booted from a
       stick sees the laptop's own disk here - and before anyone
       proposes to write to it, it is worth reading what would be
       written over. */
    printk("dysk:     %s, %u sektorow po 512 B (%u MiB)\n", blk_name(),
           blk_sectors(), blk_sectors() / 2048);
    int np = part_scan();
    if (np < 0)
        printk("          brak dysku\n");
    else if (np == 0)
        printk("          bez tablicy partycji - caly dysk jest nasz\n");
    else {
        printk("          tablica %s, partycji: %u\n",
               part_is_gpt() ? "GPT" : "MBR", (uint32_t)np);
        for (int i = 0; i < np; i++) {
            struct partition pt;
            if (!part_get(i, &pt))
                break;
            printk("          %u: od %u, %u MiB, %s%s%s\n",
                   (uint32_t)(i + 1), (uint32_t)pt.start,
                   (uint32_t)(pt.sectors / 2048), pt.kind,
                   pt.label[0] ? " " : "", pt.label);
        }
    }

    /* And what else is in here, so that a gap has a name */
    int serial_bus = 0;
    printk("PCI:\n");
    for (int i = 0; ; i++) {
        struct pci_dev d;
        uint32_t class;
        if (!pci_enumerate(i, &d, &class))
            break;
        uint8_t base = (uint8_t)(class >> 24);
        uint8_t sub = (uint8_t)((class >> 16) & 0xFF);
        if ((base == 0x0C && sub != 0x03) || base == 0x11)
            serial_bus++;
        /* printk has no field widths, so the columns are spelled out
           rather than padded - and a slot number is more readable in
           decimal here anyway. */
        printk("  %u.%u  %x:%x  klasa %x.%x  %s\n",
               (uint32_t)d.slot, (uint32_t)d.func,
               (uint32_t)d.vendor, (uint32_t)d.device,
               (uint32_t)base, (uint32_t)sub, pci_class_name(class));
    }

    if (!mouse_present() && !usbmouse_present() && serial_bus)
        printk("Wskaznika nie ma na zadnej znanej drodze, a w maszynie\n"
               "jest %d kontroler(ow) magistrali szeregowej - touchpad\n"
               "siedzi zapewne na I2C (HID przez I2C), ktorego nie umiemy.\n"
               "Mysz w gniazdo USB zadziala.\n", serial_bus);
}

/* The disks the machine has, all of them, and which one everything
   else is reading and writing. A machine with one disk says so in one
   line; a machine with two is a machine where "which one" is a
   question, and it had better be answerable without starting an
   installer. */
void hwinfo_disks(void)
{
    int n = blk_count();

    if (!n) {
        printk("Nie ma zadnego dysku.\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        printk("dysk %u: %s, %u sektorow po 512 B (%u MiB)%s\n",
               (uint32_t)(i + 1), blk_name_of(i), blk_sectors_of(i),
               blk_sectors_of(i) / 2048,
               i == blk_current() ? "  <- uzywany" : "");
        /* What is on it. Reading another disk's table means pointing
           the machine at it, so it is pointed back straight after -
           looking must not move what the filesystem is reading. */
        int was = blk_current();
        if (blk_use(i) < 0)
            continue;
        int np = part_scan();
        if (np == 0)
            printk("         bez tablicy partycji\n");
        for (int k = 0; k < np; k++) {
            struct partition pt;
            if (!part_get(k, &pt))
                break;
            printk("         %u: od %u, %u MiB, %s%s%s\n",
                   (uint32_t)(k + 1), (uint32_t)pt.start,
                   (uint32_t)(pt.sectors / 2048), pt.kind,
                   pt.label[0] ? " " : "", pt.label);
        }
        blk_use(was);
    }
    /* And the table of the disk in use, read again - the loop above
       left every driver pointed where it found it, and the next thing
       to touch the disk should not have to know that. */
    part_scan();
}

/* How loud, and it is remembered.
 *
 * The percentage is of what the codec's amplifier actually has - its
 * steps are decibels, so thirty per cent is far quieter than a third.
 * That is how loudness works and not something to correct: a mixer
 * that spread the numbers evenly over the amplitude would feel wrong
 * at both ends of its travel.
 */
void hwinfo_volume(const char *arg)
{
    uint32_t v = 0;

    if (!hda_present()) {
        printk("glosnosc: nie ma dzwieku (zobacz 'sprzet')\n");
        return;
    }
    while (arg && *arg == ' ')
        arg++;

    if (arg && arg[0] == 'c') {                  /* 'cisza' */
        hda_set_mute(!hda_muted());
        printk("glosnosc: %s\n", hda_muted() ? "cisza" : "znowu slychac");
        return;
    }
    if (arg && arg[0] >= '0' && arg[0] <= '9') {
        while (*arg >= '0' && *arg <= '9')
            v = v * 10 + (uint32_t)(*arg++ - '0');
        if (v > 100) {
            printk("glosnosc: 0-100, nie %u\n", v);
            return;
        }
        hda_set_volume((int)v);
        hda_set_mute(0);
        if (hda_remember() < 0)
            printk("glosnosc: %d%% - ale nie zapisane na dysk\n",
                   hda_volume());
        else
            printk("glosnosc: %d%%\n", hda_volume());
        return;
    }
    printk("glosnosc: %d%%%s, %d stopni w kodeku\n", hda_volume(),
           hda_muted() ? " (cisza)" : "", hda_volume_steps());
}
