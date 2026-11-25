#pragma once
#include "gb.h"

extern uint8_t g_vram[2][0x2000];
extern uint8_t g_wram[8][0x1000];
extern uint8_t g_oam[0xA0];
extern uint8_t g_sram[16][0x2000];
extern uint8_t g_bgpal[64], g_objpal[64];
extern uint32_t g_fb[160 * 144];

void mem_remap(void);
void ppu_render_line(int ly, int *win_line);
void ppu_io_write(uint8_t reg, uint8_t v);

/* register numbers (FF00 + n) */
enum {
    rP1 = 0x00, rSB = 0x01, rSC = 0x02, rDIV = 0x04, rTIMA = 0x05, rTMA = 0x06, rTAC = 0x07,
    rIF = 0x0F, rLCDC = 0x40, rSTAT = 0x41, rSCY = 0x42, rSCX = 0x43, rLY = 0x44, rLYC = 0x45,
    rDMA = 0x46, rBGP = 0x47, rOBP0 = 0x48, rOBP1 = 0x49, rWY = 0x4A, rWX = 0x4B, rKEY1 = 0x4D,
    rVBK = 0x4F, rHDMA1 = 0x51, rHDMA2 = 0x52, rHDMA3 = 0x53, rHDMA4 = 0x54, rHDMA5 = 0x55,
    rRP = 0x56, rBCPS = 0x68, rBCPD = 0x69, rOCPS = 0x6A, rOCPD = 0x6B, rSVBK = 0x70, rIE = 0xFF,
};
void ppu_set_dmg_palettes(const uint32_t bg[4], const uint32_t obj0[4], const uint32_t obj1[4]);
