/* I/O registers, PPU timing (modes, LY, STAT/VBlank interrupts), DMA/HDMA,
 * timer, joypad, serial (no cable). Time is the CPU cycle counter g_cyc;
 * hw_update() catches the hardware up to it and sets g_deadline to the
 * next event, which the recompiled code checks at every branch. */
#include "hw.h"
#include <string.h>

uint8_t g_bgpal[64], g_objpal[64];
uint32_t g_fb[160 * 144];

/* ---- PPU timing state ---- */
static uint64_t line_start;   /* cpu cycle at which the current line began */
static int ppu_mode;          /* 0,1,2,3 */
static int ppu_phase;         /* 0: in mode 2, 1: in mode 3, 2: in mode 0/1 */
static int win_line;
static int stat_line;         /* STAT interrupt line, for rising edges */
static int lcd_on;
static int hdma_active;
static uint16_t hdma_src, hdma_dst;
static int hdma_left;         /* blocks of 16 bytes left */

/* timer */
static uint64_t div_base;
static uint64_t timer_last;
/* serial */
static uint64_t serial_done = UINT64_MAX;

static inline unsigned dots_to_cyc(unsigned dots) { return dots << g_double_speed; }

void request_irq(int bit) { g_io[rIF] |= (uint8_t)(1 << bit); }

static void stat_update(void)
{
    uint8_t st = g_io[rSTAT];
    int ly = g_io[rLY];
    int coinc = ly == g_io[rLYC];
    g_io[rSTAT] = (uint8_t)((st & 0x78) | (coinc ? 4 : 0) | (lcd_on ? ppu_mode : 0) | 0x80);
    int line = 0;
    if (lcd_on) {
        if ((st & 0x40) && coinc) line = 1;
        if ((st & 0x08) && ppu_mode == 0) line = 1;
        if ((st & 0x10) && ppu_mode == 1) line = 1;
        if ((st & 0x20) && ppu_mode == 2) line = 1;
    }
    if (line && !stat_line) request_irq(1);
    stat_line = line;
}

static void hdma_block(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t v = rd(hdma_src);
        g_vram[g_io[rVBK] & 1][(hdma_dst & 0x1FF0) + (hdma_dst & 0xF)] = v;
        hdma_src++;
        hdma_dst = (uint16_t)(((hdma_dst + 1) & 0x1FFF));
    }
    hdma_left--;
    if (hdma_left <= 0) { hdma_active = 0; g_io[rHDMA5] = 0xFF; }
    else g_io[rHDMA5] = (uint8_t)(hdma_left - 1);
    g_cyc += 32;   /* the CPU is stopped during the copy (8 us at single speed) */
}

static uint64_t ppu_next_event(void)
{
    if (!lcd_on) return line_start + dots_to_cyc(456);
    if (ppu_phase == 0) return line_start + dots_to_cyc(80);
    if (ppu_phase == 1) return line_start + dots_to_cyc(252);
    return line_start + dots_to_cyc(456);
}

static void ppu_step(void)
{
    if (!lcd_on) {
        /* LCD off: keep a frame clock for pacing */
        line_start += dots_to_cyc(456);
        static int off_lines;
        if (++off_lines >= 154) {
            off_lines = 0;
            for (int i = 0; i < 160 * 144; i++) g_fb[i] = 0xFFFFFFFF;
            host_frame(g_fb);
        }
        return;
    }
    int ly = g_io[rLY];
    if (ppu_phase == 0) {               /* mode 2 -> 3 */
        ppu_phase = 1;
        if (ly < 144) { ppu_mode = 3; stat_update(); }
    } else if (ppu_phase == 1) {        /* mode 3 -> 0: draw the line */
        ppu_phase = 2;
        if (ly < 144) {
            ppu_render_line(ly, &win_line);
            ppu_mode = 0;
            stat_update();
            if (hdma_active) hdma_block();
        }
    } else {                            /* next line */
        line_start += dots_to_cyc(456);
        ppu_phase = 0;
        ly = (ly + 1) % 154;
        g_io[rLY] = (uint8_t)ly;
        if (ly == 144) {
            ppu_mode = 1;
            request_irq(0);
            stat_update();
            host_frame(g_fb);
        } else if (ly < 144) {
            if (ly == 0) win_line = 0;
            ppu_mode = 2;
            stat_update();
        } else {
            stat_update();
        }
        if (ly >= 144) ppu_phase = 2;   /* VBlank lines: one event per line */
    }
}

static void lcd_switch(int on)
{
    if (on == lcd_on) return;
    lcd_on = on;
    line_start = g_cyc;
    g_io[rLY] = 0;
    win_line = 0;
    if (on) { ppu_phase = 0; ppu_mode = 2; }
    else { ppu_phase = 2; ppu_mode = 0; }
    stat_update();
}

/* ---- timer ---- */
static const unsigned tac_period[4] = { 1024, 16, 64, 256 };

static void timer_update(void)
{
    uint8_t tac = g_io[rTAC];
    if (tac & 4) {
        unsigned p = tac_period[tac & 3];
        uint64_t t0 = (timer_last - div_base) / p, t1 = (g_cyc - div_base) / p;
        uint64_t n = t1 - t0;
        while (n--) {
            if (++g_io[rTIMA] == 0) { g_io[rTIMA] = g_io[rTMA]; request_irq(2); }
        }
    }
    timer_last = g_cyc;
}

static uint64_t timer_next(void)
{
    uint8_t tac = g_io[rTAC];
    if (!(tac & 4)) return UINT64_MAX;
    unsigned p = tac_period[tac & 3];
    uint64_t ticks = 256 - g_io[rTIMA];
    uint64_t t0 = (g_cyc - div_base) / p;
    return div_base + (t0 + ticks) * p;
}

void hw_update(void)
{
    timer_update();
    while (g_cyc >= ppu_next_event()) ppu_step();
    if (g_cyc >= serial_done) {
        serial_done = UINT64_MAX;
        g_io[rSB] = 0xFF;
        g_io[rSC] &= 0x7F;
        request_irq(3);
    }
    apu_catch_up();
    uint64_t dl = ppu_next_event();
    uint64_t t = timer_next();
    if (t < dl) dl = t;
    if (serial_done < dl) dl = serial_done;
    g_deadline = dl;
}

/* ---- registers ---- */
uint8_t io_rd(uint8_t r)
{
    if (r >= 0x10 && r <= 0x3F) return apu_read(r);
    switch (r) {
    case rP1: {
        uint8_t sel = g_io[rP1] & 0x30, btn = host_buttons(), v = 0x0F;
        if (!(sel & 0x10)) v &= (uint8_t)~(btn >> 4);     /* directions */
        if (!(sel & 0x20)) v &= (uint8_t)~(btn & 0x0F);   /* buttons */
        return (uint8_t)(0xC0 | sel | (v & 0x0F));
    }
    case rDIV: return (uint8_t)((g_cyc - div_base) >> 8);
    case rTIMA: timer_update(); return g_io[rTIMA];
    case rIF: hw_update(); return g_io[rIF] | 0xE0;
    case rSTAT: case rLY:
        hw_update();
        return g_io[r];
    case rKEY1: return (uint8_t)((g_double_speed ? 0x80 : 0) | (g_io[rKEY1] & 1) | 0x7E);
    case rVBK: return g_io[rVBK] | 0xFE;
    case rSVBK: return g_io[rSVBK] | 0xF8;
    case rHDMA5: return g_io[rHDMA5];
    case rBCPD: return g_bgpal[g_io[rBCPS] & 0x3F];
    case rOCPD: return g_objpal[g_io[rOCPS] & 0x3F];
    case rRP: return 0x3E;   /* infrared: nothing received */
    }
    return g_io[r];
}

void io_wr(uint8_t r, uint8_t v)
{
    if (r >= 0x10 && r <= 0x3F) { apu_catch_up(); apu_write(r, v); g_io[r] = v; return; }
    switch (r) {
    case rP1: g_io[rP1] = v & 0x30; return;
    case rSC:
        g_io[rSC] = v;
        if ((v & 0x81) == 0x81) { serial_done = g_cyc + 4096; if (serial_done < g_deadline) g_deadline = serial_done; }
        return;
    case rDIV: timer_update(); div_base = g_cyc; timer_last = g_cyc; g_deadline = g_cyc; return;
    case rTIMA: case rTMA: case rTAC:
        timer_update(); g_io[r] = v; g_deadline = g_cyc; return;
    case rIF: g_io[rIF] = v & 0x1F; return;
    case rLCDC:
        hw_update();
        g_io[rLCDC] = v;
        lcd_switch(v >> 7);
        g_deadline = g_cyc;
        return;
    case rSTAT: hw_update(); g_io[rSTAT] = (uint8_t)((g_io[rSTAT] & 7) | (v & 0x78)); stat_update(); return;
    case rLY: return;
    case rLYC: hw_update(); g_io[rLYC] = v; stat_update(); return;
    case rDMA:
        g_io[rDMA] = v;
        for (int i = 0; i < 0xA0; i++) g_oam[i] = rd((uint16_t)((v << 8) + i));
        return;
    case rKEY1: g_io[rKEY1] = v & 1; return;
    case rVBK: g_io[rVBK] = v & 1; mem_remap(); return;
    case rSVBK: g_io[rSVBK] = v & 7; mem_remap(); return;
    case rHDMA1: hdma_src = (uint16_t)((v << 8) | (hdma_src & 0xF0)); return;
    case rHDMA2: hdma_src = (uint16_t)((hdma_src & 0xFF00) | (v & 0xF0)); return;
    case rHDMA3: hdma_dst = (uint16_t)(((v & 0x1F) << 8) | (hdma_dst & 0xF0)); return;
    case rHDMA4: hdma_dst = (uint16_t)((hdma_dst & 0x1F00) | (v & 0xF0)); return;
    case rHDMA5:
        if (hdma_active && !(v & 0x80)) {   /* stop an HBlank transfer */
            hdma_active = 0;
            g_io[rHDMA5] = (uint8_t)(0x80 | (hdma_left - 1));
            return;
        }
        hdma_left = (v & 0x7F) + 1;
        if (v & 0x80) {
            hdma_active = 1;
            g_io[rHDMA5] = v & 0x7F;
            /* if the LCD is off or already in HBlank, a first block goes now */
            if (!lcd_on) hdma_block();
        } else {
            while (hdma_left > 0) hdma_block();
            g_io[rHDMA5] = 0xFF;
        }
        return;
    case rBCPS: g_io[rBCPS] = v; return;
    case rBCPD:
        hw_update();
        g_bgpal[g_io[rBCPS] & 0x3F] = v;
        if (g_io[rBCPS] & 0x80) g_io[rBCPS] = (uint8_t)(0x80 | ((g_io[rBCPS] + 1) & 0x3F));
        return;
    case rOCPS: g_io[rOCPS] = v; return;
    case rOCPD:
        hw_update();
        g_objpal[g_io[rOCPS] & 0x3F] = v;
        if (g_io[rOCPS] & 0x80) g_io[rOCPS] = (uint8_t)(0x80 | ((g_io[rOCPS] + 1) & 0x3F));
        return;
    case rSCY: case rSCX: case rWY: case rWX: case rBGP: case rOBP0: case rOBP1:
        hw_update();
        g_io[r] = v;
        return;
    }
    g_io[r] = v;
}

void hw_init(int cgb)
{
    g_cgb = cgb;
    memset(g_io, 0, sizeof g_io);
    g_io[rLCDC] = 0x91;
    g_io[rBGP] = 0xFC;
    g_io[rP1] = 0x30;
    g_io[rHDMA5] = 0xFF;
    g_io[rSTAT] = 0x80;
    for (int i = 0; i < 64; i++) { g_bgpal[i] = 0xFF; g_objpal[i] = 0xFF; }
    lcd_on = 1;
    line_start = 0;
    ppu_phase = 0;
    ppu_mode = 2;
    div_base = timer_last = 0;
    g_deadline = 0;
}

/* speed switch (stop with KEY1 bit 0 set) */
void hw_stop(void)
{
    if (g_cgb && (g_io[rKEY1] & 1)) {
        hw_update();
        uint64_t elapsed = g_cyc - line_start;
        g_double_speed ^= 1;
        g_io[rKEY1] = 0;
        /* keep the position within the line */
        line_start = g_double_speed ? g_cyc - elapsed * 2 : g_cyc - elapsed / 2;
        g_deadline = g_cyc;
    }
}

void hw_state(FILE *f, int save)
{
    ST(g_bgpal); ST(g_objpal); ST(line_start); ST(ppu_mode); ST(ppu_phase); ST(win_line);
    ST(stat_line); ST(lcd_on); ST(hdma_active); ST(hdma_src); ST(hdma_dst); ST(hdma_left);
    ST(div_base); ST(timer_last); ST(serial_done);
}
