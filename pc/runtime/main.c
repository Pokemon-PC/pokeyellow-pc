/* Entry point, dispatch loop and SDL2 host (window, input, audio, pacing).
 *
 * Environment variables (tests and debugging):
 *   GB_MUTE=1            no sound          GB_FAST=1        no frame pacing
 *   GB_HEADLESS=1        no window         GB_FRAMES=n      quit after n frames
 *   GB_SHOT=f:file.bmp,f2:file2.bmp        screenshots at given frames
 *   GB_INPUT=f=keys;f2=keys                held buttons from frame f (a b s=select t=start u d l r)
 *   GB_INTERP=1          run everything in the interpreter (reference)
 *   GB_SAVE=path         save file (default: next to the executable)
 *   GB_TRACE=1           log interpreter fallbacks as they happen
 */
#include "hw.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_quit;
extern int g_mute;
void hw_stop(void);
unsigned apu_buffered(void);
const bank_fn *rc_banks;
unsigned rc_nbanks;
const RamCode *rc_ram;
unsigned rc_nram;
const char *rc_game_name = "game";

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_GameController *pad;
static int headless, fast, use_interp, audio_ok;
static uint64_t frame_no, max_frames;
static uint8_t buttons, script_buttons;
static int have_script;
static char *shots, *inputs;
static uint64_t perf_next;
static FILE *g_logfile;
static uint64_t state_save_at;
static int state_pending;
static int watch_pc = -1;
static uint64_t itrace_from, itrace_to;   /* GB_WATCHPC=hex: log registers when the dispatcher is at this pc */
static char state_save_path[512];

void host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_logfile) { va_start(ap, fmt); vfprintf(g_logfile, fmt, ap); va_end(ap); fflush(g_logfile); }
}

uint8_t host_buttons(void) { return have_script ? (uint8_t)(script_buttons | buttons) : buttons; }

/* ---------------------------------------------------------------- misses */
#define MAXMISS 4096
static uint32_t misses[MAXMISS];
static int nmiss;
static void note_miss(uint16_t pc)
{
    unsigned bank = pc < 0x4000 ? 0 : pc < 0x8000 ? g_rom_bank : 0xFFFF;
    uint32_t key = (bank << 16) | pc;
    for (int i = 0; i < nmiss; i++) if (misses[i] == key) return;
    if (nmiss < MAXMISS) misses[nmiss++] = key;
    if (pc < 0x8000) host_log("rc: unknown entry %02X:%04X (interpreted)\n", bank, pc);
    else host_log("rc: code in RAM at %04X (interpreted)\n", pc);
    FILE *f = fopen("rc_missing.txt", "a");
    if (f) { if (pc < 0x8000) fprintf(f, "%02X:%04X\n", bank, pc); fclose(f); }
}

void interp_invalid(uint16_t pc, uint8_t op)
{
    static int once;
    if (!once++) host_log("cpu: invalid opcode %02X at %04X (bank %02X) - locked up\n", op, pc, g_rom_bank);
}

/* ---------------------------------------------------------------- input / video */
static void apply_script(void)
{
    if (!inputs) return;
    const char *p = inputs;
    while (*p) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        if (*end != '=') break;
        const char *k = end + 1;
        uint8_t b = 0;
        while (*k && *k != ';') {
            switch (*k) {
            case 'a': b |= 1; break; case 'b': b |= 2; break; case 's': b |= 4; break; case 't': b |= 8; break;
            case 'r': b |= 16; break; case 'l': b |= 32; break; case 'u': b |= 64; break; case 'd': b |= 128; break;
            }
            k++;
        }
        if (f == frame_no) script_buttons = b;
        p = *k ? k + 1 : k;
    }
}

static void save_bmp(const uint32_t *fb, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)fb, 160, 144, 32, 160 * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); host_log("shot: %s (frame %llu)\n", path, (unsigned long long)frame_no); }
}

static void poll_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) g_quit = 1;
        else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
        else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11 && win) {
            Uint32 fl = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(win, fl ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE]) b |= 1;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_LCTRL]) b |= 2;
    if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) b |= 4;
    if (k[SDL_SCANCODE_RETURN]) b |= 8;
    if (k[SDL_SCANCODE_RIGHT]) b |= 16;
    if (k[SDL_SCANCODE_LEFT]) b |= 32;
    if (k[SDL_SCANCODE_UP]) b |= 64;
    if (k[SDL_SCANCODE_DOWN]) b |= 128;
    if (pad) {
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= 1;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) b |= 2;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) b |= 4;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) b |= 8;
        int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) b |= 16;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) b |= 32;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) b |= 64;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) b |= 128;
    }
    /* opposite directions cannot be pressed together on the real pad */
    if ((b & 48) == 48) b &= ~48;
    if ((b & 192) == 192) b &= ~192;
    buttons = b;
    fast = getenv("GB_FAST") || k[SDL_SCANCODE_TAB];
}

void host_frame(const uint32_t *fb)
{
    frame_no++;
    apply_script();
    if (shots) {
        char key[32];
        snprintf(key, sizeof key, "%llu:", (unsigned long long)frame_no);
        char *p = strstr(shots, key);
        if (p && (p == shots || p[-1] == ',')) {
            char path[512];
            p += strlen(key);
            size_t n = strcspn(p, ",");
            if (n >= sizeof path) n = sizeof path - 1;
            memcpy(path, p, n); path[n] = 0;
            save_bmp(fb, path);
        }
    }
    if (getenv("GB_PCLOG"))
        host_log("frame %llu pc=%04X bank=%02X sp=%04X lcdc=%02X ie=%02X if=%02X ime=%d halt=%d ly=%d\n",
                 (unsigned long long)frame_no, R.pc, g_rom_bank, R.sp, g_io[0x40], g_io[0xFF], g_io[0x0F],
                 R.ime, R.halted, g_io[0x44]);
    if (max_frames && frame_no >= max_frames) g_quit = 1;
    if (state_save_at && frame_no == state_save_at) state_pending = 1;
    /* GB_STATE_WHEN=ADDR:VAL:path (hex): save a state and quit once memory[ADDR] == VAL */
    static int state_when_done;
    if (getenv("GB_STATE_WHEN") && !state_pending && !state_when_done) {
        const char *v = getenv("GB_STATE_WHEN");
        char *e;
        unsigned addr = (unsigned)strtoul(v, &e, 16), val = (unsigned)strtoul(e + 1, &e, 16);
        if (rd((uint16_t)addr) == val) {
            snprintf(state_save_path, sizeof state_save_path, "%s", e + 1);
            state_pending = 1;
            state_when_done = 1;
            max_frames = frame_no + 1;
        }
    }
    if (headless) return;
    poll_events();
    SDL_UpdateTexture(tex, NULL, fb, 160 * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    if (fast) { perf_next = 0; return; }
    /* pacing: by the audio queue when there is sound, else by the clock */
    if (audio_ok && !g_mute) {
        while (apu_buffered() > 2048 && !g_quit) SDL_Delay(1);
    } else {
        uint64_t freq = SDL_GetPerformanceFrequency();
        uint64_t per = (uint64_t)(freq / 59.7275);
        uint64_t now = SDL_GetPerformanceCounter();
        if (!perf_next || now > perf_next + per * 4) perf_next = now;
        while ((now = SDL_GetPerformanceCounter()) < perf_next) {
            uint64_t left = (perf_next - now) * 1000 / freq;
            if (left > 1) SDL_Delay((Uint32)(left - 1));
        }
        perf_next += per;
    }
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    apu_mix((int16_t *)stream, len / 4);
}

/* ---------------------------------------------------------------- run loop */
static void service_interrupts(void)
{
    uint8_t pend = g_io[rIF] & g_io[rIE] & 0x1F;
    if (!pend) return;
    R.halted = 0;
    if (!R.ime) return;
    int bit = 0;
    while (!(pend & (1 << bit))) bit++;
    g_io[rIF] &= (uint8_t)~(1 << bit);
    R.ime = 0;
    R.sp -= 2;
    wr(R.sp, (uint8_t)R.pc);
    wr((uint16_t)(R.sp + 1), R.pc >> 8);
    R.pc = (uint16_t)(0x40 + bit * 8);
    g_cyc += 20;
}

/* ---- differential test (GB_DIFF=1): every translated block is replayed by
 * the interpreter from the same machine state and the results compared. ---- */
extern uint8_t *st_buf;
extern size_t st_pos, st_cap;
/* the APU comes last and is left out of the comparison: its sample clock is
 * caught up at different points, which is harmless */
static size_t cmp_len;
#define ST_SNAP_ALL(sv) do { FILE *f = NULL; int save = (sv); ST(frame_no); \
    mem_state(NULL, save); hw_state(NULL, save); cmp_len = st_pos; apu_state(NULL, save); } while (0)
static int diff_mode;
static uint64_t diff_blocks, diff_errors;

static size_t snap(uint8_t **dst)
{
    st_pos = 0;
    ST_SNAP_ALL(1);
    *dst = realloc(*dst, st_pos);
    memcpy(*dst, st_buf, st_pos);
    return st_pos;
}

static void restore(const uint8_t *src, size_t len)
{
    if (st_cap < len) { st_cap = len; st_buf = realloc(st_buf, st_cap); }
    memcpy(st_buf, src, len);
    st_pos = 0;
    ST_SNAP_ALL(0);
}

static void diff_block(bank_fn f, uint16_t pc)
{
    static uint8_t *s0, *s1, *s2;
    size_t n0 = snap(&s0);
    g_deadline = g_cyc;                 /* leave at the first taken branch */
    uint16_t pc1 = f(pc);
    if (g_rc_miss) { restore(s0, n0); return; }   /* caller handles the miss */
    R.pc = pc1;
    uint64_t cyc1 = g_cyc;
    size_t n1 = snap(&s1);
    restore(s0, n0);
    g_deadline = g_cyc;
    unsigned steps = 0;
    while (g_cyc < cyc1 && steps < 100000) { interp_step(); steps++; }
    size_t n2 = snap(&s2);
    diff_blocks++;
    if (n1 != n2 || memcmp(s1, s2, cmp_len)) {
        size_t k = 0;
        while (k < n1 && s1[k] == s2[k]) k++;
        if (diff_errors++ < 20) {
            CPU a, b;
            memcpy(&a, s1 + sizeof frame_no, sizeof a);
            memcpy(&b, s2 + sizeof frame_no, sizeof b);
            host_log("DIFF block %02X:%04X (frame %llu): first difference at state offset %zu (rc %02X, interp %02X)\n",
                     pc < 0x4000 ? 0 : g_rom_bank, pc, (unsigned long long)frame_no, k, s1[k], s2[k]);
            host_log("  rc:     pc=%04X a=%02X bc=%02X%02X de=%02X%02X hl=%02X%02X sp=%04X znhc=%d%d%d%d\n",
                     a.pc, a.a, a.b, a.c, a.d, a.e, a.h, a.l, a.sp, a.zf, a.nf, a.hf, a.cf);
            host_log("  interp: pc=%04X a=%02X bc=%02X%02X de=%02X%02X hl=%02X%02X sp=%04X znhc=%d%d%d%d\n",
                     b.pc, b.a, b.b, b.c, b.d, b.e, b.h, b.l, b.sp, b.zf, b.nf, b.hf, b.cf);
        }
    }
    restore(s1, n1);                    /* go on with the translated result */
    g_deadline = 0;
}

/* translated RAM routine for pc, if the RAM still holds its ROM bytes */
static bank_fn ram_code(uint16_t pc)
{
    for (unsigned i = 0; i < rc_nram; i++) {
        const RamCode *r = &rc_ram[i];
        if (pc < r->ram || pc >= r->ram + r->len) continue;
        int same = 1;
        for (unsigned k = 0; k < r->len && same; k++)
            same = rd((uint16_t)(r->ram + k)) == g_rom[r->rom_off + k];
        if (same) return r->fn;
    }
    return NULL;
}

static void state_file(const char *path, int save)
{
    FILE *f = fopen(path, save ? "wb" : "rb");
    if (!f) { host_log("state: cannot open %s\n", path); return; }
    uint32_t magic = 0x47425354;
    ST(magic);
    ST(frame_no);
    mem_state(f, save);
    hw_state(f, save);
    apu_state(f, save);
    fclose(f);
    host_log("state: %s %s (frame %llu)\n", save ? "saved" : "loaded", path, (unsigned long long)frame_no);
}

static void run(void)
{
    while (!g_quit) {
        if (g_cyc >= g_deadline) hw_update();
        if (state_pending) { state_pending = 0; state_file(state_save_path, 1); }
        service_interrupts();
        if (R.halted) {
            if (g_deadline > g_cyc) g_cyc = g_deadline;
            continue;
        }
        if (R.stopped) { R.stopped = 0; hw_stop(); continue; }
        uint16_t pc = R.pc;
        bank_fn f = NULL;
        if (watch_pc >= 0 && pc == watch_pc)
            host_log("watch %04X frame %llu bank=%02X a=%02X bc=%02X%02X de=%02X%02X hl=%02X%02X sp=%04X f=%d%d%d%d\n",
                     pc, (unsigned long long)frame_no, g_rom_bank, R.a, R.b, R.c, R.d, R.e, R.h, R.l, R.sp,
                     R.zf, R.nf, R.hf, R.cf);
        if (!use_interp) {
            if (pc < 0x4000) f = rc_banks[0];
            else if (pc < 0x8000 && g_rom_bank < rc_nbanks) f = rc_banks[g_rom_bank];
            else if (pc >= 0x8000) f = ram_code(pc);
        }
        if (f) {
            if (diff_mode) diff_block(f, pc);
            else R.pc = f(pc);
            if (g_rc_miss) {
                g_rc_miss = 0;
                note_miss(pc);
                interp_step();
            }
        } else {
            if (!use_interp && pc >= 0x8000) {
                /* code in RAM: interpret until we are back in ROM */
                static uint16_t last_ram_pc = 0;
                if (pc != last_ram_pc) { note_miss(pc); last_ram_pc = pc; }
            } else if (!use_interp) {
                note_miss(pc);
            }
            if (use_interp && itrace_to) {
                /* debug: GB_ITRACE=from:to - every interpreted instruction with its registers */
                static FILE *tf;
                if (frame_no >= itrace_from && frame_no <= itrace_to) {
                    if (!tf) tf = fopen("itrace.txt", "w");
                    fprintf(tf, "%llu %02X:%04X a=%02X bc=%02X%02X de=%02X%02X hl=%02X%02X sp=%04X f=%d%d%d%d ime=%d\n",
                            (unsigned long long)frame_no, g_rom_bank, pc, R.a, R.b, R.c, R.d, R.e, R.h, R.l, R.sp,
                            R.zf, R.nf, R.hf, R.cf, R.ime);
                } else if (tf && frame_no > itrace_to) { fclose(tf); tf = NULL; itrace_to = 0; }
            }
            interp_step();
        }
    }
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* ---- ROM search in a folder (rom_dir): by content, not by name ---- */
#ifdef _WIN32
#include <windows.h>
static int list_dir(const char *dir, char (*names)[1024], int max)
{
    char pat[1100];
    wchar_t wpat[1100];
    snprintf(pat, sizeof pat, "%s%s*", dir, (*dir && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\') ? "\\" : "");
    MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, 1100);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[700];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        snprintf(names[n++], 1024, "%s%s%s", dir,
                 (*dir && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\') ? "\\" : "", name);
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
#else
#include <dirent.h>
static int list_dir(const char *dir, char (*names)[1024], int max)
{
    DIR *d = opendir(*dir ? dir : ".");
    int n = 0;
    if (!d) return 0;
    for (struct dirent *e; n < max && (e = readdir(d)); ) {
        if (e->d_name[0] == '.') continue;
        snprintf(names[n++], 1024, "%s%s%s", dir, (*dir && dir[strlen(dir) - 1] != '/') ? "/" : "", e->d_name);
    }
    closedir(d);
    return n;
}
#endif

static uint32_t crc32_buf(const uint8_t *p, size_t n);

/* first translated ROM (in rc_games order, of language `lang` if given)
 * present in `dir`; candidates are files of a Game Boy ROM size */
static int find_rom(const char *dir, const char *lang, char *out, size_t outsz)
{
    static char names[256][1024];
    uint32_t crcs[256];
    int n = list_dir(dir, names, 256), m = 0;
    for (int i = 0; i < n; i++) {
        SDL_RWops *f = SDL_RWFromFile(names[i], "rb");
        if (!f) continue;
        Sint64 sz = SDL_RWsize(f);
        SDL_RWclose(f);
        if (sz < 0x8000 || sz > 0x800000 || (sz & 0x3FFF)) continue;
        size_t len = 0;
        uint8_t *d = SDL_LoadFile(names[i], &len);
        if (!d) continue;
        crcs[m] = crc32_buf(d, len);
        SDL_free(d);
        if (m != i) memcpy(names[m], names[i], sizeof names[0]);
        m++;
    }
    for (int g = 0; rc_games[g]; g++) {
        if (lang && *lang && strcmp(rc_games[g]->lang, lang)) continue;
        for (int i = 0; i < m; i++)
            if (crcs[i] == rc_games[g]->crc) {
                snprintf(out, outsz, "%s", names[i]);
                return 1;
            }
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* Configuration, for the launcher: command line
     *   [--lang en|fr] [--fullscreen] [--scale N] [--save file.sav] [--rom-dir DIR] [rom]
     * then <exe dir>/GB_CONFIG_NAME (lines key=value: lang, rom_en, rom_fr,
     * rom, rom_dir, fullscreen, scale, save, palette). The command line wins.
     * ROM search: explicit path (argument, rom=, rom_<lang>=), then the
     * executable's folder, then rom_dir (--rom-dir, GB_ROM_DIR), choosing the
     * file by its content (CRC), never by its name. */
    static char cfg_rom[1024], cfg_lang[16], cfg_save[1024], cfg_palette[16] = "gbc", cfg_romdir[1024];
    int fullscreen = 0, scale = 4;
    {
        char *base = SDL_GetBasePath();
        char path[1200];
        snprintf(path, sizeof path, "%s%s", base ? base : "", GB_CONFIG_NAME);
        SDL_free(base);
        size_t len = 0;
        char *txt = SDL_LoadFile(path, &len);
        char roms[4][2][1024] = { { { 0 } } };   /* (lang, path) */
        int nroms = 0;
        for (char *line = txt ? strtok(txt, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#' || line[0] == ';') continue;
            *eq = 0;
            char *k = line, *v = eq + 1;
            /* trim spaces; "value  # comment" -> "value" (a '#' or ';' after a space) */
            for (char *c = v; *c; c++)
                if ((*c == '#' || *c == ';') && c > v && (c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '"'); ) *--e = 0;
            if (*v == '"') v++;
            if (!strcmp(k, "lang")) snprintf(cfg_lang, sizeof cfg_lang, "%s", v);
            else if (!strcmp(k, "rom")) snprintf(cfg_rom, sizeof cfg_rom, "%s", v);
            else if (!strcmp(k, "rom_dir")) snprintf(cfg_romdir, sizeof cfg_romdir, "%s", v);
            else if (!strncmp(k, "rom_", 4) && nroms < 4) {
                snprintf(roms[nroms][0], sizeof roms[nroms][0], "%s", k + 4);
                snprintf(roms[nroms][1], sizeof roms[nroms][1], "%s", v);
                nroms++;
            } else if (!strcmp(k, "fullscreen")) fullscreen = atoi(v);
            else if (!strcmp(k, "scale")) scale = atoi(v);
            else if (!strcmp(k, "save")) snprintf(cfg_save, sizeof cfg_save, "%s", v);
            else if (!strcmp(k, "palette")) snprintf(cfg_palette, sizeof cfg_palette, "%s", v);
        }
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--lang") && i + 1 < argc) snprintf(cfg_lang, sizeof cfg_lang, "%s", argv[++i]);
            else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
            else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(cfg_save, sizeof cfg_save, "%s", argv[++i]);
            else if (!strcmp(argv[i], "--rom-dir") && i + 1 < argc) snprintf(cfg_romdir, sizeof cfg_romdir, "%s", argv[++i]);
            else if (argv[i][0] != '-') snprintf(cfg_rom, sizeof cfg_rom, "%s", argv[i]);
        }
        if (getenv("GB_ROM_DIR") && !cfg_romdir[0]) snprintf(cfg_romdir, sizeof cfg_romdir, "%s", getenv("GB_ROM_DIR"));
        if (!cfg_rom[0]) {
            for (int i = 0; i < nroms; i++)
                if (!cfg_lang[0] || !strcmp(cfg_lang, roms[i][0])) {
                    snprintf(cfg_rom, sizeof cfg_rom, "%s", roms[i][1]);
                    break;
                }
        }
        /* no explicit ROM: look in the executable's folder, then in rom_dir,
         * for a file whose content is one of the translated ROMs (CRC of the
         * whole ROM: version and language), whatever its name */
        if (!cfg_rom[0] && !getenv("GB_ROM")) {
            char *exedir = SDL_GetBasePath();
            if (!find_rom(exedir ? exedir : "", cfg_lang, cfg_rom, sizeof cfg_rom) && cfg_romdir[0])
                find_rom(cfg_romdir, cfg_lang, cfg_rom, sizeof cfg_rom);
            SDL_free(exedir);
        }
        SDL_free(txt);
        if (scale < 1) scale = 1;
        if (cfg_save[0] && !getenv("GB_SAVE")) SDL_setenv("GB_SAVE", cfg_save, 1);
    }
    const char *rom_path = cfg_rom[0] ? cfg_rom : getenv("GB_ROM");
    g_logfile = fopen("gb_log.txt", "w");
    if (!rom_path) {
        const char *msg =
            "Aucune ROM trouvee / No ROM found.\n\n"
            "Posez votre ROM a cote du jeu, ou indiquez le dossier de vos ROM dans " GB_CONFIG_NAME
            " (le bon fichier est reconnu a son contenu, quel que soit son nom) :\n"
            "  rom_dir=C:\\...\\roms\n  lang=fr\n"
            "ou un chemin precis : rom_fr=C:\\...\\rom.gbc, ou en argument.\n";
        char full[1500];
        if (cfg_lang[0])
            snprintf(full, sizeof full, "Langue demandee : \"%s\" (rom_dir : \"%s\")\n\n%s", cfg_lang, cfg_romdir, msg);
        else
            snprintf(full, sizeof full, "%s", msg);
        host_log("%s", full);
        if (!getenv("GB_HEADLESS")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Pokemon", full, NULL);
        return 1;
    }
    /* SDL_RWops: UTF-8 paths on Windows too */
    size_t rom_len = 0;
    g_rom = SDL_LoadFile(rom_path, &rom_len);
    if (!g_rom) { host_log("cannot open %s\n", rom_path); return 1; }
    g_rom_size = rom_len;
    uint32_t crc = crc32_buf(g_rom, g_rom_size);
    const RcGame *game = NULL;
    for (int i = 0; rc_games[i]; i++)
        if (rc_games[i]->crc == crc) game = rc_games[i];
    if (game) {
        rc_banks = game->banks; rc_nbanks = game->nbanks;
        rc_ram = game->ram; rc_nram = game->nram;
        rc_game_name = game->name;
        host_log("ROM: %s (%s)\n", game->name, game->lang);
    } else {
        /* unknown ROM (another revision, a hack): everything is interpreted */
        host_log("warning: ROM CRC %08X is not one of the translated ROMs; running it in the interpreter\n", crc);
        for (int i = 0; rc_games[i]; i++)
            host_log("  translated: %s (%s) CRC %08X\n", rc_games[i]->name, rc_games[i]->lang, rc_games[i]->crc);
        use_interp = 1;
        rc_game_name = rc_games[0] ? rc_games[0]->name : "game";
    }

    headless = getenv("GB_HEADLESS") != NULL;
    if (getenv("GB_INTERP")) use_interp = 1;
    diff_mode = getenv("GB_DIFF") != NULL;
    if (getenv("GB_ITRACE")) {
        itrace_from = strtoull(getenv("GB_ITRACE"), NULL, 10);
        const char *c = strchr(getenv("GB_ITRACE"), ':');
        itrace_to = c ? strtoull(c + 1, NULL, 10) : itrace_from;
    }
    if (getenv("GB_WATCHPC")) watch_pc = (int)strtol(getenv("GB_WATCHPC"), NULL, 16);
    g_mute = getenv("GB_MUTE") != NULL;
    shots = getenv("GB_SHOT");
    inputs = getenv("GB_INPUT");
    have_script = inputs != NULL;
    if (getenv("GB_FRAMES")) max_frames = strtoull(getenv("GB_FRAMES"), NULL, 10);
    fast = getenv("GB_FAST") != NULL;

    if (SDL_Init(SDL_INIT_TIMER | (headless ? 0 : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) != 0) {
        host_log("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int rate = 48000;
    if (!headless) {
        char title[128];
        snprintf(title, sizeof title, "Pok\xC3\xA9mon %s", rc_game_name);
        if (title[9] >= 'a' && title[9] <= 'z') title[9] -= 32;   /* "Pokémon Crystal" */
        win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 160 * scale, 144 * scale,
                               SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        SDL_RenderSetLogicalSize(ren, 160, 144);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 160, 144);
        SDL_AudioSpec want = { 0 }, have;
        want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        want.callback = audio_cb;
        SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (dev) { audio_ok = 1; rate = have.freq; SDL_PauseAudioDevice(dev, 0); }
        for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
            if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
    }

    int cgb = (g_rom[0x143] & 0x80) != 0;
    mem_init();
    hw_init(cgb);
    if (!cgb && strcmp(cfg_palette, "green")) {
        /* monochrome game: colours of the Game Boy Color boot ROM for it */
        static const uint32_t white_red[4] = { 0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000 };
        static const uint32_t green[4] = { 0xFFFFFF, 0x7BFF31, 0x008400, 0x000000 };
        static const uint32_t blue[4] = { 0xFFFFFF, 0x63A5FF, 0x0000FF, 0x000000 };
        static const uint32_t gray[4] = { 0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000 };
        char title[17] = { 0 };
        memcpy(title, g_rom + 0x134, 16);
        if (!strcmp(cfg_palette, "gray")) ppu_set_dmg_palettes(gray, gray, gray);
        else if (strstr(title, "BLUE")) ppu_set_dmg_palettes(blue, white_red, blue);
        else if (strstr(title, "RED")) ppu_set_dmg_palettes(white_red, green, white_red);
        else ppu_set_dmg_palettes(gray, gray, gray);
    }
    apu_init(rate);
    char save[1024];
    if (getenv("GB_SAVE")) snprintf(save, sizeof save, "%s", getenv("GB_SAVE"));
    else {
        char *base = SDL_GetBasePath();
        snprintf(save, sizeof save, "%spokemon_%s.sav", base ? base : "", rc_game_name);
        SDL_free(base);
    }
    cart_load_save(save);
    if (getenv("GB_STATE_SAVE")) {   /* frame:path */
        const char *v = getenv("GB_STATE_SAVE");
        state_save_at = strtoull(v, NULL, 10);
        const char *c = strchr(v, ':');
        snprintf(state_save_path, sizeof state_save_path, "%s", c ? c + 1 : "state.bin");
    }

    /* CGB boot ROM hand-off state */
    R.a = cgb ? 0x11 : 0x01; R.zf = 1; R.nf = 0; R.hf = 0; R.cf = 0;
    R.b = 0; R.c = 0x13; R.d = 0; R.e = 0xD8; R.h = 0x01; R.l = 0x4D;
    R.sp = 0xFFFE; R.pc = 0x0100;
    if (getenv("GB_STATE_LOAD")) state_file(getenv("GB_STATE_LOAD"), 0);

    run();

    cart_flush_save();
    if (g_interp_count && !diff_mode)
        host_log("interpreter: %llu instructions\n", (unsigned long long)g_interp_count);
    if (diff_mode)
        host_log("diff: %llu blocks checked, %llu differences\n", (unsigned long long)diff_blocks,
                 (unsigned long long)diff_errors);
    SDL_Quit();
    return 0;
}
