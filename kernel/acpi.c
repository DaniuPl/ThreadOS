/* ACPI - wylaczanie i restart komputera
 *
 * Droga do wylaczenia zasilania wiedzie przez trzy struktury:
 *
 *   RSDP - wskaznik lezacy gdzies w pamieci ponizej megabajta,
 *          rozpoznawany po napisie "RSD PTR " i sumie kontrolnej,
 *   RSDT - spis wszystkich tablic; szukamy w nim FADT ("FACP"),
 *   FADT - mowi, pod ktore porty pisac i gdzie lezy DSDT.
 *
 * Brakuje jeszcze jednego: samej wartosci, ktora trzeba wpisac.
 * Siedzi ona w DSDT, w bajtkodzie AML, pod nazwa _S5_ - stanem
 * "wylaczony". Pelnego interpretera AML nie mamy i nie potrzebujemy:
 * ten jeden obiekt to krotka paczka, ktora da sie odczytac wprost.
 *
 * Tablice leza czesto poza obszarem, ktory jadro zarzadza jako pamiec
 * (firmware oznacza go jako zarezerwowany), wiec kazda doklejamy do
 * mapowania identycznosciowego, zanim ja przeczytamy.
 */

#include <stdint.h>
#include "include/acpi.h"
#include "include/paging.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"

struct rsdp {
    char signature[8];          /* "RSD PTR " */
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
} __attribute__((packed));

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Adres rejestru w opisie ogolnym (ACPI 2.0) - uzywamy go tylko
   dla rejestru resetu */
struct gas {
    uint8_t space;              /* 0 = pamiec, 1 = porty I/O */
    uint8_t width, offset, size;
    uint32_t addr_low, addr_high;
} __attribute__((packed));

struct fadt {
    struct sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable;
    uint8_t s4bios_req, pstate_cnt;
    uint32_t pm1a_evt_blk, pm1b_evt_blk;
    uint32_t pm1a_cnt_blk, pm1b_cnt_blk;
    uint32_t pm2_cnt_blk, pm_tmr_blk;
    uint32_t gpe0_blk, gpe1_blk;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len;
    uint8_t gpe0_blk_len, gpe1_blk_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alrm, mon_alrm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct gas reset_reg;
    uint8_t reset_value;
} __attribute__((packed));

#define SLP_EN   (1u << 13)
#define SCI_EN   1

static const struct fadt *fadt;
static uint16_t slp_typa, slp_typb;
static char oem_id[7];
static int ready;

/* Tablice ACPI leza zwykle poza pamiecia zarzadzana przez jadro -
   trzeba je najpierw wpuscic do mapowania identycznosciowego */
static const void *map(uint32_t phys, uint32_t len)
{
    if (!phys)
        return (void *)0;
    if (phys + len > paging_identity_limit() &&
        paging_map_phys(phys, len) < 0)
        return (void *)0;
    return (const void *)phys;
}

/* Odczyt pojedynczego bajtu spod stalego adresu w pamieci niskiej */
static uint8_t low_byte(uint32_t addr)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    /* Kompilator widzi tu staly adres i traktuje go jak tablice
       o zerowej dlugosci. Przepuszczenie wskaznika przez pusty wstawek
       odbiera mu te wiedze - adres jest prawdziwy, tyle ze pochodzi
       spoza jezyka. */
    __asm__("" : "+r"(p));
    return *p;
}

static int checksum_ok(const void *p, uint32_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

/* RSDP lezy albo w obszarze danych BIOS-u, albo w pamieci ROM
   miedzy 0xE0000 a 0xFFFFF; zawsze na granicy 16 bajtow. */
static const struct rsdp *find_rsdp(void)
{
    /* Adres obszaru danych BIOS-u lezy pod stalym adresem 0x40E.
       Czytamy go bajt po bajcie: dla kompilatora staly adres wygrany
       na wskaznik to "tablica o zerowej dlugosci" i slusznie sie
       przy niej krzywi. */
    uint32_t ebda = ((uint32_t)low_byte(0x40F) << 8 | low_byte(0x40E)) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (uint32_t a = ebda; a < ebda + 1024; a += 16) {
            const struct rsdp *r = (const struct rsdp *)a;
            if (!memcmp(r->signature, "RSD PTR ", 8) &&
                checksum_ok(r, 20))
                return r;
        }
    }
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        const struct rsdp *r = (const struct rsdp *)a;
        if (!memcmp(r->signature, "RSD PTR ", 8) && checksum_ok(r, 20))
            return r;
    }
    return (const struct rsdp *)0;
}

/* Wyluskuje z bajtkodu AML wartosci stanu _S5_ - "wylaczony".
 *
 * Obiekt wyglada tak: nazwa "_S5_", znacznik paczki (0x12), jej dlugosc,
 * liczba elementow, a potem dwie liczby - jedna dla kazdego z dwoch
 * blokow sterujacych. Liczby moga byc zapisane wprost (0..7 maja wlasne
 * znaczniki) albo poprzedzone znacznikiem bajtu (0x0A).
 */
static int parse_s5(const uint8_t *dsdt, uint32_t len)
{
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) != 0)
            continue;

        const uint8_t *p = dsdt + i + 4;
        if (*p != 0x12)                 /* nie paczka - szukaj dalej */
            continue;
        p++;
        p += ((*p & 0xC0) >> 6) + 2;    /* dlugosc paczki + licznik elementow */

        if (*p == 0x0A)                 /* znacznik "liczba jednobajtowa" */
            p++;
        slp_typa = (uint16_t)(*p++ << 10);
        if (*p == 0x0A)
            p++;
        slp_typb = (uint16_t)(*p << 10);
        return 1;
    }
    return 0;
}

int acpi_init(void)
{
    const struct rsdp *rsdp = find_rsdp();
    if (!rsdp)
        return 0;

    for (int i = 0; i < 6; i++)
        oem_id[i] = rsdp->oem[i];
    oem_id[6] = '\0';

    const struct sdt_header *rsdt = map(rsdp->rsdt, sizeof(*rsdt));
    if (!rsdt || memcmp(rsdt->signature, "RSDT", 4) != 0)
        return 0;
    if (!map(rsdp->rsdt, rsdt->length))
        return 0;

    uint32_t count = (rsdt->length - sizeof(*rsdt)) / 4;
    const uint32_t *entries = (const uint32_t *)(rsdt + 1);
    for (uint32_t i = 0; i < count; i++) {
        const struct sdt_header *h = map(entries[i], sizeof(*h));
        if (!h || memcmp(h->signature, "FACP", 4) != 0)
            continue;
        if (!map(entries[i], h->length))
            continue;
        fadt = (const struct fadt *)h;
        break;
    }
    if (!fadt)
        return 0;

    const struct sdt_header *dsdt = map(fadt->dsdt, sizeof(*dsdt));
    if (dsdt && map(fadt->dsdt, dsdt->length))
        parse_s5((const uint8_t *)(dsdt + 1),
                 dsdt->length - sizeof(*dsdt));

    /* Wlaczenie ACPI: gdy firmware zostawil sterowanie sobie, trzeba
       je odebrac, pisac do portu SMI. Maszyna wirtualna zwykle oddaje
       je od razu i tego kroku nie ma. */
    if (fadt->smi_cmd && fadt->acpi_enable && fadt->pm1a_cnt_blk &&
        !(inw((uint16_t)fadt->pm1a_cnt_blk) & SCI_EN)) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
        for (int i = 0; i < 300; i++) {
            if (inw((uint16_t)fadt->pm1a_cnt_blk) & SCI_EN)
                break;
            io_wait();
        }
    }

    ready = fadt->pm1a_cnt_blk != 0;
    return ready;
}

const char *acpi_oem(void)
{
    return oem_id;
}

void acpi_poweroff(void)
{
    if (!ready)
        return;

    uint32_t flags = irq_save();
    outw((uint16_t)fadt->pm1a_cnt_blk, slp_typa | SLP_EN);
    if (fadt->pm1b_cnt_blk)
        outw((uint16_t)fadt->pm1b_cnt_blk, slp_typb | SLP_EN);

    /* Zasilanie nie gasnie w tej samej instrukcji, w ktorej o to
       prosimy - bez tej chwili zwloki zdazylibysmy jeszcze wypisac,
       ze sie nie udalo, tuz przed tym, jak sie udaje. */
    for (volatile uint32_t i = 0; i < 50000000u; i++)
        ;
    irq_restore(flags);
}

int acpi_reboot(void)
{
    /* Rejestr resetu jest dopiero od ACPI 2.0 i nie kazdy go wypelnia */
    if (!fadt || fadt->header.length < sizeof(struct fadt) ||
        !fadt->reset_reg.addr_low)
        return 0;

    if (fadt->reset_reg.space == 1) {           /* port I/O */
        outb((uint16_t)fadt->reset_reg.addr_low, fadt->reset_value);
        return 1;
    }
    if (fadt->reset_reg.space == 0) {           /* rejestr w pamieci */
        if (!map(fadt->reset_reg.addr_low, 4))
            return 0;
        *(volatile uint8_t *)fadt->reset_reg.addr_low = fadt->reset_value;
        return 1;
    }
    return 0;
}
