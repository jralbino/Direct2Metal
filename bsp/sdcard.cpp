/* File: src/sdcard.cpp
 * V116 — BCM2837 EMMC + FAT32 + PPM frame save
 *
 * Implements minimal SD card access:
 *   1. BCM2835/7 SDHCI (EMMC peripheral at 0x3F300000)
 *   2. SD card init (CMD0→CMD8→ACMD41→CMD2→CMD3→CMD7→CMD16)
 *   3. Single-block read/write (CMD17/CMD24, 512-byte sectors)
 *   4. MBR + FAT32 BPB parse to locate root directory
 *   5. 8.3 filename lookup ("FRAME   PPM")
 *   6. Sequential sector write for PPM data (no FAT chain modification)
 *   7. Directory entry file-size update
 *
 * Safety: ONLY writes within the pre-allocated file's sectors.
 * No FAT chain modification → no risk of filesystem corruption.
 */
#include "sdcard.h"
#include <stdint.h>

extern void uart_puts(const char* s);
extern void uart_hex(uint32_t n);
extern void uart_dec(int n);

/* Provided by camera_debayer.cpp — writes one output row (480 px) as RGB888 */
extern void debayer_raw10_row_rgb(uint8_t* dst, int out_row);

/* ─── BCM2837 EMMC registers ────────────────────────────────────────────── */
#define EMMC_BASE       0x3F300000UL
#define EMMC_ARG2       (*((volatile uint32_t*)(EMMC_BASE + 0x00)))
#define EMMC_BLKSIZECNT (*((volatile uint32_t*)(EMMC_BASE + 0x04)))
#define EMMC_ARG1       (*((volatile uint32_t*)(EMMC_BASE + 0x08)))
#define EMMC_CMDTM      (*((volatile uint32_t*)(EMMC_BASE + 0x0C)))
#define EMMC_RESP0      (*((volatile uint32_t*)(EMMC_BASE + 0x10)))
#define EMMC_RESP1      (*((volatile uint32_t*)(EMMC_BASE + 0x14)))
#define EMMC_RESP2      (*((volatile uint32_t*)(EMMC_BASE + 0x18)))
#define EMMC_RESP3      (*((volatile uint32_t*)(EMMC_BASE + 0x1C)))
#define EMMC_DATA       (*((volatile uint32_t*)(EMMC_BASE + 0x20)))
#define EMMC_STATUS     (*((volatile uint32_t*)(EMMC_BASE + 0x24)))
#define EMMC_CONTROL0   (*((volatile uint32_t*)(EMMC_BASE + 0x28)))
#define EMMC_CONTROL1   (*((volatile uint32_t*)(EMMC_BASE + 0x2C)))
#define EMMC_INTERRUPT  (*((volatile uint32_t*)(EMMC_BASE + 0x30)))
#define EMMC_IRPT_MASK  (*((volatile uint32_t*)(EMMC_BASE + 0x34)))
#define EMMC_IRPT_EN    (*((volatile uint32_t*)(EMMC_BASE + 0x38)))
#define EMMC_SLOTISR_VER (*((volatile uint32_t*)(EMMC_BASE + 0xFC)))

/* INTERRUPT bits */
#define INT_CMD_DONE  (1u <<  0)
#define INT_DATA_DONE (1u <<  1)
#define INT_WRITE_RDY (1u <<  4)
#define INT_READ_RDY  (1u <<  5)
#define INT_ERROR     (1u << 15)
#define INT_ERR_MASK  0x017F8000u  /* all error bits */

/* STATUS bits */
#define ST_CMD_INHIBIT (1u << 0)
#define ST_DAT_INHIBIT (1u << 1)

/* CONTROL1 bits */
#define C1_CLK_INTLEN  (1u << 0)
#define C1_CLK_STABLE  (1u << 1)
#define C1_CLK_EN      (1u << 2)
#define C1_SRST_HC     (1u << 24)
#define C1_SRST_CMD    (1u << 25)
#define C1_SRST_DATA   (1u << 26)
#define C1_DATA_TOUNIT (0xEu << 16)  /* max data timeout */

/* CMDTM encoding helpers */
/* Response types */
#define RT_NONE 0u
#define RT_136  (1u << 16)
#define RT_48   (2u << 16)
#define RT_48B  (3u << 16)  /* 48-bit with busy */
/* Other bits */
#define CMD_CRCEN    (1u << 19)
#define CMD_IXEN     (1u << 20)
#define CMD_ISDATA   (1u << 21)
#define TM_DAT_WRITE (0u << 4)
#define TM_DAT_READ  (1u << 4)

/* Build CMDTM value: (idx << 24) | response | flags */
#define CMD(idx, resp, flags)  (((uint32_t)(idx) << 24) | (resp) | (flags))

/* Standard command values */
#define CMD0   CMD( 0, RT_NONE, 0)
#define CMD2   CMD( 2, RT_136, 0)
#define CMD3   CMD( 3, RT_48,  CMD_CRCEN|CMD_IXEN)
#define CMD7   CMD( 7, RT_48B, CMD_CRCEN|CMD_IXEN)
#define CMD8   CMD( 8, RT_48,  CMD_CRCEN|CMD_IXEN)
#define CMD16  CMD(16, RT_48,  CMD_CRCEN|CMD_IXEN)
#define CMD17  CMD(17, RT_48,  CMD_CRCEN|CMD_IXEN|CMD_ISDATA|TM_DAT_READ)
#define CMD24  CMD(24, RT_48,  CMD_CRCEN|CMD_IXEN|CMD_ISDATA|TM_DAT_WRITE)
#define CMD55  CMD(55, RT_48,  CMD_CRCEN|CMD_IXEN)
#define ACMD41 CMD(41, RT_48,  0)

/* ─── State ─────────────────────────────────────────────────────────────── */
static uint32_t g_rca;          /* relative card address (from CMD3) */
static bool     g_sdhc;         /* true = SDHC/SDXC card (block addressing) */
static uint32_t g_part_lba;     /* FAT32 partition start LBA */
static uint32_t g_fat_lba;      /* FAT table start LBA */
static uint32_t g_data_lba;     /* data region start LBA */
static uint32_t g_spc;          /* sectors per cluster */
static uint32_t g_root_cluster; /* root directory first cluster */
static uint8_t  g_sector[512];  /* staging buffer for one 512-byte sector */

/* ─── Delay ─────────────────────────────────────────────────────────────── */
static void sd_delay(int loops) {
    for (volatile int i = 0; i < loops; i++) asm volatile("nop");
}

/* ─── Wait for INTERRUPT bit(s) ─────────────────────────────────────────── */
static bool sd_wait_int(uint32_t mask, int timeout_loops) {
    for (int i = 0; i < timeout_loops; i++) {
        uint32_t irpt = EMMC_INTERRUPT;
        if (irpt & INT_ERR_MASK) {
            EMMC_INTERRUPT = irpt;
            return false;
        }
        if (irpt & mask) {
            EMMC_INTERRUPT = mask;  /* clear matched bits */
            return true;
        }
    }
    return false;  /* timeout */
}

/* ─── Send command ──────────────────────────────────────────────────────── */
static bool sd_cmd(uint32_t cmdtm, uint32_t arg) {
    /* Wait for CMD inhibit */
    for (int i = 0; i < 500000; i++) {
        if (!(EMMC_STATUS & ST_CMD_INHIBIT)) break;
    }
    EMMC_INTERRUPT = 0xFFFFFFFFu;  /* clear all */
    EMMC_ARG1      = arg;
    EMMC_CMDTM     = cmdtm;
    return sd_wait_int(INT_CMD_DONE, 1000000);
}

/* ─── EMMC clock setup ──────────────────────────────────────────────────── */
static void sd_set_clock(uint32_t div) {
    /* Clear existing clock enable */
    EMMC_CONTROL1 &= ~C1_CLK_EN;
    sd_delay(20000);
    /* Set divider: CLK_FREQ8 = div in bits [15:8], CLK_GENSEL=0 */
    uint32_t c1 = EMMC_CONTROL1 & 0xFFFF0000u;
    c1 |= C1_CLK_INTLEN | ((div & 0xFF) << 8);
    EMMC_CONTROL1 = c1;
    /* Wait stable */
    for (int i = 0; i < 100000; i++) {
        if (EMMC_CONTROL1 & C1_CLK_STABLE) break;
        sd_delay(100);
    }
    EMMC_CONTROL1 |= C1_CLK_EN;
    sd_delay(20000);
}

/* ─── Read 512-byte block ───────────────────────────────────────────────── */
static bool sd_read_block(uint32_t lba) {
    uint32_t arg = g_sdhc ? lba : (lba << 9);
    EMMC_BLKSIZECNT = 0x00010200u;  /* 1 block × 512 bytes */
    if (!sd_cmd(CMD17, arg)) return false;
    if (!sd_wait_int(INT_READ_RDY, 2000000)) return false;
    uint32_t* dst = (uint32_t*)(void*)g_sector;
    for (int i = 0; i < 128; i++) dst[i] = EMMC_DATA;
    return sd_wait_int(INT_DATA_DONE, 2000000);
}

/* ─── Write 512-byte block ──────────────────────────────────────────────── */
static bool sd_write_block(uint32_t lba) {
    uint32_t arg = g_sdhc ? lba : (lba << 9);
    /* Wait for DAT inhibit to clear */
    for (int i = 0; i < 500000; i++) {
        if (!(EMMC_STATUS & ST_DAT_INHIBIT)) break;
    }
    EMMC_BLKSIZECNT = 0x00010200u;
    if (!sd_cmd(CMD24, arg)) return false;
    if (!sd_wait_int(INT_WRITE_RDY, 2000000)) return false;
    const uint32_t* src = (const uint32_t*)(const void*)g_sector;
    for (int i = 0; i < 128; i++) EMMC_DATA = src[i];
    return sd_wait_int(INT_DATA_DONE, 2000000);
}

/* ─── FAT32 helpers ─────────────────────────────────────────────────────── */
static uint16_t read_u16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Convert cluster number to starting LBA */
static uint32_t cluster_to_lba(uint32_t cluster) {
    return g_data_lba + (cluster - 2) * g_spc;
}

/* Read next cluster from FAT table (follow chain) */
static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t fat_sector = g_fat_lba + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    if (!sd_read_block(fat_sector)) return 0x0FFFFFFFu;
    return read_u32(g_sector + fat_offset) & 0x0FFFFFFFu;
}

/* ─── SD card init ──────────────────────────────────────────────────────── */
bool sdcard_init() {
    uart_puts("[SD] Init EMMC...\n");

    /* Version check */
    uint32_t ver = EMMC_SLOTISR_VER;
    uart_puts("[SD] SLOTISR_VER="); uart_hex(ver); uart_puts("\n");

    /* Reset host */
    EMMC_CONTROL1 = C1_SRST_HC;
    for (int i = 0; i < 100000; i++) {
        if (!(EMMC_CONTROL1 & C1_SRST_HC)) break;
    }
    if (EMMC_CONTROL1 & C1_SRST_HC) {
        uart_puts("[SD] Reset timeout\n");
        return false;
    }

    /* Set up for card init at ~400 kHz.
     * EMMC base clock ≈ 50 MHz from VPU. Divider = 50000000/400000 = 125 = 0x7D.
     * Using divided-clock mode (CLK_GENSEL=0). */
    EMMC_CONTROL1 = C1_DATA_TOUNIT;
    sd_set_clock(125);

    /* Enable interrupts (polled, not IRQ) */
    EMMC_IRPT_EN   = 0;
    EMMC_IRPT_MASK = 0xFFFFFFFFu;

    /* CMD0: GO_IDLE */
    sd_cmd(CMD0, 0);
    sd_delay(200000);

    /* CMD8: SEND_IF_COND — check voltage / SD 2.0+ */
    bool cmd8_ok = sd_cmd(CMD8, 0x000001AAu);
    (void)cmd8_ok;

    /* ACMD41 loop: wait for card ready */
    bool card_ready = false;
    for (int retry = 0; retry < 200; retry++) {
        sd_cmd(CMD55, 0);  /* APP_CMD */
        /* arg: HCS=1, XPC=1, S18R=0, voltage window 3.3V */
        if (sd_cmd(ACMD41, 0x51FF8000u)) {
            uint32_t resp = EMMC_RESP0;
            if (resp & (1u << 31)) {  /* card power-up complete */
                g_sdhc = (resp & (1u << 30)) != 0;
                card_ready = true;
                break;
            }
        }
        sd_delay(100000);
    }
    if (!card_ready) {
        uart_puts("[SD] Card not ready (ACMD41 timeout)\n");
        return false;
    }
    uart_puts("[SD] Card ready, SDHC="); uart_dec((int)g_sdhc); uart_puts("\n");

    /* CMD2: ALL_SEND_CID — get CID (we discard it) */
    sd_cmd(CMD2, 0);

    /* CMD3: SEND_RELATIVE_ADDR — get RCA */
    if (!sd_cmd(CMD3, 0)) {
        uart_puts("[SD] CMD3 failed\n");
        return false;
    }
    g_rca = EMMC_RESP0 >> 16;
    uart_puts("[SD] RCA="); uart_hex(g_rca); uart_puts("\n");

    /* CMD7: SELECT_CARD */
    if (!sd_cmd(CMD7, (uint32_t)g_rca << 16)) {
        uart_puts("[SD] CMD7 failed\n");
        return false;
    }

    /* CMD16: SET_BLOCKLEN = 512 */
    if (!sd_cmd(CMD16, 512)) {
        uart_puts("[SD] CMD16 failed\n");
        return false;
    }

    /* Switch to higher clock ≈ 12.5 MHz (div=4) for data transfers */
    sd_set_clock(4);

    /* ── Locate FAT32 partition via MBR ────────────────────────────────── */
    if (!sd_read_block(0)) {
        uart_puts("[SD] MBR read failed\n");
        return false;
    }
    /* MBR partition table starts at offset 0x1BE; entry 0 at 0x1BE */
    /* LBA start of first partition = bytes 0x1C6..0x1C9 (offset 8 in entry) */
    if (g_sector[510] != 0x55 || g_sector[511] != 0xAA) {
        uart_puts("[SD] No valid MBR signature\n");
        return false;
    }
    g_part_lba = read_u32(g_sector + 0x1C6);
    uart_puts("[SD] Partition start LBA="); uart_hex(g_part_lba); uart_puts("\n");

    /* ── Parse FAT32 BPB ───────────────────────────────────────────────── */
    if (!sd_read_block(g_part_lba)) {
        uart_puts("[SD] BPB read failed\n");
        return false;
    }
    /* Validate FAT32 signature */
    if (g_sector[510] != 0x55 || g_sector[511] != 0xAA) {
        uart_puts("[SD] No valid BPB signature\n");
        return false;
    }
    uint32_t bytes_per_sector = read_u16(g_sector + 11);
    if (bytes_per_sector != 512) {
        uart_puts("[SD] BPS != 512 — unsupported\n");
        return false;
    }
    g_spc          = g_sector[13];               /* sectors per cluster */
    uint32_t rsvd  = read_u16(g_sector + 14);    /* reserved sectors */
    uint32_t nfats = g_sector[16];               /* number of FATs */
    uint32_t fat32_sz = read_u32(g_sector + 36); /* FAT size in sectors (FAT32 BPB ext) */
    g_root_cluster = read_u32(g_sector + 44);    /* root cluster */

    g_fat_lba  = g_part_lba + rsvd;
    g_data_lba = g_fat_lba + nfats * fat32_sz;

    uart_puts("[SD] SPC="); uart_dec((int)g_spc);
    uart_puts(" FAT_LBA="); uart_hex(g_fat_lba);
    uart_puts(" DATA_LBA="); uart_hex(g_data_lba);
    uart_puts(" ROOT_CLU="); uart_hex(g_root_cluster); uart_puts("\n");

    return true;
}

/* ─── Save PPM frame ────────────────────────────────────────────────────── */
bool sdcard_save_ppm() {
    uart_puts("[SD] Looking for FRAME.PPM...\n");

    /* Walk root directory to find "FRAME   PPM" (8.3 uppercase) */
    const char target[11] = { 'F','R','A','M','E',' ',' ',' ','P','P','M' };
    uint32_t dir_cluster = g_root_cluster;
    uint32_t dir_entry_lba = 0;
    int      dir_entry_off = -1;

    while (dir_cluster < 0x0FFFFFF8u && dir_entry_off < 0) {
        uint32_t lba = cluster_to_lba(dir_cluster);
        for (uint32_t s = 0; s < g_spc && dir_entry_off < 0; s++) {
            if (!sd_read_block(lba + s)) break;
            for (int e = 0; e < 512; e += 32) {
                uint8_t first = g_sector[e];
                if (first == 0x00) goto done_search;   /* end of directory */
                if (first == 0xE5) continue;           /* deleted entry */
                if (g_sector[e + 11] & 0x0E) continue; /* LFN/volume/dir */
                bool match = true;
                for (int c = 0; c < 11; c++) {
                    if (g_sector[e + c] != (uint8_t)target[c]) { match = false; break; }
                }
                if (match) {
                    dir_entry_lba = lba + s;
                    dir_entry_off = e;
                    break;
                }
            }
        }
        dir_cluster = fat_next_cluster(dir_cluster);
    }
done_search:

    if (dir_entry_off < 0) {
        uart_puts("[SD] FRAME.PPM not found\n");
        uart_puts("[SD] Create it first: python3 -c \"hdr=b'P6\\n480 480\\n255\\n'; open('FRAME.PPM','wb').write(hdr+bytes(480*480*3))\"\n");
        return false;
    }

    /* Re-read the directory sector (fat_next_cluster() may have replaced g_sector) */
    if (!sd_read_block(dir_entry_lba)) return false;
    uint32_t file_cluster = ((uint32_t)read_u16(g_sector + dir_entry_off + 20) << 16) |
                             read_u16(g_sector + dir_entry_off + 26);
    uart_puts("[SD] Found FRAME.PPM, first_cluster="); uart_hex(file_cluster); uart_puts("\n");

    /* ── Write PPM header + pixel data sector-by-sector ─────────────────── */
    /* PPM P6 header: "P6\n480 480\n255\n" = 15 bytes */
    const uint8_t ppm_hdr[] = { 'P','6','\n','4','8','0',' ','4','8','0','\n','2','5','5','\n' };
    const int     HDR_LEN   = (int)sizeof(ppm_hdr);
    const int     IMG_BYTES = 480 * 480 * 3;           /* 691200 bytes */
    const int     TOTAL     = HDR_LEN + IMG_BYTES;     /* 691215 bytes */

    uint8_t  row_buf[480 * 3];  /* one debayered row, 1440 bytes */
    int      written  = 0;      /* bytes committed to current sector */
    int      out_row  = 0;      /* which output row we're on (0..479) */
    int      row_byte = 0;      /* how many bytes of row_buf consumed */
    bool     in_hdr   = true;   /* still writing header bytes */
    int      hdr_pos  = 0;

    uint32_t cur_cluster = file_cluster;
    uint32_t sector_lba  = cluster_to_lba(cur_cluster);
    uint32_t sector_in_cluster = 0;
    int      sectors_written = 0;
    bool     ok = true;

    /* Build sectors on-the-fly and write each when full */
    for (int total_pos = 0; total_pos < TOTAL && ok; ) {
        /* Fill g_sector with up to 512 bytes of PPM content */
        int fill = 0;
        while (fill < 512 && total_pos < TOTAL) {
            if (in_hdr) {
                g_sector[fill++] = ppm_hdr[hdr_pos++];
                total_pos++;
                if (hdr_pos >= HDR_LEN) { in_hdr = false; }
            } else {
                /* Fetch next row if needed */
                if (row_byte == 0 && out_row < 480) {
                    debayer_raw10_row_rgb(row_buf, out_row);
                }
                g_sector[fill++] = row_buf[row_byte++];
                total_pos++;
                if (row_byte >= 480 * 3) { row_byte = 0; out_row++; }
            }
        }
        /* Pad partial last sector with zeros */
        while (fill < 512) g_sector[fill++] = 0;

        /* Write sector */
        if (!sd_write_block(sector_lba)) {
            uart_puts("[SD] Write error at sector "); uart_hex(sector_lba); uart_puts("\n");
            ok = false;
            break;
        }
        sectors_written++;
        written += 512;

        /* Advance to next sector / cluster */
        sector_in_cluster++;
        if (sector_in_cluster >= g_spc) {
            sector_in_cluster = 0;
            cur_cluster = fat_next_cluster(cur_cluster);
            if (cur_cluster >= 0x0FFFFFF8u) {
                if (total_pos < TOTAL) {
                    uart_puts("[SD] FAT chain too short — preallocate a larger FRAME.PPM\n");
                    ok = false;
                }
                break;
            }
            sector_lba = cluster_to_lba(cur_cluster);
        } else {
            sector_lba++;
        }
    }

    if (!ok) return false;

    /* ── Update directory entry file size ──────────────────────────────── */
    if (!sd_read_block(dir_entry_lba)) return false;
    write_u32(g_sector + dir_entry_off + 28, (uint32_t)TOTAL);
    if (!sd_write_block(dir_entry_lba)) {
        uart_puts("[SD] Dir entry update failed\n");
        return false;
    }

    uart_puts("[SD] FRAME.PPM written ("); uart_dec(sectors_written);
    uart_puts(" sectors, "); uart_dec(TOTAL); uart_puts(" bytes)\n");
    return true;
}
