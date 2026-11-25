/* APU: two square channels (one with sweep), wave channel, noise channel,
 * frame sequencer, stereo mixer. Samples are produced in the emulation
 * thread (apu_catch_up) into a ring buffer that the host audio callback
 * drains (apu_mix). */
#include "hw.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

#define APU_HZ 4194304.0

typedef struct {
    int on, dac;
    int length, len_en;
    int vol, env_dir, env_period, env_timer;
    int freq, timer, pos, duty;
    /* sweep (ch1) */
    int sw_period, sw_neg, sw_shift, sw_timer, sw_en, sw_shadow;
    /* noise */
    unsigned lfsr; int width7;
} Chan;

static Chan ch[4];
static uint8_t regs[0x30];
static uint8_t wave[16];
static int power = 1;
static int fs_step;
static double fs_acc;
static double cyc_per_sample;
static double sample_acc;
static uint64_t last_cyc;
static float hp_l, hp_r;
static int out_rate;

/* ring buffer (stereo int16) */
#define RING (1 << 15)
static int16_t ring[RING * 2];
static volatile unsigned ring_w, ring_r;
int g_mute;
static FILE *wav;
static uint32_t wav_frames;
static void wav_close(void);

static const uint8_t duty_tab[4] = { 0x01, 0x81, 0x87, 0x7E };
static const int noise_div[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

static int sq_period(Chan *c) { return (2048 - c->freq) * 4; }
static int wave_period(Chan *c) { return (2048 - c->freq) * 2; }
static int noise_period(void) { uint8_t r = regs[0x22]; return noise_div[r & 7] << (r >> 4); }

static int ch_out(int i)
{
    Chan *c = &ch[i];
    if (!c->on || !c->dac) return -1;
    switch (i) {
    case 0: case 1: return ((duty_tab[c->duty] >> (7 - c->pos)) & 1) ? c->vol : 0;
    case 2: {
        int s = wave[c->pos >> 1];
        s = (c->pos & 1) ? (s & 15) : (s >> 4);
        int shift = (regs[0x1C] >> 5) & 3;
        return shift ? s >> (shift - 1) : 0;
    }
    case 3: return (c->lfsr & 1) ? 0 : c->vol;
    }
    return 0;
}

static void ch_advance(int i)
{
    Chan *c = &ch[i];
    switch (i) {
    case 0: case 1: c->pos = (c->pos + 1) & 7; c->timer = sq_period(c); break;
    case 2: c->pos = (c->pos + 1) & 31; c->timer = wave_period(c); break;
    case 3: {
        unsigned x = (c->lfsr ^ (c->lfsr >> 1)) & 1;
        c->lfsr = (c->lfsr >> 1) | (x << 14);
        if (c->width7) c->lfsr = (c->lfsr & ~0x40u) | (x << 6);
        c->timer = noise_period();
        break;
    }
    }
    if (c->timer <= 0) c->timer = 4;
}

static int sweep_calc(Chan *c)
{
    int d = c->sw_shadow >> c->sw_shift;
    int f = c->sw_neg ? c->sw_shadow - d : c->sw_shadow + d;
    if (f > 2047) c->on = 0;
    return f;
}

static void frame_seq(void)
{
    int s = fs_step;
    fs_step = (fs_step + 1) & 7;
    if (!(s & 1)) {
        for (int i = 0; i < 4; i++)
            if (ch[i].len_en && ch[i].length > 0 && --ch[i].length == 0) ch[i].on = 0;
    }
    if (s == 2 || s == 6) {
        Chan *c = &ch[0];
        if (--c->sw_timer <= 0) {
            c->sw_timer = c->sw_period ? c->sw_period : 8;
            if (c->sw_en && c->sw_period) {
                int f = sweep_calc(c);
                if (f <= 2047 && c->sw_shift) {
                    c->sw_shadow = f; c->freq = f;
                    sweep_calc(c);
                }
            }
        }
    }
    if (s == 7) {
        for (int i = 0; i < 4; i++) {
            if (i == 2) continue;
            Chan *c = &ch[i];
            if (!c->env_period) continue;
            if (--c->env_timer <= 0) {
                c->env_timer = c->env_period;
                if (c->env_dir && c->vol < 15) c->vol++;
                else if (!c->env_dir && c->vol > 0) c->vol--;
            }
        }
    }
}

static void produce(int cycles_int)
{
    /* box-filtered channel outputs over the interval */
    float acc[4] = { 0 };
    for (int i = 0; i < 4; i++) {
        Chan *c = &ch[i];
        int t = cycles_int;
        float a = 0;
        while (t > 0) {
            int dlt = c->timer < t ? c->timer : t;
            if (dlt <= 0) dlt = 1;
            int o = ch_out(i);
            a += (o < 0 ? 0 : (o / 7.5f - 1.0f)) * dlt;
            if (o < 0) a += 0;   /* DAC off: silence */
            c->timer -= dlt;
            t -= dlt;
            if (c->timer <= 0) ch_advance(i);
        }
        acc[i] = ch_out(i) < 0 && !c->dac ? 0 : a / cycles_int;
    }
    uint8_t nr50 = regs[0x24], nr51 = regs[0x25];
    float l = 0, r = 0;
    for (int i = 0; i < 4; i++) {
        if (nr51 & (0x10 << i)) l += acc[i];
        if (nr51 & (1 << i)) r += acc[i];
    }
    l *= (((nr50 >> 4) & 7) + 1) / 8.0f;
    r *= ((nr50 & 7) + 1) / 8.0f;
    if (!power) l = r = 0;
    /* DC-blocking high-pass, like the GB's output capacitor */
    float ol = l - hp_l, or_ = r - hp_r;
    hp_l = l - ol * 0.996f;
    hp_r = r - or_ * 0.996f;
    int sl = (int)(ol * 6000), sr = (int)(or_ * 6000);
    if (sl > 32767) sl = 32767; if (sl < -32768) sl = -32768;
    if (sr > 32767) sr = 32767; if (sr < -32768) sr = -32768;
    if (wav) {   /* GB_WAV: record the output (tests) */
        int16_t fr[2] = { (int16_t)sl, (int16_t)sr };
        fwrite(fr, sizeof fr, 1, wav);
        wav_frames++;
    }
    unsigned w = ring_w;
    if (((w - ring_r) & (RING - 1)) < RING - 1) {
        ring[(w & (RING - 1)) * 2] = (int16_t)(g_mute ? 0 : sl);
        ring[(w & (RING - 1)) * 2 + 1] = (int16_t)(g_mute ? 0 : sr);
        ring_w = (w + 1) & (RING - 1);
    }
}

void apu_catch_up(void)
{
    if (!out_rate) { last_cyc = g_cyc; return; }
    uint64_t now = g_cyc;
    double dt = (double)(now - last_cyc) / (g_double_speed ? 2.0 : 1.0);
    last_cyc = now;
    while (dt > 0) {
        double to_sample = cyc_per_sample - sample_acc;
        double to_fs = 8192.0 - fs_acc;
        double step = dt;
        if (to_sample < step) step = to_sample;
        if (to_fs < step) step = to_fs;
        sample_acc += step;
        fs_acc += step;
        dt -= step;
        if (fs_acc >= 8192.0) { fs_acc -= 8192.0; frame_seq(); }
        if (sample_acc >= cyc_per_sample) {
            static double owed;
            sample_acc -= cyc_per_sample;
            owed += cyc_per_sample;
            int n = (int)owed;
            owed -= n;
            produce(n);
        }
    }
}

unsigned apu_buffered(void) { return (ring_w - ring_r) & (RING - 1); }

int apu_mix(int16_t *out, int frames)
{
    int n = 0;
    while (n < frames && ring_r != ring_w) {
        out[n * 2] = ring[ring_r * 2];
        out[n * 2 + 1] = ring[ring_r * 2 + 1];
        ring_r = (ring_r + 1) & (RING - 1);
        n++;
    }
    for (int i = n; i < frames; i++) {
        out[i * 2] = n ? out[(n - 1) * 2] : 0;
        out[i * 2 + 1] = n ? out[(n - 1) * 2 + 1] : 0;
    }
    return n;
}

static void trigger(int i)
{
    Chan *c = &ch[i];
    c->on = c->dac;
    if (c->length == 0) c->length = (i == 2) ? 256 : 64;
    if (i == 2) { c->timer = wave_period(c); c->pos = 0; }
    else if (i == 3) { c->timer = noise_period(); c->lfsr = 0x7FFF; }
    else c->timer = sq_period(c);
    if (i != 2) {
        uint8_t env = regs[i == 0 ? 0x12 : i == 1 ? 0x17 : 0x21];
        c->vol = env >> 4; c->env_dir = (env >> 3) & 1; c->env_period = env & 7; c->env_timer = c->env_period;
    }
    if (i == 0) {
        c->sw_shadow = c->freq;
        c->sw_timer = c->sw_period ? c->sw_period : 8;
        c->sw_en = c->sw_period || c->sw_shift;
        if (c->sw_shift) sweep_calc(c);
    }
}

void apu_write(uint8_t r, uint8_t v)
{
    if (r >= 0x30) { wave[r - 0x30] = v; return; }
    if (!power && r != 0x26) return;
    regs[r] = v;
    Chan *c;
    switch (r) {
    case 0x10: c = &ch[0]; c->sw_period = (v >> 4) & 7; c->sw_neg = (v >> 3) & 1; c->sw_shift = v & 7; break;
    case 0x11: case 0x16:
        c = &ch[r == 0x11 ? 0 : 1]; c->duty = v >> 6; c->length = 64 - (v & 63); break;
    case 0x12: case 0x17: case 0x21:
        c = &ch[r == 0x12 ? 0 : r == 0x17 ? 1 : 3];
        c->dac = (v & 0xF8) != 0;
        if (!c->dac) c->on = 0;
        break;
    case 0x13: case 0x18: case 0x1D:
        c = &ch[r == 0x13 ? 0 : r == 0x18 ? 1 : 2]; c->freq = (c->freq & 0x700) | v; break;
    case 0x14: case 0x19: case 0x1E: case 0x23: {
        int i = r == 0x14 ? 0 : r == 0x19 ? 1 : r == 0x1E ? 2 : 3;
        c = &ch[i];
        if (i != 3) c->freq = (c->freq & 0xFF) | ((v & 7) << 8);
        c->len_en = (v >> 6) & 1;
        if (v & 0x80) trigger(i);
        break;
    }
    case 0x1A: ch[2].dac = v >> 7; if (!ch[2].dac) ch[2].on = 0; break;
    case 0x1B: ch[2].length = 256 - v; break;
    case 0x20: ch[3].length = 64 - (v & 63); break;
    case 0x22: ch[3].width7 = (v >> 3) & 1; break;
    case 0x26:
        power = v >> 7;
        if (!power) {
            memset(ch, 0, sizeof ch);
            memset(regs, 0, 0x26);
        }
        break;
    }
}

uint8_t apu_read(uint8_t r)
{
    static const uint8_t mask[0x30] = {
        0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
        0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    if (r >= 0x30) return wave[r - 0x30];
    if (r == 0x26) {
        apu_catch_up();
        uint8_t v = (uint8_t)(power ? 0x80 : 0) | 0x70;
        for (int i = 0; i < 4; i++) if (ch[i].on) v |= (uint8_t)(1 << i);
        return v;
    }
    return regs[r] | mask[r - 0x10];
}

void apu_init(int rate)
{
    memset(ch, 0, sizeof ch);
    memset(regs, 0, sizeof regs);
    out_rate = rate;
    cyc_per_sample = rate ? APU_HZ / rate : 0;
    ch[3].lfsr = 0x7FFF;
    power = 1;
    if (getenv("GB_WAV") && rate) {
        wav = fopen(getenv("GB_WAV"), "wb");
        if (wav) { uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, wav); }
        atexit(wav_close);
    }
}

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void wav_close(void)
{
    if (!wav) return;
    uint8_t h[44];
    uint32_t data = wav_frames * 4;
    memcpy(h, "RIFF", 4); put_le(h + 4, 36 + data, 4); memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4); put_le(h + 20, 1, 2); put_le(h + 22, 2, 2); put_le(h + 24, out_rate, 4);
    put_le(h + 28, out_rate * 4, 4); put_le(h + 32, 4, 2); put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4); put_le(h + 40, data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fclose(wav);
    wav = NULL;
}

void apu_state(FILE *f, int save)
{
    ST(ch); ST(regs); ST(wave); ST(power); ST(fs_step); ST(fs_acc); ST(sample_acc); ST(last_cyc);
}
