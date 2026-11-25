/* Memory map, MBC3 cartridge (ROM banks, SRAM, real-time clock) and I/O
 * register dispatch. */
#include "gb.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>

CPU R;
uint64_t g_cyc, g_deadline;
uint8_t *g_rmap[16];
uint8_t *g_wmap[16];
uint8_t g_hram[0x100];
uint8_t g_io[0x100];
uint8_t *g_rom;
size_t g_rom_size;
unsigned g_rom_bank = 1, g_rom_banks;
int g_cgb = 1;
int g_double_speed;
int g_rc_miss;

uint8_t g_vram[2][0x2000];
uint8_t g_wram[8][0x1000];
uint8_t g_oam[0xA0];
uint8_t g_sram[16][0x2000];
static uint8_t g_sram_saved[16][0x2000];
static int sram_enabled;
static unsigned sram_bank;      /* 0-3 RAM, 8-C RTC */
static int mbc_type = 3;        /* 1, 3 or 5 (cartridge header 0x147) */
static int mbc1_mode, mbc1_hi;
static unsigned sram_banks = 4;  /* from the header (0x149) */
static int has_rtc = 1;          /* MBC3 with timer: RTC footer in the .sav */
#define SRAM_SIZE ((size_t)sram_banks * 0x2000)
static char save_path[1024];

/* ---- RTC (MBC3) ---- */
typedef struct {
    uint8_t s, m, h, dl, dh;     /* live registers */
    uint8_t ls, lm, lh, ldl, ldh; /* latched */
    int64_t base;                /* host time of the last sync */
    uint8_t latch_prev;
} Rtc;
static Rtc rtc;

static void rtc_sync(void)
{
    int64_t now = (int64_t)time(NULL);
    if (rtc.dh & 0x40) { rtc.base = now; return; }   /* halted */
    int64_t d = now - rtc.base;
    rtc.base = now;
    if (d <= 0) return;
    int64_t days = (rtc.dl | ((rtc.dh & 1) << 8));
    int64_t t = rtc.s + 60 * (rtc.m + 60 * (int64_t)(rtc.h + 24 * days)) + d;
    rtc.s = t % 60; t /= 60;
    rtc.m = t % 60; t /= 60;
    rtc.h = t % 24; t /= 24;
    if (t > 511) { rtc.dh |= 0x80; t &= 511; }
    rtc.dl = t & 0xFF;
    rtc.dh = (rtc.dh & 0xFE) | ((t >> 8) & 1);
}

static void update_map(void)
{
    g_rmap[0] = g_rom; g_rmap[1] = g_rom + 0x1000; g_rmap[2] = g_rom + 0x2000; g_rmap[3] = g_rom + 0x3000;
    size_t off = (size_t)(g_rom_bank % g_rom_banks) * 0x4000;
    for (int i = 0; i < 4; i++) g_rmap[4 + i] = g_rom + off + i * 0x1000;
    int vb = g_cgb ? (g_io[0x4F] & 1) : 0;
    g_rmap[8] = g_wmap[8] = g_vram[vb];
    g_rmap[9] = g_wmap[9] = g_vram[vb] + 0x1000;
    if (sram_enabled && sram_bank < sram_banks) {
        g_rmap[10] = g_wmap[10] = g_sram[sram_bank];
        g_rmap[11] = g_wmap[11] = g_sram[sram_bank] + 0x1000;
    } else {
        g_rmap[10] = g_wmap[10] = g_rmap[11] = g_wmap[11] = NULL;
    }
    int wb = g_cgb ? (g_io[0x70] & 7) : 1;
    if (!wb) wb = 1;
    g_rmap[12] = g_wmap[12] = g_wram[0];
    g_rmap[13] = g_wmap[13] = g_wram[wb];
    g_rmap[14] = g_wmap[14] = g_wram[0];
    g_rmap[15] = g_wmap[15] = NULL;
}

void mem_remap(void) { update_map(); }

static uint8_t sram_rd(uint16_t addr)
{
    if (!sram_enabled) return 0xFF;
    switch (sram_bank) {
    case 8: return rtc.ls;
    case 9: return rtc.lm;
    case 10: return rtc.lh;
    case 11: return rtc.ldl;
    case 12: return rtc.ldh;
    }
    (void)addr;
    return 0xFF;
}

static void sram_wr(uint16_t addr, uint8_t v)
{
    (void)addr;
    if (!sram_enabled) return;
    rtc_sync();
    switch (sram_bank) {
    case 8: rtc.s = v % 60; break;
    case 9: rtc.m = v % 60; break;
    case 10: rtc.h = v % 24; break;
    case 11: rtc.dl = v; break;
    case 12: rtc.dh = v & 0xC1; break;
    }
}


static void mbc_wr(uint16_t addr, uint8_t v)
{
    switch (addr >> 13) {
    case 0: {
        int en = (v & 0x0F) == 0x0A;
        if (sram_enabled && !en) cart_flush_save();
        sram_enabled = en;
        break;
    }
    case 1:
        if (mbc_type == 5) {
            if (addr < 0x3000) g_rom_bank = (g_rom_bank & 0x100) | v;
            else g_rom_bank = (g_rom_bank & 0xFF) | ((v & 1) << 8);
        } else if (mbc_type == 1) {
            unsigned lo = v & 0x1F;
            if (!lo) lo = 1;
            g_rom_bank = lo | (mbc1_mode ? 0 : (unsigned)mbc1_hi << 5);
        } else {
            g_rom_bank = v & 0x7F;
            if (!g_rom_bank) g_rom_bank = 1;
        }
        break;
    case 2:
        if (mbc_type == 1) {
            mbc1_hi = v & 3;
            if (mbc1_mode) sram_bank = mbc1_hi;
            else g_rom_bank = (g_rom_bank & 0x1F) | ((unsigned)mbc1_hi << 5);
        } else {
            sram_bank = v & 0x0F;
        }
        break;
    case 3:
        if (mbc_type == 1) {
            mbc1_mode = v & 1;
            sram_bank = mbc1_mode ? (unsigned)mbc1_hi : 0;
            break;
        }
        if (mbc_type == 3) {
            if (rtc.latch_prev == 0 && v == 1) {
                rtc_sync();
                rtc.ls = rtc.s; rtc.lm = rtc.m; rtc.lh = rtc.h; rtc.ldl = rtc.dl; rtc.ldh = rtc.dh;
            }
            rtc.latch_prev = v;
        }
        break;
    }
    update_map();
}

uint8_t mem_rd_slow(uint16_t addr)
{
    if (addr < 0xA000) return 0xFF;  /* unmapped (never: ROM/VRAM always mapped) */
    if (addr < 0xC000) return sram_rd(addr);
    if (addr < 0xFE00) return g_rmap[13][addr & 0xFFF];   /* F000-FDFF echo of D000 */
    if (addr < 0xFEA0) return g_oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;
    if (addr >= 0xFF80 && addr != 0xFFFF) return g_hram[addr & 0xFF];
    return io_rd(addr & 0xFF);
}

void mem_wr_slow(uint16_t addr, uint8_t v)
{
    if (addr < 0x8000) { mbc_wr(addr, v); return; }
    if (addr < 0xA000) return;
    if (addr < 0xC000) { sram_wr(addr, v); return; }
    if (addr < 0xFE00) { g_rmap[13][addr & 0xFFF] = v; return; }
    if (addr < 0xFEA0) { g_oam[addr - 0xFE00] = v; return; }
    if (addr < 0xFF00) return;
    if (addr >= 0xFF80 && addr != 0xFFFF) { g_hram[addr & 0xFF] = v; return; }
    io_wr(addr & 0xFF, v);
}

/* ---- save file: SRAM (32 KiB) + RTC footer in the 48-byte format of
 * VBA/BGB/mGBA, so emulator saves can be imported. ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

void cart_load_save(const char *path)
{
    snprintf(save_path, sizeof save_path, "%s", path);
    rtc.base = (int64_t)time(NULL);
    SDL_RWops *f = SDL_RWFromFile(path, "rb");
    if (!f) { memset(g_sram, 0xFF, sizeof g_sram); memcpy(g_sram_saved, g_sram, sizeof g_sram); return; }
    size_t n = SDL_RWread(f, g_sram, 1, SRAM_SIZE);
    uint8_t foot[48];
    size_t m = SDL_RWread(f, foot, 1, sizeof foot);
    SDL_RWclose(f);
    if (n < SRAM_SIZE) memset((uint8_t *)g_sram + n, 0xFF, SRAM_SIZE - n);
    if (m >= 44 && has_rtc) {
        rtc.s = get32(foot + 0); rtc.m = get32(foot + 4); rtc.h = get32(foot + 8);
        rtc.dl = get32(foot + 12); rtc.dh = get32(foot + 16);
        rtc.ls = get32(foot + 20); rtc.lm = get32(foot + 24); rtc.lh = get32(foot + 28);
        rtc.ldl = get32(foot + 32); rtc.ldh = get32(foot + 36);
        int64_t ts = get32(foot + 40);
        if (m >= 48) ts |= (int64_t)get32(foot + 44) << 32;
        rtc.base = ts;
        rtc_sync();
    }
    memcpy(g_sram_saved, g_sram, sizeof g_sram);
}

void cart_flush_save(void)
{
    if (!save_path[0]) return;
    rtc_sync();
    int changed = memcmp(g_sram_saved, g_sram, sizeof g_sram) != 0;
    static time_t last_rtc_write;
    if (!changed && time(NULL) - last_rtc_write < 60) return;
    SDL_RWops *f = SDL_RWFromFile(save_path, "wb");
    if (!f) { host_log("save: cannot write %s\n", save_path); return; }
    uint8_t foot[48];
    put32(foot + 0, rtc.s); put32(foot + 4, rtc.m); put32(foot + 8, rtc.h);
    put32(foot + 12, rtc.dl); put32(foot + 16, rtc.dh);
    put32(foot + 20, rtc.ls); put32(foot + 24, rtc.lm); put32(foot + 28, rtc.lh);
    put32(foot + 32, rtc.ldl); put32(foot + 36, rtc.ldh);
    put32(foot + 40, (uint32_t)rtc.base); put32(foot + 44, (uint32_t)((uint64_t)rtc.base >> 32));
    int ok = SDL_RWwrite(f, g_sram, 1, SRAM_SIZE) == SRAM_SIZE;
    if (has_rtc) ok &= SDL_RWwrite(f, foot, 1, sizeof foot) == sizeof foot;
    SDL_RWclose(f);
    if (!ok) host_log("save: write error\n");
    memcpy(g_sram_saved, g_sram, sizeof g_sram);
    last_rtc_write = time(NULL);
    if (changed) host_log("save: written %s\n", save_path);
}

void mem_init(void)
{
    memset(g_vram, 0, sizeof g_vram);
    memset(g_wram, 0, sizeof g_wram);
    memset(g_oam, 0, sizeof g_oam);
    memset(g_hram, 0, sizeof g_hram);
    g_rom_banks = (unsigned)(g_rom_size / 0x4000);
    g_rom_bank = 1;
    uint8_t type = g_rom[0x147];
    mbc_type = (type >= 0x01 && type <= 0x03) ? 1 : (type >= 0x19 && type <= 0x1E) ? 5 : 3;
    static const unsigned ram_kib[6] = { 0, 2, 8, 32, 128, 64 };
    unsigned kib = g_rom[0x149] < 6 ? ram_kib[g_rom[0x149]] : 32;
    sram_banks = kib >= 8 ? kib / 8 : 1;
    if (sram_banks > 16) sram_banks = 16;
    has_rtc = type == 0x0F || type == 0x10;
    update_map();
}

/* f == NULL: in-memory state (differential tests), in st_buf */
uint8_t *st_buf;
size_t st_pos, st_cap;

void st_io(FILE *f, int save, void *p, size_t n)
{
    if (!f) {
        if (save) {
            if (st_pos + n > st_cap) { st_cap = (st_pos + n) * 2; st_buf = realloc(st_buf, st_cap); }
            memcpy(st_buf + st_pos, p, n);
        } else {
            memcpy(p, st_buf + st_pos, n);
        }
        st_pos += n;
        return;
    }
    if (save) fwrite(p, 1, n, f);
    else if (fread(p, 1, n, f) != n) host_log("state: short read\n");
}

void mem_state(FILE *f, int save)
{
    ST(R); ST(g_cyc); ST(g_deadline); ST(g_double_speed); ST(g_rom_bank);
    ST(g_vram); ST(g_wram); ST(g_oam); ST(g_sram); ST(g_hram); ST(g_io);
    ST(sram_enabled); ST(sram_bank); ST(rtc);
    if (!save) update_map();
}
