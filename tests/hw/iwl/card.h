/* The Intel Wireless 3160 model: what its parts say to each other
 *
 * card.c is the PCIe side - the registers, NIC access, the periphery
 * and device-memory windows, the DMA engines (the firmware's service
 * channel, the receive ring, the command queue) and the interrupt
 * causes. fw.c is the microcode running on the card - the firmware
 * file read the way the specification describes it, ALIVE, the
 * commands, the NVM and the PHY DB. iwl_model.c is the rest of the
 * machine (PCI, memory, time, the kernel log), the scenarios and the
 * checks.
 */

#ifndef IWL_CARD_H
#define IWL_CARD_H

#include <stddef.h>
#include <stdint.h>

/* Where the firmware put BAR0, and what it decodes */
#define BAR0_PHYS 0xF7C00000ull
#define BAR0_SIZE 0x2000u

/* PCI location of the card: 02:00.0, as on the laptop */
#define CARD_BUS 2
#define CARD_SLOT 0
#define CARD_FUNC 0

/* CSR_HW_REV. The type of a 3160 is not in the specification (SPEC_T
   section 13 question 1); step 1, dash 1 make the MAC_STEP/MAC_DASH
   copy in nic_config visible. */
#define CARD_HW_REV 0x00000165u

/* Device memory: instructions, data (DCCM) - the sizes are the
   configuration's upper bounds (SPEC_T 2.1) */
#define SRAM_INST_BASE 0x00000000u
#define SRAM_INST_SIZE 0x00040000u
#define SRAM_DATA_BASE 0x00800000u
#define SRAM_DATA_SIZE 0x00014000u
/* Where this firmware keeps things the driver reads (model choices,
   all in data memory past what the file loads) */
#define SCD_BASE   0x00811000u
#define ERRLOG_PTR 0x00812000u
#define EVLOG_PTR  0x00812800u

enum scenario { SC_OK, SC_NOFW_ACK, SC_ASSERT, SC_RFKILL, SC_SCAN,
                SC_NOFILE };
extern int scenario;

/* --- time: nanoseconds of card time; it moves only when the driver
       calls into the machine (MMIO, io_wait, pit_ticks, task_sleep) */
extern uint64_t now_ns;
#define US 1000ull
#define MS 1000000ull
void model_advance(uint64_t ns);
void model_at(uint64_t delay, void (*fn)(uint32_t), uint32_t arg);

/* --- reporting */
void violation(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int violation_count(void);

/* --- host memory the driver got from pmm_alloc_contiguous */
int dma_ok(uint64_t addr, uint64_t len);
void *dma_ptr(uint64_t addr);

/* --- PCI (iwl_model.c) */
int pci_bus_master(void);
int pci_mem_enabled(void);

/* --- card.c */
void card_power_on(int rfkill_off);
uint32_t model_mmio_read(const volatile void *base, uint32_t off);
void model_mmio_write(const volatile void *base, uint32_t off, uint32_t v);
void card_tick(void);
uint8_t *sram_ptr(uint32_t addr, uint32_t len);   /* NULL: no memory there */
void card_raise(uint32_t csr_int, uint32_t fh_int);
int card_rfkill(void);                     /* 1: the switch is off */
uint32_t card_gp1(void);
uint32_t card_hw_if_config(void);
int card_osc_forced(void);
int card_rx_running(void);
/* One receive buffer's worth of packets from the firmware, sent when
   'ready' (card time) has come and the ring has room. 'cmds' is how
   many command answers it carries (they keep the card awake). */
struct rxbatch {
    uint8_t data[4096];
    uint32_t used;
    int npkts;
    int answers;
    uint64_t ready;
};
void card_rx_queue(const struct rxbatch *b);
int card_rx_backlog(void);
void card_cmd_done(void);                 /* an answer went to the host */
int card_cmds_pending(void);
void card_firmware_stopped(void);
/* What the driver did, for the checks */
struct card_stats {
    int prepare_seen, ready_granted_after_prepare;
    int chunks_started, chunks_done, chunks_unsignalled;
    int cpu_starts;
    uint64_t first_chunk_at, last_chunk_start;
    uint32_t rx_delivered;                /* receive buffers filled */
    uint32_t pkts_delivered;
    int rx_stalled;                       /* a batch waited > 1 s */
    int ict_enabled;
    uint32_t cfg41_before;
    int l1_disabled_at_start;             /* APMG_PCIDEV_STT bit 11 */
    int apm_ok_at_start;
    int shadow_at_start;
    uint32_t coalescing_at_start;
    uint32_t gio_at_start;
    int cbbc_programmed;                  /* queues with a ring base */
    int sw_reset_before_rt_load;
    int errlog_read;                      /* dwords of the log read */
    int mac_access_held;                  /* at the moment of asking */
};
extern struct card_stats cs;
uint32_t prph_value(uint32_t addr);       /* the periphery, as it is */
/* The command queue's state, for fw.c's checks */
int card_q15_wptr_set(void);
/* APMG_PS_CTRL bit 22 now, and whether it was ever set / cleared */
int card_ps22(int *set, int *cleared);
int card_nic_ready(void);
/* iwl_model.c: after every step of card time (the hang guard) */
void machine_after_advance(void);

/* --- fw.c */
void fw_parse(const uint8_t *file, size_t len);
void fw_cpu_start(void);                  /* CSR_RESET released the CPU */
void fw_cpu_reset(void);                  /* SW_RESET */
void fw_nmi(void);
int fw_running(void);
void fw_command(const uint8_t *cmd, uint32_t len, int index);
uint32_t fw_scd_base(void);               /* 0 until the firmware runs */
void fw_flood(int n);                     /* notifications after bring-up */
struct fw_stats {
    int init_starts, rt_starts;
    int init_alive, rt_alive;
    int init_calibrated;                  /* INIT_COMPLETE sent */
    int calib_sent;                       /* 0x6B notifications */
    int calib_unique;
    int rt_bytes_ok, init_bytes_ok;
    int phydb_ok;                         /* RT got exactly the results */
    int phydb_sent;
    int rt_required_missing;              /* filled by fw_rt_report */
    int rt_configured;
    int crashed;
    uint32_t crash_id;
    int flood_sent;
    int nvm_reads;
    int rt_before_calib;
    int scans, scan_cmd_ok, scans_done;   /* stage 2 */
    int scan_frames;                      /* beacons/responses sent up whole */
};
extern struct fw_stats fs;
void fw_rt_report(void);
/* The model's NVM, for the checks */
extern const uint8_t nvm_mac[6];
extern const uint16_t nvm_chan_flags[39];
extern const uint8_t nvm_chan_num[39];
#define NVM_VERSION 0x0709
#define NVM_SKU 0x000F
#define NVM_RADIO_CFG 0x1109

#endif
