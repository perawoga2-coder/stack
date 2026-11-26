/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the effects. Each track has two slots, A and B; a slot has an effect and one knob, 0 in the middle (off,
 * FX_DEAD detents each way), the amount growing with the distance from it, each side its own flavour.
 * Written for STACK. ROBOT follows the idea of the Burger Studio app's own RobotFx (Maker Hip's Robot.kt: the sound
 * times a 50 Hz tone, and a 5 ms comb). Part of the unity build: included by app.c.
 *
 * Where each runs:
 *   on the track itself, a slot's own little state (fx_inline): OVERDRIVE, LO-FI; TAPE SPEED is the track's play rate
 *   on a bus every track shares, one a flavour (fx_buses): DELAY, ECHO, REVERB, ROBOT, FLANGER, CHORUS. A track's knob
 *     sets how much of it goes in; the flavour's strength (feedback, size, rate) follows the furthest knob among the
 *     tracks that use it. So eight tracks with two slots cost no more than the twelve buses, and a bus nobody feeds,
 *     its tail gone, is skipped.
 * DELAY, REVERB run at 22.05 kHz, ECHO at 11.025 kHz (a tape's band): half and a quarter of the memory and the work.
 * Everything ends in the master's limiter (app.c). No randomness: the same knob, the same sound. */

enum { FX_OFF, FX_DELAY, FX_ECHO, FX_REVERB, FX_TAPE, FX_ROBOT, FX_FLANGER, FX_DRIVE, FX_CHORUS, FX_LOFI, NFX };
static const char *const FX_NAME[NFX] = {"OFF", "DELAY", "ECHO", "REVERB", "TAPE SPEED", "ROBOT", "FLANGER", "OVERDRIVE",
                                         "CHORUS", "LO-FI"};
/* what the knob's two sides are, for the screen */
static const char *const FX_SIDE[NFX][2] = {{"", ""}, {"SLAP", "LONG"}, {"SHORT", "ENDLESS"}, {"ROOM", "HALL"},
                                            {"SLOW", "FAST"}, {"METAL", "ROBOT"}, {"DEEP", "SHARP"}, {"WARM", "FUZZ"},
                                            {"WIDE", "WOBBLE"}, {"VINYL", "8-BIT"}};
#define FX_DEAD 3                        /* the knob: -FX_DEAD .. +FX_DEAD is off */
#define FX_MAX (50 + FX_DEAD)

/* the knob as an amount 0..1 and its side (0 left, 1 right) */
static float fx_amount(int32_t k, uint32_t *side)
{
    *side = k > 0;
    k = k < 0 ? -k : k;
    return k <= FX_DEAD ? 0.0f : (float)(k - FX_DEAD) * (1.0f / 50.0f);
}

/* ---------------------------------------------------------------- on a track --- */
typedef struct {
    float lp, hp, hold;
    uint32_t cnt;
} fx_slot_t;
static fx_slot_t fx_slot[8][2];
static float tape_rate[8];               /* each track's play rate now (1: as recorded), gliding as a tape does */

/* OVERDRIVE and LO-FI on a track's block x (before its LEVEL): left / right by side, a the amount */
static void fx_inline(uint32_t type, uint32_t side, float a, fx_slot_t *s, float *x, uint32_t n)
{
    uint32_t i;
    if (type == FX_DRIVE) {
        /* the sound pushed into a ceiling a little under a full sound's level, so it grows thick, not louder: left a
         * soft curve and a dark tone (an amp), right a hard one and a bright tone (a fuzz) */
        const float C = 0.17f;
        float g = side ? 1.0f + a * 79.0f : 1.0f + a * 11.0f, m = a * 4.0f > 1.0f ? 1.0f : a * 4.0f;
        float c = side ? 0.60f : 0.36f, lp = s->lp;
        for (i = 0; i < n; i++) {
            float u = x[i] * g * (1.0f / C), y;
            if (side) {
                u = u > 1.0f ? 1.0f : u < -1.0f ? -1.0f : u;
                y = 1.5f * u - 0.5f * u * u * u;
            } else {
                y = u / (1.0f + fm_fabsf(u));
                y *= 1.25f;
            }
            lp += c * (y * C - lp);
            x[i] = x[i] * (1.0f - m) + lp * m;
        }
        s->lp = fm_flush(lp);
    } else if (type == FX_LOFI && !side) {
        /* an old record: the top and the bottom go, more the further left (8 kHz .. 1.1 kHz, 60 .. 380 Hz) */
        float cl = 1.0f - fm_expf(-6.2831853f * 8000.0f * fm_exp2f(-a * 2.9f) * (1.0f / 44100.0f));
        float ch = 1.0f - fm_expf(-6.2831853f * 60.0f * fm_exp2f(a * 2.7f) * (1.0f / 44100.0f));
        float lp = s->lp, hp = s->hp, d = 1.0f + a;
        for (i = 0; i < n; i++) {
            float u = x[i] * d;
            u = u / (1.0f + 0.6f * fm_fabsf(u));            /* a little of a worn groove's squash */
            lp += cl * (u - lp);
            hp += ch * (lp - hp);
            x[i] = lp - hp;
        }
        s->lp = fm_flush(lp);
        s->hp = fm_flush(hp);
    } else if (type == FX_LOFI) {
        /* a game machine: every sample held longer (down to 1.8 kHz) and fewer steps of level (down to about 4 bits) */
        uint32_t period = 1u + (uint32_t)(a * 23.0f), cnt = s->cnt;
        float q = fm_exp2f(-(13.0f - 10.0f * a)), iq = 1.0f / q, hold = s->hold;
        for (i = 0; i < n; i++) {
            if (!cnt) {
                hold = fm_floorf(x[i] * iq + 0.5f) * q;
                cnt = period;
            }
            cnt--;
            x[i] = hold;
        }
        s->cnt = cnt;
        s->hold = hold;
    }
}

/* TAPE SPEED: the rate a knob asks for. Left slows, as a hand on the reel, to a stop at the very end; right speeds up
 * to twice */
static float fx_tape_target(uint32_t side, float a)
{
    if (side)
        return fm_exp2f(a);
    return a >= 0.96f ? 0.0f : fm_exp2f(-2.6f * a);
}

/* ---------------------------------------------------------------- the buses --- */
enum { BUS_DLY, BUS_ECH = 2, BUS_REV = 4, BUS_ROB = 6, BUS_FLA = 8, BUS_CHO = 10, NBUS = 12 };   /* + 1: the right side */
static const int8_t FX_BUS[NFX] = {-1, BUS_DLY, BUS_ECH, BUS_REV, -1, BUS_ROB, BUS_FLA, -1, BUS_CHO, -1};

#define DLY_SLAP 2205u                   /* 100 ms at 22.05 kHz */
#define DLY_LONG_MAX 8400u
#define ECH_SHORT 1654u                  /* 150 ms at 11.025 kHz */
#define ECH_LONG 4851u                   /* 440 ms */
static const uint16_t REV_ROOM[6] = {557, 617, 683, 743, 113, 37};      /* four combs, two all-passes: 22.05 kHz */
static const uint16_t REV_HALL[6] = {1193, 1321, 1451, 1559, 241, 83};

/* the buses' memory, 16-bit (1.0 is 16384): in the pool with the screen's canvas, out of .bss */
#define FXM __attribute__((section(".pool")))
static int16_t m_dly0[2304] FXM, m_dly1[DLY_LONG_MAX] FXM, m_ech0[1700] FXM, m_ech1[4900] FXM;
static int16_t m_rev0[2750] FXM, m_rev1[5848] FXM, m_rob0[136] FXM, m_rob1[224] FXM;
static int16_t m_fla0[512] FXM, m_fla1[512] FXM, m_cho0[2048] FXM, m_cho1[2048] FXM;
static float bus_in[NBUS][APP_BLOCK] FXM;

static struct {
    float depth;                         /* the flavour's strength: the furthest knob among its tracks */
    float want;
    uint8_t fed, awake;
    uint32_t w;                          /* where it writes */
    float lp, y0, y1, ph;                /* a filter, the last output (for the lower rates), an LFO */
    float c[4];                          /* the reverb's comb filters */
    uint16_t at[6];                      /* the reverb's six lines' places */
    float ea, et;                        /* a take: how much went in of late, from everyone and from the playing taken */
} fxb[NBUS];
static uint8_t fx_rec;                   /* a take runs: the sends are weighed (fx_send), for its share of each bus */

static inline float m_get(const int16_t *m, uint32_t i) { return (float)m[i] * (1.0f / 16384.0f); }
static inline void m_put(int16_t *m, uint32_t i, float v)
{
    v *= 16384.0f;
    m[i] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
}
static inline float tri(float ph) { return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; }   /* -1 .. 1 */

/* DELAY and ECHO: one line, read `time` samples back; step frames of the mix a sample of the line (2 or 4). Returns
 * the loudest sample out */
static float bus_line(uint32_t b, int16_t *m, uint32_t len, uint32_t time, float fb, float tone, uint32_t step,
                      float gain, float *l, float *r, uint32_t n)
{
    uint32_t i, k, w = fxb[b].w;
    float lp = fxb[b].lp, y0 = fxb[b].y0, peak = 0.0f, inv = 1.0f / (float)step;
    const float *in = bus_in[b];
    for (i = 0; i + step <= n; i += step) {
        float x = 0.0f, y, a;
        for (k = 0; k < step; k++)
            x += in[i + k];
        x *= inv;
        y = m_get(m, (w + len - time) % len);
        lp += tone * (y - lp);                   /* each time round the line it is a little darker */
        y = lp;
        m_put(m, w, x + y * fb);
        w = (w + 1u) % len;
        a = fm_fabsf(y);
        if (a > peak)
            peak = a;
        for (k = 0; k < step; k++) {             /* back to the mix's rate: a line from the last sample to this one */
            float o = (y0 + (y - y0) * (float)(k + 1u) * inv) * gain;
            l[i + k] += o;
            r[i + k] += o;
        }
        y0 = y;
    }
    fxb[b].w = w;
    fxb[b].lp = fm_flush(lp);
    fxb[b].y0 = y0;
    return peak;
}

/* REVERB: four combs side by side, each a little dark, then two all-passes; left and right take the combs with
 * different signs. 22.05 kHz */
static float bus_reverb(uint32_t b, int16_t *m, const uint16_t *len, float fb, float damp, float gain, float *l, float *r,
                        uint32_t n)
{
    uint32_t i, k, base[6], o = 0;
    float y0 = fxb[b].y0, y1 = fxb[b].y1, peak = 0.0f;
    const float *in = bus_in[b];
    for (k = 0; k < 6u; k++) {
        base[k] = o;
        o += len[k];
    }
    for (i = 0; i + 2u <= n; i += 2u) {
        float x = (in[i] + in[i + 1]) * 0.5f, c[4], s, d, yl, yr, a;
        for (k = 0; k < 4u; k++) {
            uint32_t p = base[k] + fxb[b].at[k];
            float y = m_get(m, p);
            fxb[b].c[k] += damp * (y - fxb[b].c[k]);
            m_put(m, p, x + fxb[b].c[k] * fb);
            c[k] = y;
            if (++fxb[b].at[k] >= len[k])
                fxb[b].at[k] = 0;
        }
        s = (c[0] + c[1] + c[2] + c[3]) * 0.25f;
        d = (c[0] - c[1] + c[2] - c[3]) * 0.25f;
        for (k = 4; k < 6u; k++) {
            uint32_t p = base[k] + fxb[b].at[k];
            float y = m_get(m, p), v = s + y * 0.5f;
            m_put(m, p, v);
            s = y - v * 0.5f;
            if (++fxb[b].at[k] >= len[k])
                fxb[b].at[k] = 0;
        }
        yl = (s + d) * gain;
        yr = (s - d) * gain;
        a = fm_fabsf(yl) > fm_fabsf(yr) ? fm_fabsf(yl) : fm_fabsf(yr);
        if (a > peak)
            peak = a;
        l[i] += (y0 + yl) * 0.5f;
        l[i + 1] += yl;
        r[i] += (y1 + yr) * 0.5f;
        r[i + 1] += yr;
        y0 = yl;
        y1 = yr;
    }
    for (k = 0; k < 4u; k++)
        fxb[b].c[k] = fm_flush(fxb[b].c[k]);
    fxb[b].y0 = y0;
    fxb[b].y1 = y1;
    return peak;
}

/* a line read at a place that moves (FLANGER, CHORUS): the sample `d` samples back, between two */
static inline float m_tap(const int16_t *m, uint32_t mask, uint32_t w, float d)
{
    uint32_t id = (uint32_t)d;
    float f = d - (float)id, a = m_get(m, (w - id) & mask), c = m_get(m, (w - id - 1u) & mask);
    return a + (c - a) * f;
}

/* every bus that is fed or still sounding, into the mix; then each is ready for the next block's input. rec (a take
 * runs): each bus's share of what went in of late that is the playing taken (4.05: 1 when it alone feeds the bus),
 * of what the bus gives, mono, added to it; tmp a block to work in */
static void fx_buses(float *l, float *r, uint32_t n, float step_frames, float *rec, float *tmp)
{
    uint32_t b, i;
    for (b = 0; b < NBUS; b++) {
        float d, peak = 0.0f, s = 0.0f;
        const float *in = bus_in[b];
        if (!rec) {
            fxb[b].ea = fxb[b].et = 0.0f;
        } else {                                 /* (both fall alike, ~0.6 s: the share holds while a tail rings) */
            if (fxb[b].et > 0.0f)
                s = fxb[b].et >= fxb[b].ea ? 0.5f : 0.5f * fxb[b].et / fxb[b].ea;
            fxb[b].ea = fm_flush(fxb[b].ea * 0.99f);
            fxb[b].et = fm_flush(fxb[b].et * 0.99f);
        }
        if (fxb[b].fed)
            fxb[b].depth += (fxb[b].want - fxb[b].depth) * 0.25f;   /* (nobody feeds it: its tail keeps its strength) */
        if (!fxb[b].fed && !fxb[b].awake)
            continue;
        d = fxb[b].depth;
        if (!fxb[b].fed)
            for (i = 0; i < n; i++)
                bus_in[b][i] = 0.0f;
        if (s > 0.0f)
            for (i = 0; i < n; i++)
                tmp[i] = l[i] + r[i];
        switch (b) {
        case BUS_DLY:                            /* a slap: one clear repeat 100 ms on, a few more the further left */
            peak = bus_line(b, m_dly0, 2304, DLY_SLAP, 0.12f + 0.38f * d, 0.85f, 2, 1.0f, l, r, n);
            break;
        case BUS_DLY + 1: {                      /* three steps of the tempo (a dotted eighth), repeats on and on */
            uint32_t t = (uint32_t)(step_frames * 1.5f);
            peak = bus_line(b, m_dly1, DLY_LONG_MAX, t >= DLY_LONG_MAX ? DLY_LONG_MAX - 1u : t, 0.35f + 0.47f * d, 0.80f, 2,
                            1.0f, l, r, n);
            break;
        }
        case BUS_ECH:                            /* a short tape: warm, soon gone */
            peak = bus_line(b, m_ech0, 1700, ECH_SHORT, 0.30f + 0.35f * d, 0.70f, 4, 1.1f, l, r, n);
            break;
        case BUS_ECH + 1:                        /* a long tape: each repeat darker, at the end almost for ever */
            peak = bus_line(b, m_ech1, 4900, ECH_LONG, 0.55f + 0.41f * d, 0.62f - 0.22f * d, 4, 1.2f, l, r, n);
            break;
        case BUS_REV:                            /* a small tight room */
            peak = bus_reverb(b, m_rev0, REV_ROOM, 0.62f + 0.22f * d, 0.55f, 1.3f, l, r, n);
            break;
        case BUS_REV + 1:                        /* a big hall */
            peak = bus_reverb(b, m_rev1, REV_HALL, 0.80f + 0.145f * d, 0.40f, 1.5f, l, r, n);
            break;
        case BUS_ROB: {                          /* metallic: a 3 ms comb that rings */
            uint32_t w = fxb[b].w;
            float fb = 0.55f + 0.38f * d, g = 0.55f * (1.0f - 0.55f * fb);   /* (the ring is loud: held to the sound's own level) */
            for (i = 0; i < n; i++) {
                float y = in[i] + m_get(m_rob0, w) * fb, a;
                m_put(m_rob0, w, y);
                w = (w + 1u) % 132u;
                y *= g;
                a = fm_fabsf(y);
                if (a > peak)
                    peak = a;
                l[i] += y;
                r[i] += y;
            }
            fxb[b].w = w;
            break;
        }
        case BUS_ROB + 1: {                      /* the app's robot: the sound times a 50 Hz tone, into a 5 ms comb */
            uint32_t w = fxb[b].w;
            float ph = fxb[b].ph, fb = 0.55f * d;
            for (i = 0; i < n; i++) {
                float y, a;
                ph += 50.0f / 44100.0f;
                if (ph >= 1.0f)
                    ph -= 1.0f;
                y = in[i] * fm_sin_turns(ph) * 1.4f + m_get(m_rob1, w) * fb;
                m_put(m_rob1, w, y);
                w = (w + 1u) % 221u;
                y *= 1.1f;
                a = fm_fabsf(y);
                if (a > peak)
                    peak = a;
                l[i] += y;
                r[i] += y;
            }
            fxb[b].w = w;
            fxb[b].ph = ph;
            break;
        }
        case BUS_FLA:
        case BUS_FLA + 1: {                      /* left slow and deep (0.15 Hz, to 7 ms); right fast and sharp (to 7 Hz) */
            int16_t *m = b == BUS_FLA ? m_fla0 : m_fla1;
            uint32_t w = fxb[b].w, right = b != BUS_FLA;
            float ph = fxb[b].ph, rate = (right ? 0.8f + 6.2f * d : 0.10f + 0.12f * d) * (1.0f / 44100.0f);
            float lo = right ? 9.0f : 22.0f, span = right ? 90.0f : 290.0f * (0.4f + 0.6f * d);
            float fb = right ? -(0.50f + 0.42f * d) : 0.30f + 0.50f * d;
            float og = right ? 1.0f + 0.62f * fb : 1.0f - 0.35f * fb;    /* (the more it rings, the less of it) */
            for (i = 0; i < n; i++) {
                float y, a;
                ph += rate;
                if (ph >= 1.0f)
                    ph -= 1.0f;
                y = m_tap(m, 511u, w, lo + span * (0.5f + 0.5f * tri(ph)));
                m_put(m, w, in[i] + y * fb);
                w = (w + 1u) & 511u;
                y *= og;
                a = fm_fabsf(y);
                if (a > peak)
                    peak = a;
                l[i] += y;
                r[i] += y;
            }
            fxb[b].w = w;
            fxb[b].ph = ph;
            break;
        }
        default: {                               /* CHORUS: two copies that drift apart, one a side. Left wide and
                                                  * soft; right far apart and wobbling */
            int16_t *m = b == BUS_CHO ? m_cho0 : m_cho1;
            uint32_t w = fxb[b].w, right = b != BUS_CHO;
            float ph = fxb[b].ph, rate = (right ? 1.5f + 3.5f * d : 0.35f) * (1.0f / 44100.0f);
            float mid = right ? 620.0f : 700.0f, span = (right ? 180.0f + 300.0f * d : 90.0f + 130.0f * d);
            for (i = 0; i < n; i++) {
                float yl, yr, a, p2;
                ph += rate;
                if (ph >= 1.0f)
                    ph -= 1.0f;
                p2 = ph + 0.25f;
                if (p2 >= 1.0f)
                    p2 -= 1.0f;
                m_put(m, w, in[i]);
                yl = m_tap(m, 2047u, w, mid + span * tri(ph));
                yr = m_tap(m, 2047u, w, mid - span * tri(p2));
                w = (w + 1u) & 2047u;
                a = fm_fabsf(yl) > fm_fabsf(yr) ? fm_fabsf(yl) : fm_fabsf(yr);
                if (a > peak)
                    peak = a;
                l[i] += yl;
                r[i] += yr;
            }
            fxb[b].w = w;
            fxb[b].ph = ph;
            break;
        }
        }
        if (s > 0.0f)                            /* (what this bus added: the mix after it, less the mix before) */
            for (i = 0; i < n; i++)
                rec[i] += s * (l[i] + r[i] - tmp[i]);
        fxb[b].awake = (uint8_t)(fxb[b].fed || peak > 0.0002f);
        fxb[b].fed = 0;
        fxb[b].want = 0.0f;
    }
}

/* a track's block y (after its LEVEL) into the bus of an effect: how much, and how much of the track's own sound stays.
 * ty: the part of y a take records (0: none), weighed against all that goes in while a take runs (fx_buses) */
static float fx_send(uint32_t type, uint32_t side, float a, const float *y, uint32_t n, const float *ty)
{
    static const float SEND[NFX][2] = {{0, 0}, {0.85f, 0.80f}, {0.90f, 0.85f}, {0.80f, 0.75f}, {0, 0}, {1.0f, 1.0f},
                                       {1.0f, 1.0f}, {0, 0}, {0.65f, 0.9f}, {0, 0}};
    uint32_t b = (uint32_t)FX_BUS[type] + side, i;
    float g = a * SEND[type][side], *in = bus_in[b];
    if (!fxb[b].fed) {
        for (i = 0; i < n; i++)
            in[i] = 0.0f;
        fxb[b].fed = 1;
    }
    if (a > fxb[b].want)
        fxb[b].want = a;
    for (i = 0; i < n; i++)
        in[i] += y[i] * g;
    if (fx_rec) {
        float e = 0.0f, et = 0.0f;
        for (i = 0; i < n; i++)
            e += fm_fabsf(y[i]);
        for (i = 0; ty && i < n; i++)
            et += fm_fabsf(ty[i]);
        fxb[b].ea += e * g;
        fxb[b].et += et * g;
    }
    return type == FX_ROBOT ? (side ? 1.0f - a : 1.0f - 0.6f * a) : 1.0f;    /* ROBOT takes the sound's place */
}

static void fx_reset(void)                   /* a start: every line silent */
{
    memset(fxb, 0, sizeof fxb);
    memset(fx_slot, 0, sizeof fx_slot);
    for (uint32_t t = 0; t < 8u; t++)
        tape_rate[t] = 1.0f;
    memset(m_dly0, 0, sizeof m_dly0);
    memset(m_dly1, 0, sizeof m_dly1);
    memset(m_ech0, 0, sizeof m_ech0);
    memset(m_ech1, 0, sizeof m_ech1);
    memset(m_rev0, 0, sizeof m_rev0);
    memset(m_rev1, 0, sizeof m_rev1);
    memset(m_rob0, 0, sizeof m_rob0);
    memset(m_rob1, 0, sizeof m_rob1);
    memset(m_fla0, 0, sizeof m_fla0);
    memset(m_fla1, 0, sizeof m_fla1);
    memset(m_cho0, 0, sizeof m_cho0);
    memset(m_cho1, 0, sizeof m_cho1);
}
