/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: PERFORM. Written for STACK, from its own description (SEL held, a white key held: its effect on everything
 * that sounds, gone when the key is let go); nothing here comes from another firmware. Part of the unity build:
 * included by app.c.
 *
 * Sixteen effects, one a white key (PK_*). They are of four kinds, and effects of different kinds stack:
 *   - the buffer's (REPEAT 1/8, 1/16, 1/32, REVERSE, FREEZE): a piece of the mix is recorded and played again. One at
 *     a time (the main loop names it: the last key pressed of those held). There is no memory of its own for it: it
 *     borrows the looper's second block, the one a take records into and the undo sleeps in. A take always has that
 *     block first (the buffer's effects wait); an undo that slept there is given up when one of them first writes,
 *     and the screen says so (pf_undo_gone).
 *   - the mix's (LOW-PASS, HIGH-PASS, PHASER, LO-FI, GATE): worked on the mix, after the tracks' effects, before the
 *     limiter.
 *   - the sends (FLANGER, ROBOT, REVERB THROW, ECHO THROW): every track's sound goes into the bus the tracks' own
 *     effects use (fx.c: the fast flanger, the robot, the hall, the long echo); no track's setting is touched.
 *   - the speed (TAPE STOP, OCT DOWN): every voice's play rate (the loop, which is no voice, only fades at a standstill).
 * Each comes in and goes out on a ramp. While none is on and all ramps are down nothing here runs (pf_run 0). */

enum { PK_REP8, PK_REP16, PK_REP32, PK_REV, PK_FREEZE, PK_TAPE, PK_LP, PK_HP, PK_FLANGER, PK_PHASER, PK_ROBOT, PK_LOFI,
       PK_REVERB, PK_ECHO, PK_OCT, PK_GATE, NPK };
#define PK_BUFFER 0x001Fu                    /* the buffer's keys */
#define PK_NOTAKE (PK_BUFFER | 1u << PK_TAPE | 1u << PK_OCT)   /* not while a take records (they would be in it, or need its block) */
static volatile uint16_t pf_on;              /* the effects that are on, but the buffer's (the main loop sets them) */
static volatile uint8_t pf_buf;              /* the buffer's effect wanted: 0 none, else 1 + PK_* */
static volatile uint8_t pf_undo_gone;        /* a buffer effect took the block the undo was in: the screen says so once */
static uint8_t pf_run;                       /* something of it sounds: its stages run */
static float pf_g[NPK];                      /* each effect's ramp, 0 .. 1 (the audio interrupt) */
static float pf_rate = 1.0f;                 /* the voices' rate (TAPE STOP, OCT DOWN): 1 none */

static struct {
    uint8_t cur, want;                       /* the effect that has the buffer, the one that is to have it (1 + PK_*) */
    uint8_t ph;                              /* 0 waits for the step, 1 records, 2 plays */
    uint8_t half, have;                      /* REVERSE: the half being recorded; the other one is full */
    float mix;                               /* 0 the mix as it is .. 1 the buffer's */
    uint32_t len;                            /* the piece, in the buffer's samples (half the mix's rate, two channels) */
    uint32_t pos;                            /* frames into it */
    float al, ar;                            /* a frame kept for the next (two frames make a sample) */
} pb;
static struct {
    float lp[2][2], hp[2][2];                /* the two filters, a side: two poles each */
    float ap[2][4], fb[2], ph;               /* PHASER: four all-pass stages a side, its feedback, its sweep */
    float hl, hr;                            /* LO-FI: the sample held */
    uint32_t hn;
    float gate, gpos;                        /* GATE: its gain now; its place in the step while the pattern is stopped */
} pm;

static int perform_block_free(void)          /* the looper's second block may be borrowed now */
{
    return lp.state == LP_IDLE && !lp.fading && lp.req != LQ_ARM;
}

/* each block, before the tracks: the ramps, the speed, and whether anything of PERFORM is to run */
static void perform_begin(void)
{
    uint32_t k, on = pf_on, any = 0;
    float want = 1.0f;
    if (lp.state != LP_IDLE)                 /* a take: its sound is not to be bent, its block not to be borrowed */
        on &= ~PK_NOTAKE;
    pb.want = (uint8_t)(perform_block_free() ? pf_buf : 0u);
    lp.hold = pb.cur != 0u;                  /* (a take that has begun writes there only once this is over) */
    if (!on && !pb.want && !pb.cur && !pf_run) {
        return;                              /* idle: nothing */
    }
    for (k = PK_TAPE; k < NPK; k++) {
        const float to = on >> k & 1u ? 1.0f : 0.0f;
        pf_g[k] += to > pf_g[k] ? 0.125f : to < pf_g[k] ? -0.125f : 0.0f;       /* 46 ms up or down */
        pf_g[k] = pf_g[k] < 0.0f ? 0.0f : pf_g[k] > 1.0f ? 1.0f : pf_g[k];
        any |= pf_g[k] > 0.0f;
    }
    if (on >> PK_TAPE & 1u)
        want = 0.0f;
    else if (on >> PK_OCT & 1u)
        want = 0.5f;
    /* TAPE STOP runs down over about a third of a second; the rest comes and goes faster */
    pf_rate += (want - pf_rate) * (want == 0.0f ? 0.07f : 0.2f);
    if (want == 1.0f && pf_rate > 0.999f)
        pf_rate = 1.0f;
    if (want == 0.0f && pf_rate < 0.002f)
        pf_rate = 0.0f;
    pf_run = (uint8_t)(any || pf_rate != 1.0f || pb.want || pb.cur || pb.mix > 0.0f || pm.gate < 0.999f);
}

/* a track's sound (after its LEVEL) into the buses PERFORM's sends are on; what is left of it dry (ROBOT takes its place) */
static float perform_sends(const float *y, uint32_t n)
{
    float dry = 1.0f;
    if (pf_g[PK_FLANGER] > 0.0f)
        fx_send(FX_FLANGER, 1, pf_g[PK_FLANGER] * 0.8f, y, n, 0);
    if (pf_g[PK_ROBOT] > 0.0f)
        dry = fx_send(FX_ROBOT, 1, pf_g[PK_ROBOT] * 0.9f, y, n, 0);
    if (pf_g[PK_REVERB] > 0.0f)
        fx_send(FX_REVERB, 1, pf_g[PK_REVERB] * 0.6f, y, n, 0);
    if (pf_g[PK_ECHO] > 0.0f)
        fx_send(FX_ECHO, 1, pf_g[PK_ECHO] * 0.65f, y, n, 0);
    return dry;
}

static void perform_buffer(float *l, float *r, uint32_t n, int32_t boff, float sf)
{
    int16_t *b = loop_m[lp.cur ^ 1u];        /* (LOOP_PCM 16-bit samples of it: looper.c) */
    uint32_t i;
    for (i = 0; i < n; i++) {
        float wl = 0.0f, wr = 0.0f, to = 0.0f;
        if (pb.cur != pb.want && pb.mix <= 0.0f) {       /* another effect, or none: from its start (the old one faded) */
            pb.cur = pb.want;
            pb.ph = pb.cur == 1u + PK_FREEZE ? 1u : 0u;
            pb.pos = 0;
            pb.half = pb.have = 0;
            if (pb.cur == 1u + PK_FREEZE)
                pb.len = 2048u;                          /* 93 ms, whatever the tempo */
            else if (pb.cur == 1u + PK_REV)
                pb.len = (uint32_t)(sf * (sf * 2.0f <= 22000.0f ? 1.0f : 0.5f));     /* an eighth (a sixteenth when slow) */
            else if (pb.cur)
                pb.len = (uint32_t)(sf * (pb.cur == 1u + PK_REP8 ? 1.0f : pb.cur == 1u + PK_REP16 ? 0.5f : 0.25f));
            if (pb.len > LOOP_PCM / 2u - 2u)
                pb.len = LOOP_PCM / 2u - 2u;
            if (pb.len < 64u)
                pb.len = 64u;
        }
        if (pb.cur && pb.cur == pb.want) {
            if (pb.ph == 0u && ((int32_t)i == boff || !seq_running))     /* on the step (stopped: at once) */
                pb.ph = 1;
            if (pb.ph == 1u && !pb.pos && lp.undo) {     /* its first write: the undo that slept in this block is given up */
                lp.undo = 0;
                lp.prev_ok = 0;
                pf_undo_gone = 1;
            }
            if (pb.cur == 1u + PK_REV) {
                if (pb.ph) {                             /* one half records as it comes, the other plays backwards */
                    const uint32_t h = pb.pos >> 1, o = pb.half ? pb.len : 0u, p = (pb.half ? 0u : pb.len) + pb.len - 1u - h;
                    const uint32_t e = pb.pos < 2u * pb.len - 1u - pb.pos ? pb.pos : 2u * pb.len - 1u - pb.pos;
                    if (pb.pos & 1u) {
                        b[2u * (o + h)] = (int16_t)((pb.al + l[i]) * 8192.0f);
                        b[2u * (o + h) + 1u] = (int16_t)((pb.ar + r[i]) * 8192.0f);
                    } else {
                        pb.al = fm_clampf(l[i], -1.9f, 1.9f);
                        pb.ar = fm_clampf(r[i], -1.9f, 1.9f);
                    }
                    if (pb.have) {
                        const float f = e < 96u ? (float)e * (1.0f / 96.0f) : 1.0f;
                        wl = (float)b[2u * p] * (1.0f / 16384.0f) * f;
                        wr = (float)b[2u * p + 1u] * (1.0f / 16384.0f) * f;
                        to = 1.0f;
                    }
                    if (++pb.pos >= 2u * pb.len) {
                        pb.pos = 0;
                        pb.half ^= 1u;
                        pb.have = 1;
                    }
                }
            } else if (pb.ph == 1u) {                    /* REPEAT, FREEZE: the piece is recorded (and heard as it is) */
                if (pb.pos & 1u) {
                    b[pb.pos - 1u] = (int16_t)((pb.al + l[i]) * 8192.0f);
                    b[pb.pos] = (int16_t)((pb.ar + r[i]) * 8192.0f);
                } else {
                    pb.al = fm_clampf(l[i], -1.9f, 1.9f);
                    pb.ar = fm_clampf(r[i], -1.9f, 1.9f);
                }
                if (++pb.pos >= 2u * pb.len) {
                    pb.pos = 0;
                    pb.ph = 2;
                }
            } else if (pb.ph == 2u) {
                const uint32_t h = pb.pos >> 1;
                to = 1.0f;
                if (pb.cur == 1u + PK_FREEZE) {          /* two readers half the piece apart, each fading in and out */
                    const uint32_t h2 = (h + pb.len / 2u) % pb.len;
                    const float w = (float)(h < pb.len - h ? h : pb.len - h) * (2.0f / (float)pb.len);
                    wl = ((float)b[2u * h] * w + (float)b[2u * h2] * (1.0f - w)) * (1.0f / 16384.0f);
                    wr = ((float)b[2u * h + 1u] * w + (float)b[2u * h2 + 1u] * (1.0f - w)) * (1.0f / 16384.0f);
                } else {                                 /* REPEAT: the piece again and again, its ends faded */
                    const uint32_t e = pb.pos < 2u * pb.len - 1u - pb.pos ? pb.pos : 2u * pb.len - 1u - pb.pos;
                    const float f = e < 96u ? (float)e * (1.0f / 96.0f) : 1.0f;
                    wl = (float)b[2u * h] * (1.0f / 16384.0f) * f;
                    wr = (float)b[2u * h + 1u] * (1.0f / 16384.0f) * f;
                }
                if (++pb.pos >= 2u * pb.len)
                    pb.pos = 0;
            }
        }
        pb.mix += to > pb.mix ? 1.0f / 128.0f : to < pb.mix ? -1.0f / 128.0f : 0.0f;     /* 3 ms across */
        pb.mix = pb.mix < 0.0f ? 0.0f : pb.mix > 1.0f ? 1.0f : pb.mix;
        l[i] += pb.mix * (wl - l[i]);
        r[i] += pb.mix * (wr - r[i]);
    }
}

/* the mix, after the tracks' effects: the buffer's effect, then the filters, PHASER, LO-FI, GATE. sf: frames a step;
 * boff: where in this block a step of the pattern starts (-1: not in it) */
static void perform_mix(float *l, float *r, uint32_t n, int32_t boff, float sf)
{
    static float gl[NPK];                    /* the ramps at the last block's end */
    uint32_t i, c;
    if (pb.cur || pb.want || pb.mix > 0.0f)
        perform_buffer(l, r, n, boff, sf);
    if (pf_g[PK_LP] > 0.0f || gl[PK_LP] > 0.0f) {        /* a low-pass at 500 Hz, two poles */
        const float a = 0.0688f, g0 = gl[PK_LP], dg = (pf_g[PK_LP] - g0) / (float)n;
        for (i = 0; i < n; i++) {
            const float g = g0 + dg * (float)(i + 1u);
            pm.lp[0][0] += a * (l[i] - pm.lp[0][0]);
            pm.lp[0][1] += a * (pm.lp[0][0] - pm.lp[0][1]);
            pm.lp[1][0] += a * (r[i] - pm.lp[1][0]);
            pm.lp[1][1] += a * (pm.lp[1][0] - pm.lp[1][1]);
            l[i] += g * (pm.lp[0][1] - l[i]);
            r[i] += g * (pm.lp[1][1] - r[i]);
        }
        gl[PK_LP] = pf_g[PK_LP];
        if (gl[PK_LP] == 0.0f)
            memset(pm.lp, 0, sizeof pm.lp);
    }
    if (pf_g[PK_HP] > 0.0f || gl[PK_HP] > 0.0f) {        /* a high-pass at 1.5 kHz, two poles */
        const float a = 0.1925f, g0 = gl[PK_HP], dg = (pf_g[PK_HP] - g0) / (float)n;
        for (i = 0; i < n; i++) {
            const float g = g0 + dg * (float)(i + 1u);
            float x = l[i], y;
            pm.hp[0][0] += a * (x - pm.hp[0][0]);
            y = x - pm.hp[0][0];
            pm.hp[0][1] += a * (y - pm.hp[0][1]);
            l[i] = x + g * (y - pm.hp[0][1] - x);
            x = r[i];
            pm.hp[1][0] += a * (x - pm.hp[1][0]);
            y = x - pm.hp[1][0];
            pm.hp[1][1] += a * (y - pm.hp[1][1]);
            r[i] = x + g * (y - pm.hp[1][1] - x);
        }
        gl[PK_HP] = pf_g[PK_HP];
        if (gl[PK_HP] == 0.0f)
            memset(pm.hp, 0, sizeof pm.hp);
    }
    if (pf_g[PK_PHASER] > 0.0f || gl[PK_PHASER] > 0.0f) {    /* four all-pass stages swept over two bars, a little fed back */
        const float g0 = gl[PK_PHASER], dg = (pf_g[PK_PHASER] - g0) / (float)n;
        float k[2];
        pm.ph += (float)n / (sf * 32.0f);
        pm.ph -= (float)(uint32_t)pm.ph;
        for (c = 0; c < 2u; c++) {                           /* (the two sides a quarter of the sweep apart) */
            const float f = 300.0f * fm_exp2f(1.5f + 1.5f * fm_sinf(6.2831853f * (pm.ph + 0.25f * (float)c)));
            const float t = 3.14159265f * f * (1.0f / 44100.0f);
            k[c] = (1.0f - t) / (1.0f + t);
        }
        for (i = 0; i < n; i++) {
            const float g = (g0 + dg * (float)(i + 1u)) * 0.5f;
            for (c = 0; c < 2u; c++) {
                float *x = c ? &r[i] : &l[i], y = *x + pm.fb[c] * 0.4f;
                uint32_t q;
                for (q = 0; q < 4u; q++) {
                    const float o = pm.ap[c][q] - k[c] * y;
                    pm.ap[c][q] = y + k[c] * o;
                    y = o;
                }
                pm.fb[c] = y;
                *x += g * (y - *x);                          /* (at full: the sound and its swept copy, half and half) */
            }
        }
        gl[PK_PHASER] = pf_g[PK_PHASER];
        if (gl[PK_PHASER] == 0.0f) {
            memset(pm.ap, 0, sizeof pm.ap);
            pm.fb[0] = pm.fb[1] = 0.0f;
        }
    }
    if (pf_g[PK_LOFI] > 0.0f || gl[PK_LOFI] > 0.0f) {        /* every sixth frame held, in 64 steps a side */
        const float g0 = gl[PK_LOFI], dg = (pf_g[PK_LOFI] - g0) / (float)n;
        for (i = 0; i < n; i++) {
            const float g = g0 + dg * (float)(i + 1u);
            if (!(pm.hn++ % 6u)) {
                pm.hl = (float)(int32_t)(l[i] * 32.0f) * (1.0f / 32.0f);
                pm.hr = (float)(int32_t)(r[i] * 32.0f) * (1.0f / 32.0f);
            }
            l[i] += g * (pm.hl - l[i]);
            r[i] += g * (pm.hr - r[i]);
        }
        gl[PK_LOFI] = pf_g[PK_LOFI];
    }
    if (pf_g[PK_GATE] > 0.0f || gl[PK_GATE] > 0.0f || pm.gate < 0.999f) {    /* each step: its first half sounds, its second not */
        const float g0 = gl[PK_GATE], dg = (pf_g[PK_GATE] - g0) / (float)n;
        const float d = seq_running ? seq_dphase * (float)seq_len : 1.0f / sf;
        float p = seq_running ? seq_phase * (float)seq_len : pm.gpos;
        for (i = 0; i < n; i++) {
            const float g = g0 + dg * (float)(i + 1u), open = p - (float)(uint32_t)p < 0.5f ? 1.0f : 0.0f;
            pm.gate += (1.0f - g * (1.0f - open) - pm.gate) * 0.012f;        /* (2 ms: no click) */
            l[i] *= pm.gate;
            r[i] *= pm.gate;
            p += d;
        }
        pm.gpos = p - (float)(uint32_t)p;
        gl[PK_GATE] = pf_g[PK_GATE];
        if (gl[PK_GATE] == 0.0f && pm.gate > 0.999f)
            pm.gate = 1.0f;
    }
}

static void perform_reset(void)
{
    memset(&pb, 0, sizeof pb);
    memset(&pm, 0, sizeof pm);
    memset(pf_g, 0, sizeof pf_g);
    pm.gate = 1.0f;
    pf_rate = 1.0f;
    pf_on = 0;
    pf_buf = 0;
    pf_run = 0;
    pf_undo_gone = 0;
}
