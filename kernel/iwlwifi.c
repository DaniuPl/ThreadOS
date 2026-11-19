/* Intel Wireless 3160 (802.11ac, one antenna, PCIe)
 *
 * This card is not a radio the driver drives register by register. It
 * is a small computer of its own - a CPU with its own SRAM - and it
 * does nothing until the driver has copied Intel's firmware into it.
 * From then on the driver talks to the firmware in messages: host
 * commands going in, responses and notifications coming back.
 *
 * The firmware file (third_party/iwlwifi) holds two images that matter
 * here. INIT runs first: it reads the card's NVM - the MAC address and
 * the channels this card may use where it was sold - and calibrates
 * the radio. Its calibration results (the "PHY DB") come back to the
 * driver, which then stops the card, loads the RUNTIME image and hands
 * the results to it, because the runtime image does not calibrate the
 * radio from scratch. A runtime image without them would run, but on
 * a radio nobody measured.
 *
 * Messages travel over DMA rings, as on a network card. Each transmit
 * queue is a ring of 256 descriptors (TFDs) of 128 bytes, each pointing
 * at up to 20 buffers; queue 9 carries host commands, the data queues
 * are set up by the firmware when asked. Everything the card sends
 * arrives through one receive ring of 4 KiB buffers, several packets
 * per buffer. The firmware itself is copied in through a separate DMA
 * "service channel", a chunk at a time.
 *
 * The registers lie in BAR0. A few (the CSR block) are plain; the rest
 * of the chip - the "periphery" (PRPH) and the card's SRAM - is reached
 * through address/data register pairs, and only while the card is held
 * awake (MAC_ACCESS_REQ in GP_CNTRL, then wait for the clock). The
 * 3160 has a hardware bug there: while a host command is in flight the
 * card must be held awake the whole time, not only around each access,
 * or the firmware misses the command.
 *
 * The driver polls; it asks for no interrupt. The interrupt cause
 * register (CSR_INT) latches whatever the mask says, so a thread that
 * reads it sees what a handler would, and INTx is turned off at the
 * PCI level so that nothing reaches the interrupt controller even
 * through the causes the card raises when masked. Received packets are
 * found by comparing the card's "closed" index in the RX status block
 * with our own read index, which needs no interrupt at all.
 *
 * Every wait has a timeout and every failure names the step it stopped
 * at, so one boot on a laptop says how far the card got. A firmware
 * crash is reported with the error id from the firmware's error log.
 *
 * Layout: the transport first (register access, power-up, firmware
 * load, the RX ring, the TX queues, host commands), then the firmware
 * side (the INIT run, the PHY DB, the NVM, the runtime bring-up).
 */

#include <stdint.h>
#include <stdarg.h>
#include "include/iwlwifi.h"
#include "include/wifi_info.h"
#include "include/mmio.h"
#include "include/pci.h"
#include "include/devices.h"
#include "include/pmm.h"
#include "include/initrd.h"
#include "include/printk.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/string.h"
#include "include/io.h"
#include "include/wlan.h"
#include "include/wpa.h"
#include "include/net.h"
#include "include/dhcp.h"
#include "include/random.h"
#include "include/spinlock.h"

/* The registers are in memory and have to be mapped; the window's
   address comes from the BAR. */
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))

#define INTEL_VENDOR     0x8086
#define FW_FILE          "iwl3160.ucode"
/* The registers used reach up to 0x1EB8 */
#define BAR_SIZE         0x2000

/* --- CSR registers (BAR0) ------------------------------------------- */

#define CSR_HW_IF_CONFIG_REG   0x000
#define CSR_INT_COALESCING     0x004
#define CSR_INT                0x008   /* cause, write 1 to clear */
#define CSR_INT_MASK           0x00C
#define CSR_FH_INT_STATUS      0x010   /* DMA cause, write 1 to clear */
#define CSR_RESET              0x020
#define CSR_GP_CNTRL           0x024
#define CSR_HW_REV             0x028
#define CSR_GIO_REG            0x03C
#define CSR_UCODE_DRV_GP1_CLR  0x05C
#define CSR_MBOX_SET_REG       0x088
#define CSR_MAC_SHADOW_REG_CTRL 0x0A8
#define CSR_GIO_CHICKEN_BITS   0x100
#define CSR_DBG_HPET_MEM_REG   0x240
#define CSR_DBG_LINK_PWR_MGMT_REG 0x250

#define HW_IF_HAP_WAKE_L1A     0x00080000
#define HW_IF_NIC_READY        0x00400000
#define HW_IF_PREPARE          0x08000000
/* What nic_config writes: MAC dash/step from HW_REV, the PHY type, dash
   and step from the firmware's PHY_SKU, RADIO_SI on, MAC_SI off */
#define HW_IF_NIC_CONFIG_MASK  0x0000FF0F
#define HW_IF_RADIO_SI         0x00000200

#define INT_FH_RX              0x80000000
#define INT_HW_ERR             0x20000000
#define INT_RX_PERIODIC        0x10000000
#define INT_FH_TX              0x08000000
#define INT_SW_ERR             0x02000000
#define INT_RF_KILL            0x00000080
#define INT_CT_KILL            0x00000040
#define INT_SW_RX              0x00000008
#define FH_INT_RX_MASK         0x40030000
#define FH_INT_TX_MASK         0x00000003

#define RESET_FORCE_NMI        0x00000002
#define RESET_SW_RESET         0x00000080
#define RESET_MASTER_DISABLED  0x00000100
#define RESET_STOP_MASTER      0x00000200

#define GP_MAC_CLOCK_READY     0x00000001
#define GP_INIT_DONE           0x00000004
#define GP_MAC_ACCESS_REQ      0x00000008
#define GP_GOING_TO_SLEEP      0x00000010
#define GP_HW_RF_KILL_SW       0x08000000   /* set: the radio may run */

#define GIO_L0S_ENABLED        0x00000002
#define GIO_CHICKEN_L1A_NO_L0S_RX     0x00800000
#define GIO_CHICKEN_DIS_L0S_EXIT_TIMER 0x20000000
#define GP1_SW_BIT_RFKILL      0x00000002
#define GP1_CMD_BLOCKED        0x00000004
#define MBOX_OS_ALIVE          0x00000020
#define HOST_INT_TIMEOUT_DEF   0x40          /* x 32 us */
#define HOST_INT_OPER_MODE     0x80000000    /* a 7260/3160 coalescing fix */

/* --- HBUS: the indirect windows ------------------------------------- */

#define HBUS_TARG_MEM_RADDR    0x40C
#define HBUS_TARG_MEM_WADDR    0x410
#define HBUS_TARG_MEM_WDAT     0x418
#define HBUS_TARG_MEM_RDAT     0x41C
#define HBUS_TARG_PRPH_WADDR   0x444
#define HBUS_TARG_PRPH_RADDR   0x448
#define HBUS_TARG_PRPH_WDAT    0x44C
#define HBUS_TARG_PRPH_RDAT    0x450
#define HBUS_TARG_WRPTR        0x460         /* index | queue << 8 */

/* --- FH: the DMA engine --------------------------------------------- */

#define FH_TFDIB_CTRL0(ch)     (0x1900 + 8 * (ch))
#define FH_TFDIB_CTRL1(ch)     (0x1904 + 8 * (ch))
#define FH_KW_MEM_ADDR_REG     0x197C
#define FH_SRVC_SRAM_ADDR(ch)  (0x19A4 + 4 * (ch))
#define FH_RSCSR_STTS_WPTR     0x1BC0
#define FH_RSCSR_RBDCB_BASE    0x1BC4
#define FH_RSCSR_WPTR          0x1BC8
#define FH_RSCSR_RDPTR         0x1BCC
#define FH_RCSR_CONFIG         0x1C00
#define FH_RCSR_RBDCB_WPTR     0x1C08
#define FH_RCSR_FLUSH_RB_REQ   0x1C10
#define FH_RSSR_RX_STATUS      0x1C44
#define FH_TCSR_CONFIG(ch)     (0x1D00 + 0x20 * (ch))
#define FH_TCSR_BUF_STS(ch)    (0x1D08 + 0x20 * (ch))
#define FH_TX_CHICKEN_BITS     0x1E98
#define FH_TSSR_TX_STATUS      0x1EB0

#define FH_SRVC_CHNL           9
#define FH_TX_CONFIG_ENABLE    0x80000000
#define FH_TX_CREDIT_ENABLE    0x00000008
#define FH_TX_CIRQ_HOST_ENDTFD 0x00100000
#define FH_TX_BUF_STS_LOAD     0x00101003    /* one TB, index 1, valid */
#define FH_TX_CHICKEN_AUTO_RETRY 0x00000002
#define FH_RX_STATUS_IDLE      0x01000000
/* RX on, IGNORE_RXF_EMPTY, interrupt to the host, 4 KiB buffers,
   timeout 0x11 x 32 us, 2^8 = 256 descriptors */
#define FH_RX_CONFIG           0x80801114
#define FH_CHUNK_MAX           0x20000
#define FH_MEM_EXT_START       0x40000
#define FH_MEM_EXT_END         0x57FFF

static uint32_t fh_cbbc(int q)
{
    if (q < 16)
        return 0x19D0 + 4 * (uint32_t)q;
    if (q < 20)
        return 0x1BF0 + 4 * (uint32_t)(q - 16);
    return 0x1B20 + 4 * (uint32_t)(q - 20);
}

/* --- PRPH: the periphery -------------------------------------------- */

#define APMG_CLK_EN_REG        0x3004
#define APMG_CLK_DIS_REG       0x3008
#define APMG_PS_CTRL_REG       0x300C
#define APMG_PCIDEV_STT_REG    0x3010
#define APMG_RTC_INT_STT_REG   0x301C
#define APMG_CLK_DMA_RQT       0x00000200
#define APMG_PS_EARLY_PWR_OFF_RESET_DIS 0x00400000
#define APMG_PS_PWR_SRC_MASK   0x03000000
#define APMG_PCIDEV_L1_ACT_DIS 0x00000800
#define APMG_RTC_INT_RFKILL    0x10000000
#define OSC_CLK                0xA04068
#define OSC_CLK_FORCE_CONTROL  0x00000008
#define LMPM_CHICK             0xA01FF8
#define DEVICE_SET_NMI_REG     0xA01C30

#define SCD_SRAM_BASE_ADDR     0xA02C00
#define SCD_DRAM_BASE_ADDR     0xA02C08
#define SCD_TXFACT             0xA02C10
#define SCD_QUEUECHAIN_SEL     0xA02CE8
#define SCD_GP_CTRL            0xA02DA8
#define SCD_AGGR_SEL           0xA02E48
#define SCD_EN_CTRL            0xA02E54
#define SCD_GP_CTRL_AUTO_ACTIVE 0x00040000
#define SCD_GP_CTRL_31_QUEUES  0x00000001
/* (the high halves add 4 x q, not 4 x (q - 20); only queue 9 is ever
   programmed here, the firmware programs the others) */
#define SCD_QUEUE_RDPTR(q)     ((q) < 20 ? 0xA02C68 + 4 * (q) : 0xA02EB4 + 4 * (q))
#define SCD_QUEUE_STATUS(q)    ((q) < 20 ? 0xA02D0C + 4 * (q) : 0xA02F34 + 4 * (q))
#define SCD_STTS_ACTIVE        (1u << 3)
#define SCD_STTS_WSL           (1u << 4)
#define SCD_STTS_ACT_EN        (1u << 19)
#define SCD_STTS_MSK           0x017F0000
/* The scheduler's SRAM, from its base */
#define SCD_CONTEXT_MEM        0x600
#define SCD_TRANS_TBL_END      ((0x7E0 + 2 * 31) & 0xFFFC)

/* --- queues, rings, buffers ----------------------------------------- */

#define NUM_QUEUES             31
#define CMD_QUEUE              9
#define CMD_FIFO               7
#define TFD_QUEUE_SIZE         256
#define TFD_SIZE               128
#define TFD_MAX_TBS            20
#define TB_MAX_LEN             4080          /* the length field is 12 bits */
#define SCRATCH_SIZE           16            /* TB0: what the FH writes back */
#define BC_TABLE_STRIDE        640           /* 320 x u16 per queue */
#define CMD_SLOTS              32            /* command queue window */
#define CMD_SLOT_SIZE          512
#define CMD_INLINE_MAX         324           /* header + payload, inline */
#define CMD_BIG_SIZE           16384         /* the larger ones (scan) */
#define TX_SLOTS               64            /* frames in flight per data queue */
#define TX_SLOT_SIZE           2560
#define TX_BODY_OFF            128
#define FRAME_LIMIT            64

#define RX_QUEUE_SIZE          256
#define RX_BUF_SIZE            4096
#define RX_FRAME_INVALID       0x55550000
#define RX_FRAME_SIZE_MASK     0x3FFF
#define RX_FRAME_ALIGN         64

#define SEQ_RX_FRAME           0x8000

/* --- firmware file -------------------------------------------------- */

#define FW_MAGIC               0x0A4C5749    /* "IWL\n" */
#define FW_HEADER_SIZE         88
#define TLV_PROBE_MAX_LEN      6
#define TLV_FLAGS              18
#define TLV_SEC_RT             19
#define TLV_SEC_INIT           20
#define TLV_DEF_CALIB          22
#define TLV_PHY_SKU            23
#define TLV_NUM_OF_CPU         27
#define TLV_API_CHANGES_SET    29
#define TLV_ENABLED_CAPA       30
#define TLV_N_SCAN_CHANNELS    31
#define TLV_FW_VERSION         36
#define FW_SECTIONS_MAX        16
#define FW_SEP_CPU1_CPU2       0xFFFFCCCC
#define FW_SEP_PAGING          0xAAAABBBB
#define API_NEW_VERSION        (1u << 20)
#define UCODE_API_MIN          13
#define UCODE_API_MAX          17
#define UCODE_REGULAR          0
#define UCODE_INIT             1

/* --- host commands and notifications -------------------------------- */

#define MVM_ALIVE              0x01
#define REPLY_ERROR            0x02
#define INIT_COMPLETE_NOTIF    0x04
#define PHY_CONTEXT_CMD        0x08
#define ADD_STA                0x18
#define TX_CMD                 0x1C
#define SCD_QUEUE_CFG          0x1D
#define PHY_CONFIGURATION_CMD  0x6A
#define CALIB_RES_NOTIF_PHY_DB 0x6B
#define PHY_DB_CMD             0x6C
#define POWER_TABLE_CMD        0x77
#define NVM_ACCESS_CMD         0x88
#define TX_ANT_CONFIGURATION_CMD 0x98
#define BT_CONFIG              0x9B
#define CARD_STATE_NOTIFICATION 0xA1
#define REPLY_SF_CFG_CMD       0xD1

#define ALIVE_STATUS_OK        0xCAFE
#define ERRLOG_MIN_ADDR        0x800000
#define ERRLOG_DWORDS          38            /* LOG_ERROR_TABLE_API_S_VER_2 */
#define UMAC_ERRLOG_DWORDS     15

#define NVM_SECTIONS           13
#define NVM_CHUNK              2048
#define NVM_BUF_SIZE           16384
#define NVM_SEC_HW             0
#define NVM_SEC_SW             1
#define NVM_SEC_CALIB          4
#define NVM_HW_ADDR_OFF        0x2A
#define NVM_SW_CHANNELS_OFF    0x40
#define NVM_VERSION_3160       0x0709
#define NVM_CH_VALID           0x0001
#define NVM_CH_IBSS            0x0002
#define NVM_CH_ACTIVE          0x0008
#define NVM_CH_RADAR           0x0010
#define NVM_CH_INDOOR_ONLY     0x0020
#define NVM_CH_GO_CONCURRENT   0x0040
#define NVM_CH_40MHZ           0x0200
#define NVM_CH_80MHZ           0x0400
#define NVM_CH_160MHZ          0x0800
#define NVM_SKU_BAND_52        0x0002
#define DEFAULT_MAX_TX_POWER   22

#define PHY_DB_CFG             1
#define PHY_DB_CALIB_NCH       2
#define PHY_DB_CALIB_CHG_PAPD  4
#define PHY_DB_CALIB_CHG_TXP   5
#define PHY_DB_GROUPS          9
#define PHY_DB_POOL_SIZE       32768

#define AUX_QUEUE              15
#define AUX_FIFO               5
#define AUX_MAC_INDEX          4
#define AUX_STA_ID             1
#define ADD_STA_SUCCESS        1
#define MAX_TID_COUNT          8
#define NUM_PHY_CTX            3
#define PHY_BAND_24            1
#define PHY_BAND_5             0
#define FW_CTXT_ACTION_ADD     1

/* Timeouts, in milliseconds */
#define ALIVE_TIMEOUT_MS       1000
#define CALIB_TIMEOUT_MS       2000
#define CMD_TIMEOUT_MS         2000
#define CMDQ_WATCHDOG_MS       2500
#define CHUNK_TIMEOUT_MS       5000
#define POLL_INTERVAL_MS       10

/* ===================================================================
 * State
 * =================================================================== */

struct fw_section {
    uint32_t addr;
    const uint8_t *data;
    uint32_t len;
};

struct fw_image {
    struct fw_section sec[FW_SECTIONS_MAX];
    int n;
};

static struct {
    const uint8_t *file;
    uint32_t size;
    struct fw_image init, rt;
    uint32_t flags, api0, capa0, phy_sku, n_cpu;
    uint32_t n_scan_channels;   /* room for channels in a scan command */
    uint32_t calib_flow[4], calib_event[4];
    uint32_t major, minor, local;
} fw;

static struct iwlwifi_state st;
static struct pci_dev pdev;
static volatile uint8_t *regs;
static int aspm_l1;
static int probed, fw_parsed, dma_ready;

/* What the hardware is doing */
static int hw_up;               /* start_hw ran, stop_device not yet */
static int device_enabled;      /* APM init ran */
static int rx_ready;            /* the RX ring is live */
static int fw_running;          /* ALIVE seen and the command queue up */
static int fw_err;              /* the firmware crashed */
static int hw_gone;             /* reads came back all ones */
static int access_failed;       /* MAC_ACCESS_REQ was not granted */
static int cmd_hold_awake;      /* the 3160's in-flight command wake-up */
static int cur_ucode;           /* UCODE_INIT or UCODE_REGULAR */

/* Why the current step failed, and which step it was */
static const char *step = "";
static char reason[128];

static struct {
    int seen;
    uint32_t len;
    uint16_t status;
    uint32_t errlog, evlog, scd_base, umac_errlog;
} alive;

static struct {
    int read;
    uint32_t valid, id, pc, blink1, blink2, ilink1, ilink2;
    uint32_t data1, data2, data3, hcmd, major, minor;
} crash;

/* DMA memory: from the frame allocator, identity-mapped, below 4 GiB */
static uint8_t *kw_buf;               /* keep-warm page */
static uint8_t *bc_tables;            /* 31 x 640 bytes, 1 KiB aligned */
static uint8_t *fw_bounce;            /* one firmware chunk */
static uint32_t fw_chunk_max;
static volatile uint32_t *rx_bd;      /* the receive descriptor ring */
static volatile uint8_t *rx_status;   /* struct iwl_rb_status */
static uint8_t *rx_buf[RX_QUEUE_SIZE];
static uint32_t rx_read, rx_write, rx_write_actual;
static uint8_t *cmd_bufs;             /* 32 slots of the command queue */
static uint8_t *cmd_big;              /* for the commands too large inline */
static int cmd_big_owner = -1;        /* ring index holding it */

struct txq {
    uint8_t *tfds;                    /* 256 x 128 */
    uint8_t *scratch;                 /* 256 x 16: the first TB of each */
    uint8_t *slots;                   /* data queues: TX_SLOTS x TX_SLOT_SIZE */
    uint32_t read, write;
    uint32_t stamp;                   /* pit ticks at the last progress */
    int active;
    uint8_t sta[TX_SLOTS];
};
static struct txq txq[NUM_QUEUES];
static uint32_t scd_base;

/* Per command-queue slot: what is in flight there */
static struct {
    uint32_t id;
    uint8_t flags;
} cmd_meta[CMD_SLOTS];

/* The one synchronous command allowed at a time */
static struct {
    int active, done;
    uint32_t idx;
    uint8_t *resp;
    uint32_t cap, len;
    int got_resp;
} sync_cmd;

/* The PHY DB: what the INIT image measured, handed to the runtime image */
struct phydb_entry {
    uint32_t off;
    uint16_t len;
    uint8_t present;
};
static uint8_t phydb_pool[PHY_DB_POOL_SIZE];
static uint32_t phydb_used;
static struct phydb_entry phydb_cfg, phydb_nch;
static struct phydb_entry phydb_papd[PHY_DB_GROUPS], phydb_txp[PHY_DB_GROUPS];
static int init_complete;

/* The NVM as read: all sections end to end in one buffer */
static uint8_t nvm_buf[NVM_BUF_SIZE];
static struct {
    uint32_t off, len;
    int present;
} nvm_sec[NVM_SECTIONS];
static uint8_t nvm_resp[8 + NVM_CHUNK + 64];
static struct iwlwifi_channel channels[IWLWIFI_MAX_CHANNELS];

/* Notification handlers registered by the later stages */
#define NOTIF_HANDLERS 16
static struct {
    uint32_t id;
    iwl_notif_fn fn;
} notif_handlers[NOTIF_HANDLERS];

/* ===================================================================
 * Small helpers
 * =================================================================== */

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t phys(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

/* Our writes to DMA memory must be visible before the register write
   that tells the card to look, and the card's before we read them */
static void dma_barrier(void)
{
    __sync_synchronize();
}

/* A tiny formatter: printk has no field widths, and a MAC address or a
   register dump wants them. %s %c %d %u %x, with an optional 0 and a
   width (%02x). */
static void vfmt(char *out, uint32_t cap, const char *f, va_list ap)
{
    uint32_t n = 0;
    /* PUT's argument is evaluated only while there is room: anything
       that has to happen whatever the room (moving along a string,
       taking the next argument) is done outside it */
#define PUT(c) do { if (n + 1 < cap) out[n++] = (char)(c); } while (0)
    for (; *f; f++) {
        if (*f != '%') {
            PUT(*f);
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0;
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            for (; *s; s++)
                PUT(*s);
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            PUT(c);
            break;
        }
        case 'd':
        case 'u':
        case 'x': {
            uint32_t v;
            int neg = 0;
            if (*f == 'd') {
                int d = va_arg(ap, int);
                neg = d < 0;
                v = neg ? (uint32_t)0 - (uint32_t)d : (uint32_t)d;
            } else {
                v = va_arg(ap, uint32_t);
            }
            uint32_t base = *f == 'x' ? 16 : 10;
            char tmp[12];
            int k = 0;
            do {
                tmp[k++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v);
            if (neg)
                PUT('-');
            for (int w = k + neg; w < width; w++)
                PUT(pad);
            while (k > 0) {
                k--;
                PUT(tmp[k]);
            }
            break;
        }
        case '\0':
            f--;
            break;
        default:
            PUT('%');
            PUT(*f);
        }
    }
    if (cap)
        out[n] = '\0';
#undef PUT
}

static void fmt(char *out, uint32_t cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vfmt(out, cap, f, ap);
    va_end(ap);
}

static void say(const char *f, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    va_end(ap);
    printk("%s", line);
}

/* The reason a step failed. The first one wins: what went wrong first
   is the cause, what followed is consequence. */
static void why(const char *f, ...)
{
    if (reason[0])
        return;
    va_list ap;
    va_start(ap, f);
    vfmt(reason, sizeof(reason), f, ap);
    va_end(ap);
}

static void mac_text(char *out, const uint8_t *m)
{
    fmt(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
        m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void *dma_alloc(uint32_t bytes)
{
    uint32_t frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t addr = pmm_alloc_contiguous(frames);
    if (!addr)
        return 0;
    void *p = (void *)(uintptr_t)addr;
    memset(p, 0, frames * PAGE_SIZE);
    return p;
}

/* ===================================================================
 * Transport: delays and waits
 * =================================================================== */

/* A port write costs about a microsecond on a PC; good enough for the
   short settling delays, which are all minimums */
static void iwl_udelay(uint32_t us)
{
    while (us--)
        io_wait();
}

/* Waits that may last: spin for the first millisecond (a command
   usually comes back in microseconds), then sleep between looks. The
   second limit, on the number of sleeps, ends the wait even if the
   clock should stand still. */
struct waiter {
    uint32_t start, ms, spins, sleeps;
};

static void wait_begin(struct waiter *w, uint32_t ms)
{
    w->start = pit_ticks();
    w->ms = ms;
    w->spins = 0;
    w->sleeps = 0;
}

/* 1 = paused, look again; 0 = the time is up */
static int wait_more(struct waiter *w)
{
    if (w->spins < 100) {
        w->spins++;
        iwl_udelay(10);
        return 1;
    }
    if ((pit_ticks() - w->start) * 10 >= w->ms || w->sleeps >= w->ms)
        return 0;
    task_sleep(1);
    w->sleeps++;
    return 1;
}

/* ===================================================================
 * Transport: registers, NIC access, PRPH and SRAM
 * =================================================================== */

static uint32_t csr_rd(uint32_t off)
{
    return mmio_read32(regs, off);
}

static void csr_wr(uint32_t off, uint32_t v)
{
    mmio_write32(regs, off, v);
}

static void csr_set(uint32_t off, uint32_t bits)
{
    csr_wr(off, csr_rd(off) | bits);
}

static void csr_clear(uint32_t off, uint32_t bits)
{
    csr_wr(off, csr_rd(off) & ~bits);
}

/* Waits until (reg & mask) == (bits & mask); 0, or -1 on timeout */
static int poll_bit(uint32_t off, uint32_t bits, uint32_t mask,
                    uint32_t timeout_us)
{
    for (uint32_t t = 0;; t += 10) {
        if ((csr_rd(off) & mask) == (bits & mask))
            return 0;
        if (t >= timeout_us)
            return -1;
        iwl_udelay(10);
    }
}

/* The card's internals are reachable only while it is awake: ask, and
   wait for the clock - and for it not to be on its way to sleep, or
   access would be granted to a card that is going down */
static int nic_grab(void)
{
    if (cmd_hold_awake)
        return 0;
    csr_set(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    if (poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY,
                 GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP, 15000) < 0) {
        csr_wr(CSR_RESET, RESET_FORCE_NMI);
        if (!access_failed)
            say("[wifi] timeout waiting for hardware access (GP_CNTRL 0x%08x)\n",
                csr_rd(CSR_GP_CNTRL));
        access_failed = 1;
        why("the card did not wake up for register access");
        return -1;
    }
    return 0;
}

static void nic_release(void)
{
    if (cmd_hold_awake)
        return;     /* a command in flight keeps it awake (the 3160 bug) */
    csr_clear(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
}

/* A CSR/FH register written with the card held awake */
static void direct_wr(uint32_t off, uint32_t v)
{
    if (nic_grab() < 0)
        return;
    csr_wr(off, v);
    nic_release();
}

static uint32_t prph_addr(uint32_t addr)
{
    return (addr & 0x000FFFFF) | (3u << 24);    /* a whole dword */
}

static uint32_t prph_rd_held(uint32_t addr)
{
    csr_wr(HBUS_TARG_PRPH_RADDR, prph_addr(addr));
    return csr_rd(HBUS_TARG_PRPH_RDAT);
}

static void prph_wr_held(uint32_t addr, uint32_t v)
{
    csr_wr(HBUS_TARG_PRPH_WADDR, prph_addr(addr));
    csr_wr(HBUS_TARG_PRPH_WDAT, v);
}

static uint32_t prph_rd(uint32_t addr)
{
    if (nic_grab() < 0)
        return 0x5A5A5A5A;
    uint32_t v = prph_rd_held(addr);
    nic_release();
    return v;
}

static void prph_wr(uint32_t addr, uint32_t v)
{
    if (nic_grab() < 0)
        return;
    prph_wr_held(addr, v);
    nic_release();
}

static void prph_set_bits(uint32_t addr, uint32_t bits)
{
    if (nic_grab() < 0)
        return;
    prph_wr_held(addr, prph_rd_held(addr) | bits);
    nic_release();
}

static void prph_clear_bits(uint32_t addr, uint32_t bits)
{
    if (nic_grab() < 0)
        return;
    prph_wr_held(addr, prph_rd_held(addr) & ~bits);
    nic_release();
}

/* 'keep' is the set of bits to keep, not the ones to change */
static void prph_set_bits_mask(uint32_t addr, uint32_t bits, uint32_t keep)
{
    if (nic_grab() < 0)
        return;
    prph_wr_held(addr, (prph_rd_held(addr) & keep) | bits);
    nic_release();
}

/* The card's SRAM, a dword at a time; the address auto-increments */
static int sram_rd(uint32_t addr, uint32_t *buf, int n)
{
    if (nic_grab() < 0)
        return -1;
    csr_wr(HBUS_TARG_MEM_RADDR, addr);
    for (int i = 0; i < n; i++)
        buf[i] = csr_rd(HBUS_TARG_MEM_RDAT);
    nic_release();
    return 0;
}

/* buf == 0 writes zeros */
static int sram_wr(uint32_t addr, const uint32_t *buf, int n)
{
    if (nic_grab() < 0)
        return -1;
    csr_wr(HBUS_TARG_MEM_WADDR, addr);
    for (int i = 0; i < n; i++)
        csr_wr(HBUS_TARG_MEM_WDAT, buf ? buf[i] : 0);
    nic_release();
    return 0;
}

static void force_nmi(void)
{
    prph_wr(DEVICE_SET_NMI_REG, 0x80);      /* the driver's NMI ... */
    prph_wr(DEVICE_SET_NMI_REG, 0x01);      /* ... and the hardware's */
}

static int rfkill_set(void)
{
    if (!regs)
        return 0;
    return !(csr_rd(CSR_GP_CNTRL) & GP_HW_RF_KILL_SW);
}

static void disable_interrupts(void)
{
    csr_wr(CSR_INT_MASK, 0);
    csr_wr(CSR_INT, 0xFFFFFFFF);
    csr_wr(CSR_FH_INT_STATUS, 0xFFFFFFFF);
}

/* ===================================================================
 * Transport: power-up, APM, stop
 * =================================================================== */

/* "The driver is here": NIC_READY, a handshake with the management
   engine, which may own the card */
static int set_hw_ready(void)
{
    csr_set(CSR_HW_IF_CONFIG_REG, HW_IF_NIC_READY);
    if (poll_bit(CSR_HW_IF_CONFIG_REG, HW_IF_NIC_READY, HW_IF_NIC_READY, 50) < 0)
        return -1;
    csr_set(CSR_MBOX_SET_REG, MBOX_OS_ALIVE);
    return 0;
}

static int prepare_card_hw(void)
{
    if (set_hw_ready() == 0)
        return 0;
    csr_set(CSR_DBG_LINK_PWR_MGMT_REG, 0x80000000);
    task_sleep(1);
    /* t runs across all ten rounds: only the first gets the full
       150 ms, the later ones a single try each */
    uint32_t t = 0;
    for (int round = 0; round < 10; round++) {
        csr_set(CSR_HW_IF_CONFIG_REG, HW_IF_PREPARE);
        do {
            if (set_hw_ready() == 0)
                return 0;
            iwl_udelay(200);
            t += 200;
        } while (t < 150000);
        task_sleep(25);
    }
    why("the card did not become ready (NIC_READY); the management engine may own it");
    return -1;
}

static int apm_init(void)
{
    csr_set(CSR_GIO_CHICKEN_BITS, GIO_CHICKEN_DIS_L0S_EXIT_TIMER);
    csr_set(CSR_GIO_CHICKEN_BITS, GIO_CHICKEN_L1A_NO_L0S_RX);
    csr_set(CSR_DBG_HPET_MEM_REG, 0xFFFF0000);
    csr_set(CSR_HW_IF_CONFIG_REG, HW_IF_HAP_WAKE_L1A);
    /* (the bit is named L0S_ENABLED, and it is set when L1 is on) */
    if (aspm_l1)
        csr_set(CSR_GIO_REG, GIO_L0S_ENABLED);
    else
        csr_clear(CSR_GIO_REG, GIO_L0S_ENABLED);
    csr_set(CSR_GP_CNTRL, GP_INIT_DONE);
    if (poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000) < 0) {
        why("the MAC clock did not come up (APM init)");
        return IWL_ERR_TIMEOUT;
    }
    /* The 7260/3160 oscillator wants exactly this read-read-set-read-
       read pattern, so that it keeps counting the L1 exit time */
    prph_rd(OSC_CLK);
    prph_rd(OSC_CLK);
    prph_set_bits(OSC_CLK, OSC_CLK_FORCE_CONTROL);
    prph_rd(OSC_CLK);
    prph_rd(OSC_CLK);
    prph_wr(APMG_CLK_EN_REG, APMG_CLK_DMA_RQT);
    iwl_udelay(20);
    prph_set_bits(APMG_PCIDEV_STT_REG, APMG_PCIDEV_L1_ACT_DIS);
    prph_wr(APMG_RTC_INT_STT_REG, APMG_RTC_INT_RFKILL);
    if (access_failed)
        return IWL_ERR_IO;
    device_enabled = 1;
    return IWL_OK;
}

static int start_hw(void)
{
    access_failed = 0;
    if (prepare_card_hw() < 0)
        return IWL_ERR_IO;
    csr_wr(CSR_RESET, RESET_SW_RESET);
    iwl_udelay(15);
    int r = apm_init();
    if (r < 0)
        return r;
    /* (polled: the interrupt mask stays 0, see the top of the file) */
    hw_up = 1;
    st.rfkill = rfkill_set();
    return IWL_OK;
}

static void txq_reset_all(void)
{
    for (int q = 0; q < NUM_QUEUES; q++) {
        struct txq *t = &txq[q];
        if (t->tfds)
            memset(t->tfds, 0, TFD_QUEUE_SIZE * TFD_SIZE);
        t->read = t->write = 0;
        t->active = 0;
    }
    memset(cmd_meta, 0, sizeof(cmd_meta));
    cmd_big_owner = -1;
    sync_cmd.active = 0;
}

static void tx_stop(void)
{
    prph_wr(SCD_TXFACT, 0);
    if (nic_grab() == 0) {
        for (int ch = 0; ch < 8; ch++) {
            uint32_t idle = (1u << ch) << 16;
            csr_wr(FH_TCSR_CONFIG(ch), 0);
            if (poll_bit(FH_TSSR_TX_STATUS, idle, idle, 5000) < 0)
                say("[wifi] TX DMA channel %d did not go idle\n", ch);
        }
        nic_release();
    }
    txq_reset_all();
}

static void rx_stop(void)
{
    if (nic_grab() < 0)
        return;
    csr_wr(FH_RCSR_CONFIG, 0);
    if (poll_bit(FH_RSSR_RX_STATUS, FH_RX_STATUS_IDLE, FH_RX_STATUS_IDLE, 1000) < 0)
        say("[wifi] RX DMA did not go idle\n");
    nic_release();
}

static void apm_stop(void)
{
    device_enabled = 0;
    csr_set(CSR_RESET, RESET_STOP_MASTER);
    if (poll_bit(CSR_RESET, RESET_MASTER_DISABLED, RESET_MASTER_DISABLED, 100) < 0)
        say("[wifi] master disable timed out\n");
    csr_set(CSR_RESET, RESET_SW_RESET);
    iwl_udelay(10);
    csr_clear(CSR_GP_CNTRL, GP_INIT_DONE);
}

/* Everything off, the card back in reset and quiet, ready for the next
   start (or for nothing) */
static void stop_device(void)
{
    if (!hw_up || !regs)
        return;
    hw_up = 0;
    fw_running = 0;
    disable_interrupts();
    if (device_enabled && !hw_gone) {
        tx_stop();
        rx_stop();
        prph_wr(APMG_CLK_DIS_REG, APMG_CLK_DMA_RQT);
        iwl_udelay(5);
    }
    rx_ready = 0;
    cmd_hold_awake = 0;
    csr_clear(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    apm_stop();
    csr_wr(CSR_RESET, RESET_SW_RESET);
    iwl_udelay(20);
    /* Again: with the radio switch off, the APM stop raises an RF-kill
       interrupt that some parts keep raising until it is acked */
    disable_interrupts();
    st.rfkill = rfkill_set();
    prepare_card_hw();          /* take the card back */
}

/* ===================================================================
 * Transport: RX
 * =================================================================== */

static void rx_restock(void)
{
    /* Each ring index keeps its own buffer: what was taken out has been
       copied, so the buffer goes straight back. At most 255 are the
       card's - read == write would look "full" to it. */
    while (((rx_read - rx_write - 1) & (RX_QUEUE_SIZE - 1)) > 0) {
        rx_bd[rx_write] = phys(rx_buf[rx_write]) >> 8;
        rx_write = (rx_write + 1) & (RX_QUEUE_SIZE - 1);
    }
    dma_barrier();
    /* The card takes the write index in steps of eight only */
    if (rx_write_actual != (rx_write & ~7u)) {
        rx_write_actual = rx_write & ~7u;
        csr_wr(FH_RSCSR_WPTR, rx_write_actual);
    }
}

static int rx_init(void)
{
    rx_read = rx_write = rx_write_actual = 0;
    memset((void *)rx_status, 0, 16);
    while (((rx_read - rx_write - 1) & (RX_QUEUE_SIZE - 1)) > 0) {
        rx_bd[rx_write] = phys(rx_buf[rx_write]) >> 8;
        rx_write = (rx_write + 1) & (RX_QUEUE_SIZE - 1);
    }
    dma_barrier();
    if (nic_grab() < 0)
        return IWL_ERR_IO;
    csr_wr(FH_RCSR_CONFIG, 0);
    csr_wr(FH_RCSR_RBDCB_WPTR, 0);
    csr_wr(FH_RCSR_FLUSH_RB_REQ, 0);
    csr_wr(FH_RSCSR_RDPTR, 0);
    csr_wr(FH_RSCSR_WPTR, 0);
    csr_wr(FH_RSCSR_RBDCB_BASE, phys((const void *)rx_bd) >> 8);
    csr_wr(FH_RSCSR_STTS_WPTR, phys((const void *)rx_status) >> 4);
    csr_wr(FH_RCSR_CONFIG, FH_RX_CONFIG);
    nic_release();
    /* The coalescing timer is the low byte of this register; only 32-bit
       accesses go to the card, so the byte goes in by read-modify-write */
    csr_wr(CSR_INT_COALESCING,
           (csr_rd(CSR_INT_COALESCING) & ~0xFFu) | HOST_INT_TIMEOUT_DEF);
    csr_set(CSR_INT_COALESCING, HOST_INT_OPER_MODE);
    rx_write_actual = rx_write & ~7u;
    csr_wr(FH_RSCSR_WPTR, rx_write_actual);
    rx_ready = 1;
    return IWL_OK;
}

static void handle_packet(const uint8_t *pkt, uint32_t len);

/* One receive buffer: packets one after another, each on a 64-byte
   boundary, until an end marker or the end of the buffer */
static void rx_parse_buffer(const uint8_t *rb)
{
    uint32_t off = 0;
    while (off + 4 + 4 < RX_BUF_SIZE) {
        uint32_t lnf = get32(rb + off);
        if (lnf == RX_FRAME_INVALID)
            break;
        uint32_t len = lnf & RX_FRAME_SIZE_MASK;   /* header + payload */
        if (len < 4 || off + 4 + len > RX_BUF_SIZE)
            break;          /* not a packet: nothing sane follows */
        handle_packet(rb + off, len);
        off += (len + 4 + RX_FRAME_ALIGN - 1) & ~(uint32_t)(RX_FRAME_ALIGN - 1);
    }
}

static void rx_process(void)
{
    /* A handler that waited for a command's answer would read the ring
       again from inside its own buffer, and see that buffer twice */
    static int reading;
    if (!rx_ready || reading)
        return;
    reading = 1;
    /* closed_rb_num, read in one access: the card may be writing it */
    uint32_t closed = *(volatile uint16_t *)rx_status & 0x0FFF;
    closed &= RX_QUEUE_SIZE - 1;
    dma_barrier();
    int n = 0;
    while (rx_read != closed && n < RX_QUEUE_SIZE) {
        rx_parse_buffer(rx_buf[rx_read]);
        rx_read = (rx_read + 1) & (RX_QUEUE_SIZE - 1);
        n++;
        if (!rx_ready) {
            reading = 0;
            return;     /* a handler stopped the card */
        }
    }
    if (n)
        rx_restock();
    reading = 0;
}

/* ===================================================================
 * Transport: TX queues
 * =================================================================== */

static uint8_t *tfd_at(int q, uint32_t idx)
{
    return txq[q].tfds + (idx & (TFD_QUEUE_SIZE - 1)) * TFD_SIZE;
}

static uint8_t *scratch_at(int q, uint32_t idx)
{
    return txq[q].scratch + (idx & (TFD_QUEUE_SIZE - 1)) * SCRATCH_SIZE;
}

static int tfd_add_tb(uint8_t *tfd, uint32_t addr, uint32_t len)
{
    uint32_t n = tfd[3] & 0x1F;
    if (n >= TFD_MAX_TBS || len > 0xFFF)
        return -1;
    uint8_t *tb = tfd + 4 + 6 * n;
    put32(tb, addr);
    put16(tb + 4, len << 4);         /* address bits 35:32 are 0: below 4 GiB */
    tfd[3] = (uint8_t)(n + 1);
    return 0;
}

static void bc_set(int q, uint32_t idx, uint16_t entry)
{
    uint8_t *bc = bc_tables + q * BC_TABLE_STRIDE;
    put16(bc + 2 * idx, entry);
    /* the scheduler reads past the ring's end for its window */
    if (idx < 64)
        put16(bc + 2 * (TFD_QUEUE_SIZE + idx), entry);
}

static int tx_init(void)
{
    txq_reset_all();
    prph_wr(SCD_TXFACT, 0);
    direct_wr(FH_KW_MEM_ADDR_REG, phys(kw_buf) >> 4);
    if (nic_grab() < 0)
        return IWL_ERR_IO;
    for (int q = 0; q < NUM_QUEUES; q++)
        csr_wr(fh_cbbc(q), phys(txq[q].tfds) >> 8);
    nic_release();
    prph_set_bits(SCD_GP_CTRL, SCD_GP_CTRL_AUTO_ACTIVE);
    prph_set_bits(SCD_GP_CTRL, SCD_GP_CTRL_31_QUEUES);
    return access_failed ? IWL_ERR_IO : IWL_OK;
}

/* The command queue is the one queue the driver programs into the
   scheduler itself; the firmware does the rest */
static void txq_enable_cmd(void)
{
    const int q = CMD_QUEUE;
    prph_wr(SCD_EN_CTRL, 0);
    prph_wr(SCD_QUEUE_STATUS(q), SCD_STTS_ACT_EN);       /* inactive */
    prph_clear_bits(SCD_AGGR_SEL, 1u << q);
    uint32_t ssn = txq[q].read;
    txq[q].read = txq[q].write = ssn & 0xFF;
    direct_wr(HBUS_TARG_WRPTR, (ssn & 0xFF) | (q << 8));
    prph_wr(SCD_QUEUE_RDPTR(q), ssn);
    uint32_t ctx[2] = {
        0,
        (FRAME_LIMIT & 0x7F) | ((FRAME_LIMIT << 16) & 0x7F0000),
    };
    sram_wr(scd_base + SCD_CONTEXT_MEM + 8 * q, ctx, 2);
    prph_wr(SCD_QUEUE_STATUS(q), SCD_STTS_ACTIVE | CMD_FIFO | SCD_STTS_WSL |
                                 SCD_STTS_MSK);
    prph_wr(SCD_EN_CTRL, 1u << q);
    txq[q].active = 1;
}

/* After ALIVE: the scheduler's memory cleared, the command queue on,
   the FIFOs and DMA channels running */
static int tx_start(void)
{
    for (int q = 0; q < NUM_QUEUES; q++)
        txq[q].active = 0;
    uint32_t base = prph_rd(SCD_SRAM_BASE_ADDR);
    if (alive.scd_base && alive.scd_base != base)
        say("[wifi] scheduler base: ALIVE says 0x%x, the card 0x%x\n",
            alive.scd_base, base);
    scd_base = base;
    sram_wr(scd_base + SCD_CONTEXT_MEM, 0,
            (SCD_TRANS_TBL_END - SCD_CONTEXT_MEM) / 4);
    prph_wr(SCD_DRAM_BASE_ADDR, phys(bc_tables) >> 10);
    txq_enable_cmd();
    prph_wr(SCD_TXFACT, 0xFF);
    if (nic_grab() == 0) {
        for (int ch = 0; ch < 8; ch++)
            csr_wr(FH_TCSR_CONFIG(ch), FH_TX_CONFIG_ENABLE | FH_TX_CREDIT_ENABLE);
        nic_release();
    }
    if (nic_grab() == 0) {
        csr_wr(FH_TX_CHICKEN_BITS,
               csr_rd(FH_TX_CHICKEN_BITS) | FH_TX_CHICKEN_AUTO_RETRY);
        nic_release();
    }
    prph_clear_bits(APMG_PCIDEV_STT_REG, APMG_PCIDEV_L1_ACT_DIS);
    if (access_failed)
        return IWL_ERR_IO;
    return IWL_OK;
}

/* A data queue's transport side: the firmware configures the scheduler
   for it (SCD_QUEUE_CFG), the driver only sets the pointers */
static void txq_enable_transport(int q, uint32_t ssn)
{
    txq[q].read = txq[q].write = ssn & 0xFF;
    direct_wr(HBUS_TARG_WRPTR, (ssn & 0xFF) | ((uint32_t)q << 8));
    txq[q].active = 1;
    txq[q].stamp = pit_ticks();
}

static void tx_reclaim(int q, uint32_t ssn)
{
    struct txq *t = &txq[q];
    uint32_t to = ssn & 0xFF;
    if (!t->active || t->read == to)
        return;
    uint32_t last = (to - 1) & 0xFF;
    if (((last - t->read) & 0xFF) >= ((t->write - t->read) & 0xFF)) {
        say("[wifi] queue %d: reclaim to %u, but %u..%u are in flight\n",
            q, to, t->read, t->write);
        return;
    }
    while (t->read != to) {
        bc_set(q, t->read, (uint16_t)(1 | (t->sta[t->read % TX_SLOTS] << 12)));
        tfd_at(q, t->read)[3] = 0;
        t->read = (t->read + 1) & 0xFF;
    }
    t->stamp = pit_ticks();
}

int iwl_tx_frame(int queue, const uint8_t *tx_cmd, const void *hdr,
                 uint32_t hdr_len, const void *body, uint32_t body_len)
{
    if (queue < 0 || queue >= NUM_QUEUES || queue == CMD_QUEUE ||
        !txq[queue].active || !fw_running)
        return IWL_ERR_STATE;
    if (hdr_len < 2 || hdr_len > 40 ||
        body_len > TX_SLOT_SIZE - TX_BODY_OFF)
        return IWL_ERR_STATE;
    struct txq *t = &txq[queue];
    if (((t->write - t->read) & 0xFF) >= TX_SLOTS - 1)
        return IWL_ERR_BUSY;
    if (!t->slots) {
        t->slots = dma_alloc(TX_SLOTS * TX_SLOT_SIZE);
        if (!t->slots)
            return IWL_ERR_NOMEM;
    }
    uint32_t w = t->write;
    uint8_t *cmd = t->slots + (w % TX_SLOTS) * TX_SLOT_SIZE;
    memset(cmd, 0, TX_BODY_OFF);
    cmd[0] = TX_CMD;
    cmd[1] = 0;
    put16(cmd + 2, ((uint32_t)queue << 8) | w);
    memcpy(cmd + 4, tx_cmd, 56);
    memcpy(cmd + 60, hdr, hdr_len);
    /* The device writes its retry counts back into the TX command's
       scratch field, which lives in TB0 */
    uint32_t tb0 = phys(scratch_at(queue, w));
    put32(cmd + 4 + 44, tb0 + 4 + 8);
    cmd[4 + 48] = 0;
    uint32_t len = 56 + 4 + hdr_len - SCRATCH_SIZE;
    uint32_t tb1_len = (len + 3) & ~3u;
    if (tb1_len != len)         /* tell it about the pad after the header */
        put32(cmd + 8, get32(cmd + 8) | 0x00100000);
    memcpy(scratch_at(queue, w), cmd, SCRATCH_SIZE);
    uint8_t *tfd = tfd_at(queue, w);
    memset(tfd, 0, TFD_SIZE);
    tfd_add_tb(tfd, tb0, SCRATCH_SIZE);
    tfd_add_tb(tfd, phys(cmd) + SCRATCH_SIZE, tb1_len);
    if (body_len) {
        memcpy(cmd + TX_BODY_OFF, body, body_len);
        tfd_add_tb(tfd, phys(cmd) + TX_BODY_OFF, body_len);
    }
    /* Byte count for the scheduler, in dwords for this firmware: the
       frame, CRC and delimiter, plus what encryption adds */
    uint32_t n = get16(tx_cmd) + 4 + 4;
    switch (tx_cmd[17] & 7) {
    case 1: n += 4 + 4; break;      /* WEP: IV + ICV */
    case 2: n += 8; break;          /* CCM: MIC */
    case 3: n += 4; break;          /* TKIP: ICV */
    }
    n = (n + 3) / 4;
    if (n > 0xFFF)
        return IWL_ERR_STATE;
    bc_set(queue, w, (uint16_t)(n | ((uint32_t)tx_cmd[16] << 12)));
    t->sta[w % TX_SLOTS] = tx_cmd[16];
    if (t->read == t->write)
        t->stamp = pit_ticks();
    t->write = (w + 1) & 0xFF;
    dma_barrier();
    csr_wr(HBUS_TARG_WRPTR, t->write | ((uint32_t)queue << 8));
    return IWL_OK;
}

/* ===================================================================
 * Transport: host commands
 * =================================================================== */

/* The 3160 bug: from the first command until the queue drains, the card
   is held awake, and the NIC-access helpers leave the bit alone */
static int set_cmd_in_flight(void)
{
    if (cmd_hold_awake)
        return 0;
    csr_set(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    if (poll_bit(CSR_GP_CNTRL, GP_MAC_CLOCK_READY,
                 GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP, 15000) < 0) {
        csr_clear(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
        why("failed to wake the card for a host command");
        return IWL_ERR_IO;
    }
    cmd_hold_awake = 1;
    return 0;
}

static void clear_cmd_in_flight(void)
{
    if (!cmd_hold_awake)
        return;
    cmd_hold_awake = 0;
    csr_clear(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
}

static int enqueue_hcmd(struct iwl_hcmd *c)
{
    struct txq *t = &txq[CMD_QUEUE];
    uint32_t group = (c->id >> 8) & 0xFF;
    uint32_t hdr_len = group ? 8 : 4;
    uint32_t len = c->len[0] + c->len[1];
    uint32_t total = hdr_len + len;
    uint32_t used = (t->write - t->read) & 0xFF;
    uint32_t need = (c->flags & IWL_CMD_ASYNC) ? 2 : 1;
    if (CMD_SLOTS - used < need) {
        why("no space in the command queue");
        return IWL_ERR_BUSY;
    }
    uint32_t w = t->write;
    uint8_t *buf;
    if (total <= CMD_INLINE_MAX) {
        buf = cmd_bufs + (w & (CMD_SLOTS - 1)) * CMD_SLOT_SIZE;
    } else {
        /* Too large for the queue's own buffer: a buffer of its own,
           mapped as the following TBs */
        if (total > CMD_BIG_SIZE || cmd_big_owner >= 0) {
            why("command 0x%x of %u bytes cannot be queued now", c->id, len);
            return IWL_ERR_BUSY;
        }
        buf = cmd_big;
        cmd_big_owner = (int)w;
    }
    buf[0] = (uint8_t)c->id;
    buf[1] = (uint8_t)group;
    put16(buf + 2, (CMD_QUEUE << 8) | w);
    if (group) {
        put16(buf + 4, len);
        buf[6] = 0;
        buf[7] = (uint8_t)(c->id >> 16);
    }
    if (c->len[0])
        memcpy(buf + hdr_len, c->data[0], c->len[0]);
    if (c->len[1])
        memcpy(buf + hdr_len + c->len[0], c->data[1], c->len[1]);

    uint32_t tb0_len = total < SCRATCH_SIZE ? total : SCRATCH_SIZE;
    memcpy(scratch_at(CMD_QUEUE, w), buf, tb0_len);
    uint8_t *tfd = tfd_at(CMD_QUEUE, w);
    memset(tfd, 0, TFD_SIZE);
    tfd_add_tb(tfd, phys(scratch_at(CMD_QUEUE, w)), tb0_len);
    uint32_t at = phys(buf) + SCRATCH_SIZE;
    for (uint32_t rest = total - tb0_len; rest;) {
        uint32_t n = rest < TB_MAX_LEN ? rest : TB_MAX_LEN;
        if (tfd_add_tb(tfd, at, n) < 0) {
            why("command 0x%x does not fit in one descriptor", c->id);
            if (cmd_big_owner == (int)w)
                cmd_big_owner = -1;
            return IWL_ERR_BUSY;
        }
        at += n;
        rest -= n;
    }
    cmd_meta[w & (CMD_SLOTS - 1)].id = c->id;
    cmd_meta[w & (CMD_SLOTS - 1)].flags = (uint8_t)c->flags;

    int r = set_cmd_in_flight();
    if (r < 0) {
        if (cmd_big_owner == (int)w)
            cmd_big_owner = -1;
        return r;
    }
    if (t->read == t->write)
        t->stamp = pit_ticks();
    t->write = (w + 1) & 0xFF;
    dma_barrier();
    csr_wr(HBUS_TARG_WRPTR, t->write | (CMD_QUEUE << 8));
    return (int)w;
}

/* A response came back for the command at its sequence's index */
static void hcmd_complete(const uint8_t *pkt, uint32_t len)
{
    static int warned;
    uint16_t seq = get16(pkt + 6);
    uint32_t q = (seq >> 8) & 0x1F;
    uint32_t idx = seq & 0xFF;
    struct txq *t = &txq[CMD_QUEUE];
    if (q != CMD_QUEUE) {
        if (warned++ < 4)
            say("[wifi] response 0x%x for queue %u ignored\n", pkt[4], q);
        return;
    }
    uint32_t used = (t->write - t->read) & 0xFF;
    if (((idx - t->read) & 0xFF) >= used) {
        if (warned++ < 4)
            say("[wifi] response 0x%x for command %u, which is not in flight\n",
                pkt[4], idx);
        return;
    }
    if (sync_cmd.active && idx == sync_cmd.idx) {
        uint32_t plen = len - 4;
        if (sync_cmd.resp) {
            uint32_t n = plen < sync_cmd.cap ? plen : sync_cmd.cap;
            memcpy(sync_cmd.resp, pkt + 8, n);
        }
        sync_cmd.len = plen;
        sync_cmd.got_resp = 1;
        sync_cmd.done = 1;
    }
    tfd_at(CMD_QUEUE, idx)[3] = 0;
    if (cmd_big_owner == (int)idx)
        cmd_big_owner = -1;
    /* Answers come in order, one per command: anything skipped means
       the firmware lost track */
    uint32_t freed = ((idx + 1) - t->read) & 0xFF;
    if (freed > 1) {
        say("[wifi] HCMD skipped: answer to %u while %u was the oldest\n",
            idx, t->read);
        force_nmi();
    }
    t->read = (idx + 1) & 0xFF;
    t->stamp = pit_ticks();
    if (t->read == t->write)
        clear_cmd_in_flight();
}

static void service(void);

int iwl_send_cmd(struct iwl_hcmd *c)
{
    if (fw_err)
        return IWL_ERR_FW;
    if (hw_gone)
        return IWL_ERR_GONE;
    if (!fw_running || !hw_up) {
        why("no firmware is running to take command 0x%x", c->id);
        return IWL_ERR_STATE;
    }
    if (rfkill_set() && !(c->flags & IWL_CMD_SEND_IN_RFKILL))
        return IWL_ERR_RFKILL;
    if (c->flags & IWL_CMD_ASYNC) {
        int r = enqueue_hcmd(c);
        return r < 0 ? r : IWL_OK;
    }
    if (sync_cmd.active)
        return IWL_ERR_BUSY;

    sync_cmd.active = 1;
    sync_cmd.done = 0;
    sync_cmd.got_resp = 0;
    sync_cmd.len = 0;
    sync_cmd.resp = (c->flags & IWL_CMD_WANT_RESP) ? c->resp : 0;
    sync_cmd.cap = c->resp_cap;
    sync_cmd.idx = txq[CMD_QUEUE].write;
    int r = enqueue_hcmd(c);
    if (r < 0) {
        sync_cmd.active = 0;
        return r;
    }

    struct waiter w;
    wait_begin(&w, CMD_TIMEOUT_MS);
    int result = IWL_ERR_TIMEOUT;
    do {
        service();
        if (fw_err) {
            result = IWL_ERR_FW;
            break;
        }
        if (hw_gone) {
            result = IWL_ERR_GONE;
            break;
        }
        if (sync_cmd.done) {
            result = IWL_OK;
            break;
        }
        if (!(c->flags & IWL_CMD_SEND_IN_RFKILL) && rfkill_set()) {
            st.rfkill = 1;
            result = IWL_ERR_RFKILL;
            break;
        }
    } while (wait_more(&w));
    sync_cmd.active = 0;
    c->resp_len = sync_cmd.len;

    if (result == IWL_ERR_TIMEOUT) {
        say("[wifi] command 0x%x: no answer within %u ms\n", c->id,
            (uint32_t)CMD_TIMEOUT_MS);
        why("command 0x%x got no answer within %u ms", c->id,
            (uint32_t)CMD_TIMEOUT_MS);
        /* An NMI makes the firmware write its error log, which says
           what it was doing instead */
        force_nmi();
        struct waiter w2;
        wait_begin(&w2, 200);
        do {
            service();
        } while (!fw_err && !hw_gone && wait_more(&w2));
        fw_err = 1;
        return IWL_ERR_TIMEOUT;
    }
    if (result == IWL_OK && (c->flags & IWL_CMD_WANT_RESP) && !sync_cmd.got_resp)
        return IWL_ERR_IO;
    return result;
}

int iwl_send_cmd_pdu(uint32_t id, const void *data, uint32_t len,
                     uint32_t flags)
{
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.data[0] = data;
    c.len[0] = len;
    c.flags = flags;
    return iwl_send_cmd(&c);
}

/* ===================================================================
 * Transport: firmware load
 * =================================================================== */

static int load_chunk(uint32_t dst, const uint8_t *src, uint32_t n)
{
    memcpy(fw_bounce, src, n);
    dma_barrier();
    /* Leftovers of an earlier chunk must not look like this one done */
    csr_wr(CSR_FH_INT_STATUS, FH_INT_TX_MASK);
    csr_wr(CSR_INT, INT_FH_TX);
    if (nic_grab() < 0)
        return IWL_ERR_IO;
    csr_wr(FH_TCSR_CONFIG(FH_SRVC_CHNL), 0);           /* pause */
    csr_wr(FH_SRVC_SRAM_ADDR(FH_SRVC_CHNL), dst);
    csr_wr(FH_TFDIB_CTRL0(FH_SRVC_CHNL), phys(fw_bounce));
    csr_wr(FH_TFDIB_CTRL1(FH_SRVC_CHNL), n);           /* address bits 35:32 = 0 */
    csr_wr(FH_TCSR_BUF_STS(FH_SRVC_CHNL), FH_TX_BUF_STS_LOAD);
    csr_wr(FH_TCSR_CONFIG(FH_SRVC_CHNL),
           FH_TX_CONFIG_ENABLE | FH_TX_CIRQ_HOST_ENDTFD);
    nic_release();

    struct waiter w;
    wait_begin(&w, CHUNK_TIMEOUT_MS);
    do {
        uint32_t inta = csr_rd(CSR_INT);
        if (inta == 0xFFFFFFFF) {
            hw_gone = 1;
            why("the card stopped answering during the firmware load");
            return IWL_ERR_GONE;
        }
        if ((inta & INT_FH_TX) || (csr_rd(CSR_FH_INT_STATUS) & FH_INT_TX_MASK)) {
            csr_wr(CSR_FH_INT_STATUS, FH_INT_TX_MASK);
            csr_wr(CSR_INT, INT_FH_TX);
            return IWL_OK;
        }
    } while (wait_more(&w));
    return IWL_ERR_TIMEOUT;
}

static int load_section(const char *image, int i, const struct fw_section *s)
{
    uint32_t chunk = s->len < fw_chunk_max ? s->len : fw_chunk_max;
    for (uint32_t off = 0; off < s->len; off += chunk) {
        uint32_t dst = s->addr + off;
        uint32_t n = s->len - off < chunk ? s->len - off : chunk;
        int ext = dst >= FH_MEM_EXT_START && dst <= FH_MEM_EXT_END;
        if (ext)
            prph_set_bits(LMPM_CHICK, 1);
        int r = load_chunk(dst, s->data + off, n);
        if (ext)
            prph_clear_bits(LMPM_CHICK, 1);
        if (r == IWL_ERR_TIMEOUT)
            why("%s image, section %d: the chunk of %u bytes to 0x%x was not "
                "acknowledged within %u ms", image, i, n, dst,
                (uint32_t)CHUNK_TIMEOUT_MS);
        if (r < 0)
            return r;
    }
    return IWL_OK;
}

static int load_image(const char *name, const struct fw_image *img)
{
    for (int i = 0; i < img->n; i++) {
        const struct fw_section *s = &img->sec[i];
        /* A single-CPU image ends at the separator */
        if (!s->len || s->addr == FW_SEP_CPU1_CPU2 || s->addr == FW_SEP_PAGING)
            break;
        int r = load_section(name, i, s);
        if (r < 0)
            return r;
    }
    csr_wr(CSR_RESET, 0);       /* the firmware's CPU out of reset: go */
    return IWL_OK;
}

/* ===================================================================
 * Transport: start of an image
 * =================================================================== */

/* The radio and PHY description for CSR_HW_IF_CONFIG_REG: MAC step and
   dash from the chip, the radio's from the firmware's PHY_SKU */
static void nic_config(void)
{
    uint32_t radio_type = fw.phy_sku & 0x3;
    uint32_t radio_step = (fw.phy_sku >> 2) & 0x3;
    uint32_t radio_dash = (fw.phy_sku >> 4) & 0x3;
    uint32_t v = (st.hw_rev & 0xF) | (radio_type << 10) | (radio_step << 14) |
                 (radio_dash << 12) | HW_IF_RADIO_SI;
    csr_wr(CSR_HW_IF_CONFIG_REG,
           (csr_rd(CSR_HW_IF_CONFIG_REG) & ~HW_IF_NIC_CONFIG_MASK) | v);
    /* against the card staying in reset after an early PCIe power-off */
    prph_set_bits_mask(APMG_PS_CTRL_REG, APMG_PS_EARLY_PWR_OFF_RESET_DIS,
                       ~APMG_PS_EARLY_PWR_OFF_RESET_DIS);
}

static int nic_init(void)
{
    int r = apm_init();
    if (r < 0)
        return r;
    prph_set_bits_mask(APMG_PS_CTRL_REG, 0, ~APMG_PS_PWR_SRC_MASK);  /* VMAIN */
    nic_config();
    r = rx_init();
    if (r < 0)
        return r;
    r = tx_init();
    if (r < 0)
        return r;
    /* Shadow registers: the RX and TX write pointers become plain writes,
       even with the card asleep */
    csr_set(CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFF);
    return access_failed ? IWL_ERR_IO : IWL_OK;
}

static int start_fw(const char *name, const struct fw_image *img,
                    int run_in_rfkill)
{
    if (!hw_up) {
        why("the transport is not started");
        return IWL_ERR_STATE;
    }
    if (prepare_card_hw() < 0)
        return IWL_ERR_IO;
    st.rfkill = rfkill_set();
    if (st.rfkill && !run_in_rfkill) {
        why("the radio is switched off (hardware RF-kill switch)");
        return IWL_ERR_RFKILL;
    }
    csr_wr(CSR_INT, 0xFFFFFFFF);
    int r = nic_init();
    if (r < 0)
        return r;
    /* The RF-kill handshake bits cleared, and once more for good
       measure, as the reference driver does */
    csr_wr(CSR_UCODE_DRV_GP1_CLR, GP1_SW_BIT_RFKILL);
    csr_wr(CSR_UCODE_DRV_GP1_CLR, GP1_CMD_BLOCKED);
    csr_wr(CSR_INT, 0xFFFFFFFF);
    csr_wr(CSR_UCODE_DRV_GP1_CLR, GP1_SW_BIT_RFKILL);
    csr_wr(CSR_UCODE_DRV_GP1_CLR, GP1_SW_BIT_RFKILL);
    memset(&alive, 0, sizeof(alive));
    memset(&crash, 0, sizeof(crash));
    fw_err = 0;
    return load_image(name, img);
}

/* ===================================================================
 * Firmware errors
 * =================================================================== */

static const char *error_name(uint32_t id)
{
    static const struct {
        uint32_t id;
        const char *name;
    } names[] = {
        { 0x34, "NMI_INTERRUPT_WDG" },
        { 0x35, "SYSASSERT" },
        { 0x37, "UCODE_VERSION_MISMATCH" },
        { 0x38, "BAD_COMMAND" },
        { 0x3C, "NMI_INTERRUPT_DATA_ACTION_PT" },
        { 0x3D, "FATAL_ERROR" },
        { 0x46, "NMI_TRM_HW_ERR" },
        { 0x4C, "NMI_INTERRUPT_TRM" },
        { 0x54, "NMI_INTERRUPT_BREAK_POINT" },
        { 0x5C, "NMI_INTERRUPT_WDG_RXF_FULL" },
        { 0x64, "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL" },
        { 0x66, "NMI_INTERRUPT_HOST" },
        { 0x7C, "NMI_INTERRUPT_ACTION_PT" },
        { 0x84, "NMI_INTERRUPT_UNKNOWN" },
        { 0x86, "NMI_INTERRUPT_INST_ACTION_PT" },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].id == id)
            return names[i].name;
    return "ADVANCED_SYSASSERT";
}

/* The firmware crashed (SW_ERR) or the DMA engine did (HW_ERR): read
   the error log while the card still holds it */
static void firmware_crashed(int hw_error)
{
    if (fw_err)
        return;
    fw_err = 1;
    why(hw_error ? "hardware (DMA) error reported by the card"
                 : "the firmware crashed");
    if (alive.errlog < ERRLOG_MIN_ADDR) {
        say("[wifi] firmware error, no valid error log pointer (0x%x)\n",
            alive.errlog);
        return;
    }
    uint32_t log[ERRLOG_DWORDS];
    if (sram_rd(alive.errlog, log, ERRLOG_DWORDS) < 0)
        return;
    crash.read = 1;
    crash.valid = log[0];
    crash.id = log[1];
    crash.pc = log[2];
    crash.blink1 = log[3];
    crash.blink2 = log[4];
    crash.ilink1 = log[5];
    crash.ilink2 = log[6];
    crash.data1 = log[7];
    crash.data2 = log[8];
    crash.data3 = log[9];
    crash.major = log[16];
    crash.minor = log[17];
    crash.hcmd = log[23];
    st.fw_error_id = crash.id;
    if (!crash.valid)
        say("[wifi] firmware error log at 0x%x is empty\n", alive.errlog);
    say("[wifi] %s image error log: 0x%08x %s, pc 0x%08x, blink1 0x%08x, "
        "blink2 0x%08x, ilink1 0x%08x, ilink2 0x%08x\n",
        cur_ucode == UCODE_INIT ? "INIT" : "runtime", crash.id, error_name(crash.id),
        crash.pc, crash.blink1, crash.blink2, crash.ilink1, crash.ilink2);
    say("[wifi] error log: data1 0x%08x, data2 0x%08x, data3 0x%08x, "
        "last command 0x%08x, firmware %u.%u\n", crash.data1, crash.data2,
        crash.data3, crash.hcmd, crash.major, crash.minor);
    if (alive.umac_errlog >= ERRLOG_MIN_ADDR) {
        uint32_t u[UMAC_ERRLOG_DWORDS];
        if (sram_rd(alive.umac_errlog, u, UMAC_ERRLOG_DWORDS) == 0 && u[0])
            say("[wifi] UMAC error log: 0x%08x, blink1 0x%08x, blink2 0x%08x, "
                "data1 0x%08x\n", u[1], u[2], u[3], u[6]);
    }
}

/* ===================================================================
 * Serving the card: interrupt causes and received packets
 * =================================================================== */

static void service(void)
{
    if (!regs || hw_gone || !hw_up)
        return;
    uint32_t inta = csr_rd(CSR_INT);
    if (inta == 0xFFFFFFFF || (inta & 0xFFFFFFF0) == 0xA5A5A5A0) {
        hw_gone = 1;
        why("the card stopped answering (CSR_INT reads 0x%08x)", inta);
        return;
    }
    if (inta) {
        csr_wr(CSR_INT, inta);
        if (inta & (INT_FH_RX | INT_SW_RX | INT_RX_PERIODIC))
            csr_wr(CSR_FH_INT_STATUS, FH_INT_RX_MASK);
        if (inta & INT_FH_TX)
            csr_wr(CSR_FH_INT_STATUS, FH_INT_TX_MASK);
        if (inta & (INT_SW_ERR | INT_HW_ERR))
            firmware_crashed((inta & INT_HW_ERR) != 0);
        if (inta & INT_RF_KILL)
            st.rfkill = rfkill_set();
        if (inta & INT_CT_KILL)
            say("[wifi] the card reports a critical temperature\n");
    }
    /* RX by the status block, not by the cause bit: the interrupt can
       come before the block is written, the block is what counts */
    rx_process();
}

static void handle_alive(const uint8_t *p, uint32_t len)
{
    alive.seen = 1;
    alive.len = len;
    if (len != 44 && len != 64 && len != 68)
        return;
    alive.status = get16(p);
    alive.errlog = get32(p + 20);
    alive.evlog = get32(p + 24);
    alive.scd_base = get32(p + 40);
    if (len == 64)
        alive.umac_errlog = get32(p + 56);
    else if (len == 68)
        alive.umac_errlog = get32(p + 60);
}

static void phydb_store(const uint8_t *p, uint32_t len);

static void handle_notification(uint32_t id, const uint8_t *p, uint32_t len,
                                uint16_t seq)
{
    switch (id) {
    case MVM_ALIVE:
        handle_alive(p, len);
        return;
    case INIT_COMPLETE_NOTIF:
        init_complete = 1;
        return;
    case CALIB_RES_NOTIF_PHY_DB:
        /* only the INIT image's results make the PHY DB */
        if (cur_ucode == UCODE_INIT)
            phydb_store(p, len);
        return;
    case REPLY_ERROR:
        if (len >= 8)
            say("[wifi] the firmware rejected command 0x%x (error type 0x%x, "
                "sequence 0x%x)\n", p[4], get32(p), get16(p + 6));
        return;
    case TX_CMD: {
        /* The TX response: frames up to the scheduler's SSN are done */
        uint32_t q = (seq >> 8) & 0x1F;
        if (len < 36 || q == CMD_QUEUE || q >= NUM_QUEUES)
            return;
        uint32_t at = 36 + 4 * (uint32_t)p[0];
        if (len >= at + 4)
            tx_reclaim((int)q, get32(p + at) & 0xFFF);
        break;
    }
    case CARD_STATE_NOTIFICATION:
        if (len >= 4 && (get32(p) & 0x0F))
            say("[wifi] card state: 0x%x\n", get32(p));
        return;
    }
    for (int i = 0; i < NOTIF_HANDLERS; i++)
        if (notif_handlers[i].fn && notif_handlers[i].id == id)
            notif_handlers[i].fn(id, p, len, seq);
}

static void handle_packet(const uint8_t *pkt, uint32_t len)
{
    uint8_t cmd = pkt[4];
    uint32_t id = cmd | ((uint32_t)pkt[5] << 8);
    uint16_t seq = get16(pkt + 6);
    /* The firmware side first (ALIVE and friends), then the transport:
       a response with bit 15 clear completes the command it answers,
       except TX responses, which point at a data queue */
    handle_notification(id, pkt + 8, len - 4, seq);
    if (!(seq & SEQ_RX_FRAME) && cmd != TX_CMD)
        hcmd_complete(pkt, len);
}

int iwl_notif_register(uint32_t id, iwl_notif_fn fn)
{
    for (int i = 0; i < NOTIF_HANDLERS; i++)
        if (!notif_handlers[i].fn || notif_handlers[i].id == id) {
            notif_handlers[i].id = id;
            notif_handlers[i].fn = fn;
            return IWL_OK;
        }
    return IWL_ERR_BUSY;
}

/* ===================================================================
 * The firmware file
 * =================================================================== */

static int add_section(struct fw_image *img, const uint8_t *d, uint32_t len)
{
    if (len < 4 || img->n >= FW_SECTIONS_MAX)
        return -1;
    img->sec[img->n].addr = get32(d);
    img->sec[img->n].data = d + 4;
    img->sec[img->n].len = len - 4;
    img->n++;
    return 0;
}

/* "<a>+<b>" - the sizes of the sections an image loads */
static void image_sizes(char *out, uint32_t cap, const struct fw_image *img)
{
    uint32_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < img->n; i++) {
        const struct fw_section *s = &img->sec[i];
        if (!s->len || s->addr == FW_SEP_CPU1_CPU2 || s->addr == FW_SEP_PAGING)
            break;
        fmt(out + n, cap - n, i ? "+%u" : "%u", s->len);
        n = strlen(out);
    }
}

/* The firmware file is not there at all - as against one that is
   there and wrong: the Device Manager says the first as "needs
   firmware", which is what a person can do something about */
static int fw_missing;

static int parse_firmware(void)
{
    uint32_t size;
    const uint8_t *d = initrd_find(FW_FILE, &size);
    if (!d) {
        fw_missing = 1;
        why("there is no %s in the initrd", FW_FILE);
        return IWL_ERR_BADFW;
    }
    fw_missing = 0;
    memset(&fw, 0, sizeof(fw));
    fw.file = d;
    fw.size = size;
    fw.n_cpu = 1;
    if (size < FW_HEADER_SIZE || get32(d) != 0 || get32(d + 4) != FW_MAGIC) {
        why("%s is not a TLV firmware file", FW_FILE);
        return IWL_ERR_BADFW;
    }
    uint32_t ver = get32(d + 72), build = get32(d + 76);
    fw.major = ver;
    fw.minor = build;
    fw.local = 0;

    uint32_t p = FW_HEADER_SIZE;
    while (size - p >= 8) {
        uint32_t type = get32(d + p), len = get32(d + p + 4);
        const uint8_t *v = d + p + 8;
        if (len > size - p - 8) {
            why("%s: TLV %u at offset %u runs past the end", FW_FILE, type, p);
            return IWL_ERR_BADFW;
        }
        int bad = 0;
        switch (type) {
        case TLV_SEC_RT:
            bad = add_section(&fw.rt, v, len);
            break;
        case TLV_SEC_INIT:
            bad = add_section(&fw.init, v, len);
            break;
        case TLV_FLAGS:
            bad = len < 4;
            if (!bad)
                fw.flags = get32(v);
            break;
        case TLV_API_CHANGES_SET:
            bad = len < 8;
            if (!bad && get32(v) == 0)
                fw.api0 = get32(v + 4);
            break;
        case TLV_ENABLED_CAPA:
            bad = len < 8;
            if (!bad && get32(v) == 0)
                fw.capa0 = get32(v + 4);
            break;
        case TLV_PHY_SKU:
            bad = len < 4;
            if (!bad)
                fw.phy_sku = get32(v);
            break;
        case TLV_DEF_CALIB:
            bad = len < 12 || get32(v) >= 4;
            if (!bad) {
                fw.calib_flow[get32(v)] = get32(v + 4);
                fw.calib_event[get32(v)] = get32(v + 8);
            }
            break;
        case TLV_N_SCAN_CHANNELS:
            bad = len < 4 || get32(v) < 1 || get32(v) > 64;
            if (!bad)
                fw.n_scan_channels = get32(v);
            break;
        case TLV_NUM_OF_CPU:
            bad = len < 4 || get32(v) < 1 || get32(v) > 2;
            if (!bad)
                fw.n_cpu = get32(v);
            break;
        case TLV_FW_VERSION:
            if (len >= 12) {
                fw.major = get32(v);
                fw.minor = get32(v + 4);
                fw.local = get32(v + 8);
            }
            break;
        }
        if (bad) {
            why("%s: TLV %u at offset %u is malformed", FW_FILE, type, p);
            return IWL_ERR_BADFW;
        }
        uint32_t step_len = 8 + ((len + 3) & ~3u);
        p = step_len > size - p ? size : p + step_len;
    }
    if (p != size) {
        why("%s: %u stray bytes at the end", FW_FILE, size - p);
        return IWL_ERR_BADFW;
    }
    uint32_t api = (fw.api0 & API_NEW_VERSION) ? ver : (ver >> 8) & 0xFF;
    if (api < UCODE_API_MIN || api > UCODE_API_MAX) {
        why("%s has API %u; this driver knows %u..%u", FW_FILE, api,
            (uint32_t)UCODE_API_MIN, (uint32_t)UCODE_API_MAX);
        return IWL_ERR_BADFW;
    }
    if (fw.n_cpu != 1) {
        why("%s is a dual-CPU image, which the 3160 does not use", FW_FILE);
        return IWL_ERR_BADFW;
    }
    if (!fw.init.n || !fw.rt.n || !fw.init.sec[0].len || !fw.rt.sec[0].len) {
        why("%s lacks the INIT or the runtime image", FW_FILE);
        return IWL_ERR_BADFW;
    }
    fmt(st.fw_version, sizeof(st.fw_version), "%u.%x.%u",
        fw.major, fw.minor, fw.local);
    char a[48], b[48];
    image_sizes(a, sizeof(a), &fw.init);
    image_sizes(b, sizeof(b), &fw.rt);
    say("[wifi] firmware %s: %u bytes, init %s B, runtime %s B\n",
        st.fw_version, size, a, b);
    return IWL_OK;
}

/* ===================================================================
 * Probe and DMA memory
 * =================================================================== */

static int find_card(struct pci_dev *dev)
{
    return pci_find_device(INTEL_VENDOR, 0x08B3, dev) ||
           pci_find_device(INTEL_VENDOR, 0x08B4, dev);
}

static uint32_t cfg_rd(int off)
{
    return pci_config_read(pdev.bus, pdev.slot, pdev.func, off);
}

static void cfg_wr(int off, uint32_t v)
{
    pci_config_write(pdev.bus, pdev.slot, pdev.func, off, v);
}

/* The PCI Express capability: whether ASPM L1 is on (link control)
   decides a bit in APM init. (LTR, which would add LTR_CONFIG, is left
   out: that command is optional and this firmware does not list it.) */
static void read_pcie_cap(void)
{
    if (!((cfg_rd(0x04) >> 16) & 0x10))
        return;
    int p = (int)(cfg_rd(0x34) & 0xFC);
    for (int n = 0; p && n < 48; n++) {
        uint32_t v = cfg_rd(p);
        if ((v & 0xFF) == 0x10) {
            aspm_l1 = (cfg_rd(p + 0x10) & 0x2) != 0;
            return;
        }
        p = (int)((v >> 8) & 0xFC);
    }
}

/* For the Device Manager: what became of the card. Asked each time,
   because the card starts in a thread of its own and can stop later
   (an error, the radio switch) */
static int device_state(char *out, int n)
{
    switch (st.stage) {
    case IWL_STAGE_READY:
        return DEV_WORKING;
    case IWL_STAGE_RFKILL:
        fmt(out, (uint32_t)n, "the radio switch is off");
        return DEV_WORKING;
    case IWL_STAGE_FAILED:
        if (fw_missing) {
            fmt(out, (uint32_t)n, "%s is not on the system", FW_FILE);
            return DEV_FIRMWARE;
        }
        fmt(out, (uint32_t)n, "%s", st.last_error);
        return DEV_FAILED;
    default:
        fmt(out, (uint32_t)n, "%s", iwlwifi_stage_name(st.stage));
        return DEV_STARTING;
    }
}

static int probe(void)
{
    if (!find_card(&pdev)) {
        st.stage = IWL_STAGE_NO_CARD;
        why("no Intel Wireless 3160 on the PCI bus");
        return IWL_ERR_NOCARD;
    }
    device_claim(&pdev, "iwlwifi");
    device_live(&pdev, device_state);
    st.bus = pdev.bus;
    st.slot = pdev.slot;
    st.func = pdev.func;
    st.device = pdev.device;
    if (pdev.bar[0] & 1) {
        why("BAR0 is not a memory window");
        return IWL_ERR_IO;
    }
    uint64_t addr = pdev.bar[0] & ~0xFULL;
    if (((pdev.bar[0] >> 1) & 0x3) == 0x2)
        addr |= (uint64_t)pdev.bar[1] << 32;
    if (!addr) {
        why("the firmware gave the card no address");
        return IWL_ERR_IO;
    }
    pci_enable_bus_master(&pdev);       /* memory decoding and DMA */
    if (MAP_PHYS(addr, BAR_SIZE) < 0) {
        why("could not map the card's registers");
        return IWL_ERR_IO;
    }
    regs = (volatile uint8_t *)(uintptr_t)addr;
    /* PCI retry timeout 0: keeps PCI TX retries from interfering with
       the processor's C3 state. A byte register, written in its dword. */
    cfg_wr(0x40, cfg_rd(0x40) & ~0x0000FF00u);
    disable_interrupts();
    /* Polled: INTx off, no MSI. (The status half is written as 0, which
       clears nothing - its bits are write-1-to-clear.) */
    cfg_wr(0x04, (cfg_rd(0x04) & 0xFFFF) | 0x400);
    st.hw_rev = csr_rd(CSR_HW_REV);
    if (st.hw_rev == 0xFFFFFFFF) {
        hw_gone = 1;
        why("the card does not answer (HW_REV reads 0xffffffff)");
        return IWL_ERR_GONE;
    }
    read_pcie_cap();
    say("[wifi] Intel Wireless 3160 at %02x:%02x.%x, hw rev 0x%x\n",
        pdev.bus, pdev.slot, pdev.func, st.hw_rev);
    st.stage = IWL_STAGE_PROBED;
    return IWL_OK;
}

static int alloc_dma(void)
{
    kw_buf = dma_alloc(PAGE_SIZE);
    bc_tables = dma_alloc(NUM_QUEUES * BC_TABLE_STRIDE);
    rx_bd = dma_alloc(RX_QUEUE_SIZE * 4);
    rx_status = dma_alloc(16);
    cmd_bufs = dma_alloc(CMD_SLOTS * CMD_SLOT_SIZE);
    cmd_big = dma_alloc(CMD_BIG_SIZE);
    if (!kw_buf || !bc_tables || !rx_bd || !rx_status || !cmd_bufs || !cmd_big)
        return IWL_ERR_NOMEM;
    fw_chunk_max = FH_CHUNK_MAX;
    fw_bounce = dma_alloc(FH_CHUNK_MAX);
    if (!fw_bounce) {
        /* a page at a time is slower, but loads the same */
        fw_chunk_max = PAGE_SIZE;
        fw_bounce = dma_alloc(PAGE_SIZE);
        if (!fw_bounce)
            return IWL_ERR_NOMEM;
    }
    for (int i = 0; i < RX_QUEUE_SIZE; i++)
        if (!(rx_buf[i] = dma_alloc(RX_BUF_SIZE)))
            return IWL_ERR_NOMEM;
    for (int q = 0; q < NUM_QUEUES; q++) {
        txq[q].tfds = dma_alloc(TFD_QUEUE_SIZE * TFD_SIZE);
        /* 256 first-TB areas for every queue, the command queue too:
           they are indexed by the ring index */
        txq[q].scratch = dma_alloc(TFD_QUEUE_SIZE * SCRATCH_SIZE);
        if (!txq[q].tfds || !txq[q].scratch)
            return IWL_ERR_NOMEM;
    }
    return IWL_OK;
}

/* ===================================================================
 * Firmware side: the PHY DB
 * =================================================================== */

static struct phydb_entry *phydb_slot(uint32_t type, const uint8_t *data,
                                      uint32_t len)
{
    switch (type) {
    case PHY_DB_CFG:
        return &phydb_cfg;
    case PHY_DB_CALIB_NCH:
        return &phydb_nch;
    case PHY_DB_CALIB_CHG_PAPD:
    case PHY_DB_CALIB_CHG_TXP: {
        if (len < 2)
            return 0;
        uint32_t group = get16(data);
        if (group >= PHY_DB_GROUPS)
            return 0;
        return type == PHY_DB_CALIB_CHG_PAPD ? &phydb_papd[group]
                                             : &phydb_txp[group];
    }
    }
    return 0;
}

static void phydb_reset(void)
{
    phydb_used = 0;
    memset(&phydb_cfg, 0, sizeof(phydb_cfg));
    memset(&phydb_nch, 0, sizeof(phydb_nch));
    memset(phydb_papd, 0, sizeof(phydb_papd));
    memset(phydb_txp, 0, sizeof(phydb_txp));
    st.calib_results = 0;
}

/* One calibration result from the INIT image, kept whole (the group id
   at its start included) to be sent back as it came */
static void phydb_store(const uint8_t *p, uint32_t len)
{
    if (len < 4)
        return;
    uint32_t type = get16(p), dlen = get16(p + 2);
    const uint8_t *data = p + 4;
    if (dlen > len - 4) {
        say("[wifi] calibration result of type %u is cut short\n", type);
        return;
    }
    struct phydb_entry *e = phydb_slot(type, data, dlen);
    if (!e) {
        say("[wifi] calibration result of type %u ignored\n", type);
        return;
    }
    /* A later result for the same slot replaces the earlier one; in
       place when it fits */
    if (!e->present || dlen > e->len) {
        if (phydb_used + dlen > PHY_DB_POOL_SIZE) {
            say("[wifi] calibration results exceed %u bytes; one dropped\n",
                (uint32_t)PHY_DB_POOL_SIZE);
            return;
        }
        e->off = phydb_used;
        phydb_used += dlen;
    }
    memcpy(phydb_pool + e->off, data, dlen);
    e->len = (uint16_t)dlen;
    e->present = 1;
    st.calib_results++;
}

static int phydb_send_one(uint32_t type, const struct phydb_entry *e)
{
    uint8_t head[4];
    put16(head, type);
    put16(head + 2, e->present ? e->len : 0);
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = PHY_DB_CMD;
    c.data[0] = head;
    c.len[0] = 4;
    c.data[1] = phydb_pool + e->off;
    c.len[1] = e->present ? e->len : 0;
    return iwl_send_cmd(&c);
}

/* To the runtime image, in this order: configuration, non-channel
   calibration, then the per-channel-group ones */
static int phydb_send_all(void)
{
    int r = phydb_send_one(PHY_DB_CFG, &phydb_cfg);
    if (r < 0)
        return r;
    if ((r = phydb_send_one(PHY_DB_CALIB_NCH, &phydb_nch)) < 0)
        return r;
    for (int i = 0; i < PHY_DB_GROUPS; i++)
        if (phydb_papd[i].present && phydb_papd[i].len &&
            (r = phydb_send_one(PHY_DB_CALIB_CHG_PAPD, &phydb_papd[i])) < 0)
            return r;
    for (int i = 0; i < PHY_DB_GROUPS; i++)
        if (phydb_txp[i].present && phydb_txp[i].len &&
            (r = phydb_send_one(PHY_DB_CALIB_CHG_TXP, &phydb_txp[i])) < 0)
            return r;
    return IWL_OK;
}

/* ===================================================================
 * Firmware side: the NVM
 * =================================================================== */

/* Bytes read, 0 at the end of a section, IWL_ERR_REJECTED when the
   section is not there; any other error is the firmware's or the
   card's and ends the read */
static int nvm_read_chunk(uint32_t section, uint32_t offset, uint8_t *out)
{
    uint8_t req[8];
    req[0] = 0;                 /* read */
    req[1] = 0;                 /* target: the cache */
    put16(req + 2, section);
    put16(req + 4, offset);
    put16(req + 6, NVM_CHUNK);
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = NVM_ACCESS_CMD;
    c.data[0] = req;
    c.len[0] = sizeof(req);
    c.flags = IWL_CMD_WANT_RESP | IWL_CMD_SEND_IN_RFKILL;
    c.resp = nvm_resp;
    c.resp_cap = sizeof(nvm_resp);
    int r = iwl_send_cmd(&c);
    if (r < 0)
        return r;
    if (c.resp_len < 8)
        return IWL_ERR_REJECTED;
    uint32_t r_off = get16(nvm_resp), r_len = get16(nvm_resp + 2);
    uint32_t status = get16(nvm_resp + 6);
    if (status) {
        /* past the end of a section that filled whole chunks */
        if (offset && status == 1)
            return 0;
        return IWL_ERR_REJECTED;
    }
    if (r_off != offset || r_len > NVM_CHUNK || 8 + r_len > c.resp_len ||
        8 + r_len > sizeof(nvm_resp))
        return IWL_ERR_REJECTED;
    memcpy(out, nvm_resp + 8, r_len);
    return (int)r_len;
}

static int nvm_read(void)
{
    uint32_t size_read = 0;
    memset(nvm_sec, 0, sizeof(nvm_sec));
    for (uint32_t s = 0; s < NVM_SECTIONS; s++) {
        uint32_t off = 0;
        int ret = NVM_CHUNK, absent = 0;
        while (ret == NVM_CHUNK) {
            if (size_read + off + NVM_CHUNK > NVM_BUF_SIZE) {
                absent = 1;
                break;
            }
            ret = nvm_read_chunk(s, off, nvm_buf + size_read + off);
            if (ret == IWL_ERR_REJECTED) {
                absent = 1;
                break;
            }
            if (ret < 0) {
                why("reading NVM section %u at offset %u failed", s, off);
                return ret;
            }
            off += (uint32_t)ret;
        }
        if (absent)
            continue;
        nvm_sec[s].off = size_read;
        nvm_sec[s].len = off;
        nvm_sec[s].present = 1;
        size_read += off;
    }
    if (!size_read)
        say("[wifi] the NVM is blank\n");
    return IWL_OK;
}

static const uint8_t nvm_channel_list[IWLWIFI_MAX_CHANNELS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};

static uint16_t channel_flags(int idx, uint32_t ch, uint16_t nf)
{
    uint16_t f = 0;
    if (nf & NVM_CH_40MHZ) {
        if (idx < 14) {
            if (ch <= 9)
                f |= IWLWIFI_CHAN_HT40_PLUS;
            if (ch >= 5)
                f |= IWLWIFI_CHAN_HT40_MINUS;
        } else if (ch <= 165) {
            f |= ((idx - 14) % 2 == 0) ? IWLWIFI_CHAN_HT40_PLUS
                                       : IWLWIFI_CHAN_HT40_MINUS;
        }
    }
    if (!(nf & NVM_CH_80MHZ))
        f |= IWLWIFI_CHAN_NO_80MHZ;
    if (!(nf & NVM_CH_160MHZ))
        f |= IWLWIFI_CHAN_NO_160MHZ;
    if (!(nf & NVM_CH_IBSS) || !(nf & NVM_CH_ACTIVE))
        f |= IWLWIFI_CHAN_NO_IR;
    if (nf & NVM_CH_RADAR)
        f |= IWLWIFI_CHAN_RADAR;
    if (nf & NVM_CH_INDOOR_ONLY)
        f |= IWLWIFI_CHAN_INDOOR;
    if ((nf & NVM_CH_GO_CONCURRENT) && (f & IWLWIFI_CHAN_NO_IR))
        f |= IWLWIFI_CHAN_IR_CONCURRENT;
    return f;
}

static int nvm_parse(void)
{
    if (!nvm_sec[NVM_SEC_HW].present || !nvm_sec[NVM_SEC_HW].len ||
        !nvm_sec[NVM_SEC_SW].present || !nvm_sec[NVM_SEC_SW].len) {
        why("the NVM's HW or SW section is missing or empty");
        return IWL_ERR_REJECTED;
    }
    if (!nvm_sec[NVM_SEC_CALIB].present) {
        why("the NVM's calibration section is missing");
        return IWL_ERR_REJECTED;
    }
    const uint8_t *hw = nvm_buf + nvm_sec[NVM_SEC_HW].off;
    const uint8_t *sw = nvm_buf + nvm_sec[NVM_SEC_SW].off;
    if (nvm_sec[NVM_SEC_HW].len < NVM_HW_ADDR_OFF + 6 ||
        nvm_sec[NVM_SEC_SW].len < NVM_SW_CHANNELS_OFF + 2 * IWLWIFI_MAX_CHANNELS) {
        why("the NVM's HW or SW section is too short");
        return IWL_ERR_REJECTED;
    }
    /* The address is stored with its bytes swapped in pairs */
    const uint8_t *a = hw + NVM_HW_ADDR_OFF;
    st.mac[0] = a[1];
    st.mac[1] = a[0];
    st.mac[2] = a[3];
    st.mac[3] = a[2];
    st.mac[4] = a[5];
    st.mac[5] = a[4];
    st.mac_valid = 1;
    st.nvm_version = get16(sw);
    st.radio_cfg = get16(sw + 2);
    st.sku = get16(sw + 4);

    int n = 0, n24 = 0, n5 = 0;
    for (int i = 0; i < IWLWIFI_MAX_CHANNELS; i++) {
        uint16_t nf = get16(sw + NVM_SW_CHANNELS_OFF + 2 * i);
        if (i >= 14 && !(st.sku & NVM_SKU_BAND_52))
            continue;
        if (!(nf & NVM_CH_VALID))
            continue;
        uint32_t ch = nvm_channel_list[i];
        struct iwlwifi_channel *c = &channels[n++];
        c->number = (uint16_t)ch;
        if (i < 14) {
            c->band = IWLWIFI_BAND_24;
            c->freq = (uint16_t)(ch == 14 ? 2484 : 2407 + 5 * ch);
            n24++;
        } else {
            c->band = IWLWIFI_BAND_5;
            c->freq = (uint16_t)(5000 + 5 * ch);
            n5++;
        }
        c->max_power = DEFAULT_MAX_TX_POWER;
        c->nvm_flags = nf;
        c->flags = channel_flags(i, ch, nf);
    }
    st.n_channels = n;
    st.n_24ghz = n24;
    st.n_5ghz = n5;
    return IWL_OK;
}

/* ===================================================================
 * Firmware side: waits and commands
 * =================================================================== */

static int wait_alive(void)
{
    struct waiter w;
    wait_begin(&w, ALIVE_TIMEOUT_MS);
    do {
        service();
        if (fw_err)
            return IWL_ERR_FW;
        if (hw_gone)
            return IWL_ERR_GONE;
        if (alive.seen)
            break;
    } while (wait_more(&w));
    if (!alive.seen) {
        why("no ALIVE from the firmware within %u ms", (uint32_t)ALIVE_TIMEOUT_MS);
        return IWL_ERR_TIMEOUT;
    }
    if (alive.len != 44 && alive.len != 64 && alive.len != 68) {
        why("ALIVE of an unknown length (%u bytes)", alive.len);
        return IWL_ERR_FW;
    }
    if (alive.status != ALIVE_STATUS_OK) {
        why("the loaded firmware is not valid (ALIVE status 0x%x)", alive.status);
        return IWL_ERR_FW;
    }
    return IWL_OK;
}

static uint32_t valid_tx_ant(void)
{
    return (fw.phy_sku >> 16) & 0xF;
}

static uint32_t valid_rx_ant(void)
{
    return (fw.phy_sku >> 20) & 0xF;
}

static int send_u32(uint32_t id, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return iwl_send_cmd_pdu(id, b, 4, 0);
}

static int send_bt_config(void)
{
    uint8_t b[8];
    put32(b, 1);                /* BT_COEX_NW: the antenna is shared */
    put32(b + 4, 0x17);         /* MPLUT, MPLUT boost, SYNC2SCO, high band */
    return iwl_send_cmd_pdu(BT_CONFIG, b, sizeof(b), 0);
}

static int send_phy_cfg(int ucode)
{
    uint8_t b[12];
    /* The NVM of a 7000-family card gives no antenna masks, so the
       firmware's own are the ones in force */
    put32(b, fw.phy_sku);
    put32(b + 4, fw.calib_flow[ucode]);
    put32(b + 8, fw.calib_event[ucode]);
    return iwl_send_cmd_pdu(PHY_CONFIGURATION_CMD, b, sizeof(b), 0);
}

/* Smart FIFO off while nothing is associated. The values are what the
   reference driver sends for this firmware: with its API read as 0 it
   takes the SF_INIT_OFF state and the "BSS" table of full-on timers. */
static int send_sf_cfg(void)
{
    static const uint32_t full_on[5][2] = {
        { 2016, 320 }, { 2016, 320 }, { 10016, 2016 }, { 2016, 320 },
        { 2016, 320 },
    };
    uint8_t b[92];
    memset(b, 0, sizeof(b));
    put32(b, 3);                /* SF_INIT_OFF */
    put32(b + 4, 4096);
    put32(b + 8, 8192);
    for (int i = 0; i < 10; i++)
        put32(b + 12 + 4 * i, 1000000);
    for (int i = 0; i < 5; i++) {
        put32(b + 52 + 8 * i, full_on[i][0]);
        put32(b + 52 + 8 * i + 4, full_on[i][1]);
    }
    return iwl_send_cmd_pdu(REPLY_SF_CFG_CMD, b, sizeof(b), IWL_CMD_ASYNC);
}

static int send_scd_queue_cfg(int q, int enable, int fifo, int sta_id,
                              int tid)
{
    uint8_t b[12];
    memset(b, 0, sizeof(b));
    b[1] = (uint8_t)sta_id;
    b[2] = (uint8_t)tid;
    b[3] = (uint8_t)q;
    b[4] = (uint8_t)enable;
    b[6] = (uint8_t)fifo;
    b[7] = FRAME_LIMIT;
    return iwl_send_cmd_pdu(SCD_QUEUE_CFG, b, sizeof(b), 0);
}

int iwl_txq_enable(int queue, int fifo, int sta_id, int tid)
{
    if (queue < 0 || queue >= NUM_QUEUES || queue == CMD_QUEUE)
        return IWL_ERR_STATE;
    txq_enable_transport(queue, 0);
    int r = send_scd_queue_cfg(queue, 1, fifo, sta_id, tid);
    if (r < 0)
        txq[queue].active = 0;
    return r;
}

int iwl_txq_disable(int queue)
{
    if (queue < 0 || queue >= NUM_QUEUES || queue == CMD_QUEUE ||
        !txq[queue].active)
        return IWL_ERR_STATE;
    int r = send_scd_queue_cfg(queue, 0, 0, 0, 0);
    txq[queue].active = 0;
    memset(txq[queue].tfds, 0, TFD_QUEUE_SIZE * TFD_SIZE);
    txq[queue].read = txq[queue].write = 0;
    return r;
}

/* The auxiliary station: what the firmware sends scan probes as */
static int add_aux_sta(void)
{
    int r = iwl_txq_enable(AUX_QUEUE, AUX_FIFO, 0, MAX_TID_COUNT);
    if (r < 0)
        return r;
    uint8_t b[44], resp[16];
    memset(b, 0, sizeof(b));
    put32(b + 4, AUX_MAC_INDEX);                /* mac_id_n_color */
    b[16] = AUX_STA_ID;
    put32(b + 40, 1u << AUX_QUEUE);             /* tfd_queue_msk */
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = ADD_STA;
    c.data[0] = b;
    c.len[0] = sizeof(b);
    c.flags = IWL_CMD_WANT_RESP;
    c.resp = resp;
    c.resp_cap = sizeof(resp);
    r = iwl_send_cmd(&c);
    if (r < 0)
        return r;
    if (c.resp_len != 4 || (get32(resp) & 0xFF) != ADD_STA_SUCCESS) {
        why("ADD_STA for the aux station answered %u bytes, status 0x%x",
            c.resp_len, c.resp_len >= 4 ? get32(resp) : 0);
        return IWL_ERR_REJECTED;
    }
    return IWL_OK;
}

static int add_phy_contexts(void)
{
    /* On the first 2.4 GHz channel the NVM allows (the contexts move to
       a real channel later) */
    const struct iwlwifi_channel *ch = 0;
    for (int i = 0; i < st.n_channels && !ch; i++)
        if (channels[i].band == IWLWIFI_BAND_24)
            ch = &channels[i];
    if (!ch && st.n_channels)
        ch = &channels[0];
    if (!ch) {
        why("the NVM lists no channel at all");
        return IWL_ERR_REJECTED;
    }
    for (uint32_t i = 0; i < NUM_PHY_CTX; i++) {
        uint8_t b[36];
        memset(b, 0, sizeof(b));
        put32(b, i);                            /* id, color 0 */
        put32(b + 4, FW_CTXT_ACTION_ADD);
        b[16] = ch->band == IWLWIFI_BAND_24 ? PHY_BAND_24 : PHY_BAND_5;
        b[17] = (uint8_t)ch->number;
        b[18] = 0;                              /* 20 MHz */
        b[19] = 0;
        put32(b + 20, valid_tx_ant());
        /* one RX antenna: no diversity, both counts 1 */
        put32(b + 24, (valid_rx_ant() << 1) | (1u << 10) | (1u << 12));
        int r = iwl_send_cmd_pdu(PHY_CONTEXT_CMD, b, sizeof(b), 0);
        if (r < 0)
            return r;
    }
    return IWL_OK;
}

static int send_power_table(void)
{
    uint8_t b[4];
    put16(b, 0x0001);           /* power save allowed (the default scheme) */
    put16(b + 2, 0);
    return iwl_send_cmd_pdu(POWER_TABLE_CMD, b, sizeof(b), 0);
}

/* ===================================================================
 * Bring-up
 * =================================================================== */

#define STEP(name, expr) do {                           \
        step = (name);                                  \
        int r_ = (expr);                                \
        if (r_ < 0)                                     \
            return r_;                                  \
    } while (0)

/* The INIT image: NVM, calibration, the PHY DB */
static int run_init(void)
{
    phydb_reset();
    init_complete = 0;
    cur_ucode = UCODE_INIT;

    STEP("loading the INIT image", start_fw("INIT", &fw.init, 1));
    STEP("starting the INIT image", wait_alive());
    say("[wifi] INIT image alive (status 0x%x)\n", alive.status);
    st.stage = IWL_STAGE_INIT_ALIVE;
    STEP("starting the command queue (INIT)", tx_start());
    fw_running = 1;

    /* With the radio switched off, the command is dropped, not failed */
    step = "BT_CONFIG (INIT)";
    int r = send_bt_config();
    if (r < 0 && r != IWL_ERR_RFKILL)
        return r;
    STEP("reading the NVM", nvm_read());
    STEP("parsing the NVM", nvm_parse());
    char mac[18];
    mac_text(mac, st.mac);
    say("[wifi] NVM: MAC %s, %d channels (2.4 GHz %d, 5 GHz %d)\n", mac,
        st.n_channels, st.n_24ghz, st.n_5ghz);
    if (st.nvm_version != NVM_VERSION_3160)
        say("[wifi] NVM version 0x%x (the 3160's is 0x%x); going on\n",
            st.nvm_version, (uint32_t)NVM_VERSION_3160);
    st.stage = IWL_STAGE_NVM_READ;

    if (rfkill_set()) {
        st.rfkill = 1;
        step = "calibration (INIT)";
        why("the radio is switched off (hardware RF-kill switch)");
        return IWL_ERR_RFKILL;
    }
    /* The antennas first: calibration runs on them */
    STEP("TX_ANT_CONFIGURATION (INIT)",
         send_u32(TX_ANT_CONFIGURATION_CMD, valid_tx_ant()));
    STEP("PHY_CONFIGURATION (INIT)", send_phy_cfg(UCODE_INIT));

    step = "calibration (INIT)";
    struct waiter w;
    wait_begin(&w, CALIB_TIMEOUT_MS);
    do {
        service();
        if (fw_err)
            return IWL_ERR_FW;
        if (hw_gone)
            return IWL_ERR_GONE;
        if (init_complete)
            break;
        if (rfkill_set()) {
            st.rfkill = 1;
            why("the radio was switched off while calibrating");
            return IWL_ERR_RFKILL;
        }
    } while (wait_more(&w));
    if (!init_complete) {
        if (rfkill_set()) {
            st.rfkill = 1;
            why("the radio was switched off while calibrating");
            return IWL_ERR_RFKILL;
        }
        why("INIT_COMPLETE did not come within %u ms", (uint32_t)CALIB_TIMEOUT_MS);
        return IWL_ERR_TIMEOUT;
    }
    say("[wifi] calibration done: %d results\n", st.calib_results);
    st.stage = IWL_STAGE_CALIBRATED;
    return IWL_OK;
}

/* The runtime image, configured to idle: the "minimal safe" set, in
   the reference driver's order */
static int run_runtime(void)
{
    cur_ucode = UCODE_REGULAR;
    STEP("loading the runtime image", start_fw("runtime", &fw.rt, 0));
    STEP("starting the runtime image", wait_alive());
    say("[wifi] runtime image alive\n");
    st.stage = IWL_STAGE_RT_ALIVE;
    STEP("starting the command queue (runtime)", tx_start());
    fw_running = 1;

    STEP("SF_CFG", send_sf_cfg());
    STEP("TX_ANT_CONFIGURATION", send_u32(TX_ANT_CONFIGURATION_CMD, valid_tx_ant()));
    STEP("BT_CONFIG", send_bt_config());
    /* The calibration results before PHY_CONFIGURATION */
    STEP("sending the PHY DB", phydb_send_all());
    STEP("PHY_CONFIGURATION", send_phy_cfg(UCODE_REGULAR));
    STEP("adding the aux station", add_aux_sta());
    STEP("adding the PHY contexts", add_phy_contexts());
    STEP("POWER_TABLE", send_power_table());
    /* An async command may still be in the queue; let it drain, so that
       the card is truly idle and a late failure lands here */
    step = "draining the command queue";
    struct waiter w;
    wait_begin(&w, CMD_TIMEOUT_MS);
    do {
        service();
        if (fw_err)
            return IWL_ERR_FW;
        if (hw_gone)
            return IWL_ERR_GONE;
        if (txq[CMD_QUEUE].read == txq[CMD_QUEUE].write)
            break;
    } while (wait_more(&w));
    if (txq[CMD_QUEUE].read != txq[CMD_QUEUE].write) {
        why("commands still unanswered after %u ms", (uint32_t)CMD_TIMEOUT_MS);
        return IWL_ERR_TIMEOUT;
    }
    st.stage = IWL_STAGE_READY;
    return IWL_OK;
}

static int bringup(void)
{
    STEP("powering the card up (INIT)", start_hw());
    int r = run_init();
    if (r < 0)
        return r;
    /* INIT done: the card stopped and started again, then the runtime
       image */
    stop_device();
    STEP("powering the card up (runtime)", start_hw());
    return run_runtime();
}

/* "[ !! ] Wi-Fi: <step>: <reason>[; firmware error ...]" - once */
static void report_failure(int err)
{
    char *e = st.last_error;
    uint32_t cap = sizeof(st.last_error);
    const char *what = reason[0] ? reason :
                       err == IWL_ERR_TIMEOUT ? "timed out" :
                       err == IWL_ERR_FW ? "the firmware crashed" :
                       err == IWL_ERR_NOMEM ? "out of memory for DMA structures" :
                       err == IWL_ERR_RFKILL ?
                           "the radio is switched off (hardware RF-kill switch)" :
                       "failed";
    if (step[0])
        fmt(e, cap, "%s: %s", step, what);
    else
        fmt(e, cap, "%s", what);
    if (crash.read) {
        uint32_t n = strlen(e);
        fmt(e + n, cap - n, "; firmware error 0x%x %s, pc 0x%x, blink1 0x%x, "
            "blink2 0x%x", crash.id, error_name(crash.id), crash.pc,
            crash.blink1, crash.blink2);
    }
    st.stage = err == IWL_ERR_RFKILL ? IWL_STAGE_RFKILL : IWL_STAGE_FAILED;
    say("[ !! ] Wi-Fi: %s\n", e);
}

int iwlwifi_run(void)
{
    reason[0] = '\0';
    step = "";
    st.last_error[0] = '\0';
    st.fw_error_id = 0;
    int r = IWL_OK;
    if (!probed) {
        step = "finding the card";
        r = probe();
        if (r == IWL_ERR_NOCARD)
            return r;
        if (r == IWL_OK)
            probed = 1;
    }
    if (r == IWL_OK && !fw_parsed) {
        step = "reading the firmware file";
        r = parse_firmware();
        if (r == IWL_OK) {
            fw_parsed = 1;
            st.stage = IWL_STAGE_FW_LOADED;
        }
    }
    if (r == IWL_OK && !dma_ready) {
        step = "allocating DMA memory";
        r = alloc_dma();
        if (r == IWL_OK)
            dma_ready = 1;
        else
            why("out of memory for DMA structures");
    }
    if (r == IWL_OK)
        r = bringup();
    if (r < 0) {
        report_failure(r);
        stop_device();
        return r;
    }
    char mac[18];
    mac_text(mac, st.mac);
    say("[ OK ] Wi-Fi: Intel Wireless 3160, MAC %s, firmware ready\n", mac);
    return IWL_OK;
}

/* The running card: notifications taken in, the switch and the
   firmware watched */
static void link_service(void);

void iwlwifi_poll(void)
{
    if (st.stage != IWL_STAGE_READY)
        return;
    reason[0] = '\0';
    step = "running";
    service();
    int err = 0;
    if (hw_gone) {
        err = IWL_ERR_GONE;
    } else if (fw_err) {
        err = IWL_ERR_FW;
    } else if (rfkill_set()) {
        st.rfkill = 1;
        why("the radio was switched off (hardware RF-kill switch)");
        err = IWL_ERR_RFKILL;
    } else {
        struct txq *t = &txq[CMD_QUEUE];
        if (t->read != t->write &&
            (pit_ticks() - t->stamp) * 10 > CMDQ_WATCHDOG_MS) {
            why("the command queue is stuck (no answer for %u ms)",
                (uint32_t)CMDQ_WATCHDOG_MS);
            force_nmi();
            err = IWL_ERR_TIMEOUT;
        }
    }
    if (err) {
        report_failure(err);
        stop_device();
        return;
    }
    link_service();
}

/* ===================================================================
 * Stage 2: scanning
 *
 * This firmware (API 17) scans the old way, "LMAC": the capability for
 * the newer UMAC scan is not in its TLVs. One host command,
 * SCAN_OFFLOAD_REQUEST_CMD, carries everything - the dwell times, the
 * channels, the probe request to send on them - and the firmware then
 * does the whole sweep by itself. What it hears comes in as received
 * frames, each a pair of packets: REPLY_RX_PHY_CMD (how it was heard:
 * channel, signal) and REPLY_RX_MPDU_CMD (the 802.11 frame itself).
 * SCAN_OFFLOAD_COMPLETE ends it. The command is filled the way Linux's
 * iwlmvm fills it for a regular scan with this firmware: active, the
 * wildcard SSID, every beacon passed up, extended dwell on 1, 6 and 11.
 * =================================================================== */

#define SCAN_OFFLOAD_REQUEST_CMD 0x51
#define SCAN_OFFLOAD_COMPLETE    0x6D
#define REPLY_RX_PHY_CMD         0xC0
#define REPLY_RX_MPDU_CMD        0xC1

#define SCAN_FIXED        764   /* SCAN_REQUEST_FIXED_PART_API_S_VER_7 + the rest */
#define SCAN_CHAN_CFG      12   /* SCAN_CHANNEL_CFG_S_VER_1 */
#define SCAN_PREQ         528   /* PROBE_REQUEST_FRAME_API_S_VER_2 */
#define SCAN_MAX_CHANNELS  64
#define SCAN_FLAG_PASS_ALL       0x01
#define SCAN_FLAG_EXTENDED_DWELL 0x80
#define SCAN_CHANNEL_PARTIAL     (1u << 28)
#define SCAN_DONE_MS      15000

static uint8_t scan_cmd[SCAN_FIXED + SCAN_CHAN_CFG * SCAN_MAX_CHANNELS +
                        SCAN_PREQ];
static struct iwlwifi_bss bss[IWLWIFI_MAX_BSS];
static int n_bss;
static volatile int scan_running, scan_status;
static int bss_found = -1;          /* after a scan; -1 = none yet */
static int bss_recorded;            /* the list is the self-test's */
/* The PHY half of the frame being received: where and how loud */
static uint16_t rx_channel;
static int rx_signal;
static uint32_t rx_device_ts;
/* What each network's beacon said, beside bss[] (stage 3 joins from it) */
static struct wlan_bss_info bss_info[IWLWIFI_MAX_BSS];
static uint32_t bss_seen_ts[IWLWIFI_MAX_BSS];
static void link_rx(const uint8_t *f, uint32_t n, uint32_t status);

static uint32_t scan_cmd_size(void)
{
    return SCAN_FIXED + SCAN_CHAN_CFG * fw.n_scan_channels + SCAN_PREQ;
}

static uint32_t scan_build(void)
{
    uint8_t *c = scan_cmd;
    uint32_t size = scan_cmd_size();
    memset(c, 0, size);
    uint32_t n = (uint32_t)st.n_channels < fw.n_scan_channels
                     ? (uint32_t)st.n_channels : fw.n_scan_channels;
    c[4] = (uint8_t)n;
    c[5] = 10;                          /* active dwell, TU */
    c[6] = 110;                         /* passive */
    c[7] = 44;                          /* fragmented */
    c[8] = 90;                          /* extended: channels 1, 6, 11 */
    uint32_t rx = valid_rx_ant(), tx = valid_tx_ant();
    put16(c + 10, (uint16_t)((rx << 1) | (rx << 7) | (rx << 4) | 1));
    put32(c + 12, SCAN_FLAG_PASS_ALL | SCAN_FLAG_EXTENDED_DWELL);
    /* max_out_time and suspend_time 0: not associated */
    put32(c + 24, channels[0].band == IWLWIFI_BAND_24 ? 1 : 0);
    put32(c + 28, (1u << 2) | (1u << 6));  /* ACCEPT_GRP, IN_BEACON */
    /* The probe request's TX command, per band: no CCK on 5 GHz */
    uint32_t ant = (tx & -tx) << 14;    /* the first antenna there is */
    put32(c + 32, (1u << 12) | (1u << 13));      /* BT_DIS, SEQ_CTL */
    put32(c + 36, 10 | (1u << 9) | ant);         /* 1 Mb/s CCK */
    c[40] = AUX_STA_ID;
    put32(c + 44, (1u << 12) | (1u << 13));
    put32(c + 48, 13 | ant);                     /* 6 Mb/s OFDM */
    c[52] = AUX_STA_ID;
    put32(c + 736, 6);                  /* scan priority EXT_6 */
    put32(c + 740, 1);                  /* one iteration */
    c[750] = 1;                         /* schedule[0]: once, */
    c[751] = 1;                         /* ... every channel */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *ch = c + SCAN_FIXED + SCAN_CHAN_CFG * i;
        put32(ch, SCAN_CHANNEL_PARTIAL);
        put16(ch + 4, channels[i].number);
        put16(ch + 6, 1);
    }
    /* The probe request: a header and the wildcard SSID, then what is
       said per band - the rates, and on 2.4 GHz a DS parameter set the
       firmware fills with the channel - and nothing common */
    uint8_t *pr = c + SCAN_FIXED + SCAN_CHAN_CFG * fw.n_scan_channels;
    uint8_t *f = pr + 16;
    f[0] = 0x40;                        /* management, probe request */
    memset(f + 4, 0xFF, 6);             /* to everybody */
    memcpy(f + 10, st.mac, 6);
    memset(f + 16, 0xFF, 6);            /* any BSS */
    f[24] = 0;                          /* SSID, empty: any */
    f[25] = 0;
    static const uint8_t band24[] = {
        1, 8, 0x02, 0x04, 0x0B, 0x16, 0x0C, 0x12, 0x18, 0x24,
        50, 4, 0x30, 0x48, 0x60, 0x6C,
        3, 1, 0 };
    static const uint8_t band5[] = {
        1, 8, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
    memcpy(f + 26, band24, sizeof(band24));
    memcpy(f + 26 + sizeof(band24), band5, sizeof(band5));
    put16(pr + 0, 0);                   /* mac_header */
    put16(pr + 2, 26);
    put16(pr + 4, 26);                  /* band_data[0]: 2.4 GHz */
    put16(pr + 6, sizeof(band24));
    put16(pr + 8, 26 + sizeof(band24)); /* band_data[1]: 5 GHz */
    put16(pr + 10, sizeof(band5));
    put16(pr + 12, 26 + sizeof(band24) + sizeof(band5));
    put16(pr + 14, 0);                  /* common_data */
    return size;
}

static void rx_phy(uint32_t id, const uint8_t *p, uint32_t len, uint16_t seq)
{
    (void)id;
    (void)seq;
    if (len < 68)
        return;
    rx_channel = get16(p + 22);
    rx_device_ts = get32(p + 4);        /* GP2, in us: when it was heard */
    /* The energy the antennas heard, as a positive number: -dBm */
    uint32_t e = get32(p + 24 + 4 * 1);
    int a = (int)(e & 0xFF), b = (int)((e >> 8) & 0xFF);
    int sa = a ? -a : -128, sb = b ? -b : -128;
    rx_signal = sa > sb ? sa : sb;
}

static struct iwlwifi_bss *bss_for(const uint8_t *bssid)
{
    for (int i = 0; i < n_bss; i++)
        if (!memcmp(bss[i].bssid, bssid, 6))
            return &bss[i];
    if (n_bss >= IWLWIFI_MAX_BSS)
        return 0;
    struct iwlwifi_bss *b = &bss[n_bss];
    memset(&bss_info[n_bss], 0, sizeof(bss_info[0]));
    n_bss++;
    memset(b, 0, sizeof(*b));
    memcpy(b->bssid, bssid, 6);
    b->rssi = -128;
    return b;
}

/* A beacon or a probe response: who it is, on which channel, and how
   it is protected - RSN is WPA2, Microsoft's WPA element is WPA, the
   privacy bit alone is WEP */
static void rx_mpdu(uint32_t id, const uint8_t *p, uint32_t len, uint16_t seq)
{
    (void)id;
    (void)seq;
    if (len < 4)
        return;
    uint32_t n = get16(p);
    if (n < WLAN_HDR || 4 + n + 4 > len)
        return;
    uint32_t status = get32(p + 4 + n);
    if ((status & 3) != 3)              /* CRC and no overrun */
        return;
    const uint8_t *f = p + 4;
    link_rx(f, n, status);
    if (n < 36)                         /* shorter than a beacon */
        return;
    uint16_t fc = get16(f);
    uint32_t type = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
    if (type != 0 || (sub != 5 && sub != 8))
        return;
    struct iwlwifi_bss *b = bss_for(f + 16);
    if (!b)
        return;
    struct wlan_bss_info info;
    if (wlan_parse_beacon(f, n, &info) == 0) {
        int k = (int)(b - bss);
        /* a probe response has no TIM: the beacon's DTIM stays */
        if (!info.has_tim && bss_info[k].has_tim) {
            info.has_tim = 1;
            info.dtim_count = bss_info[k].dtim_count;
            info.dtim_period = bss_info[k].dtim_period;
        }
        bss_info[k] = info;
        bss_seen_ts[k] = rx_device_ts;
    }
    int privacy = (get16(f + 34) & 0x10) != 0;
    int rsn = 0, wpa = 0, channel = rx_channel;
    for (uint32_t i = 36; i + 2 <= n;) {
        uint8_t eid = f[i], elen = f[i + 1];
        const uint8_t *v = f + i + 2;
        if (i + 2 + elen > n)
            break;
        if (eid == 0 && elen <= 32) {
            memcpy(b->ssid, v, elen);
            b->ssid[elen] = 0;
            b->ssid_len = elen;
        } else if (eid == 3 && elen == 1) {
            channel = v[0];
        } else if (eid == 48) {
            rsn = 1;
        } else if (eid == 221 && elen >= 4 && v[0] == 0x00 &&
                   v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) {
            wpa = 1;
        }
        i += 2 + elen;
    }
    b->channel = (uint8_t)channel;
    b->security = rsn ? IWLWIFI_SEC_WPA2 : wpa ? IWLWIFI_SEC_WPA
                  : privacy ? IWLWIFI_SEC_WEP : IWLWIFI_SEC_OPEN;
    if (rx_signal > b->rssi)
        b->rssi = (int8_t)rx_signal;
}

static void scan_complete(uint32_t id, const uint8_t *p, uint32_t len,
                          uint16_t seq)
{
    (void)id;
    (void)seq;
    scan_status = len >= 3 ? p[2] : 0;
    scan_running = 0;
}

int iwlwifi_scan_now(void)
{
    if (st.stage != IWL_STAGE_READY)
        return IWL_ERR_STATE;
    if (!fw.n_scan_channels) {
        why("the firmware file says nothing of how many channels a scan "
            "takes");
        return IWL_ERR_BADFW;
    }
    iwl_notif_register(REPLY_RX_PHY_CMD, rx_phy);
    iwl_notif_register(REPLY_RX_MPDU_CMD, rx_mpdu);
    iwl_notif_register(SCAN_OFFLOAD_COMPLETE, scan_complete);
    n_bss = 0;
    rx_signal = -128;
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = SCAN_OFFLOAD_REQUEST_CMD;
    c.data[0] = scan_cmd;
    c.len[0] = scan_build();
    scan_running = 1;
    scan_status = 0;
    int r = iwl_send_cmd(&c);
    if (r != IWL_OK) {
        scan_running = 0;
        say("[wifi] the scan did not start (%d)\n", r);
        return r;
    }
    struct waiter w;
    wait_begin(&w, SCAN_DONE_MS);
    do {
        service();
        if (fw_err || hw_gone)
            break;
    } while (scan_running && wait_more(&w));
    if (scan_running) {
        scan_running = 0;
        why("the scan did not end within %u ms", (uint32_t)SCAN_DONE_MS);
        return fw_err ? IWL_ERR_FW : IWL_ERR_TIMEOUT;
    }
    say("[wifi] scan done (status %u): %d network(s)\n",
        (uint32_t)scan_status, n_bss);
    bss_found = n_bss;
    bss_recorded = 0;
    return n_bss;
}

int iwlwifi_bss(struct iwlwifi_bss *out, int max)
{
    int n = n_bss < max ? n_bss : max;
    for (int i = 0; i < n; i++)
        out[i] = bss[i];
    return n_bss;
}

/* From any thread: the scan is the driver thread's to run */
static volatile int scan_wanted, scan_result;
static volatile uint32_t scan_gen;

int iwlwifi_scan_start(void)
{
    if (st.stage != IWL_STAGE_READY || scan_wanted || scan_running)
        return -1;
    scan_wanted = 1;
    return 0;
}

static void hex_addr(char *out, const uint8_t *a)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[3 * i] = hx[a[i] >> 4];
        out[3 * i + 1] = hx[a[i] & 15];
        out[3 * i + 2] = i < 5 ? ':' : 0;
    }
}

static void copy_text(char *dst, uint32_t room, const char *src)
{
    uint32_t i = 0;
    for (; src && src[i] && i + 1 < room; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void link_info(struct wifi_info *o);

void iwlwifi_info_fill(struct wifi_info *o)
{
    memset(o, 0, sizeof(*o));
    o->present = st.stage != IWL_STAGE_NONE && st.stage != IWL_STAGE_NO_CARD;
    o->ready = st.stage == IWL_STAGE_READY;
    o->scanning = scan_wanted || scan_running;
    o->found = bss_found;
    o->recorded = bss_recorded;
    if (o->present)
        copy_text(o->card, sizeof(o->card), "Intel Wireless 3160");
    copy_text(o->state, sizeof(o->state), iwlwifi_stage_name(st.stage));
    if (st.mac_valid)
        hex_addr(o->mac, st.mac);
    copy_text(o->error, sizeof(o->error), st.last_error);
    link_info(o);
    /* The loudest first: that is the order a person picks from */
    int used[IWLWIFI_MAX_BSS] = { 0 };
    for (int k = 0; k < WIFI_INFO_NETS && k < n_bss; k++) {
        int best = -1;
        for (int i = 0; i < n_bss; i++)
            if (!used[i] && (best < 0 || bss[i].rssi > bss[best].rssi))
                best = i;
        used[best] = 1;
        struct wifi_net *w = &o->net[k];
        copy_text(w->ssid, sizeof(w->ssid), bss[best].ssid);
        hex_addr(w->bssid, bss[best].bssid);
        w->rssi = bss[best].rssi;
        w->channel = bss[best].channel;
        w->security = bss[best].security;
    }
}

/* --- what can be checked without a card ------------------------------
 *
 * Beacons and a probe response of the shape 802.11 gives them, each
 * with the PHY packet the firmware sends before it, pushed through the
 * same two handlers a scan uses - and one with a broken checksum, which
 * has to come to nothing. This proves the reading of frames, not that a
 * card hears anything.
 */
static void test_frame(const uint8_t *bssid, const char *ssid, int channel,
                       int rssi, int sec, int sub, uint32_t status)
{
    static uint8_t phy[68], m[4 + 160 + 4];
    memset(phy, 0, sizeof(phy));
    put16(phy + 22, (uint32_t)channel);
    put32(phy + 28, (uint32_t)(-rssi) & 0xFF);
    rx_phy(REPLY_RX_PHY_CMD, phy, sizeof(phy), 0);
    uint8_t *f = m + 4;
    memset(m, 0, sizeof(m));
    f[0] = (uint8_t)(sub << 4);
    memset(f + 4, 0xFF, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    put16(f + 32, 100);
    put16(f + 34, sec == IWLWIFI_SEC_OPEN ? 0x0001 : 0x0011);
    uint32_t n = 36, sl = 0;
    while (ssid[sl])
        sl++;
    f[n++] = 0;
    f[n++] = (uint8_t)sl;
    memcpy(f + n, ssid, sl);
    n += sl;
    f[n++] = 3;
    f[n++] = 1;
    f[n++] = (uint8_t)channel;
    if (sec == IWLWIFI_SEC_WPA2) {
        static const uint8_t rsn[] = { 48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                       1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0,
                                       0x00, 0x0F, 0xAC, 2, 0, 0 };
        memcpy(f + n, rsn, sizeof(rsn));
        n += sizeof(rsn);
    }
    put16(m, n);
    put32(f + n, status);
    rx_mpdu(REPLY_RX_MPDU_CMD, m, 4 + n + 4, 0);
}

void iwlwifi_selftest(void)
{
    static const uint8_t a[6] = { 0x02, 0x7A, 0x00, 0x00, 0x10, 0x01 };
    static const uint8_t b[6] = { 0x02, 0x7A, 0x00, 0x00, 0x10, 0x02 };
    static const uint8_t c[6] = { 0x02, 0x7A, 0x00, 0x00, 0x10, 0x03 };
    static const uint8_t d[6] = { 0x02, 0x7A, 0x00, 0x00, 0x10, 0x04 };
    n_bss = 0;
    rx_signal = -128;
    test_frame(a, "ThreadOS-Test", 6, -52, IWLWIFI_SEC_WPA2, 8, 3);
    test_frame(b, "Guest", 11, -74, IWLWIFI_SEC_OPEN, 5, 3);
    test_frame(c, "", 1, -81, IWLWIFI_SEC_WPA2, 8, 3);
    test_frame(d, "Broken", 6, -40, IWLWIFI_SEC_OPEN, 8, 1);  /* bad CRC */
    bss_found = n_bss;
    bss_recorded = 1;
    printk("wifi: reading frames, with no card - of the shape 802.11 "
           "gives them\n");
    for (int i = 0; i < n_bss; i++) {
        char t[18];
        hex_addr(t, bss[i].bssid);
        printk("  %s  ch %u  %d dBm  %s  %s\n", t,
               (uint32_t)bss[i].channel, (int)bss[i].rssi,
               bss[i].security == IWLWIFI_SEC_WPA2 ? "WPA2" : "open",
               bss[i].ssid_len ? bss[i].ssid : "(hidden)");
    }
    printk("wifi: networks read: %d\n", n_bss);
}

int iwlwifi_scan(void)
{
    if (st.stage != IWL_STAGE_READY)
        return IWL_ERR_STATE;
    uint32_t gen = scan_gen;
    scan_wanted = 1;
    for (int i = 0; i < 400 && scan_gen == gen; i++)
        task_sleep(50);
    return scan_gen == gen ? IWL_ERR_TIMEOUT : scan_result;
}

/* ===================================================================
 * Stage 3: joining a network
 *
 * What Linux 4.9's iwlmvm, driven by mac80211, sends this firmware to
 * join a WPA2 network as a station, cut to what the firmware needs
 * (the API headers Linux carries under BSD-3-Clause give the layouts):
 *
 *   the station's four TX queues (VO, VI, BE, BK -> queues 0..3, FIFOs
 *   3..0); its MAC context (a BSS station, not yet associated); PHY
 *   context 0 moved to the network's channel; the binding of the two;
 *   the MAC's power table (no power saving); the MAC context again with
 *   the BSSID and the beacon interval; the access point as station 0;
 *   a time event keeping the radio on the channel while we join;
 *
 *   then over the air, as kernel/wlan.c builds and reads them: open
 *   system authentication and the association, management frames at
 *   1 Mb/s on the VO queue;
 *
 *   then: the station modified, a rate table (LQ) for the data frames,
 *   the MAC context associated (the AID, the DTIM taken from the last
 *   beacon), the time quota for the binding, every multicast frame let
 *   through - and for WPA2 the four-way handshake (kernel/wpa.c) in
 *   EAPOL data frames on the BE queue, after which the pairwise key and
 *   the group key go into the firmware's key table (ADD_STA_KEY,
 *   version 1 for this file).
 *
 * The card encrypts what we send with the key in the TX command, but
 * the CCMP header - the packet number - is ours to write; it decrypts
 * what comes in and leaves the header and MIC in place, and the replay
 * check is ours too.
 * =================================================================== */

#define ADD_STA_KEY_CMD        0x17
#define REMOVE_STA             0x19
#define MAC_CONTEXT_CMD        0x28
#define TIME_EVENT_CMD         0x29
#define TIME_EVENT_NOTIFICATION 0x2A
#define BINDING_CONTEXT_CMD    0x2B
#define TIME_QUOTA_CMD         0x2C
#define LQ_CMD                 0x4E
#define MAC_PM_POWER_TABLE     0xA9
#define MCAST_FILTER_CMD       0xD0

#define FW_CTXT_ACTION_MODIFY  2
#define FW_CTXT_ACTION_REMOVE  3
#define FW_CTXT_INVALID        0xFFFFFFFFu
#define FW_MAC_TYPE_BSS_STA    5
#define MAC_FILTER_ACCEPT_GRP  0x04
#define MAC_FILTER_IN_BEACON   0x40
#define MAC_QOS_FLG_UPDATE_EDCA 0x01
#define MAC_PROT_FLG_TGG_PROTECT 0x08
#define MAC_FLG_SHORT_PREAMBLE 0x20
#define MAC_FLG_SHORT_SLOT     0x10
#define STA_FLG_FAT_MIMO_MSK   0x3C000000u
#define TE_BSS_STA_AGGRESSIVE_ASSOC 0
#define TE_POLICY_JOIN         0x0803       /* start, end, at once */

#define TX_FLG_ACK             0x00000008u
#define TX_FLG_STA_RATE        0x00000010u
#define TX_FLG_BT_PRIO_3       0x00001800u
#define TX_FLG_SEQ_CTL         0x00002000u
#define TX_SEC_CCM             2
#define TX_RATE_1M_CCK_ANT_A   0x0000420Au
#define TID_NON_QOS            8
#define PM_FRAME_MGMT          2
#define PM_FRAME_ASSOC         3
#define TX_STATUS_SUCCESS      0x01
#define TX_STATUS_DIRECT_DONE  0x02

#define STA_KEY_FLG_CCM        0x0002
#define STA_KEY_FLG_WEP_KEY_MAP 0x0008
#define STA_KEY_MULTICAST      0x4000

#define RX_RES_STATUS_MIC_OK   (1u << 6)
#define RX_RES_STATUS_SEC_SHIFT 8
#define RX_RES_STATUS_SEC_CCM  2

#define Q_VO 0
#define Q_BE 2
#define AP_STA_ID 0
#define JOIN_TRIES 3
#define JOIN_WAIT_MS 500
#define KEYS_WAIT_MS 5000
#define TXQ_SOFT 16                     /* frames waiting for the thread */

static struct {
    volatile int state;                 /* IWLWIFI_LINK_* */
    int idx;                            /* in bss[] / bss_info[] */
    struct wlan_bss_info info;
    uint32_t info_ts;                   /* GP2 when its beacon came */
    int secure;
    struct wpa_sm wpa;
    uint16_t aid;
    int queues_on, mac_added, bound, sta_added;
    uint32_t te_id;
    int te_state;                       /* 0 none, 1 asked, 2 on, 3 over */
    volatile int got_auth, got_assoc;
    uint16_t status;
    int tx_status;                      /* the last management frame's */
    int keys_in;
    volatile int keys_pending, gtk_pending;
    uint64_t tx_pn;                     /* the last packet number sent */
    uint8_t rx_pn[4][6];                /* per key: the last one heard */
    int rx_pn_set[4];
    uint32_t rx_frames, tx_frames;
} link;

static void link_say(const char *what, const char *detail)
{
    say("[wifi] %s%s\n", what, detail ? detail : "");
}

static int ack_status_ok(uint32_t v)
{
    return (v & 0xFF) == ADD_STA_SUCCESS;
}

/* A command that answers with a 4-byte status: 0 if it is 'ok' */
static int cmd_status(uint32_t id, const void *b, uint32_t n, uint32_t *status)
{
    uint8_t resp[32];
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.data[0] = b;
    c.len[0] = n;
    c.flags = IWL_CMD_WANT_RESP;
    c.resp = resp;
    c.resp_cap = sizeof(resp);
    int r = iwl_send_cmd(&c);
    if (r < 0)
        return r;
    *status = c.resp_len >= 4 ? get32(resp) : 0xFFFFFFFFu;
    return IWL_OK;
}

/* --- the MAC context ----------------------------------------------- */

static void put_ac(uint8_t *ac, int fifo, uint16_t cw_min, uint16_t cw_max,
                   uint8_t aifsn, uint16_t txop_us)
{
    uint8_t *e = ac + 8 * fifo;
    put16(e, cw_min);
    put16(e + 2, cw_max);
    e[4] = aifsn;
    e[5] = (uint8_t)(1u << fifo);
    put16(e + 6, txop_us);
}

static uint32_t reciprocal(uint32_t v)
{
    return v ? 0xFFFFFFFFu / v : 0;
}

/* MAC_CONTEXT_API_S_VER_1, 148 bytes: phase 0 = added, 1 = told the
   BSSID, 2 = associated */
static int mac_context(uint32_t action, int phase)
{
    uint8_t b[148];
    memset(b, 0, sizeof(b));
    put32(b + 4, action);
    put32(b + 8, FW_MAC_TYPE_BSS_STA);
    memcpy(b + 16, st.mac, 6);
    if (phase >= 1)
        memcpy(b + 24, link.info.bssid, 6);
    /* the rates frames are acknowledged at: CCK 1-11, OFDM 6, 12, 24 */
    put32(b + 32, 0x0F);
    put32(b + 36, 0x15);
    uint16_t cap = link.info.capability;
    if (phase == 2) {
        if (cap & WLAN_CAP_SHORT_PRE)
            put32(b + 44, MAC_FLG_SHORT_PREAMBLE);
        if (cap & WLAN_CAP_SHORT_SLOT)
            put32(b + 48, MAC_FLG_SHORT_SLOT);
        put32(b + 52, MAC_FILTER_ACCEPT_GRP);
        put32(b + 56, MAC_QOS_FLG_UPDATE_EDCA);
        /* the EDCA mac80211 gives a network without WMM, in ERP mode */
        put_ac(b + 60, 0, 15, 1023, 7, 0);       /* BK */
        put_ac(b + 60, 1, 15, 1023, 3, 0);       /* BE */
        put_ac(b + 60, 2, 7, 15, 2, 3008);       /* VI */
        put_ac(b + 60, 3, 3, 7, 2, 1504);        /* VO */
    } else {
        put32(b + 52, MAC_FILTER_ACCEPT_GRP | MAC_FILTER_IN_BEACON);
        for (int f = 0; f < 4; f++)
            put_ac(b + 60, f, 15, 1023, 2, 0);
    }
    uint32_t bi = phase >= 1 ? link.info.beacon_int : 0;
    uint32_t dtim = link.info.dtim_period ? link.info.dtim_period : 1;
    if (phase == 2) {
        /* the next DTIM beacon, from the last beacon heard: its clock
           and ours, plus the beacons still to come before it */
        uint32_t offs = (uint32_t)link.info.dtim_count * bi * 1024;
        put32(b + 100, 1);
        put32(b + 104, link.info_ts + offs);
        uint64_t tsf = link.info.tsf + offs;
        put32(b + 108, (uint32_t)tsf);
        put32(b + 112, (uint32_t)(tsf >> 32));
        put32(b + 124, bi * dtim);
        put32(b + 128, reciprocal(bi * dtim));
        put32(b + 136, link.aid);
    }
    put32(b + 116, bi);
    put32(b + 120, reciprocal(bi));
    put32(b + 132, 10);                     /* listen interval */
    return iwl_send_cmd_pdu(MAC_CONTEXT_CMD, b, sizeof(b), 0);
}

static int phy_context_move(void)
{
    uint8_t b[36];
    memset(b, 0, sizeof(b));
    put32(b + 4, FW_CTXT_ACTION_MODIFY);
    b[16] = link.info.channel <= 14 ? PHY_BAND_24 : PHY_BAND_5;
    b[17] = link.info.channel;
    put32(b + 20, valid_tx_ant());
    put32(b + 24, (valid_rx_ant() << 1) | (1u << 10) | (1u << 12));
    return iwl_send_cmd_pdu(PHY_CONTEXT_CMD, b, sizeof(b), 0);
}

static int binding(uint32_t action)
{
    uint8_t b[24];
    put32(b, 0);
    put32(b + 4, action);
    put32(b + 8, action == FW_CTXT_ACTION_REMOVE ? FW_CTXT_INVALID : 0);
    put32(b + 12, FW_CTXT_INVALID);
    put32(b + 16, FW_CTXT_INVALID);
    put32(b + 20, 0);
    uint32_t status;
    int r = cmd_status(BINDING_CONTEXT_CMD, b, sizeof(b), &status);
    if (r == IWL_OK && status != 0) {
        why("BINDING_CONTEXT_CMD answered status 0x%x", status);
        return IWL_ERR_REJECTED;
    }
    return r;
}

static int power_table(void)
{
    uint8_t b[40];
    memset(b, 0, sizeof(b));
    put16(b + 4, 0x0001);                   /* power save allowed, not on */
    put16(b + 6, 25);                       /* keep alive, seconds */
    return iwl_send_cmd_pdu(MAC_PM_POWER_TABLE, b, sizeof(b), 0);
}

static int time_quota(int on)
{
    uint8_t b[48];
    for (int i = 0; i < 4; i++) {
        put32(b + 12 * i, i == 0 && on ? 0 : FW_CTXT_INVALID);
        put32(b + 12 * i + 4, i == 0 && on ? 128 : 0);
        put32(b + 12 * i + 8, 0);
    }
    return iwl_send_cmd_pdu(TIME_QUOTA_CMD, b, sizeof(b), 0);
}

/* ADD_STA_CMD_API_S_VER_7 for the access point: add, then modify */
static int ap_station(int modify)
{
    uint8_t b[44];
    memset(b, 0, sizeof(b));
    b[0] = (uint8_t)modify;
    put16(b + 2, 0xFFFF);                   /* no aggregation */
    if (!modify)
        memcpy(b + 8, link.info.bssid, 6);
    b[16] = AP_STA_ID;
    put32(b + 24, STA_FLG_FAT_MIMO_MSK);    /* 20 MHz, one stream */
    if (!modify)
        put32(b + 40, 0x0F);                /* queues 0..3 */
    uint32_t status;
    int r = cmd_status(ADD_STA, b, sizeof(b), &status);
    if (r == IWL_OK && !ack_status_ok(status)) {
        why("ADD_STA for the access point answered status 0x%x", status);
        return IWL_ERR_REJECTED;
    }
    return r;
}

static int remove_station(void)
{
    uint8_t b[4] = { AP_STA_ID, 0, 0, 0 };
    return iwl_send_cmd_pdu(REMOVE_STA, b, sizeof(b), 0);
}

/* Every data frame to the access point goes at the rates of its table:
   1 Mb/s on the one antenna, as iwlmvm's rate scaling starts it */
static int rate_table(void)
{
    uint8_t b[88];
    memset(b, 0, sizeof(b));
    b[0] = AP_STA_ID;
    b[6] = 1;                               /* antenna A */
    b[7] = 3;
    put16(b + 12, 4000);
    b[14] = 3;
    b[15] = 63;
    for (int i = 0; i < 16; i++)
        put32(b + 20 + 4 * i, TX_RATE_1M_CCK_ANT_A);
    return iwl_send_cmd_pdu(LQ_CMD, b, sizeof(b), 0);
}

static int multicast_all(void)
{
    uint8_t b[12];
    memset(b, 0, sizeof(b));
    b[3] = 1;                               /* pass all */
    memcpy(b + 4, link.info.bssid, 6);
    return iwl_send_cmd_pdu(MCAST_FILTER_CMD, b, sizeof(b), 0);
}

/* --- the time event: the radio kept on the channel ------------------ */

static void te_notif(uint32_t id, const uint8_t *p, uint32_t len, uint16_t seq)
{
    (void)id;
    (void)seq;
    if (len < 24 || get32(p + 8) != link.te_id || !link.te_state)
        return;
    if (get32(p + 16) == 1)                 /* start */
        link.te_state = get32(p + 20) == 1 ? 2 : 3;
    else
        link.te_state = 3;                  /* over */
}

static int time_event_add(void)
{
    uint8_t b[36], resp[16];
    memset(b, 0, sizeof(b));
    put32(b + 4, FW_CTXT_ACTION_ADD);
    put32(b + 8, TE_BSS_STA_AGGRESSIVE_ASSOC);
    put32(b + 16, 500);                     /* max delay, TU */
    put32(b + 24, 1);                       /* interval */
    put32(b + 28, 600);                     /* duration, TU */
    b[32] = 1;                              /* once */
    put16(b + 34, TE_POLICY_JOIN);
    struct iwl_hcmd c;
    memset(&c, 0, sizeof(c));
    c.id = TIME_EVENT_CMD;
    c.data[0] = b;
    c.len[0] = sizeof(b);
    c.flags = IWL_CMD_WANT_RESP;
    c.resp = resp;
    c.resp_cap = sizeof(resp);
    link.te_state = 1;
    link.te_id = 0xFFFFFFFFu;
    int r = iwl_send_cmd(&c);
    if (r < 0)
        return r;
    if (c.resp_len < 16 || !(get32(resp) & 1)) {
        why("TIME_EVENT_CMD was not taken (status 0x%x)",
            c.resp_len >= 4 ? get32(resp) : 0);
        return IWL_ERR_REJECTED;
    }
    link.te_id = get32(resp + 8);
    /* on the channel before the first frame goes */
    struct waiter w;
    wait_begin(&w, 1000);
    do {
        service();
        if (fw_err || hw_gone)
            return IWL_ERR_FW;
    } while (link.te_state == 1 && wait_more(&w));
    if (link.te_state != 2) {
        why("the firmware did not go to channel %u (time event %s)",
            (uint32_t)link.info.channel,
            link.te_state == 1 ? "never started" : "not scheduled");
        return IWL_ERR_TIMEOUT;
    }
    return IWL_OK;
}

static void time_event_remove(void)
{
    if (link.te_state != 2)
        return;
    uint8_t b[36];
    memset(b, 0, sizeof(b));
    put32(b + 4, FW_CTXT_ACTION_REMOVE);
    put32(b + 8, link.te_id);
    iwl_send_cmd_pdu(TIME_EVENT_CMD, b, sizeof(b), 0);
    link.te_state = 0;
}

/* --- frames out ------------------------------------------------------ */

static void tx_status(uint32_t id, const uint8_t *p, uint32_t len, uint16_t seq)
{
    (void)id;
    if (len >= 40 && ((seq >> 8) & 0x1F) == Q_VO)
        link.tx_status = (int)(get16(p + 36) & 0xFF);
}

/* A management frame to the access point: 1 Mb/s, acknowledged, the
   firmware numbering it */
static int send_mgmt(const uint8_t *f, uint32_t n, int pm)
{
    uint8_t tx[56];
    memset(tx, 0, sizeof(tx));
    put16(tx, n);
    put32(tx + 4, TX_FLG_ACK | TX_FLG_BT_PRIO_3 | TX_FLG_SEQ_CTL);
    put32(tx + 12, TX_RATE_1M_CCK_ANT_A);
    tx[16] = AP_STA_ID;
    put32(tx + 40, 0xFFFFFFFFu);            /* no life time limit */
    tx[49] = 60;
    tx[50] = 15;
    tx[51] = TID_NON_QOS;
    put16(tx + 52, (uint32_t)pm);
    link.tx_status = -1;
    return iwl_tx_frame(Q_VO, tx, f, WLAN_HDR, f + WLAN_HDR, n - WLAN_HDR);
}

/* A data frame (24-byte header, then the body): at the station's
   rates, encrypted by the card once the pairwise key is in */
static int send_data(const uint8_t *hdr, const uint8_t *body, uint32_t n,
                     int eapol)
{
    uint8_t tx[56], h[WLAN_HDR];
    static uint8_t frame[WLAN_FRAME_MAX];
    memcpy(h, hdr, WLAN_HDR);
    memset(tx, 0, sizeof(tx));
    put32(tx + 4, TX_FLG_ACK | TX_FLG_STA_RATE | TX_FLG_SEQ_CTL);
    tx[16] = AP_STA_ID;
    put32(tx + 40, 0xFFFFFFFFu);
    tx[49] = 60;
    tx[50] = 15;
    tx[51] = TID_NON_QOS;
    put16(tx + 52, eapol ? PM_FRAME_MGMT : 0);
    uint32_t len = n;
    if (link.keys_in) {
        /* The CCMP header: the packet number, never used twice, and
           the key id (0, pairwise) with Ext IV */
        if (n + WLAN_CCMP_HDR > sizeof(frame))
            return IWL_ERR_STATE;
        uint64_t pn = ++link.tx_pn;
        h[1] |= WLAN_FC1_PROTECTED;
        frame[0] = (uint8_t)pn;
        frame[1] = (uint8_t)(pn >> 8);
        frame[2] = 0;
        frame[3] = 0x20;
        for (int i = 0; i < 4; i++)
            frame[4 + i] = (uint8_t)(pn >> (16 + 8 * i));
        memcpy(frame + WLAN_CCMP_HDR, body, n);
        body = frame;
        len = n + WLAN_CCMP_HDR;
        tx[17] = TX_SEC_CCM;
        memcpy(tx + 20, link.wpa.keys.tk, WPA_TK_LEN);
    }
    put16(tx, WLAN_HDR + len);
    int r = iwl_tx_frame(Q_BE, tx, h, WLAN_HDR, body, len);
    if (r == IWL_OK)
        link.tx_frames++;
    return r;
}

static int send_eapol(const uint8_t *e, uint32_t n)
{
    static uint8_t eth[14 + WPA_FRAME_MAX], body[WLAN_FRAME_MAX];
    uint8_t hdr[WLAN_HDR];
    memcpy(eth, link.info.bssid, 6);
    memcpy(eth + 6, st.mac, 6);
    eth[12] = 0x88;
    eth[13] = 0x8E;
    memcpy(eth + 14, e, n);
    uint32_t bn = wlan_from_eth(eth, 14 + n, link.info.bssid, 0, hdr, body);
    if (!bn)
        return IWL_ERR_STATE;
    return send_data(hdr, body, bn, 1);
}

/* --- keys -------------------------------------------------------------- */

/* ADD_MODIFY_STA_KEY_API_S_VER_1, 64 bytes */
static int install_key(int offset, uint16_t flags, const uint8_t *key,
                       uint32_t len)
{
    uint8_t b[64];
    memset(b, 0, sizeof(b));
    b[0] = AP_STA_ID;
    b[1] = (uint8_t)offset;
    put16(b + 2, flags);
    memcpy(b + 4, key, len > 32 ? 32 : len);
    uint32_t status;
    int r = cmd_status(ADD_STA_KEY_CMD, b, sizeof(b), &status);
    memset(b, 0, sizeof(b));
    if (r == IWL_OK && !ack_status_ok(status)) {
        why("ADD_STA_KEY answered status 0x%x", status);
        return IWL_ERR_REJECTED;
    }
    return r;
}

static int install_keys(void)
{
    const struct wpa_keys *k = &link.wpa.keys;
    int r = install_key(0, STA_KEY_FLG_CCM | STA_KEY_FLG_WEP_KEY_MAP,
                        k->tk, WPA_TK_LEN);
    if (r == IWL_OK)
        r = install_key(1, (uint16_t)(STA_KEY_FLG_CCM | STA_KEY_FLG_WEP_KEY_MAP |
                                      STA_KEY_MULTICAST | (k->gtk_idx << 8)),
                        k->gtk, k->gtk_len);
    if (r == IWL_OK) {
        link.keys_in = 1;
        memset(link.rx_pn_set, 0, sizeof(link.rx_pn_set));
    }
    return r;
}

/* --- frames in ----------------------------------------------------------- */

/* The CCMP header's packet number must grow, per key: a frame heard
   twice, or replayed, is dropped (802.11 12.5.3.4.4) */
static int pn_fresh(const uint8_t *f, uint32_t hdr, int group)
{
    const uint8_t *c = f + hdr;
    uint8_t pn[6] = { c[7], c[6], c[5], c[4], c[1], c[0] };   /* PN5..PN0 */
    int key = group ? (c[3] >> 6) & 3 : 0;
    if (group && key == 0)
        return 0;
    if (link.rx_pn_set[key] && memcmp(pn, link.rx_pn[key], 6) <= 0)
        return 0;
    memcpy(link.rx_pn[key], pn, 6);
    link.rx_pn_set[key] = 1;
    return 1;
}

/* A handshake frame that came before the station was ready for data
   (message 1 follows the association response at once): kept until the
   rate table is in, since an answer before it would ask the firmware
   for a station's rates it does not have */
static uint8_t eapol_early[WPA_FRAME_MAX + 512];
static uint32_t eapol_early_len;

static void got_eapol(const uint8_t *e, uint32_t n)
{
    static uint8_t out[WPA_FRAME_MAX];
    uint32_t out_len;
    if (!link.secure)
        return;
    if (link.state == IWLWIFI_LINK_JOINING) {
        if (n <= sizeof(eapol_early)) {
            memcpy(eapol_early, e, n);
            eapol_early_len = n;
        }
        return;
    }
    int r = wpa_rx(&link.wpa, e, n, out, sizeof(out), &out_len);
    if (r == WPA_DROP) {
        if (link.wpa.why[0])
            link_say("handshake: a frame refused: ", link.wpa.why);
        return;
    }
    if (send_eapol(out, out_len) != IWL_OK)
        return;
    /* The keys go in from outside the receive path: a command waits for
       its answer, which comes through the receive ring being read now */
    if (r == WPA_KEYS)
        link.keys_pending = 1;
    else if (r == WPA_GROUP && link.keys_in)
        link.gtk_pending = 1;
}

static void keys_waiting(void)
{
    if (link.keys_pending) {
        link.keys_pending = 0;
        if (install_keys() == IWL_OK)
            link_say("handshake done: keys in", 0);
        else
            link_say("the keys did not go into the card", 0);
    }
    if (link.gtk_pending) {
        link.gtk_pending = 0;
        const struct wpa_keys *k = &link.wpa.keys;
        install_key(1, (uint16_t)(STA_KEY_FLG_CCM | STA_KEY_FLG_WEP_KEY_MAP |
                                  STA_KEY_MULTICAST | (k->gtk_idx << 8)),
                    k->gtk, k->gtk_len);
        link.rx_pn_set[k->gtk_idx & 3] = 0;
        link_say("a new group key", 0);
    }
}

static void link_rx(const uint8_t *f, uint32_t n, uint32_t status)
{
    if (link.state == IWLWIFI_LINK_DOWN || n < WLAN_HDR)
        return;
    const uint8_t *bssid = link.info.bssid;
    uint16_t code, aid;
    if (f[0] == WLAN_FC_BEACON && !memcmp(f + 16, bssid, 6)) {
        struct wlan_bss_info b;
        if (wlan_parse_beacon(f, n, &b) == 0 && b.has_tim) {
            link.info.tsf = b.tsf;
            link.info.dtim_count = b.dtim_count;
            link.info.dtim_period = b.dtim_period;
            link.info_ts = rx_device_ts;
        }
        return;
    }
    if (wlan_auth_resp(f, n, st.mac, bssid, &code) == 0) {
        link.status = code;
        link.got_auth = 1;
        return;
    }
    if (wlan_assoc_resp(f, n, st.mac, bssid, &code, &aid) == 0) {
        link.status = code;
        link.aid = aid;
        link.got_assoc = 1;
        return;
    }
    int kicked = wlan_kicked(f, n, st.mac, bssid);
    if (kicked >= 0) {
        char r[16];
        fmt(r, sizeof(r), "%d", kicked);
        link_say("the access point sent us away, reason ", r);
        link.state = IWLWIFI_LINK_DOWN;
        return;
    }
    if ((f[0] & 0x0C) != 0x08)
        return;
    /* A data frame: decrypted by the card, or plain before the keys */
    int ccmp = 0;
    if (f[1] & WLAN_FC1_PROTECTED) {
        uint32_t sec = (status >> RX_RES_STATUS_SEC_SHIFT) & 7;
        if (sec != RX_RES_STATUS_SEC_CCM || !(status & RX_RES_STATUS_MIC_OK))
            return;                     /* not decrypted, or tampered with */
        uint32_t hdr = WLAN_HDR + ((f[0] & 0x80) ? 2 : 0);
        if (n < hdr + WLAN_CCMP_HDR + WLAN_CCMP_MIC ||
            !pn_fresh(f, hdr, f[4] & 1))
            return;
        ccmp = 1;
    } else if (link.keys_in) {
        return;                         /* plain text after the keys: no */
    }
    static uint8_t eth[WLAN_FRAME_MAX];
    int len = wlan_to_eth(f, n, bssid, ccmp, eth);
    if (len < 14)
        return;
    if (eth[12] == 0x88 && eth[13] == 0x8E) {
        got_eapol(eth + 14, (uint32_t)len - 14);
        return;
    }
    if (link.state == IWLWIFI_LINK_UP) {
        link.rx_frames++;
        net_rx(eth, (uint16_t)len);
    }
}

/* --- joining ------------------------------------------------------------ */

/* Waits for a flag the receive path sets, serving the card */
static int wait_flag(volatile int *flag, uint32_t ms)
{
    struct waiter w;
    wait_begin(&w, ms);
    do {
        service();
        if (fw_err || hw_gone)
            return IWL_ERR_FW;
        if (link.state == IWLWIFI_LINK_DOWN)
            return IWL_ERR_REJECTED;
        keys_waiting();
    } while (!*flag && wait_more(&w));
    return *flag ? IWL_OK : IWL_ERR_TIMEOUT;
}

/* Sends a management frame and waits for the answer, a few times */
static int exchange(const uint8_t *f, uint32_t n, int pm, volatile int *got,
                    const char *what)
{
    for (int i = 0; i < JOIN_TRIES; i++) {
        *got = 0;
        int r = send_mgmt(f, n, pm);
        if (r != IWL_OK)
            return r;
        r = wait_flag(got, JOIN_WAIT_MS);
        if (r == IWL_OK)
            return link.status == 0 ? IWL_OK : IWL_ERR_REJECTED;
        if (r != IWL_ERR_TIMEOUT)
            return r;
    }
    why("no answer to the %s request from the access point", what);
    return IWL_ERR_TIMEOUT;
}

static void undo_join(void)
{
    time_event_remove();
    if (link.sta_added)
        remove_station();
    if (link.state >= IWLWIFI_LINK_KEYS || link.aid)
        time_quota(0);
    if (link.bound)
        binding(FW_CTXT_ACTION_REMOVE);
    if (link.mac_added) {
        uint8_t b[8];
        put32(b, 0);
        put32(b + 4, FW_CTXT_ACTION_REMOVE);
        iwl_send_cmd_pdu(MAC_CONTEXT_CMD, b, sizeof(b), 0);
    }
    if (link.queues_on)
        for (int q = 0; q < 4; q++)
            iwl_txq_disable(q);
    wpa_end(&link.wpa);
    memset(&link, 0, sizeof(link));
    eapol_early_len = 0;
}

static int join_steps(const char *password)
{
    struct wlan_bss_info *b = &link.info;
    if (!b->channel || !b->beacon_int) {
        why("the network's beacon was not heard whole: no channel or "
            "beacon interval");
        return IWL_ERR_STATE;
    }
    if (b->beacon_int < 16) {
        why("the network's beacon interval (%u TU) is one this firmware "
            "falls over on", (uint32_t)b->beacon_int);
        return IWL_ERR_STATE;
    }
    if (b->capability & WLAN_CAP_PRIVACY) {
        if (!b->rsn_len || wpa_rsn_usable(b->rsn, b->rsn_len) < 0) {
            why("the network is protected some other way than WPA2 with "
                "CCMP and a password");
            return IWL_ERR_STATE;
        }
        uint32_t pl = 0;
        while (password && password[pl])
            pl++;
        if (pl < 8 || pl > 63) {
            why("a WPA2 password has 8 to 63 characters");
            return IWL_ERR_STATE;
        }
        uint8_t pmk[WPA_PMK_LEN], snonce[WPA_NONCE_LEN];
        wpa_password_pmk(password, b->ssid, b->ssid_len, pmk);
        random_bytes(snonce, sizeof(snonce));
        wpa_begin(&link.wpa, pmk, b->bssid, st.mac, b->rsn, b->rsn_len,
                  snonce);
        memset(pmk, 0, sizeof(pmk));
        link.secure = 1;
    }
    link.state = IWLWIFI_LINK_JOINING;
    iwl_notif_register(TIME_EVENT_NOTIFICATION, te_notif);
    iwl_notif_register(TX_CMD, tx_status);
    iwl_notif_register(REPLY_RX_PHY_CMD, rx_phy);
    iwl_notif_register(REPLY_RX_MPDU_CMD, rx_mpdu);

    static const uint8_t fifo[4] = { 3, 2, 1, 0 };
    for (int q = 0; q < 4; q++)
        STEP("the station's TX queues", iwl_txq_enable(q, fifo[q], 0, TID_NON_QOS));
    link.queues_on = 1;
    STEP("MAC_CONTEXT_CMD (add)", mac_context(FW_CTXT_ACTION_ADD, 0));
    link.mac_added = 1;
    STEP("PHY_CONTEXT_CMD (to the network's channel)", phy_context_move());
    STEP("BINDING_CONTEXT_CMD", binding(FW_CTXT_ACTION_ADD));
    link.bound = 1;
    STEP("MAC_PM_POWER_TABLE", power_table());
    STEP("MAC_CONTEXT_CMD (the BSSID)", mac_context(FW_CTXT_ACTION_MODIFY, 1));
    STEP("ADD_STA (the access point)", ap_station(0));
    link.sta_added = 1;
    STEP("TIME_EVENT_CMD (staying on the channel)", time_event_add());

    uint8_t f[256];
    step = "authentication";
    uint32_t n = wlan_auth_req(f, b->bssid, st.mac);
    int r = exchange(f, n, PM_FRAME_MGMT, &link.got_auth, "authentication");
    if (r == IWL_ERR_REJECTED)
        why("the access point refused the authentication (status %u)",
            (uint32_t)link.status);
    if (r != IWL_OK)
        return r;
    link_say("authenticated", 0);
    step = "association";
    n = wlan_assoc_req(f, b, st.mac, link.secure ? link.wpa.ie : 0,
                       link.secure ? link.wpa.ie_len : 0);
    r = exchange(f, n, PM_FRAME_ASSOC, &link.got_assoc, "association");
    if (r == IWL_ERR_REJECTED)
        why("the access point refused the association (status %u)",
            (uint32_t)link.status);
    if (r != IWL_OK)
        return r;
    {
        char a[16];
        fmt(a, sizeof(a), "%u", (uint32_t)link.aid);
        link_say("associated, AID ", a);
    }
    STEP("ADD_STA (modify)", ap_station(1));
    STEP("LQ_CMD", rate_table());
    STEP("MAC_CONTEXT_CMD (associated)", mac_context(FW_CTXT_ACTION_MODIFY, 2));
    STEP("TIME_QUOTA_CMD", time_quota(1));
    STEP("MCAST_FILTER_CMD", multicast_all());
    if (link.secure) {
        link.state = IWLWIFI_LINK_KEYS;
        step = "the four-way handshake";
        if (eapol_early_len) {
            uint32_t len = eapol_early_len;
            eapol_early_len = 0;
            got_eapol(eapol_early, len);
        }
        r = wait_flag(&link.keys_in, KEYS_WAIT_MS);
        if (r == IWL_ERR_TIMEOUT)
            why("the four-way handshake did not finish%s%s",
                link.wpa.why[0] ? ": " : " (a wrong password?)",
                link.wpa.why);
        if (r != IWL_OK)
            return r;
    }
    time_event_remove();
    link.state = IWLWIFI_LINK_UP;
    return IWL_OK;
}

int iwlwifi_join_now(const uint8_t bssid[6], const char *password)
{
    if (st.stage != IWL_STAGE_READY)
        return IWL_ERR_STATE;
    reason[0] = 0;
    if (link.state != IWLWIFI_LINK_DOWN || link.queues_on)
        undo_join();
    int k = -1;
    for (int i = 0; i < n_bss; i++)
        if (!memcmp(bss[i].bssid, bssid, 6))
            k = i;
    if (k < 0) {
        why("that network was not heard in the last scan");
        return IWL_ERR_STATE;
    }
    link.idx = k;
    link.info = bss_info[k];
    link.info_ts = bss_seen_ts[k];
    char name[40];
    fmt(name, sizeof(name), "%s", bss[k].ssid);
    link_say("joining ", name);
    int r = join_steps(password);
    if (r != IWL_OK) {
        link_say("not joined: ", reason);
        char keep[sizeof(reason)];
        memcpy(keep, reason, sizeof(keep));
        undo_join();
        memcpy(reason, keep, sizeof(reason));
        fmt(st.last_error, sizeof(st.last_error), "%s", reason);
        return r;
    }
    link_say("joined ", name);
    st.last_error[0] = 0;
    dhcp_start();
    return IWL_OK;
}

/* --- the network layer's side ------------------------------------------ */

static struct spinlock txq_soft_lock = SPINLOCK_FREE;
static uint8_t txq_soft[TXQ_SOFT][1600];
static uint16_t txq_soft_len[TXQ_SOFT];
static volatile uint32_t txq_soft_head, txq_soft_tail;

int iwlwifi_send(const void *eth, uint16_t len)
{
    if (link.state != IWLWIFI_LINK_UP || len < 14 || len > 1600)
        return -1;
    spin_lock(&txq_soft_lock);
    if (txq_soft_head - txq_soft_tail >= TXQ_SOFT) {
        spin_unlock(&txq_soft_lock);
        return -1;
    }
    uint32_t i = txq_soft_head % TXQ_SOFT;
    memcpy(txq_soft[i], eth, len);
    txq_soft_len[i] = len;
    txq_soft_head++;
    spin_unlock(&txq_soft_lock);
    return 0;
}

/* On the driver's thread: what the network layer left to send */
static void send_waiting(void)
{
    static uint8_t hdr[WLAN_HDR], body[WLAN_FRAME_MAX];
    while (txq_soft_tail != txq_soft_head) {
        uint32_t i = txq_soft_tail % TXQ_SOFT;
        if (link.state == IWLWIFI_LINK_UP) {
            uint32_t n = wlan_from_eth(txq_soft[i], txq_soft_len[i],
                                       link.info.bssid, 0, hdr, body);
            if (n && send_data(hdr, body, n, 0) == IWL_ERR_BUSY)
                return;                 /* the ring is full: later */
        }
        spin_lock(&txq_soft_lock);
        txq_soft_tail++;
        spin_unlock(&txq_soft_lock);
    }
}

int iwlwifi_present(void)
{
    struct pci_dev dev;
    return find_card(&dev);
}

const uint8_t *iwlwifi_mac(void)
{
    static const uint8_t zeros[6];
    return st.mac_valid ? st.mac : zeros;
}

int iwlwifi_link(void)
{
    return link.state == IWLWIFI_LINK_UP;
}

int iwlwifi_link_state(void)
{
    return link.state;
}

uint32_t iwlwifi_rx_packets(void)
{
    return link.rx_frames;
}

uint32_t iwlwifi_tx_packets(void)
{
    return link.tx_frames;
}

/* From other threads: the driver's thread does the joining */
static volatile int join_wanted, leave_wanted, join_result;
static volatile uint32_t join_gen;
static char join_ssid[33], join_password[64];

static char join_said[96];              /* the last join's outcome, kept */

int iwlwifi_join_start(const char *ssid, const char *password)
{
    if (st.stage != IWL_STAGE_READY) {
        fmt(join_said, sizeof(join_said), "%s",
            st.stage == IWL_STAGE_NO_CARD || st.stage == IWL_STAGE_NONE
            ? "there is no Wi-Fi card in this machine"
            : "the Wi-Fi card is not ready");
        return -1;
    }
    fmt(join_ssid, sizeof(join_ssid), "%s", ssid);
    fmt(join_password, sizeof(join_password), "%s", password ? password : "");
    join_said[0] = 0;
    join_wanted = 1;
    return 0;
}

int iwlwifi_join(const char *ssid, const char *password)
{
    uint32_t gen = join_gen;
    if (iwlwifi_join_start(ssid, password) < 0)
        return IWL_ERR_STATE;
    for (int i = 0; i < 600 && join_gen == gen; i++)
        task_sleep(50);
    return join_gen == gen ? IWL_ERR_TIMEOUT : join_result;
}

void iwlwifi_leave(void)
{
    leave_wanted = 1;
}

static void link_info(struct wifi_info *o)
{
    o->link = link.state;
    if (join_wanted && link.state == IWLWIFI_LINK_DOWN)
        o->link = IWLWIFI_LINK_JOINING;     /* asked for, not begun */
    if (o->link != IWLWIFI_LINK_DOWN)
        copy_text(o->joined, sizeof(o->joined),
                  link.state != IWLWIFI_LINK_DOWN ? bss[link.idx].ssid
                                                  : join_ssid);
    copy_text(o->join_error, sizeof(o->join_error), join_said);
}

/* On the driver's thread: a scan for the name, then the join */
static int join_by_name(void)
{
    int r = iwlwifi_scan_now();
    if (r < 0)
        return r;
    int best = -1;
    for (int i = 0; i < n_bss; i++)
        if (bss[i].ssid_len && !strcmp(bss[i].ssid, join_ssid) &&
            (best < 0 || bss[i].rssi > bss[best].rssi))
            best = i;
    if (best < 0) {
        why("no network called \"%s\" is within reach", join_ssid);
        fmt(st.last_error, sizeof(st.last_error), "%s", reason);
        return IWL_ERR_STATE;
    }
    r = iwlwifi_join_now(bss[best].bssid, join_password);
    memset(join_password, 0, sizeof(join_password));
    return r;
}

static void link_service(void)
{
    if (join_wanted) {
        join_wanted = 0;
        join_result = join_by_name();
        if (join_result != IWL_OK)
            fmt(join_said, sizeof(join_said), "%s", st.last_error);
        join_gen++;
    }
    if (leave_wanted) {
        leave_wanted = 0;
        if (link.state != IWLWIFI_LINK_DOWN || link.queues_on)
            undo_join();
    }
    keys_waiting();
    if (link.state == IWLWIFI_LINK_UP)
        send_waiting();
    else if (link.queues_on && link.state == IWLWIFI_LINK_DOWN) {
        link_say("the link is down", 0);
        undo_join();
    }
}

static void iwl_thread(void *arg)
{
    (void)arg;
    int r = iwlwifi_run();
    for (;;) {
        if (r == IWL_OK) {
            iwlwifi_poll();
            if (st.stage == IWL_STAGE_READY && scan_wanted) {
                scan_wanted = 0;
                scan_result = iwlwifi_scan_now();
                scan_gen++;
            }

            if (st.stage == IWL_STAGE_READY) {
                task_sleep(POLL_INTERVAL_MS);
                continue;
            }
            if (st.stage != IWL_STAGE_RFKILL)
                return;         /* stopped by an error; said so */
            r = IWL_ERR_RFKILL;
        }
        if (r != IWL_ERR_RFKILL)
            return;
        /* Wait for the switch, then start over from INIT: calibration
           was skipped (or the card stopped) while it was off */
        while (rfkill_set())
            task_sleep(500);
        st.rfkill = 0;
        say("[wifi] the radio switch is on; starting the card again\n");
        r = iwlwifi_run();
    }
}

void iwlwifi_start(void)
{
    struct pci_dev dev;
    if (!find_card(&dev)) {
        st.stage = IWL_STAGE_NO_CARD;
        printk("[ -- ] Wi-Fi: no Intel Wireless 3160\n");
        return;
    }
    /* The firmware takes a second or two: not on the boot's time */
    task_create("iwlwifi", iwl_thread, 0);
}

/* ===================================================================
 * State for the rest of the system
 * =================================================================== */

const struct iwlwifi_state *iwlwifi_state(void)
{
    return &st;
}

const char *iwlwifi_stage_name(int stage)
{
    switch (stage) {
    case IWL_STAGE_NONE:       return "not started";
    case IWL_STAGE_NO_CARD:    return "no card";
    case IWL_STAGE_PROBED:     return "card found";
    case IWL_STAGE_FW_LOADED:  return "firmware file read";
    case IWL_STAGE_INIT_ALIVE: return "INIT image running";
    case IWL_STAGE_NVM_READ:   return "NVM read";
    case IWL_STAGE_CALIBRATED: return "calibrated";
    case IWL_STAGE_RT_ALIVE:   return "runtime image running";
    case IWL_STAGE_READY:      return "ready (idle)";
    case IWL_STAGE_RFKILL:     return "radio switched off";
    case IWL_STAGE_FAILED:     return "failed";
    }
    return "?";
}

int iwlwifi_channels(struct iwlwifi_channel *out, int max)
{
    int n = st.n_channels < max ? st.n_channels : max;
    for (int i = 0; i < n; i++)
        out[i] = channels[i];
    return st.n_channels;
}
