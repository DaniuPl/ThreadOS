/* The Intel Wireless 3160 model: the firmware running on the card
 *
 * Written from the init specification (SPEC_I, iwlspec_init.md), not from
 * the driver. The firmware file is read here the way SPEC_I section 1
 * describes it, so the model knows what the driver must load where and
 * which values the commands must carry (DEF_CALIB, PHY_SKU) without
 * taking them from the driver.
 *
 * The INIT image: ALIVE (VER_3, 68 bytes, as SPEC_I 3 says this firmware
 * sends) with an MFUART notification in the same receive buffer;
 * BT_CONFIG, answered with a BT profile notification beside it; NVM
 * reads from a model NVM (a known MAC, a known channel set); TX_ANT,
 * then PHY_CONFIGURATION, which starts the calibration: CALIB_RES
 * notifications for the PHY DB, several to a buffer, one section sent
 * twice (the later copy is the one that counts, SPEC_I 6.1), then
 * INIT_COMPLETE. With the radio switch off nothing is calibrated.
 *
 * The runtime image: ALIVE, then the bring-up commands of SPEC_I 7,
 * each checked byte for byte, the PHY DB checked against what the INIT
 * image produced, the order constraints the specification states held
 * as rules and the rest of the order only noted.
 *
 * A command with a payload of the wrong size, or one this firmware does
 * not know, makes it assert (BAD_COMMAND), as the card's own firmware
 * does; the error log is the VER_2 table (SPEC_I 8.2 and the contract's
 * correction).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"

struct fw_stats fs;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* --- the file ------------------------------------------------------------- */

struct sec {
    uint32_t addr;
    const uint8_t *data;
    uint32_t len;
};
struct image {
    struct sec s[16];
    int n, nload;
    const char *name;
};
#define IMG_RT 0
#define IMG_INIT 1
static struct image img[2] = { { .name = "runtime" }, { .name = "INIT" } };
static uint32_t def_calib[4][2];
static int def_calib_seen[4];
static uint32_t phy_sku, api0, capa0, n_cpu;

static void bad_file(const char *why)
{
    printf("FAIL: the firmware file: %s\n", why);
    exit(2);
}

void fw_parse(const uint8_t *f, size_t len)
{
    if (len < 88 || rd32(f) != 0 || rd32(f + 4) != 0x0A4C5749u)
        bad_file("not a TLV firmware file (SPEC_I 1.1)");
    size_t o = 88;
    while (len - o >= 8) {
        uint32_t t = rd32(f + o), l = rd32(f + o + 4);
        if (l > len - o - 8)
            bad_file("a TLV longer than the file");
        const uint8_t *d = f + o + 8;
        if (t == 19 || t == 20) {
            struct image *im = &img[t == 19 ? IMG_RT : IMG_INIT];
            if (l < 4 || im->n >= 16)
                bad_file("a section TLV the specification does not allow");
            im->s[im->n].addr = rd32(d);
            im->s[im->n].data = d + 4;
            im->s[im->n].len = l - 4;
            im->n++;
        } else if (t == 22 && l == 12 && rd32(d) < 4) {
            def_calib[rd32(d)][0] = rd32(d + 4);
            def_calib[rd32(d)][1] = rd32(d + 8);
            def_calib_seen[rd32(d)] = 1;
        } else if (t == 23 && l >= 4) {
            phy_sku = rd32(d);
        } else if (t == 29 && l == 8 && rd32(d) == 0) {
            api0 = rd32(d + 4);
        } else if (t == 30 && l == 8 && rd32(d) == 0) {
            capa0 = rd32(d + 4);
        } else if (t == 27 && l == 4) {
            n_cpu = rd32(d);
        }
        o += 8 + ((l + 3) & ~3u);
    }
    if (o != len)
        bad_file("bytes left over after the last TLV");
    for (int i = 0; i < 2; i++) {
        struct image *im = &img[i];
        while (im->nload < im->n && im->s[im->nload].len &&
               im->s[im->nload].addr != 0xFFFFCCCCu &&
               im->s[im->nload].addr != 0xAAAABBBBu)
            im->nload++;
        if (im->nload == 0)
            bad_file("an image with nothing to load");
    }
    if (!def_calib_seen[0] || !def_calib_seen[1] || !phy_sku)
        bad_file("no DEF_CALIB or PHY_SKU");
    if (n_cpu != 1)
        bad_file("not a single-CPU firmware");
    /* Where the specifications disagree, or a value is the model's
       own reading of the file */
    if (api0 & (1u << 20))
        note("API bit 20 (NEW_VERSION) is set in the file (0x%08x): "
             "SPEC_T 10.4 says clear, SPEC_I and the contract say set; the "
             "error log is the VER_2 table", api0);
    note("the file's sections: INIT %u+%u B, runtime %u+%u B at 0x%x/0x%x; "
         "SPEC_I 1.2 puts the first SEC_RT TLV at offset 200, the file has "
         "it at 204", img[IMG_INIT].s[0].len, img[IMG_INIT].s[1].len,
         img[IMG_RT].s[0].len, img[IMG_RT].s[1].len, img[IMG_RT].s[0].addr,
         img[IMG_RT].s[1].addr);
    (void)capa0;
}

/* What the commands must carry, from the file (SPEC_I 1.4, 1.8) */
static uint32_t valid_tx(void) { return (phy_sku >> 16) & 0xF; }
static uint32_t phy_cfg_want(void)
{
    uint32_t vt = (phy_sku >> 16) & 0xF, vr = (phy_sku >> 20) & 0xF;
    uint32_t mask = ~(0xFu << 16 | 0xFu << 20) | vt << 16 | vr << 20;
    return phy_sku & mask;
}

/* --- the model's NVM (SPEC_I 5) ---------------------------------------------- */

const uint8_t nvm_mac[6] = { 0x7C, 0x5C, 0xF8, 0x12, 0x34, 0x56 };
const uint8_t nvm_chan_num[39] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
/* VALID 0x1, IBSS 0x2, ACTIVE 0x8, RADAR 0x10, INDOOR 0x20, GO_CONC 0x40,
   WIDE 0x100, 40MHZ 0x200, 80MHZ 0x400, 160MHZ 0x800 */
const uint16_t nvm_chan_flags[39] = {
    /* 1-11 active, 12 passive, 13 passive but GO-concurrent, 14 off */
    0x30B, 0x30B, 0x30B, 0x30B, 0x30B, 0x30B, 0x30B, 0x30B, 0x30B, 0x30B,
    0x30B, 0x301, 0x341, 0x000,
    /* 36-48 up to 80 MHz; 52-64 radar */
    0x70B, 0x70B, 0x70B, 0x70B, 0x711, 0x711, 0x711, 0x711,
    /* 100-116 radar with 160 MHz; 120-128 off (weather radar); 132-140
       radar; 144 radar, no 80 MHz */
    0xF11, 0xF11, 0xF11, 0xF11, 0x711, 0x000, 0x000, 0x000, 0x711, 0x711,
    0x711, 0x311,
    /* 149 indoor only; 153-161; 165 20 MHz only */
    0x72B, 0x70B, 0x70B, 0x70B, 0x00B,
};

static uint8_t nvm_hw[4096];   /* a multiple of 2048: ends on status 1 */
static uint8_t nvm_sw[2688];
static uint8_t nvm_calib[1200];
static uint8_t nvm_prod[512];

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)(x >> 16);
    }
}

static void nvm_build(void)
{
    fill(nvm_hw, sizeof(nvm_hw), 1);
    fill(nvm_sw, sizeof(nvm_sw), 2);
    fill(nvm_calib, sizeof(nvm_calib), 3);
    fill(nvm_prod, sizeof(nvm_prod), 4);
    /* The MAC at word 0x15, its bytes pairwise swapped ("214365") */
    for (int i = 0; i < 6; i += 2) {
        nvm_hw[0x2A + i] = nvm_mac[i + 1];
        nvm_hw[0x2A + i + 1] = nvm_mac[i];
    }
    wr16(nvm_sw + 0, NVM_VERSION);
    wr16(nvm_sw + 2, NVM_RADIO_CFG);
    wr16(nvm_sw + 4, NVM_SKU);
    wr16(nvm_sw + 6, 1);
    for (int i = 0; i < 39; i++)
        wr16(nvm_sw + 0x40 + 2 * i, nvm_chan_flags[i]);
    wr16(nvm_calib + 0xBC, 0x1234);
    wr16(nvm_calib + 0xBE, 0x5678);
}

static const uint8_t *nvm_section(int type, uint32_t *size)
{
    switch (type) {
    case 0: *size = sizeof(nvm_hw); return nvm_hw;
    case 1: *size = sizeof(nvm_sw); return nvm_sw;
    case 4: *size = sizeof(nvm_calib); return nvm_calib;
    case 5: *size = sizeof(nvm_prod); return nvm_prod;
    }
    *size = 0;
    return NULL;
}

/* --- the PHY DB the INIT image produces (SPEC_I 6) ---------------------------- */

struct pdb {
    int present;
    uint16_t type, len;
    uint8_t data[1600];
};
static struct pdb db_cfg, db_nch, db_papd[9], db_txp[9];
static const uint16_t txp_max_idx[9] = { 13, 17, 21, 25, 29, 32, 34, 36, 38 };
static int calib_run;

static void pdb_make(struct pdb *e, uint16_t type, int chg, uint16_t len,
                     uint32_t seed)
{
    e->present = 1;
    e->type = type;
    e->len = len;
    fill(e->data, len, seed);
    if (type == 4)
        wr16(e->data, (uint16_t)chg);
    if (type == 5) {
        wr32(e->data, (uint32_t)chg);         /* space: low 16 bits chg_id */
        wr16(e->data + 4, txp_max_idx[chg]);
    }
}

/* --- the firmware's state ----------------------------------------------------- */

enum { FW_OFF, FW_BOOTING, FW_RUNNING, FW_CRASHED };
static int st = FW_OFF;
static int image = -1;
static uint32_t gen;
static int sw_blocked;

struct cmd {
    uint8_t id;
    uint16_t seq;
    uint32_t len;
    uint8_t data[20 * 4096];
};
static struct cmd cq[16];
static int cq_head, cq_n;
static uint64_t proc_free_at;

/* INIT image */
static int init_bt, init_txant, init_phycfg;
/* Runtime image */
enum { S_SF, S_TXANT, S_BT, S_PHYDB, S_PHYCFG, S_SCDQ, S_ADDSTA, S_PHYCTX,
       S_POWER, S_N };
static const char *step_name[S_N] = {
    "REPLY_SF_CFG_CMD (0xD1)", "TX_ANT_CONFIGURATION_CMD (0x98)",
    "BT_CONFIG (0x9B)", "PHY_DB_CMD (0x6C)", "PHY_CONFIGURATION_CMD (0x6A)",
    "SCD_QUEUE_CFG (0x1D)", "ADD_STA (0x18)", "PHY_CONTEXT_CMD (0x08)",
    "POWER_TABLE_CMD (0x77)",
};
static int rt_seen[S_N];
static int phydb_next, phydb_bad, phyctx_n, rt_cmds;
static const struct pdb *phydb_want[32];
static int phydb_nwant;

int fw_running(void)
{
    return st == FW_RUNNING;
}

uint32_t fw_scd_base(void)
{
    return st == FW_RUNNING || st == FW_CRASHED ? SCD_BASE : 0;
}

/* --- answering: packets into receive buffers ------------------------------------ */

static struct rxbatch cur;

static void batch_begin(void)
{
    memset(&cur, 0, sizeof(cur));
}

static void batch_send(uint64_t delay)
{
    if (!cur.npkts)
        return;
    cur.ready = now_ns + delay;
    card_rx_queue(&cur);
    batch_begin();
}

/* One packet: len_n_flags, the 4-byte header, the payload; the next one
   64-byte aligned (SPEC_T 6.4) */
static void batch_add(uint8_t id, uint16_t seq, const void *p, uint32_t n,
                      int answer)
{
    uint32_t step = (n + 8 + 63) & ~63u;
    if (cur.used + step > sizeof(cur.data))
        batch_send(0);
    uint8_t *q = cur.data + cur.used;
    memset(q, 0xEE, step);                    /* the gap is not zero */
    wr32(q, 4 + n);
    q[4] = id;
    q[5] = 0;
    wr16(q + 6, seq);
    memcpy(q + 8, p, n);
    cur.used += step;
    cur.npkts++;
    cur.answers += answer;
}

static void answer(const struct cmd *c, const void *p, uint32_t n)
{
    batch_add(c->id, c->seq, p, n, 1);
}

static void answer_status(const struct cmd *c, uint32_t status)
{
    uint8_t s[4];
    wr32(s, status);
    answer(c, s, 4);
}

/* --- crashing ----------------------------------------------------------------- */

static void crash(uint32_t id)
{
    if (st != FW_RUNNING)
        return;
    st = FW_CRASHED;
    gen++;
    cq_n = 0;
    fs.crashed = 1;
    fs.crash_id = id;
    /* LOG_ERROR_TABLE_API_S_VER_2, 38 dwords (SPEC_I 8.2) */
    uint8_t *t = sram_ptr(ERRLOG_PTR, 152);
    static const uint32_t vals[38] = {
        1, 0, 0x00012F4A, 0x0001A2B0, 0x0000C3D4, 0x00011111, 0x00022222,
        0xDA7A0001, 0xDA7A0002, 0xDA7A0003, 0x0BC0, 0x7570, 0,
        0x1, 0x2, 0x3, 17, 0xBFB58538, CARD_HW_REV, 0x00000003, 0x0000C001,
        0x00805000, 0x00806000, 0x00000918, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0x12345678, 0,
    };
    for (int i = 0; i < 38; i++)
        wr32(t + 4 * i, vals[i]);
    wr32(t + 4, id);
    batch_begin();
    card_raise(0x02000000u, 0);               /* SW_ERR */
    card_firmware_stopped();
}

static void bad_command(const struct cmd *c, const char *why)
{
    violation("command 0x%02x (sequence 0x%04x): %s - the firmware asserts "
              "(BAD_COMMAND)", c->id, c->seq, why);
    crash(0x38);
}

static int want_len(const struct cmd *c, uint32_t n, const char *ref)
{
    if (c->len == n)
        return 1;
    char why[160];
    snprintf(why, sizeof(why), "a %u-byte payload where the command has %u "
             "(%s)", c->len, n, ref);
    bad_command(c, why);
    return 0;
}

/* --- boot ------------------------------------------------------------------- */

static void fw_boot(uint32_t g)
{
    if (g != gen || st != FW_BOOTING)
        return;
    st = FW_RUNNING;
    /* The firmware's own memory: the scheduler area is whatever it was
       until the driver clears it (SPEC_T 7.4 step 3); the error log empty */
    uint8_t *m = sram_ptr(SCD_BASE, 0x1000);
    for (int i = 0; i < 0x1000; i++)
        m[i] = (uint8_t)(0xC3 ^ i);
    memset(sram_ptr(ERRLOG_PTR, 152), 0, 152);

    /* ALIVE, VER_3 (SPEC_I 3.1) */
    uint8_t a[68];
    memset(a, 0, sizeof(a));
    wr16(a + 0, 0xCAFE);
    wr16(a + 2, card_rfkill() ? 1 : 0);
    wr32(a + 4, 0);                           /* ucode_minor */
    wr32(a + 8, 17);                          /* ucode_major */
    a[12] = image == IMG_INIT ? 9 : 0;        /* ver_subtype */
    wr32(a + 16, 0x00ABCDEF);
    wr32(a + 20, ERRLOG_PTR);
    wr32(a + 24, EVLOG_PTR);
    wr32(a + 40, SCD_BASE);
    batch_begin();
    batch_add(0x01, 0x8000, a, sizeof(a), 0);
    if (image == IMG_INIT) {
        /* MFUART_LOAD_NOTIFICATION, in the same buffer: a notification
           the driver must accept and ignore (SPEC_I 2.3) */
        uint8_t mf[16];
        memset(mf, 0, sizeof(mf));
        wr32(mf, 0x00010003);
        batch_add(0xB1, 0x8000, mf, sizeof(mf), 0);
        fs.init_alive++;
    } else {
        fs.rt_alive++;
    }
    batch_send(0);
    card_raise(0x00000001u, 0);               /* ALIVE */
}

static uint32_t mismatches(int im, uint32_t *at, uint8_t *want, uint8_t *got)
{
    uint32_t n = 0;
    for (int k = 0; k < img[im].nload; k++) {
        const struct sec *s = &img[im].s[k];
        const uint8_t *m = sram_ptr(s->addr, s->len);
        if (!m)
            return 0xFFFFFFFFu;
        for (uint32_t i = 0; i < s->len; i++)
            if (m[i] != s->data[i]) {
                if (!n) {
                    *at = s->addr + i;
                    *want = s->data[i];
                    *got = m[i];
                }
                n++;
            }
    }
    return n;
}

void fw_cpu_reset(void)
{
    st = FW_OFF;
    image = -1;
    gen++;
    cq_n = 0;
}

void fw_cpu_start(void)
{
    gen++;
    cq_n = 0;
    uint32_t at_i = 0, at_r = 0;
    uint8_t w_i = 0, g_i = 0, w_r = 0, g_r = 0;
    uint32_t mi = mismatches(IMG_INIT, &at_i, &w_i, &g_i);
    uint32_t mr = mismatches(IMG_RT, &at_r, &w_r, &g_r);
    if (mi == 0) {
        image = IMG_INIT;
        fs.init_starts++;
        fs.init_bytes_ok = 1;
    } else if (mr == 0) {
        image = IMG_RT;
        fs.rt_starts++;
        fs.rt_bytes_ok = 1;
    } else {
        int near_init = mi <= mr;
        violation("the CPU was started on device memory that holds neither "
                  "image: nearest is the %s image, %u bytes differ, first "
                  "at 0x%06x (file 0x%02x, card 0x%02x) (SPEC_T 5; SPEC_I "
                  "1.3)", near_init ? "INIT" : "runtime",
                  near_init ? mi : mr, near_init ? at_i : at_r,
                  near_init ? w_i : w_r, near_init ? g_i : g_r);
        st = FW_OFF;                          /* it runs into the weeds */
        return;
    }
    if (image == IMG_RT) {
        if (card_rfkill())
            violation("the runtime image started with the radio switch off "
                      "(SPEC_T 4.4 step 4; SPEC_I 7 step 4)");
        if (!fs.init_calibrated) {
            fs.rt_before_calib = 1;
            violation("the runtime image started before the INIT image "
                      "finished calibrating: it has no PHY DB to be given "
                      "(SPEC_I 4, 7)");
        }
        memset(rt_seen, 0, sizeof(rt_seen));
        phydb_next = phydb_bad = phyctx_n = rt_cmds = 0;
    } else {
        init_bt = init_txant = init_phycfg = 0;
    }
    sw_blocked = (card_gp1() & 0x6) != 0;
    if (sw_blocked)
        violation("the firmware started with CSR_UCODE_DRV_GP1 = 0x%x: "
                  "SW_BIT_RFKILL/CMD_BLOCKED were not cleared before the "
                  "load (SPEC_T 4.4 steps 7, 10)", card_gp1());
    uint32_t want = 0x9200u | (CARD_HW_REV & 0xF);
    uint32_t hw = card_hw_if_config();
    if ((hw & 0xFF0F) != want)
        violation("CSR_HW_IF_CONFIG_REG holds 0x%04x in the MAC/PHY/radio "
                  "fields at the firmware start, not 0x%04x (nic_config from "
                  "PHY_SKU and CSR_HW_REV, SPEC_T 4.5; SPEC_I 1.4)",
                  hw & 0xFF0F, want);
    if (!card_osc_forced())
        violation("the firmware started without OSC_CLK_FORCE_CONTROL (the "
                  "7260/3160 oscillator sequence, SPEC_T 4.2 step 9)");
    st = FW_BOOTING;
    model_at(15 * MS, fw_boot, gen);
}

void fw_nmi(void)
{
    if (st == FW_RUNNING) {
        note("the driver forced an NMI; the firmware reports "
             "NMI_INTERRUPT_HOST (0x66)");
        crash(0x66);
    }
}

/* --- the INIT image ---------------------------------------------------------- */

static void calib_step(uint32_t arg)
{
    if (arg >> 3 != gen || st != FW_RUNNING)
        return;
    int step = arg & 7;
    uint32_t s = (uint32_t)calib_run * 1000;
    uint8_t buf[1700];
    batch_begin();
    if (step == 0) {
        pdb_make(&db_cfg, 1, 0, 264, s + 1);
        pdb_make(&db_nch, 2, 0, 1440, s + 2);
        for (int i = 0; i < 9; i++)
            db_papd[i].present = db_txp[i].present = 0;
    }
    struct pdb *list[10];
    int n = 0;
    if (step == 0) {
        list[n++] = &db_cfg;
        list[n++] = &db_nch;
    } else if (step == 1) {
        for (int i = 0; i < 4; i++) {
            pdb_make(&db_papd[i], 4, i, 112, s + 10 + (uint32_t)i);
            list[n++] = &db_papd[i];
        }
    } else if (step == 2) {
        for (int i = 0; i < 9; i++) {
            pdb_make(&db_txp[i], 5, i, 160, s + 20 + (uint32_t)i);
            list[n++] = &db_txp[i];
        }
    } else {
        /* The same section again, newer: it replaces the first */
        pdb_make(&db_txp[3], 5, 3, 160, s + 99);
        list[n++] = &db_txp[3];
    }
    for (int i = 0; i < n; i++) {
        wr16(buf, list[i]->type);
        wr16(buf + 2, list[i]->len);
        memcpy(buf + 4, list[i]->data, list[i]->len);
        batch_add(0x6B, 0x8000, buf, 4u + list[i]->len, 0);
        fs.calib_sent++;
    }
    if (step == 3) {
        /* INIT_COMPLETE_NOTIF: its payload is not in the specification
           (SPEC_I 4 step 10); four zero bytes */
        uint8_t z[4] = { 0, 0, 0, 0 };
        batch_add(0x04, 0x8000, z, 4, 0);
        fs.init_calibrated = 1;
        fs.calib_unique = 2 + 4 + 9;
    }
    batch_send(0);
    if (step < 3)
        model_at(10 * MS, calib_step, gen << 3 | (uint32_t)(step + 1));
}

static void nvm_access(const struct cmd *c)
{
    if (c->len < 8) {
        want_len(c, 8, "SPEC_I 5.1");
        return;
    }
    const uint8_t *p = c->data;
    uint8_t op = p[0], target = p[1];
    uint16_t type = rd16(p + 2), off = rd16(p + 4), len = rd16(p + 6);
    uint8_t r[8 + 2048];
    memset(r, 0, sizeof(r));
    wr16(r + 0, off);
    wr16(r + 4, type);
    if (op != 0) {
        violation("NVM_ACCESS_CMD with op_code %u: stage 1 only reads the "
                  "NVM (SPEC_I 5.1)", op);
        wr16(r + 6, 2);
        answer(c, r, 8);
        return;
    }
    if (c->len != 8) {
        want_len(c, 8, "SPEC_I 5.1: a read is 8 bytes");
        return;
    }
    if (target != 0)
        note("NVM_ACCESS_CMD with target %u; Linux reads the cache (0)",
             target);
    if (len != 2048)
        note("NVM read of %u bytes; the spec reads 2048 at a time", len);
    if (len > 2048)
        len = 2048;
    fs.nvm_reads++;
    if (init_phycfg && image == IMG_INIT)
        note("NVM read after PHY_CONFIGURATION_CMD; SPEC_I 4 reads it "
             "before the calibration");
    uint32_t size;
    const uint8_t *s = nvm_section(type, &size);
    if (!s || off >= size) {
        /* An absent section, or the end of one whose size is a multiple
           of 2048. Which non-zero status a missing section gives is not
           in the specification; status 1 for both. */
        if (!s)
            note("NVM sections the model's card does not have answer status "
                 "1 (READ_NVM_CHUNK_NOT_VALID_ADDRESS) at offset 0; the "
                 "specification does not say which status a real one gives");
        wr16(r + 6, 1);
        answer(c, r, 8);
        return;
    }
    uint32_t n = size - off < len ? size - off : len;
    wr16(r + 2, (uint16_t)n);
    memcpy(r + 8, s + off, n);
    answer(c, r, 8 + n);
}

static void bt_config(const struct cmd *c)
{
    if (!want_len(c, 8, "BT_COEX_CMD_API_S_VER_6, SPEC_I 7.5"))
        return;
    uint32_t mode = rd32(c->data), mods = rd32(c->data + 4);
    if (mode != 1 && mode != 0)
        violation("BT_CONFIG mode %u: 1 (BT_COEX_NW), or 0 with coexistence "
                  "off (SPEC_I 7.5)", mode);
    if (mode == 0)
        note("BT_CONFIG with coexistence disabled (mode 0)");
    if (mods != 0x17)
        violation("BT_CONFIG enabled_modules 0x%x, not 0x17 (SPEC_I 7.5)",
                  mods);
    answer_status(c, 0);
    /* A BT profile notification beside the answer, in the same buffer */
    uint8_t prof[40];
    memset(prof, 0, sizeof(prof));
    batch_add(0xCE, 0x8000, prof, sizeof(prof), 0);
}

static void tx_ant(const struct cmd *c)
{
    if (!want_len(c, 4, "SPEC_I 4 step 8"))
        return;
    if (rd32(c->data) != valid_tx())
        violation("TX_ANT_CONFIGURATION_CMD 0x%x: the firmware's valid TX "
                  "antennas (PHY_SKU) are 0x%x (SPEC_I 1.4, 4 step 8)",
                  rd32(c->data), valid_tx());
    answer_status(c, 0);
}

static int phy_cfg_check(const struct cmd *c, int which)
{
    if (!want_len(c, 12, "SPEC_I 4 step 9"))
        return 0;
    uint32_t a = rd32(c->data), f = rd32(c->data + 4), e = rd32(c->data + 8);
    if (a != phy_cfg_want() || f != def_calib[which][0] ||
        e != def_calib[which][1])
        violation("PHY_CONFIGURATION_CMD for the %s image carries %08x %08x "
                  "%08x, not %08x %08x %08x (PHY_SKU and DEF_CALIB[%d] from "
                  "the file; SPEC_I 4 step 9, 7.7)", which ? "INIT" : "runtime",
                  a, f, e, phy_cfg_want(), def_calib[which][0],
                  def_calib[which][1], which);
    answer_status(c, 0);
    return 1;
}

static void init_command(const struct cmd *c)
{
    switch (c->id) {
    case 0x9B:
        if (card_rfkill())
            note("BT_CONFIG sent under RF-kill; Linux's transport drops it "
                 "(no CMD_SEND_IN_RFKILL), SPEC_I 4 lists it before the "
                 "RF-kill check - the model takes it");
        init_bt = 1;
        bt_config(c);
        break;
    case 0x88:
        nvm_access(c);
        break;
    case 0x98:
        if (card_rfkill())
            note("TX_ANT sent to the INIT image under RF-kill; SPEC_I 4 "
                 "step 6 returns before it");
        init_txant = 1;
        tx_ant(c);
        break;
    case 0x6A:
        if (!init_txant)
            violation("PHY_CONFIGURATION_CMD before TX_ANT_CONFIGURATION_CMD: "
                      "\"send TX valid antennas before triggering "
                      "calibrations\" (SPEC_I 4 steps 8-9)");
        if (!init_bt)
            note("the INIT image calibrates without BT_CONFIG (SPEC_I 4 "
                 "step 3 sends it first; whether the firmware needs it is "
                 "undetermined)");
        if (!phy_cfg_check(c, 1))
            return;
        if (init_phycfg) {
            note("PHY_CONFIGURATION_CMD sent twice to the INIT image");
            return;
        }
        init_phycfg = 1;
        if (card_rfkill() || sw_blocked) {
            note("the radio is switched off: the INIT image does not "
                 "calibrate (no CALIB_RES_NOTIF_PHY_DB, no INIT_COMPLETE)");
            return;
        }
        calib_run++;
        model_at(40 * MS, calib_step, gen << 3);
        break;
    case 0x03:
        answer(c, NULL, 0);
        break;
    default:
        if (c->id == 0x6C || c->id == 0x1D || c->id == 0x18 ||
            c->id == 0x08 || c->id == 0x77 || c->id == 0xD1 ||
            c->id == 0x25 || c->id == 0x7E || c->id == 0xEE) {
            violation("command 0x%02x sent to the INIT image: it belongs to "
                      "the runtime bring-up (SPEC_I 4, 7)", c->id);
            answer_status(c, 0);
            return;
        }
        bad_command(c, "not a command this firmware knows");
    }
}

/* --- the runtime image ----------------------------------------------------- */

static void rt_step(int s)
{
    if (!rt_seen[s])
        for (int j = s + 1; j < S_N; j++)
            if (rt_seen[j]) {
                note("runtime bring-up: %s after %s; SPEC_I 7.15's order has "
                     "it before (a firmware need for that order is not "
                     "documented)", step_name[s], step_name[j]);
                break;
            }
    rt_seen[s]++;
}

static void phydb_list(void)
{
    phydb_nwant = 0;
    phydb_want[phydb_nwant++] = &db_cfg;
    phydb_want[phydb_nwant++] = &db_nch;
    for (int i = 0; i < 9; i++)
        if (db_papd[i].present)
            phydb_want[phydb_nwant++] = &db_papd[i];
    for (int i = 0; i < 9; i++)
        if (db_txp[i].present)
            phydb_want[phydb_nwant++] = &db_txp[i];
}

static void phy_db(const struct cmd *c)
{
    if (c->len < 4 || c->len != 4u + rd16(c->data + 2)) {
        bad_command(c, "PHY_DB_CMD whose length field does not match its "
                       "payload (SPEC_I 6.2)");
        return;
    }
    if (phydb_next == 0)
        phydb_list();
    uint16_t type = rd16(c->data), len = rd16(c->data + 2);
    const uint8_t *d = c->data + 4;
    if (phydb_next >= phydb_nwant) {
        violation("PHY_DB_CMD #%d (type %u): the INIT image produced only "
                  "%d sections (SPEC_I 6.2)", phydb_next + 1, type,
                  phydb_nwant);
        phydb_bad = 1;
    } else {
        const struct pdb *w = phydb_want[phydb_next];
        if (type != w->type || len != w->len || memcmp(d, w->data, len)) {
            int chg = w->type == 4 || w->type == 5 ? rd16(w->data) : 0;
            violation("PHY_DB_CMD #%d: type %u, %u bytes%s, where the "
                      "runtime image expects type %u chg %d, %u bytes, the "
                      "INIT image's latest data for it (SPEC_I 6.1, 6.2)",
                      phydb_next + 1, type, len,
                      type == w->type && len == w->len ? " (other data)" : "",
                      w->type, chg, w->len);
            phydb_bad = 1;
        }
    }
    phydb_next++;
    fs.phydb_sent++;
    answer_status(c, 0);
}

static void sf_cfg(const struct cmd *c)
{
    if (!want_len(c, 92, "SF_CFG_API_S_VER_2, SPEC_I 7.3"))
        return;
    const uint8_t *p = c->data;
    static const uint32_t bss[10] = { 2016, 320, 2016, 320, 10016, 2016,
                                      2016, 320, 2016, 320 };
    int ok = rd32(p) == 3 && rd32(p + 4) == 4096 && rd32(p + 8) == 8192;
    for (int i = 0; i < 10; i++)
        ok = ok && rd32(p + 12 + 4 * i) == 1000000;
    int is_bss = 1, is_def = 1;
    for (int i = 0; i < 10; i++) {
        is_bss = is_bss && rd32(p + 52 + 4 * i) == bss[i];
        is_def = is_def && rd32(p + 52 + 4 * i) == (i & 1 ? 160u : 400u);
    }
    if (!ok || (!is_bss && !is_def))
        violation("REPLY_SF_CFG_CMD does not carry SF_INIT_OFF with the "
                  "watermarks and timers of SPEC_I 7.3");
    else if (is_def)
        note("SF_CFG with the default full-on table (later kernels); "
             "SPEC_I 7.3 follows v4.4's BSS table - both accepted");
    answer_status(c, 0);
}

static void scd_queue_cfg(const struct cmd *c)
{
    if (!want_len(c, 12, "SPEC_I 7.8b"))
        return;
    const uint8_t *p = c->data;
    static const uint8_t want[12] = { 0, 0, 8, 15, 1, 0, 5, 64, 0, 0, 0, 0 };
    if (p[1] != 0 && !memcmp(p + 2, want + 2, 10) && p[0] == 0)
        note("SCD_QUEUE_CFG with sta_id %u; SPEC_I 7.8b has 0", p[1]);
    else if (memcmp(p, want, 12))
        violation("SCD_QUEUE_CFG is %02x %02x %02x %02x %02x %02x %02x %02x "
                  "%02x%02x, not the aux queue 15 on FIFO 5 of SPEC_I 7.8b",
                  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
    if (!card_q15_wptr_set())
        note("queue 15 configured in the firmware before its write pointer "
             "was set to the SSN (SPEC_I 7.8a)");
    uint8_t r[4] = { p[0], p[1], p[2], p[3] };
    answer(c, r, 4);
}

static void add_sta(const struct cmd *c)
{
    if (!want_len(c, 44, "ADD_STA_CMD_API_S_VER_7, SPEC_I 7.8c"))
        return;
    const uint8_t *p = c->data;
    if (!rt_seen[S_SCDQ])
        violation("ADD_STA for the aux station before its queue was mapped "
                  "with SCD_QUEUE_CFG (SPEC_I 7.8)");
    uint8_t want[44];
    memset(want, 0, sizeof(want));
    wr32(want + 4, 4);                      /* MAC_INDEX_AUX, color 0 */
    want[16] = p[16];
    wr32(want + 40, 0x8000);
    if (memcmp(p, want, 44))
        violation("ADD_STA (aux station) differs from SPEC_I 7.8c: add, "
                  "mac_id_n_color 4, no address, tfd_queue_msk 0x8000, the "
                  "rest zero");
    if (p[16] != 1)
        note("aux station id %u; on a fresh start SPEC_I 7.8c has 1", p[16]);
    answer_status(c, 1);                    /* ADD_STA_SUCCESS */
}

static void phy_context(const struct cmd *c)
{
    if (!want_len(c, 36, "SPEC_I 7.9"))
        return;
    uint8_t want[36];
    memset(want, 0, sizeof(want));
    wr32(want + 0, (uint32_t)phyctx_n);
    wr32(want + 4, 1);                      /* ADD */
    want[16] = 1;                           /* PHY_BAND_24 */
    want[17] = 1;                           /* the first 2.4 GHz channel */
    wr32(want + 20, valid_tx());
    wr32(want + 24, 0x1402);
    if (memcmp(c->data, want, 36))
        violation("PHY_CONTEXT_CMD #%d differs from SPEC_I 7.9 (id %d, ADD, "
                  "2.4 GHz channel 1, 20 MHz, tx 0x1, rx 0x1402)",
                  phyctx_n, phyctx_n);
    phyctx_n++;
    answer_status(c, 0);
}

static void rt_command(const struct cmd *c)
{
    rt_cmds++;
    if (scenario == SC_ASSERT && rt_cmds == 1) {
        /* The scenario: the runtime firmware dies at its first command */
        crash(0x34);
        return;
    }
    switch (c->id) {
    case 0xD1:
        rt_step(S_SF);
        sf_cfg(c);
        break;
    case 0x98:
        rt_step(S_TXANT);
        tx_ant(c);
        break;
    case 0x9B:
        rt_step(S_BT);
        bt_config(c);
        break;
    case 0x6C:
        rt_step(S_PHYDB);
        if (rt_seen[S_PHYCFG])
            violation("PHY_DB_CMD after PHY_CONFIGURATION_CMD: \"send phy db "
                      "control command and then phy db calibration\" "
                      "(SPEC_I 7.6)");
        phy_db(c);
        break;
    case 0x6A:
        if (!rt_seen[S_TXANT])
            violation("PHY_CONFIGURATION_CMD before TX_ANT_CONFIGURATION_CMD "
                      "in the runtime image (SPEC_I 7.4)");
        if (phydb_next == 0)
            phydb_list();
        if (phydb_next < phydb_nwant)
            violation("PHY_CONFIGURATION_CMD after %d of the %d PHY DB "
                      "sections: the runtime image needs them all first "
                      "(SPEC_I 7.6)", phydb_next, phydb_nwant);
        else if (!phydb_bad && !rt_seen[S_PHYCFG])
            fs.phydb_ok = 1;
        rt_step(S_PHYCFG);
        phy_cfg_check(c, 0);
        break;
    case 0x1D:
        rt_step(S_SCDQ);
        scd_queue_cfg(c);
        break;
    case 0x18:
        rt_step(S_ADDSTA);
        add_sta(c);
        break;
    case 0x08:
        rt_step(S_PHYCTX);
        phy_context(c);
        break;
    case 0x77:
        if (!want_len(c, 4, "SPEC_I 7.12"))
            return;
        if (rd16(c->data) > 1 || rd16(c->data + 2))
            violation("POWER_TABLE_CMD flags 0x%x: 0x1 (power save) or 0 "
                      "(CAM), reserved 0 (SPEC_I 7.12)", rd16(c->data));
        rt_step(S_POWER);
        answer_status(c, 0);
        break;
    case 0x25: {
        if (!want_len(c, 0, "SPEC_I 7.1"))
            return;
        note("SHARED_MEM_CFG asked (optional); whether this firmware answers "
             "is undetermined - the model does");
        uint8_t r[68];
        memset(r, 0, sizeof(r));
        wr32(r, 0x00800000);
        wr32(r + 4, 0x14000);
        answer(c, r, sizeof(r));
        break;
    }
    case 0x7E:
        if (!want_len(c, 4, "SPEC_I 7.10"))
            return;
        note("REPLY_THERMAL_MNG_BACKOFF sent (optional, SPEC_I 7.10)");
        answer_status(c, 0);
        break;
    case 0xEE:
        if (!want_len(c, 32, "SPEC_I 7.11"))
            return;
        note("LTR_CONFIG sent although the card's PCIe LTR is off (SPEC_I "
             "7.11 sends it only when on)");
        answer_status(c, 0);
        break;
    case 0x88:
        nvm_access(c);
        break;
    case 0x03:
        answer(c, NULL, 0);
        break;
    default:
        bad_command(c, "not a command this firmware knows");
    }
    int all = 1;
    for (int s = 0; s < S_N; s++)
        all = all && rt_seen[s];
    if (all && !fs.rt_configured)
        fs.rt_configured = 1;
}

void fw_rt_report(void)
{
    for (int s = 0; s < S_N; s++)
        if (!rt_seen[s]) {
            fs.rt_required_missing++;
            violation("the runtime image never got %s (SPEC_I 7.15, the "
                      "minimal-safe set)", step_name[s]);
        }
    if (rt_seen[S_PHYCTX] && phyctx_n < 3)
        note("%d PHY contexts added; Linux adds 3 (SPEC_I 7.9: at least "
             "context 0)", phyctx_n);
}

/* --- commands in --------------------------------------------------------------- */

static void fw_process(uint32_t g)
{
    if (g != gen || st != FW_RUNNING || !cq_n)
        return;
    struct cmd *c = &cq[cq_head];
    cq_head = (cq_head + 1) % 16;
    cq_n--;
    batch_begin();
    if (image == IMG_INIT)
        init_command(c);
    else
        rt_command(c);
    if (st == FW_RUNNING)
        batch_send(0);
}

void fw_command(const uint8_t *b, uint32_t len, int index)
{
    if (st != FW_RUNNING) {
        violation("a host command reached a firmware that is not running");
        card_cmd_done();
        return;
    }
    if (len < 4) {
        violation("a host command of %u bytes: shorter than its header", len);
        card_cmd_done();
        return;
    }
    if (cq_n >= 16) {
        violation("more than 16 host commands waiting in the firmware");
        card_cmd_done();
        return;
    }
    uint8_t id = b[0], grp = b[1];
    uint16_t seq = rd16(b + 2);
    uint16_t want_seq = (uint16_t)(0x0900 | index);
    if (seq != want_seq)
        violation("command 0x%02x at queue 9 index %d carries sequence "
                  "0x%04x, not 0x%04x (SPEC_T 8.1, 8.3)", id, index, seq,
                  want_seq);
    struct cmd *c = &cq[(cq_head + cq_n) % 16];
    c->id = id;
    c->seq = seq;
    if (grp != 0) {
        violation("command 0x%02x with group %u: every stage-1 command is "
                  "group 0, with the 4-byte header (SPEC_I 2.1, 2.3)", id, grp);
        card_cmd_done();
        crash(0x38);
        return;
    }
    c->len = len - 4;
    memcpy(c->data, b + 4, len - 4);
    cq_n++;
    uint64_t at = (proc_free_at > now_ns ? proc_free_at : now_ns) + 150 * US;
    proc_free_at = at;
    model_at(at - now_ns, fw_process, gen);
}

/* --- after bring-up ------------------------------------------------------------- */

static void flood_one(uint32_t g)
{
    if (g != gen || st != FW_RUNNING)
        return;
    uint8_t s[64];
    memset(s, 0, sizeof(s));
    wr32(s, (uint32_t)fs.flood_sent);
    batch_begin();
    batch_add(0x9D, 0x8000, s, sizeof(s), 0);  /* STATISTICS_NOTIFICATION */
    batch_send(0);
    fs.flood_sent++;
}

void fw_flood(int n)
{
    for (int i = 0; i < n; i++)
        model_at(50 * MS + (uint64_t)i * 10 * MS, flood_one, gen);
}

__attribute__((constructor)) static void fw_init(void)
{
    nvm_build();
}
