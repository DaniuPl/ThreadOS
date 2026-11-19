/* The Intel Wireless 3160 model: the PCIe side of the card
 *
 * Written from the transport specification (SPEC_T, iwlspec_transport.md),
 * not from the driver. What is here:
 *
 *   the CSR registers with their side effects - the NIC_READY handshake
 *   that needs PREPARE at power-on, INIT_DONE bringing the MAC clock up,
 *   MAC_ACCESS_REQ waking a dozing NIC (through GOING_TO_SLEEP, which a
 *   driver checking CLOCK_READY alone mistakes for awake), CSR_RESET
 *   holding and releasing the firmware's CPU, the interrupt causes
 *   cleared by writing ones;
 *   the periphery (PRPH) and device-memory (SRAM) windows, reachable
 *   only with NIC access held;
 *   the FH service channel that DMAs firmware chunks into device memory
 *   and signals each one done;
 *   the receive ring: buffers taken only up to the driver's write
 *   pointer (a multiple of 8), several packets to a buffer, closed_rb_num
 *   in the status block;
 *   the command queue: TFDs fetched when the write pointer moves, only
 *   once the scheduler was set up after ALIVE and only while
 *   MAC_ACCESS_REQ is held (the 3160's apmg_wake_up_wa bug).
 *
 * Where the driver breaks a rule the specification states, the model
 * says so in one "FAIL:" line naming the rule, and then does what the
 * card would: the access goes nowhere, the chunk never lands, the
 * command is never seen.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"

struct card_stats cs;

/* Set by iwl_model.c */
extern int bar_mapped;

/* --- time and events --------------------------------------------------- */

uint64_t now_ns;

struct ev {
    uint64_t at, seq;
    void (*fn)(uint32_t);
    uint32_t arg;
};
static struct ev evq[2048];
static int nev;
static uint64_t evseq;

void model_at(uint64_t delay, void (*fn)(uint32_t), uint32_t arg)
{
    if (nev >= (int)(sizeof(evq) / sizeof(evq[0]))) {
        printf("FAIL: the model ran out of event slots (a bug in the model)\n");
        exit(3);
    }
    evq[nev].at = now_ns + delay;
    evq[nev].seq = evseq++;
    evq[nev].fn = fn;
    evq[nev].arg = arg;
    nev++;
}

void model_advance(uint64_t ns)
{
    uint64_t target = now_ns + ns;
    for (;;) {
        int best = -1;
        for (int i = 0; i < nev; i++) {
            if (evq[i].at > target)
                continue;
            if (best < 0 || evq[i].at < evq[best].at ||
                (evq[i].at == evq[best].at && evq[i].seq < evq[best].seq))
                best = i;
        }
        if (best < 0)
            break;
        struct ev e = evq[best];
        evq[best] = evq[--nev];
        if (e.at > now_ns)
            now_ns = e.at;
        e.fn(e.arg);
        card_tick();
    }
    now_ns = target;
    card_tick();
    machine_after_advance();
}

/* --- reporting --------------------------------------------------------- */

static char seen_msgs[256][240];
static int nseen;
static int nviolations;

static int seen_before(const char *m)
{
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen_msgs[i], m))
            return 1;
    if (nseen < 256)
        snprintf(seen_msgs[nseen++], sizeof(seen_msgs[0]), "%s", m);
    return 0;
}

void violation(const char *fmt, ...)
{
    char m[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    nviolations++;
    if (!seen_before(m))
        printf("FAIL: [t=%llu.%03llu ms] %s\n",
               (unsigned long long)(now_ns / MS),
               (unsigned long long)(now_ns % MS / US), m);
}

void note(const char *fmt, ...)
{
    char m[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if (!seen_before(m))
        printf("note: %s\n", m);
}

int violation_count(void)
{
    return nviolations;
}

/* --- register names ----------------------------------------------------- */

#define CSR_HW_IF_CONFIG   0x000
#define CSR_INT_COALESCING 0x004
#define CSR_INT            0x008
#define CSR_INT_MASK       0x00C
#define CSR_FH_INT_STATUS  0x010
#define CSR_GPIO_IN        0x018
#define CSR_RESET          0x020
#define CSR_GP_CNTRL       0x024
#define CSR_HW_REV         0x028
#define CSR_GIO_REG        0x03C
#define CSR_GP_UCODE       0x048
#define CSR_GP_DRIVER      0x050
#define CSR_GP1            0x054
#define CSR_GP1_SET        0x058
#define CSR_GP1_CLR        0x05C
#define CSR_GP2            0x060
#define CSR_MBOX_SET       0x088
#define CSR_LED            0x094
#define CSR_ICT            0x0A0
#define CSR_SHADOW         0x0A8
#define CSR_GIO_CHICKEN    0x100
#define CSR_HPET           0x240
#define CSR_LINK_PWR       0x250

#define HW_NIC_READY       0x00400000u
#define HW_PREPARE_DONE    0x02000000u
#define HW_PREPARE         0x08000000u

#define GP_CLOCK_READY     0x00000001u
#define GP_INIT_DONE       0x00000004u
#define GP_MAC_REQ         0x00000008u
#define GP_GOING_TO_SLEEP  0x00000010u
#define GP_RFKILL_HW       0x08000000u

#define RST_NMI            0x00000002u
#define RST_SW             0x00000080u
#define RST_MASTER_DIS     0x00000100u
#define RST_STOP_MASTER    0x00000200u

#define HBUS_MEM_RADDR     0x40C
#define HBUS_MEM_WADDR     0x410
#define HBUS_MEM_WDAT      0x418
#define HBUS_MEM_RDAT      0x41C
#define HBUS_MBX_C         0x430
#define HBUS_PRPH_WADDR    0x444
#define HBUS_PRPH_RADDR    0x448
#define HBUS_PRPH_WDAT     0x44C
#define HBUS_PRPH_RDAT     0x450
#define HBUS_WRPTR         0x460

#define FH_STTS_WPTR       0x1BC0
#define FH_RBDCB_BASE      0x1BC4
#define FH_RX_WPTR         0x1BC8
#define FH_RX_RDPTR        0x1BCC
#define FH_RX_CONFIG       0x1C00
#define FH_RX_RBDCB_WPTR   0x1C08
#define FH_RX_FLUSH        0x1C10
#define FH_RX_STATUS       0x1C44
#define FH_KW_ADDR         0x197C
#define FH_TFDIB_CTRL0_9   0x1948
#define FH_TFDIB_CTRL1_9   0x194C
#define FH_SRVC_SRAM_9     0x19C8
#define FH_TX_CONFIG(ch)   (0x1D00 + 0x20 * (ch))
#define FH_TX_BUF_STS(ch)  (0x1D08 + 0x20 * (ch))
#define FH_TX_CHICKEN      0x1E98
#define FH_TSSR_STATUS     0x1EB0

/* PRPH, as the hardware sees them (address bits 19:0) */
#define P_OSC_CLK          0x04068
#define P_APMG_CLK_EN      0x03004
#define P_APMG_CLK_DIS     0x03008
#define P_APMG_PS_CTRL     0x0300C
#define P_APMG_PCIDEV_STT  0x03010
#define P_APMG_RTC_INT     0x0301C
#define P_LMPM_CHICK       0x01FF8
#define P_NMI              0x01C30
#define P_MON_BUFF         0x03C00
#define P_WFMP_MAC0        0x03080
#define P_WFMP_MAC1        0x03084
#define P_SCD_LO           0x02C00
#define P_SCD_HI           0x03000
#define P_SCD_SRAM_BASE    0x02C00
#define P_SCD_DRAM_BASE    0x02C08
#define P_SCD_TXFACT       0x02C10
#define P_SCD_RDPTR(q)     (0x02C68 + 4 * (q))
#define P_SCD_QUEUECHAIN   0x02CE8
#define P_SCD_STATUS(q)    (0x02D0C + 4 * (q))
#define P_SCD_GP_CTRL      0x02DA8
#define P_SCD_CHAINEXT     0x02E44
#define P_SCD_AGGR_SEL     0x02E48
#define P_SCD_EN_CTRL      0x02E54

/* --- state --------------------------------------------------------------- */

static int rfkill_off;              /* the hardware switch is off */

static uint32_t hw_if;              /* HW_IF_CONFIG as stored */
static int nic_ready, prepared, prepare_started;
static uint32_t coalescing, csr_int, int_mask, fh_int;
static uint32_t rst;                /* CSR_RESET as written */
static uint64_t stop_master_at;
static int cpu_held;                /* SW_RESET seen, CPU not released */
static uint32_t gp_ctl;             /* GP_CNTRL: the driver's bits */
static int clk_on;
static uint32_t clk_gen;
static uint32_t gio, gp1, mbox, ict, shadow, chicken, hpet, link_pwr;

/* The NIC dozes when the firmware runs and nothing keeps it awake */
static uint64_t last_busy;
static int waking;
static uint64_t wake_start;
#define WAKE_GTS (20 * US)          /* still going to sleep */
#define WAKE_OFF (60 * US)          /* clock off, then awake */
#define DOZE_AFTER (1 * MS)

/* Windows */
static uint32_t mem_raddr, mem_waddr, prph_raddr, prph_waddr;
static int prph_waddr_set, prph_raddr_set;

/* Periphery */
static uint32_t osc_clk, apmg_clk_en, apmg_ps, apmg_stt, lmpm_chick;
static uint32_t scd[(P_SCD_HI - P_SCD_LO) / 4];
static int dma_clk;
static uint64_t dma_clk_at;
static int nmi_armed;
static uint32_t ps_at_last_write;
static int ps22_set_seen, ps22_cleared_seen;

/* FH */
static uint32_t rx_cfg, rx_base, rx_stts, rx_wptr, rx_rdptr;
static int rx_on;
static uint32_t rx_n = 256, rb_size = 4096;
static uint32_t rx_closed;
static uint32_t kw, cbbc[32], txcfg[10], txbufsts[10], tfdib0, tfdib1;
static uint32_t srvc_sram, tx_chicken;

/* Service channel */
static int chunk_inflight, chunk_unsignalled;
static struct {
    uint64_t phys;
    uint32_t n, dst, no;
    int signal;
} chunk;

/* The command queue */
static uint32_t q9_read, q9_wptr;
static int cmdq_ok;
static int cmds_pending;
static int q15_wptr_set;
static uint32_t dq_read[32];             /* the data queues, as read */

/* Device memory */
static uint8_t sram_inst[SRAM_INST_SIZE];
static uint8_t sram_data[SRAM_DATA_SIZE];

/* Receive batches waiting for the ring */
static struct rxbatch rxq[600];
static int rxq_head, rxq_n;
static uint64_t stall_since;
static int stalling;

/* --- small helpers --------------------------------------------------------- */

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

uint8_t *sram_ptr(uint32_t addr, uint32_t len)
{
    uint64_t end = (uint64_t)addr + len;
    if (end <= SRAM_INST_BASE + SRAM_INST_SIZE)
        return sram_inst + (addr - SRAM_INST_BASE);
    if (addr >= SRAM_DATA_BASE && end <= SRAM_DATA_BASE + SRAM_DATA_SIZE)
        return sram_data + (addr - SRAM_DATA_BASE);
    return NULL;
}

static int fw_on(void)
{
    return fw_running();
}

static int busy(void)
{
    return (gp_ctl & GP_MAC_REQ) || cmds_pending > 0 || rxq_n > 0;
}

/* Asleep: the firmware runs, nobody asked for the NIC for a while */
static int asleep_now(int req)
{
    return fw_on() && !req && !(cmds_pending > 0 || rxq_n > 0) &&
           now_ns - last_busy >= DOZE_AFTER && !waking;
}

static void update_wake(void)
{
    if (waking && now_ns - wake_start >= WAKE_OFF) {
        waking = 0;
        last_busy = now_ns;
    }
}

static int access_granted(void)
{
    update_wake();
    return (gp_ctl & GP_MAC_REQ) && clk_on && !waking;
}

/* NIC access, as every PRPH, SRAM and FH direct access needs it
   (SPEC_T 3.2, 11.9). 0: the access goes nowhere. */
static int need_access(const char *what, uint32_t off)
{
    update_wake();
    if (!(gp_ctl & GP_MAC_REQ)) {
        violation("%s (BAR0+0x%03x) without NIC access: MAC_ACCESS_REQ "
                  "is clear (SPEC_T 3.2, 11.9)", what, off);
        return 0;
    }
    if (!clk_on) {
        violation("%s (BAR0+0x%03x) with the MAC clock off: INIT_DONE not "
                  "set or not ready yet (SPEC_T 4.2 steps 7-8)", what, off);
        return 0;
    }
    if (waking) {
        if (now_ns - wake_start < WAKE_GTS)
            violation("%s (BAR0+0x%03x) while the NIC is still going to "
                      "sleep: CLOCK_READY with GOING_TO_SLEEP set is not "
                      "access (SPEC_T 3.2 step 4, 12.3)", what, off);
        else
            violation("%s (BAR0+0x%03x) before the NIC woke: "
                      "MAC_CLOCK_READY still 0 (SPEC_T 3.2 step 4)",
                      what, off);
        return 0;
    }
    return 1;
}

static int shadow_on(void)
{
    return (shadow & 0x800FFFFFu) == 0x800FFFFFu;
}

int card_rfkill(void) { return rfkill_off; }
uint32_t card_gp1(void) { return gp1; }
uint32_t card_hw_if_config(void) { return hw_if; }
int card_osc_forced(void) { return (osc_clk & 0x8) != 0; }
int card_rx_running(void) { return rx_on; }
int card_q15_wptr_set(void) { return q15_wptr_set; }
int card_cmds_pending(void) { return cmds_pending; }
int card_rx_backlog(void) { return rxq_n; }

void card_raise(uint32_t ci, uint32_t fi)
{
    csr_int |= ci;
    fh_int |= fi;
}

void card_cmd_done(void)
{
    if (cmds_pending > 0)
        cmds_pending--;
}

void card_firmware_stopped(void)
{
    cmds_pending = 0;
}

/* --- power on ----------------------------------------------------------------- */

void card_power_on(int rf_off)
{
    rfkill_off = rf_off;
    /* BOARD_VER and PHY_TYPE zero, so the two specifications' masks for
       nic_config (0xFF0F in SPEC_T, 0xF3CF in SPEC_I) give one result */
    hw_if = 0;
    coalescing = 0x40;
    /* Left set by whoever had the card before: the driver clears them
       before each load (SPEC_T 4.4 steps 7, 10) */
    gp1 = 0x6;
    cpu_held = 0;
    /* Device memory as a powered-up card has it: not zero */
    for (uint32_t i = 0; i < SRAM_INST_SIZE; i++)
        sram_inst[i] = (uint8_t)(0x5A ^ (i * 13));
    for (uint32_t i = 0; i < SRAM_DATA_SIZE; i++)
        sram_data[i] = (uint8_t)(0xA5 ^ (i * 7));
}

/* --- SW_RESET: the CPU held, the periphery and DMA back to reset -------- */

static void sw_reset(void)
{
    cpu_held = 1;
    fw_cpu_reset();
    osc_clk = 0;
    apmg_clk_en = 0;
    apmg_stt = 0;
    dma_clk = 0;
    lmpm_chick = 0;
    memset(scd, 0, sizeof(scd));
    rx_cfg = rx_base = rx_stts = rx_wptr = rx_rdptr = 0;
    rx_on = 0;
    rx_closed = 0;
    kw = 0;
    memset(cbbc, 0, sizeof(cbbc));
    memset(txcfg, 0, sizeof(txcfg));
    memset(txbufsts, 0, sizeof(txbufsts));
    tfdib0 = tfdib1 = srvc_sram = tx_chicken = 0;
    chunk_inflight = 0;
    chunk_unsignalled = 0;
    q9_read = q9_wptr = 0;
    cmdq_ok = 0;
    cmds_pending = 0;
    q15_wptr_set = 0;
    memset(dq_read, 0, sizeof(dq_read));
    rxq_head = rxq_n = 0;
    stalling = 0;
}

/* --- the receive ring ---------------------------------------------------- */

void card_rx_queue(const struct rxbatch *b);
static void rx_try(void);

static void rx_kick(uint32_t unused)
{
    (void)unused;
    rx_try();
}

void card_rx_queue(const struct rxbatch *b)
{
    if (rxq_n >= (int)(sizeof(rxq) / sizeof(rxq[0]))) {
        violation("the firmware has 600 receive buffers' worth of packets "
                  "nobody took: the receive ring is not being served");
        return;
    }
    rxq[(rxq_head + rxq_n) % (int)(sizeof(rxq) / sizeof(rxq[0]))] = *b;
    rxq_n++;
    last_busy = now_ns;
    model_at(b->ready > now_ns ? b->ready - now_ns : 0, rx_kick, 0);
}

static void rx_try(void)
{
    const int qsize = (int)(sizeof(rxq) / sizeof(rxq[0]));
    while (rxq_n > 0) {
        struct rxbatch *b = &rxq[rxq_head];
        if (b->ready > now_ns)
            return;
        const char *why = NULL;
        if (!rx_on)
            why = "receive DMA is not enabled";
        else if (!pci_bus_master())
            why = "bus mastering is off";
        else if (!dma_clk)
            why = "the DMA clock is off";
        else if (((rx_wptr - rx_closed) & (rx_n - 1)) == 0)
            why = "no receive buffer before the write pointer";
        if (why) {
            if (!stalling) {
                stalling = 1;
                stall_since = now_ns;
            }
            if (now_ns - stall_since > 1000 * MS && fw_on()) {
                cs.rx_stalled = 1;
                violation("the firmware's packets have waited 1 s: %s "
                          "(write pointer %u, the card at %u; SPEC_T 6.2-6.5)",
                          why, rx_wptr, rx_closed);
            }
            return;
        }
        uint64_t rbd = (uint64_t)rx_base + 4 * rx_closed;
        if (!dma_ok(rbd, 4)) {
            violation("the receive ring (base 0x%llx) is not in memory the "
                      "driver allocated", (unsigned long long)rx_base);
            rxq_n = 0;
            return;
        }
        uint64_t rb = (uint64_t)rd32(dma_ptr(rbd)) << 8;
        if (!dma_ok(rb, rb_size)) {
            violation("receive descriptor %u holds 0x%llx: not a %u-byte "
                      "buffer the driver allocated (SPEC_T 6.1, 6.5)",
                      rx_closed, (unsigned long long)rb, rb_size);
            if (!stalling) {
                stalling = 1;
                stall_since = now_ns;
            }
            return;
        }
        stalling = 0;
        uint8_t *p = dma_ptr(rb);
        memcpy(p, b->data, b->used);
        if (b->used + 4 <= rb_size)
            wr32(p + b->used, 0x55550000u);     /* FH_RSCSR_FRAME_INVALID */
        rx_closed = (rx_closed + 1) & (rx_n - 1);
        if (dma_ok(rx_stts, 12)) {
            uint8_t *s = dma_ptr(rx_stts);
            wr16(s + 0, (uint16_t)rx_closed);
            wr16(s + 2, (uint16_t)rx_closed);
            wr16(s + 4, (uint16_t)rx_closed);
            wr16(s + 6, (uint16_t)rx_closed);
        } else {
            violation("the receive status block (0x%llx) is not in memory "
                      "the driver allocated (SPEC_T 6.1)",
                      (unsigned long long)rx_stts);
        }
        csr_int |= 0x80000000u;                 /* FH_RX */
        fh_int |= 0x00010000u;                  /* RX channel 0 */
        cs.rx_delivered++;
        cs.pkts_delivered += (uint32_t)b->npkts;
        for (int i = 0; i < b->answers; i++)
            card_cmd_done();
        rxq_head = (rxq_head + 1) % qsize;
        rxq_n--;
        last_busy = now_ns;
    }
}

static void rx_config_write(uint32_t v)
{
    rx_cfg = v;
    if (!(v & 0x80000000u)) {
        rx_on = 0;
        return;
    }
    if ((v >> 30) != 2)
        note("RX config 0x%08x: bits 31:30 are not 10 (normal); the "
             "spec writes 0x80801114 (SPEC_T 6.2)", v);
    uint32_t nlog = (v >> 20) & 0xF;
    uint32_t size = 4096u * (1 + ((v >> 16) & 3));   /* 4K, 8K, 12K, 16K */
    if (v != 0x80801114u)
        note("RX config written as 0x%08x; the spec's value is 0x80801114 "
             "(SPEC_T 6.2 step h)", v);
    if (!(v & 0x1000u))
        note("RX config without IRQ_DEST_INT_HOST (0x1000)");
    if (nlog < 1 || nlog > 12) {
        violation("RX config 0x%08x names 2^%u descriptors", v, nlog);
        return;
    }
    rx_n = 1u << nlog;
    if (rx_n != 256)
        note("a receive ring of %u descriptors; the spec has 256", rx_n);
    rb_size = size;
    if (!rx_base || !dma_ok(rx_base, 4 * rx_n)) {
        violation("receive DMA enabled without a descriptor ring the driver "
                  "allocated (FH_RSCSR_CHNL0_RBDCB_BASE_REG = 0x%x; SPEC_T "
                  "6.2 step f)", rx_base >> 8);
        return;
    }
    if (!rx_stts || !dma_ok(rx_stts, 12)) {
        violation("receive DMA enabled without a status block the driver "
                  "allocated (FH_RSCSR_CHNL0_STTS_WPTR_REG; SPEC_T 6.2 step g)");
        return;
    }
    if (!dma_clk || now_ns < dma_clk_at)
        violation("receive DMA enabled with the DMA clock off or not yet "
                  "settled (APMG_CLK_EN, SPEC_T 4.2 step 10)");
    rx_on = 1;
    rx_closed = rx_rdptr & (rx_n - 1);
    rx_try();
}

static void rx_wptr_write(uint32_t v)
{
    if (v & 7)
        violation("RX write pointer written as %u: the card takes it in "
                  "multiples of 8 (SPEC_T 6.3, 12.10)", v);
    if (v >= rx_n)
        violation("RX write pointer %u past the ring's %u descriptors",
                  v, rx_n);
    rx_wptr = v & (rx_n - 1) & ~7u;
    rx_try();
}

/* --- the firmware's service channel ---------------------------------------- */

static void chunk_done(uint32_t no)
{
    if (!chunk_inflight || chunk.no != no)
        return;                       /* a SW_RESET dropped it */
    uint8_t *dst = sram_ptr(chunk.dst, chunk.n);
    if (dst && dma_ok(chunk.phys, chunk.n))
        memcpy(dst, dma_ptr(chunk.phys), chunk.n);
    chunk_inflight = 0;
    if (chunk.signal) {
        csr_int |= 0x08000000u;       /* FH_TX */
        /* Which TX bit the service channel raises is not in the
           specification (SPEC_T 13 question 3): the model alternates,
           so a driver must take either */
        fh_int |= (no & 1) ? 0x2u : 0x1u;
        cs.chunks_done++;
    } else {
        chunk_unsignalled = 1;
        cs.chunks_unsignalled++;
    }
}

static void srvc_kick(uint32_t v)
{
    uint32_t no = (uint32_t)cs.chunks_started++;
    if (no == 0)
        cs.first_chunk_at = now_ns;
    cs.last_chunk_start = now_ns;
    int bad = 0;
    if (fw_on() || !cpu_held) {
        violation("firmware chunk %u loaded while the CPU is not held in "
                  "reset: no SW_RESET since the last start (SPEC_T 4.3, 4.7)",
                  no);
        bad = 1;
    }
    if (chunk_inflight)
        violation("firmware chunk %u started before chunk %u completed "
                  "(SPEC_T 5.4)", no, chunk.no);
    else if (chunk_unsignalled)
        violation("firmware chunk %u started although the card never "
                  "signalled chunk %u done: the load has failed, the driver "
                  "must give up (SPEC_T 5.4)", no, chunk.no);
    if ((csr_int & 0x08000000u) && (fh_int & 3))
        violation("firmware chunk %u started with the previous completion "
                  "unacknowledged in CSR_INT and CSR_FH_INT_STATUS "
                  "(SPEC_T 5.4, 12.16)", no);
    if (txbufsts[9] != 0x00101003u) {
        violation("firmware chunk %u: FH_TCSR_CHNL_TX_BUF_STS_REG(9) is "
                  "0x%08x, not 0x00101003 (SPEC_T 5.4 step 5)", no,
                  txbufsts[9]);
        bad = 1;
    }
    if (v != 0x80100000u)
        note("service channel enabled with 0x%08x; the spec writes "
             "0x80100000 (SPEC_T 5.4 step 6)", v);
    uint32_t n = tfdib1 & 0x0FFFFFFFu;
    uint64_t phys = tfdib0 | (uint64_t)((tfdib1 >> 28) & 0xF) << 32;
    uint32_t dst = srvc_sram;
    if (n == 0 || n > 0x20000) {
        violation("firmware chunk %u of %u bytes: a chunk is 1 to 0x20000 "
                  "bytes (FH_MEM_TB_MAX_LENGTH, SPEC_T 5.3, 12.15)", no, n);
        bad = 1;
    } else if (n & 3) {
        violation("firmware chunk %u of %u bytes: not whole dwords", no, n);
        bad = 1;
    }
    if (!bad && !dma_ok(phys, n)) {
        violation("firmware chunk %u: the bounce buffer 0x%llx+%u is not "
                  "memory the driver allocated", no,
                  (unsigned long long)phys, n);
        bad = 1;
    }
    if (dst == 0xFFFFCCCCu || dst == 0xAAAABBBBu) {
        violation("firmware chunk %u to 0x%08x: a separator section loaded "
                  "as if it were code (SPEC_T 5.2, 12.14)", no, dst);
        bad = 1;
    } else if (!bad && !sram_ptr(dst, n)) {
        violation("firmware chunk %u to 0x%08x+%u: the card has no memory "
                  "there", no, dst, n);
        bad = 1;
    }
    if (dst >= 0x40000 && dst <= 0x57FFF && !(lmpm_chick & 1)) {
        violation("firmware chunk %u to 0x%x without LMPM_CHICK "
                  "EXTENDED_ADDR_SPACE (SPEC_T 5.3)", no, dst);
        bad = 1;
    }
    if (!pci_bus_master()) {
        violation("firmware chunk %u with PCI bus mastering off: the card "
                  "cannot read host memory", no);
        bad = 1;
    }
    if (!dma_clk || now_ns < dma_clk_at) {
        violation("firmware chunk %u with the DMA clock off or not yet "
                  "settled (APMG_CLK_EN, 20 us; SPEC_T 4.2 step 10)", no);
        bad = 1;
    }
    if (bad)
        return;                        /* the card hangs on it */
    chunk_inflight = 1;
    chunk_unsignalled = 0;
    chunk.phys = phys;
    chunk.n = n;
    chunk.dst = dst;
    chunk.no = no;
    chunk.signal = (v & 0x00100000u) && scenario != SC_NOFW_ACK;
    if (!(v & 0x00100000u))
        note("service channel without CIRQ_HOST_ENDTFD: the card does not "
             "signal the end of the chunk");
    /* The data is read when the transfer runs, not when it is asked
       for: a driver that refills its bounce buffer early loads the
       wrong bytes */
    model_at(10 * US + n, chunk_done, no);
}

/* --- the command queue ------------------------------------------------------ */

static char why_buf[200];

/* What is missing for the card to take a command from queue 9: what
   tx_start and txq_enable set up after ALIVE (SPEC_T 7.3-7.5) */
static const char *cmdq_unready(void)
{
    if (!cbbc[9] || !dma_ok((uint64_t)cbbc[9] << 8, 256 * 128))
        return "queue 9 has no TFD ring the driver allocated "
               "(FH_MEM_CBBC_QUEUE(9))";
    if (!kw || !dma_ok((uint64_t)kw << 4, 4096))
        return "no keep-warm buffer (FH_KW_MEM_ADDR_REG)";
    uint32_t dram = scd[(P_SCD_DRAM_BASE - P_SCD_LO) / 4];
    if (!dram || !dma_ok((uint64_t)dram << 10, 31 * 640))
        return "no byte-count tables (SCD_DRAM_BASE_ADDR)";
    if (!(scd[(P_SCD_TXFACT - P_SCD_LO) / 4] & 0x80))
        return "FIFO 7 not activated (SCD_TXFACT)";
    uint32_t st = scd[(P_SCD_STATUS(9) - P_SCD_LO) / 4];
    if (st != 0x017F001Fu) {
        snprintf(why_buf, sizeof(why_buf), "SCD_QUEUE_STATUS_BITS(9) is "
                 "0x%08x, not 0x017F001F", st);
        return why_buf;
    }
    if (!(scd[(P_SCD_EN_CTRL - P_SCD_LO) / 4] & 0x200))
        return "SCD_EN_CTRL bit 9 not set";
    for (int ch = 0; ch < 8; ch++)
        if (txcfg[ch] != 0x80000008u) {
            snprintf(why_buf, sizeof(why_buf), "FH TX channel %d config is "
                     "0x%08x, not 0x80000008", ch, txcfg[ch]);
            return why_buf;
        }
    uint8_t *m = sram_ptr(SCD_BASE + 0x600, 0x21C);
    for (uint32_t a = 0x600; a < 0x81C; a += 4) {
        uint32_t want = a == 0x600 + 8 * 9 + 4 ? 0x00400040u : 0;
        uint32_t got = rd32(m + (a - 0x600));
        if (got != want) {
            snprintf(why_buf, sizeof(why_buf), "the scheduler's memory at "
                     "scd_base+0x%03x holds 0x%08x, not 0x%08x", a, got, want);
            return why_buf;
        }
    }
    return NULL;
}

static void fetch_cmd(uint32_t idx)
{
    uint64_t t = ((uint64_t)cbbc[9] << 8) + 128 * idx;
    const uint8_t *tfd = dma_ptr(t);
    int ntb = tfd[3] & 0x1F;
    if (tfd[0] || tfd[1] || tfd[2])
        note("TFD reserved bytes 0..2 are not zero");
    if (ntb < 1 || ntb > 20) {
        violation("command queue TFD %u has %d transfer buffers (1..20; "
                  "SPEC_T 7.2)", idx, ntb);
        return;
    }
    static uint8_t buf[20 * 4096];
    uint32_t len = 0;
    for (int k = 0; k < ntb; k++) {
        uint32_t lo = rd32(tfd + 4 + 6 * k);
        uint16_t hn = rd16(tfd + 8 + 6 * k);
        uint64_t a = lo | (uint64_t)(hn & 0xF) << 32;
        uint32_t l = hn >> 4;
        if (!l) {
            violation("command queue TFD %u: transfer buffer %d has length 0",
                      idx, k);
            return;
        }
        if (!dma_ok(a, l)) {
            violation("command queue TFD %u: transfer buffer %d (0x%llx+%u) "
                      "is not memory the driver allocated (SPEC_T 7.2, 11.3)",
                      idx, k, (unsigned long long)a, l);
            return;
        }
        memcpy(buf + len, dma_ptr(a), l);
        len += l;
    }
    cmds_pending++;
    last_busy = now_ns;
    fw_command(buf, len, (int)idx);
}

static void cmdq_wptr(uint32_t idx)
{
    if (idx == q9_read) {
        q9_wptr = idx;
        return;
    }
    uint32_t n = (idx - q9_read) & 0xFF;
    if (!fw_on()) {
        violation("a host command queued (write pointer %u) with no "
                  "firmware running to take it", idx);
        q9_read = q9_wptr = idx;
        return;
    }
    if (!access_granted()) {
        violation("a host command queued without MAC_ACCESS_REQ held and "
                  "granted: on the 3160 the firmware misses it "
                  "(apmg_wake_up_wa, SPEC_T 8.6, 11.10)");
        q9_read = q9_wptr = idx;
        return;
    }
    if (!cmdq_ok) {
        const char *why = cmdq_unready();
        if (why) {
            violation("a host command queued before the command queue was "
                      "set up after ALIVE: %s (SPEC_T 7.4, 7.5; SPEC_I 3.4)",
                      why);
            q9_read = q9_wptr = idx;
            return;
        }
        if (scd[(P_SCD_AGGR_SEL - P_SCD_LO) / 4] & 0x200)
            note("the command queue is marked aggregating (SCD_AGGR_SEL)");
        if (scd[(P_SCD_QUEUECHAIN - P_SCD_LO) / 4] & 0x200)
            note("the command queue is chained (SCD_QUEUECHAIN_SEL); the "
                 "spec leaves it out of the chain");
        cmdq_ok = 1;
    }
    for (uint32_t i = 0; i < n; i++)
        fetch_cmd((q9_read + i) & 0xFF);
    q9_read = q9_wptr = idx;
}

/* --- the data queues (stage 3) ---------------------------------------------
 *
 * A frame to send: the TFD's buffers in order - the TX command's first
 * bytes (TB0, the scratch the card writes back into), the rest of it
 * with the 802.11 header, then the frame's body - handed to the
 * firmware as one, which sends it and says so (REPLY_TX). Only a queue
 * the firmware was told about (SCD_QUEUE_CFG) is read. */
static uint32_t dq_read[32];

static void fetch_frame(int q, uint32_t idx)
{
    uint64_t t = ((uint64_t)cbbc[q] << 8) + 128 * idx;
    const uint8_t *tfd = dma_ptr(t);
    int ntb = tfd[3] & 0x1F;
    if (ntb < 2 || ntb > 20) {
        violation("queue %d TFD %u has %d transfer buffers: a frame needs "
                  "the TX command and its header at least (SPEC_T 7.2)",
                  q, idx, ntb);
        return;
    }
    static uint8_t buf[20 * 4096];
    uint32_t len = 0;
    for (int k = 0; k < ntb; k++) {
        uint32_t lo = rd32(tfd + 4 + 6 * k);
        uint16_t hn = rd16(tfd + 8 + 6 * k);
        uint64_t a = lo | (uint64_t)(hn & 0xF) << 32;
        uint32_t l = hn >> 4;
        if (!l || !dma_ok(a, l)) {
            violation("queue %d TFD %u: transfer buffer %d (0x%llx+%u) is "
                      "empty or not memory the driver allocated", q, idx, k,
                      (unsigned long long)a, l);
            return;
        }
        memcpy(buf + len, dma_ptr(a), l);
        len += l;
    }
    last_busy = now_ns;
    fw_tx(q, idx, buf, len);
}

static void dataq_wptr(int q, uint32_t idx)
{
    if (!fw_on() || !fw_queue_enabled(q)) {
        violation("HBUS_TARG_WRPTR: queue %d index %u, a queue the firmware "
                  "was not given (SCD_QUEUE_CFG)", q, idx);
        dq_read[q] = idx;
        return;
    }
    if (!cbbc[q] || !dma_ok((uint64_t)cbbc[q] << 8, 256 * 128)) {
        violation("queue %d has no TFD ring (FH_MEM_CBBC_QUEUE)", q);
        dq_read[q] = idx;
        return;
    }
    uint32_t n = (idx - dq_read[q]) & 0xFF;
    for (uint32_t i = 0; i < n; i++)
        fetch_frame(q, (dq_read[q] + i) & 0xFF);
    dq_read[q] = idx;
}

void card_queue_reset(int q, uint32_t ssn)
{
    if (q >= 0 && q < 32)
        dq_read[q] = ssn & 0xFF;
}

static void wrptr_write(uint32_t v)
{
    uint32_t q = (v >> 8) & 0x1F, idx = v & 0xFF;
    if (v >> 13)
        violation("HBUS_TARG_WRPTR written with 0x%08x: bits from 13 up "
                  "are not part of it (SPEC_T 3.1)", v);
    update_wake();
    if (!shadow_on() && (asleep_now(gp_ctl & GP_MAC_REQ) || waking)) {
        violation("HBUS_TARG_WRPTR written while the NIC sleeps and shadow "
                  "registers are off: the write is lost (SPEC_T 11.1)");
        return;
    }
    if (q == 9)
        cmdq_wptr(idx);
    else if (q == 15 && idx == 0 && !fw_queue_enabled(15))
        q15_wptr_set = 1;
    else if (!fw_queue_enabled((int)q) && idx == dq_read[q])
        dq_read[q] = idx;               /* the SSN, before SCD_QUEUE_CFG */
    else
        dataq_wptr((int)q, idx);
}

/* --- PRPH ------------------------------------------------------------------- */

uint32_t prph_value(uint32_t a)
{
    a &= 0xFFFFF;
    switch (a) {
    case P_OSC_CLK: return osc_clk;
    case P_APMG_CLK_EN: return apmg_clk_en;
    case P_APMG_PS_CTRL: return apmg_ps;
    case P_APMG_PCIDEV_STT: return apmg_stt;
    case P_LMPM_CHICK: return lmpm_chick;
    }
    if (a == P_SCD_SRAM_BASE)
        return fw_scd_base();
    if (a >= P_SCD_LO && a < P_SCD_HI)
        return scd[(a - P_SCD_LO) / 4];
    return 0;
}

static uint32_t prph_read(uint32_t a)
{
    switch (a) {
    case P_OSC_CLK: case P_APMG_CLK_EN: case P_APMG_PS_CTRL:
    case P_APMG_PCIDEV_STT: case P_LMPM_CHICK:
        return prph_value(a);
    case P_APMG_CLK_DIS: case P_APMG_RTC_INT: case P_NMI: case P_MON_BUFF:
        return 0;
    case P_WFMP_MAC0: case P_WFMP_MAC1:
        note("PRPH WFMP_MAC_ADDR read: only the 8000 family keeps the "
             "address there (SPEC_I 5.3)");
        return 0;
    }
    if (a >= P_SCD_LO && a < P_SCD_HI)
        return prph_value(a);
    note("PRPH read of 0x%05x, which the specifications do not name", a);
    return 0;
}

static void prph_write(uint32_t a, uint32_t v)
{
    switch (a) {
    case P_OSC_CLK:
        osc_clk = v;
        return;
    case P_APMG_CLK_EN:
        apmg_clk_en |= v;
        if ((v & 0x200) && !dma_clk) {
            dma_clk = 1;
            dma_clk_at = now_ns + 20 * US;
        }
        return;
    case P_APMG_CLK_DIS:
        apmg_clk_en &= ~v;
        if (v & 0x200)
            dma_clk = 0;
        return;
    case P_APMG_PS_CTRL:
        if ((v & 0x400000) && !(ps_at_last_write & 0x400000))
            ps22_set_seen = 1;
        if (!(v & 0x400000) && (ps_at_last_write & 0x400000))
            ps22_cleared_seen = 1;
        ps_at_last_write = v;
        apmg_ps = v;
        if (v & 0x03000000)
            violation("APMG_PS_CTRL power source bits 25:24 = %u: set_pwr "
                      "selects VMAIN (0) (SPEC_T 4.4 step 6b)",
                      (v >> 24) & 3);
        return;
    case P_APMG_PCIDEV_STT:
        apmg_stt = v;
        return;
    case P_APMG_RTC_INT:
        return;
    case P_LMPM_CHICK:
        lmpm_chick = v;
        return;
    case P_NMI:
        if (v == 0x80)
            nmi_armed = 1;
        else if (v == 0x01 && nmi_armed) {
            nmi_armed = 0;
            fw_nmi();
        }
        return;
    case P_MON_BUFF:
        note("MON_BUFF_SAMPLE_CTL written (optional, SPEC_I 7.2)");
        return;
    }
    if (a == P_SCD_SRAM_BASE) {
        violation("SCD_SRAM_BASE_ADDR written: it is the card's, read-only");
        return;
    }
    if (a >= P_SCD_LO && a < P_SCD_HI) {
        if (a == P_SCD_CHAINEXT)
            note("SCD_CHAINEXT_EN written; scd_chain_ext_wa is false for "
                 "the 3160 (SPEC_T 7.4 step 5)");
        scd[(a - P_SCD_LO) / 4] = v;
        if (a == P_SCD_STATUS(9) && (v & 0x8))
            q9_read = scd[(P_SCD_RDPTR(9) - P_SCD_LO) / 4] & 0xFF;
        return;
    }
    violation("PRPH write of 0x%08x to 0x%05x, which no specification "
              "writes", v, a);
}

static int prph_addr_ok(uint32_t v, const char *which)
{
    if ((v & ~0x000FFFFFu) != 0x03000000u) {
        violation("%s written with 0x%08x: a PRPH address is (addr & "
                  "0xFFFFF) | 0x03000000 (SPEC_T 3.3, 11.8)", which, v);
        return 0;
    }
    return 1;
}

/* --- device memory through HBUS --------------------------------------------- */

static uint32_t sram_host_read(uint32_t a)
{
    uint8_t *p = sram_ptr(a, 4);
    if (!p || (a & 3)) {
        violation("device memory read at 0x%08x, where the card has none",
                  a);
        return 0xA5A5A5A5u;
    }
    if (fs.crashed && a >= ERRLOG_PTR && a < ERRLOG_PTR + 152)
        cs.errlog_read++;
    return rd32(p);
}

static void sram_host_write(uint32_t a, uint32_t v)
{
    if (a < SCD_BASE + 0x600 || a >= SCD_BASE + 0x820 || (a & 3)) {
        violation("device memory written at 0x%08x through HBUS_TARG_MEM: "
                  "the driver writes only the scheduler's area, scd_base + "
                  "0x600..0x81F (SPEC_T 7.4, 7.5)", a);
        return;
    }
    wr32(sram_ptr(a, 4), v);
}

/* --- CSR --------------------------------------------------------------------- */

static void prepared_now(uint32_t unused)
{
    (void)unused;
    prepared = 1;
}

static void clock_up(uint32_t gen)
{
    if (gen == clk_gen && (gp_ctl & GP_INIT_DONE))
        clk_on = 1;
}

static uint32_t gp_read(void)
{
    update_wake();
    uint32_t v = gp_ctl;
    int req = (gp_ctl & GP_MAC_REQ) != 0;
    int off_phase = waking && now_ns - wake_start >= WAKE_GTS;
    if (clk_on && !asleep_now(req) && !off_phase)
        v |= GP_CLOCK_READY;
    if (waking && now_ns - wake_start < WAKE_GTS)
        v |= GP_GOING_TO_SLEEP;
    if (!rfkill_off)
        v |= GP_RFKILL_HW;
    return v;
}

static void gp_write(uint32_t v)
{
    uint32_t old = gp_ctl;
    int was_asleep = asleep_now((old & GP_MAC_REQ) != 0);
    update_wake();
    gp_ctl = v & ~(GP_CLOCK_READY | GP_GOING_TO_SLEEP | GP_RFKILL_HW);
    if ((v & GP_INIT_DONE) && !(old & GP_INIT_DONE)) {
        if (!nic_ready)
            violation("APM init (INIT_DONE) before the card was prepared: "
                      "NIC_READY not granted (SPEC_T 4.1, 4.3, 12.2)");
        clk_gen++;
        model_at(30 * US, clock_up, clk_gen);
    }
    if (!(v & GP_INIT_DONE) && (old & GP_INIT_DONE)) {
        clk_on = 0;
        clk_gen++;
    }
    if ((v & GP_MAC_REQ) && !(old & GP_MAC_REQ)) {
        if (was_asleep && clk_on) {
            waking = 1;
            wake_start = now_ns;
        }
        last_busy = now_ns;
    }
    if (!(v & GP_MAC_REQ) && (old & GP_MAC_REQ)) {
        if (cmds_pending > 0 && fw_on())
            violation("MAC_ACCESS_REQ released while a host command is in "
                      "flight: on the 3160 it stays set until the command "
                      "queue drains (apmg_wake_up_wa, SPEC_T 8.6, 12.20)");
        last_busy = now_ns;
    }
}

static void reset_write(uint32_t v)
{
    uint32_t old = rst;
    rst = v & (RST_SW | RST_STOP_MASTER | 0x80000000u | 0x1u);
    if ((v & RST_STOP_MASTER) && !(old & RST_STOP_MASTER))
        stop_master_at = now_ns + 5 * US;
    if (v & RST_NMI)
        fw_nmi();
    if (v & 0x1)
        note("CSR_RESET NEVO_RESET written; the 3160 path never does");
    if (v & RST_SW) {
        sw_reset();
        return;
    }
    if (cpu_held) {
        /* The release: the firmware's CPU runs from what device memory
           holds (SPEC_T 5.2 step 5, 11.14) */
        cpu_held = 0;
        cs.cpu_starts++;
        cs.apm_ok_at_start =
            (chicken & 0x20800000u) == 0x20800000u &&
            (hpet & 0xFFFF0000u) == 0xFFFF0000u &&
            (hw_if & 0x00080000u) && (osc_clk & 8) && dma_clk &&
            (apmg_stt & 0x800);
        cs.l1_disabled_at_start = (apmg_stt & 0x800) != 0;
        cs.shadow_at_start = shadow_on();
        cs.coalescing_at_start = coalescing;
        cs.gio_at_start = gio;
        int n = 0;
        for (int q = 0; q < 31; q++)
            if (cbbc[q] && dma_ok((uint64_t)cbbc[q] << 8, 256 * 128))
                n++;
        cs.cbbc_programmed = n;
        if (chunk_inflight)
            violation("the CPU released while firmware chunk %u is still "
                      "being transferred", chunk.no);
        fw_cpu_start();
    } else if (!(old & RST_SW) && !fw_on()) {
        note("CSR_RESET = 0x%x with no SW_RESET before it: there is no "
             "held CPU to release", v);
    }
}

static uint32_t csr_read(uint32_t off)
{
    switch (off) {
    case CSR_HW_IF_CONFIG:
        return (hw_if & ~(HW_NIC_READY | HW_PREPARE_DONE)) |
               (nic_ready ? HW_NIC_READY : 0) |
               (prepared ? HW_PREPARE_DONE : 0);
    case CSR_INT_COALESCING: return coalescing;
    case CSR_INT: return csr_int;
    case CSR_INT_MASK: return int_mask;
    case CSR_FH_INT_STATUS: return fh_int;
    case CSR_GPIO_IN: return 0x200;
    case CSR_RESET:
        return rst | ((rst & RST_STOP_MASTER) && now_ns >= stop_master_at
                      ? RST_MASTER_DIS : 0);
    case CSR_GP_CNTRL: return gp_read();
    case CSR_HW_REV: return CARD_HW_REV;
    case CSR_GIO_REG: return gio;
    case CSR_GP_UCODE: case CSR_GP_DRIVER: case CSR_GP2: return 0;
    case CSR_GP1: return gp1;
    case CSR_MBOX_SET: return mbox;
    case CSR_ICT: return ict;
    case CSR_SHADOW: return shadow;
    case CSR_GIO_CHICKEN: return chicken;
    case CSR_HPET: return hpet;
    case CSR_LINK_PWR: return link_pwr;
    }
    note("read of CSR 0x%03x, which the specifications do not name", off);
    return 0;
}

static void csr_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case CSR_HW_IF_CONFIG:
        if ((v & HW_PREPARE) && !prepare_started) {
            prepare_started = 1;
            cs.prepare_seen = 1;
            /* The card comes out of its low-power state some time after
               PREPARE (SPEC_T 4.1: up to ~150 ms) */
            model_at(3 * MS, prepared_now, 0);
        }
        if (v & HW_NIC_READY) {
            if (prepared && !nic_ready) {
                nic_ready = 1;
                cs.ready_granted_after_prepare = 1;
            }
        } else {
            nic_ready = 0;
        }
        hw_if = v & ~HW_PREPARE_DONE;
        return;
    case CSR_INT_COALESCING: coalescing = v; return;
    case CSR_INT: csr_int &= ~v; return;
    case CSR_INT_MASK: int_mask = v; return;
    case CSR_FH_INT_STATUS: fh_int &= ~v; return;
    case CSR_RESET: reset_write(v); return;
    case CSR_GP_CNTRL: gp_write(v); return;
    case CSR_GIO_REG: gio = v; return;
    case CSR_GP1_SET: gp1 |= v; return;
    case CSR_GP1_CLR: gp1 &= ~v; return;
    case CSR_MBOX_SET: mbox |= v; return;
    case CSR_ICT:
        ict = v;
        if (v & 0x80000000u) {
            cs.ict_enabled = 1;
            violation("ICT enabled (CSR_DRAM_INT_TBL_REG bit 31): the "
                      "polling design reads CSR_INT and leaves ICT off "
                      "(SPEC_T 1.3)");
        }
        return;
    case CSR_SHADOW: shadow = v; return;
    case CSR_GIO_CHICKEN: chicken = v; return;
    case CSR_HPET: hpet = v; return;
    case CSR_LINK_PWR: link_pwr = v; return;
    case CSR_GP_DRIVER: case 0x02C: case 0x030: case 0x034: case 0x20C:
    case 0x214: case 0x0EC: case 0x0F4: case CSR_GP1: case CSR_GP2:
    case CSR_GP_UCODE: case CSR_LED:
        note("CSR 0x%03x written; the 3160 path never writes it "
             "(SPEC_T 3.1, 11.17)", off);
        return;
    case CSR_HW_REV: case CSR_GPIO_IN:
        violation("CSR 0x%03x written: it is read-only", off);
        return;
    }
    violation("write of 0x%08x to CSR 0x%03x, which no specification names",
              v, off);
}

/* --- HBUS and FH -------------------------------------------------------------- */

static uint32_t hbus_read(uint32_t off)
{
    switch (off) {
    case HBUS_MEM_RDAT:
        if (!need_access("HBUS_TARG_MEM_RDAT", off))
            return 0xA5A5A5A5u;
        {
            uint32_t v = sram_host_read(mem_raddr);
            mem_raddr += 4;
            return v;
        }
    case HBUS_PRPH_RDAT:
        if (!need_access("HBUS_TARG_PRPH_RDAT", off))
            return 0xA5A5A5A5u;
        if (!prph_raddr_set)
            violation("HBUS_TARG_PRPH_RDAT read with no PRPH address set");
        return prph_read(prph_raddr & 0xFFFFF);
    case HBUS_MEM_RADDR: return mem_raddr;
    case HBUS_MEM_WADDR: return mem_waddr;
    case HBUS_PRPH_RADDR: return prph_raddr;
    case HBUS_PRPH_WADDR: return prph_waddr;
    }
    note("read of HBUS 0x%03x, which the specifications do not name", off);
    return 0;
}

static void hbus_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case HBUS_MEM_RADDR:
        if (need_access("HBUS_TARG_MEM_RADDR", off))
            mem_raddr = v;
        return;
    case HBUS_MEM_WADDR:
        if (need_access("HBUS_TARG_MEM_WADDR", off))
            mem_waddr = v;
        return;
    case HBUS_MEM_WDAT:
        if (need_access("HBUS_TARG_MEM_WDAT", off)) {
            sram_host_write(mem_waddr, v);
            mem_waddr += 4;
        }
        return;
    case HBUS_PRPH_WADDR:
        if (need_access("HBUS_TARG_PRPH_WADDR", off) &&
            prph_addr_ok(v, "HBUS_TARG_PRPH_WADDR")) {
            prph_waddr = v;
            prph_waddr_set = 1;
        }
        return;
    case HBUS_PRPH_RADDR:
        if (need_access("HBUS_TARG_PRPH_RADDR", off) &&
            prph_addr_ok(v, "HBUS_TARG_PRPH_RADDR")) {
            prph_raddr = v;
            prph_raddr_set = 1;
        }
        return;
    case HBUS_PRPH_WDAT:
        if (!need_access("HBUS_TARG_PRPH_WDAT", off))
            return;
        if (!prph_waddr_set) {
            violation("HBUS_TARG_PRPH_WDAT written with no PRPH address set");
            return;
        }
        prph_write(prph_waddr & 0xFFFFF, v);
        return;
    case HBUS_WRPTR:
        wrptr_write(v);
        return;
    case HBUS_MBX_C:
        note("HBUS_TARG_MBX_C written; the 3160 path never does");
        return;
    }
    violation("write of 0x%08x to HBUS 0x%03x, which no specification names",
              v, off);
}

static int cbbc_queue(uint32_t off)
{
    if (off >= 0x19D0 && off < 0x19D0 + 4 * 16)
        return (int)(off - 0x19D0) / 4;
    if (off >= 0x1BF0 && off < 0x1BF0 + 4 * 4)
        return 16 + (int)(off - 0x1BF0) / 4;
    if (off >= 0x1B20 && off < 0x1B20 + 4 * 12)
        return 20 + (int)(off - 0x1B20) / 4;
    return -1;
}

static uint32_t fh_read(uint32_t off)
{
    if (!need_access("FH register read", off))
        return 0xA5A5A5A5u;
    if (off == FH_RX_STATUS)
        return rx_on ? 0 : 0x01000000u;      /* channel idle */
    if (off == FH_TSSR_STATUS)
        return 0x00FF0000u;                  /* no pending requests */
    if (off == FH_RX_CONFIG) return rx_cfg;
    if (off == FH_RBDCB_BASE) return rx_base >> 8;
    if (off == FH_STTS_WPTR) return rx_stts >> 4;
    if (off == FH_RX_WPTR) return rx_wptr;
    if (off == FH_RX_RDPTR) return rx_rdptr;
    if (off == FH_KW_ADDR) return kw;
    if (off == FH_TX_CHICKEN) return tx_chicken;
    if (off == FH_SRVC_SRAM_9) return srvc_sram;
    if (off == FH_TFDIB_CTRL0_9) return tfdib0;
    if (off == FH_TFDIB_CTRL1_9) return tfdib1;
    int q = cbbc_queue(off);
    if (q >= 0) return cbbc[q];
    for (int ch = 0; ch < 10; ch++) {
        if (off == (uint32_t)FH_TX_CONFIG(ch)) return txcfg[ch];
        if (off == (uint32_t)FH_TX_BUF_STS(ch)) return txbufsts[ch];
    }
    note("read of FH 0x%04x, which the specifications do not name", off);
    return 0;
}

static void fh_write(uint32_t off, uint32_t v)
{
    if (off == FH_RX_WPTR) {
        /* With shadow registers the write pointer needs no NIC access;
           without them it is lost while the NIC sleeps (SPEC_T 6.3, 11.1) */
        update_wake();
        if (!shadow_on() && (asleep_now(gp_ctl & GP_MAC_REQ) || waking) &&
            !access_granted()) {
            violation("FH_RSCSR_CHNL0_WPTR written while the NIC sleeps and "
                      "shadow registers are off: the write is lost "
                      "(SPEC_T 11.1)");
            return;
        }
        rx_wptr_write(v);
        return;
    }
    if (!need_access("FH register write", off))
        return;
    switch (off) {
    case FH_RX_CONFIG: rx_config_write(v); return;
    case FH_RX_RBDCB_WPTR: case FH_RX_FLUSH:
        if (v)
            note("FH 0x%04x written with 0x%x; RX init writes 0", off, v);
        return;
    case FH_RX_RDPTR:
        rx_rdptr = v;
        if (!rx_on)
            rx_closed = v & 0xFF;
        return;
    case FH_RBDCB_BASE: rx_base = v << 8; return;
    case FH_STTS_WPTR: rx_stts = v << 4; return;
    case FH_KW_ADDR: kw = v; return;
    case FH_TX_CHICKEN: tx_chicken = v; return;
    case FH_SRVC_SRAM_9: srvc_sram = v; return;
    case FH_TFDIB_CTRL0_9: tfdib0 = v; return;
    case FH_TFDIB_CTRL1_9: tfdib1 = v; return;
    case FH_TSSR_STATUS: case FH_RX_STATUS:
        violation("FH 0x%04x written: it is a status register", off);
        return;
    }
    int q = cbbc_queue(off);
    if (q >= 0) {
        if (q >= 31)
            violation("FH_MEM_CBBC_QUEUE(%d) written: the 3160 has 31 "
                      "queues", q);
        cbbc[q] = v;
        return;
    }
    for (int ch = 0; ch < 10; ch++) {
        if (off == (uint32_t)FH_TX_CONFIG(ch)) {
            if (ch == 9) {
                if (v & 0x80000000u) {
                    txcfg[9] = v;
                    srvc_kick(v);
                } else {
                    txcfg[9] = v;
                }
                return;
            }
            if (ch == 8)
                note("FH TX channel 8 configured; the spec configures 0..7 "
                     "and the service channel 9");
            txcfg[ch] = v;
            return;
        }
        if (off == (uint32_t)FH_TX_BUF_STS(ch)) {
            if (ch != 9)
                note("FH_TCSR_CHNL_TX_BUF_STS_REG(%d) written", ch);
            txbufsts[ch] = v;
            return;
        }
    }
    violation("write of 0x%08x to FH 0x%04x, which no specification names",
              v, off);
}

/* --- MMIO entry points ----------------------------------------------------- */

static int window_ok(const volatile void *base, uint32_t off)
{
    if ((uintptr_t)base != (uintptr_t)BAR0_PHYS) {
        violation("MMIO through base %p: BAR0 is at 0x%llx", (void *)base,
                  (unsigned long long)BAR0_PHYS);
        return 0;
    }
    if (!bar_mapped)
        violation("BAR0 used before paging64_map_phys mapped it");
    if (off >= BAR0_SIZE || (off & 3)) {
        violation("MMIO at BAR0+0x%x: outside the registers or not 32-bit "
                  "aligned", off);
        return 0;
    }
    if (!pci_mem_enabled()) {
        violation("MMIO with PCI memory decoding off (COMMAND bit 1)");
        return 0;
    }
    return 1;
}

uint32_t model_mmio_read(const volatile void *base, uint32_t off)
{
    /* A read crosses PCIe and back: about a microsecond */
    model_advance(1 * US);
    if (!window_ok(base, off))
        return 0xFFFFFFFFu;
    uint32_t v;
    if (off < 0x400)
        v = csr_read(off);
    else if (off < 0x1000)
        v = hbus_read(off);
    else
        v = fh_read(off);
    return v;
}

void model_mmio_write(const volatile void *base, uint32_t off, uint32_t v)
{
    model_advance(200);
    if (!window_ok(base, off))
        return;
    if (off < 0x400)
        csr_write(off, v);
    else if (off < 0x1000)
        hbus_write(off, v);
    else
        fh_write(off, v);
    card_tick();
}

void card_tick(void)
{
    update_wake();
    if (busy())
        last_busy = now_ns;
    if (rxq_n)
        rx_try();
    cs.mac_access_held = (gp_ctl & GP_MAC_REQ) != 0;
}

/* What the checks look at besides prph_value */
int card_ps22(int *set, int *cleared)
{
    *set = ps22_set_seen;
    *cleared = ps22_cleared_seen;
    return (apmg_ps & 0x400000) != 0;
}

int card_nic_ready(void)
{
    return nic_ready;
}
