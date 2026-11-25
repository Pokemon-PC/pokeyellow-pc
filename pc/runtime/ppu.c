/* Scanline renderer (CGB and DMG modes). Called at the end of mode 3 of
 * each visible line with the registers as they are at that moment, so
 * mid-frame register changes (raster effects) are honoured. */
#include "hw.h"

static uint32_t cgb_color(const uint8_t *pal, int idx)
{
    unsigned c = pal[idx * 2] | (pal[idx * 2 + 1] << 8);
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* monochrome games: colours given to them the way the Game Boy Color boot
 * ROM does (one palette for the background, two for sprites); default green */
static uint32_t dmg_pal[3][4] = {
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 },
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 },
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 },
};

void ppu_set_dmg_palettes(const uint32_t bg[4], const uint32_t obj0[4], const uint32_t obj1[4])
{
    for (int i = 0; i < 4; i++) {
        dmg_pal[0][i] = 0xFF000000u | bg[i];
        dmg_pal[1][i] = 0xFF000000u | obj0[i];
        dmg_pal[2][i] = 0xFF000000u | obj1[i];
    }
}

static inline int tile_pixel(int bank, unsigned tile_addr, int row, int col)
{
    const uint8_t *t = &g_vram[bank][tile_addr + row * 2];
    int bit = 7 - col;
    return ((t[0] >> bit) & 1) | (((t[1] >> bit) & 1) << 1);
}

void ppu_render_line(int ly, int *win_line)
{
    uint8_t lcdc = g_io[rLCDC];
    uint32_t *out = &g_fb[ly * 160];
    uint8_t bgcol[160], bgpri[160], bgpal[160];
    int cgb = g_cgb;

    /* ---- background and window ---- */
    int bg_on = cgb ? 1 : (lcdc & 1);
    int wx = g_io[rWX] - 7, wy = g_io[rWY];
    int win = (lcdc & 0x20) && ly >= wy && g_io[rWX] <= 166 && (cgb || (lcdc & 1));
    int scx = g_io[rSCX], scy = g_io[rSCY];
    for (int x = 0; x < 160; x++) {
        int use_win = win && x >= wx;
        unsigned map;
        int px, py;
        if (use_win) {
            map = (lcdc & 0x40) ? 0x1C00 : 0x1800;
            px = x - wx;
            py = *win_line;
        } else {
            map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
            px = (scx + x) & 255;
            py = (scy + ly) & 255;
        }
        if (!bg_on && !use_win) { bgcol[x] = 0; bgpri[x] = 0; bgpal[x] = 0; continue; }
        unsigned mi = map + (py >> 3) * 32 + (px >> 3);
        uint8_t idx = g_vram[0][mi];
        uint8_t attr = cgb ? g_vram[1][mi] : 0;
        unsigned ta = (lcdc & 0x10) ? idx * 16u : (unsigned)(0x1000 + (int8_t)idx * 16);
        int row = py & 7, col = px & 7;
        if (attr & 0x40) row = 7 - row;
        if (attr & 0x20) col = 7 - col;
        bgcol[x] = (uint8_t)tile_pixel((attr >> 3) & 1, ta, row, col);
        bgpri[x] = attr >> 7;
        bgpal[x] = attr & 7;
    }
    if (win && wx < 160) (*win_line)++;

    for (int x = 0; x < 160; x++) {
        if (cgb) out[x] = cgb_color(g_bgpal, bgpal[x] * 4 + bgcol[x]);
        else out[x] = dmg_pal[0][(g_io[rBGP] >> (bgcol[x] * 2)) & 3];
    }

    /* ---- sprites ---- */
    if (!(lcdc & 0x02)) return;
    int hgt = (lcdc & 0x04) ? 16 : 8;
    int list[10], n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int sy = g_oam[i * 4] - 16;
        if (ly >= sy && ly < sy + hgt) list[n++] = i;
    }
    if (!cgb) {
        /* DMG: smaller x first, then lower index */
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && g_oam[list[j] * 4 + 1] < g_oam[list[j - 1] * 4 + 1]; j--) {
                int t = list[j]; list[j] = list[j - 1]; list[j - 1] = t;
            }
    }
    uint8_t drawn[160] = { 0 };
    for (int k = 0; k < n; k++) {
        const uint8_t *s = &g_oam[list[k] * 4];
        int sy = s[0] - 16, sx = s[1] - 8;
        uint8_t tile = s[2], attr = s[3];
        int row = ly - sy;
        if (attr & 0x40) row = hgt - 1 - row;
        if (hgt == 16) tile &= 0xFE;
        unsigned ta = tile * 16u + (row >= 8 ? 16 : 0);
        row &= 7;
        int bank = cgb ? (attr >> 3) & 1 : 0;
        for (int c = 0; c < 8; c++) {
            int x = sx + c;
            if (x < 0 || x >= 160 || drawn[x]) continue;
            int col = (attr & 0x20) ? 7 - c : c;
            int ci = tile_pixel(bank, ta, row, col);
            if (!ci) continue;
            drawn[x] = 1;
            if (cgb) {
                if ((lcdc & 1) && bgcol[x] && ((attr & 0x80) || bgpri[x])) continue;
                out[x] = cgb_color(g_objpal, (attr & 7) * 4 + ci);
            } else {
                if ((attr & 0x80) && bgcol[x]) continue;
                uint8_t pal = (attr & 0x10) ? g_io[rOBP1] : g_io[rOBP0];
                out[x] = dmg_pal[(attr & 0x10) ? 2 : 1][(pal >> (ci * 2)) & 3];
            }
        }
    }
}
