/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the looper on track 8. Written for STACK. Part of the unity build: included by app.c.
 *
 * A loop is as long as the pattern and runs with it: it starts where the pattern's first step does, every time round,
 * so it cannot drift from the steps. It holds what was played by hand (render_part's rec_in: the live voices, before
 * the effects, mono), a layer at a time: a take records one pass of the pattern, from a bar's start to the next, as
 * the loop that plays plus the new playing, into the other of two blocks; at the take's end the blocks change places
 * between two samples, and the one left is the undo. Nothing here touches the flash.
 *
 * No seams: the new playing fades in over the take's first 5 ms and out over its last 10 ms, so a take's two ends
 * meet at silence of its own; the loop under it is copied as it is. An undo crosses from one block to the other over
 * 10 ms at the same place in the loop; a clear and a stop of the transport fade out over 15 ms.
 *
 * With a clock coming in (clock.c), or a tempo taken from one, a bar is not a whole number of frames, nor quite the
 * loop's own length: the loop is then steered to the bar instead of set back at it (loop_block's phase): it plays up to
 * 0.1 % faster or slower, a frame doubled or left out here and there, which is not heard, and turns round by itself
 * where it ends. More than that is a change of tempo: the loop cannot follow it (it is sound, it does not stretch), it
 * goes out of time and lp.drift says so.
 *
 * The blocks are 16-bit, at half the mix's rate (22.05 kHz), or a quarter (11.025 kHz) for a pattern too long for a
 * block at half: looper_fit says which before a take is asked for. 1.0 is 16384, so two layers at full level fit. */

#define LOOP_CAP 44104u                  /* samples a block: 2 s at 22.05 kHz (16 steps at 120), 4 s at 11.025 kHz */
static int16_t loop_m[2][LOOP_CAP] __attribute__((section(".pool")));

enum { LP_IDLE, LP_ARMED, LP_REC };
enum { LQ_NONE, LQ_ARM, LQ_CANCEL, LQ_UNDO, LQ_CLEAR };
static struct {
    volatile uint8_t own;                /* track 8 is the looper's (the mode on with track 8 free, or a loop there) */
    volatile uint8_t has;                /* a loop plays */
    volatile uint8_t state;              /* LP_* */
    volatile uint8_t layers;
    volatile uint8_t undo;               /* the other block holds the loop as it was a layer ago */
    volatile uint8_t req;                /* LQ_*: the main loop asks, the audio interrupt does */
    volatile uint8_t div;                /* frames of the mix a sample: 2 or 4 */
    volatile uint8_t bars;               /* the pattern's passes a loop spans: 1 (more only in the simulator's tests) */
    volatile uint32_t done;              /* takes finished (the screen says so) */
    volatile uint32_t ver;               /* goes up whenever the loop that plays is another (a take in, an undo, a clear): a dump knows */
    volatile uint32_t frame;             /* frames since the loop's start */
    volatile uint32_t total;             /* frames in the loop */
    uint8_t cur;                         /* the block that plays */
    uint8_t prev_ok;                     /* the other block is a whole loop (else: nothing was there) */
    uint8_t bar;                         /* the pass of the pattern the loop is in */
    uint8_t fading;                      /* 1 across to the other block (undo); 2 out (clear) */
    float fade;
    float run;                           /* 1 while the transport plays, down to 0 over 15 ms when it stops */
    float acc;                           /* a take: the frames of the sample being made */
    uint8_t accn;                        /* and how many (the loop's rate under the mix's: 2 or 4, but for a steered frame) */
    uint8_t resync;                      /* the transport started: a steered loop goes to the bar's place at once */
    uint8_t wrapped;                     /* a steered loop turned round at the last frame: its start is this one */
    volatile uint8_t drift;              /* a steered loop is over 10 ms from the bar: the tempo changed */
    float fr;                            /* a steered loop: the part of a frame it is on from lp.frame */
    float nudge;                         /* (the simulator's checks: the most it was sped or slowed) */
} lp = {.div = 2, .bars = 1};

static float rec_in[APP_BLOCK];              /* what is played by hand this block (render_part) */

/* a pattern of `frames` frames: 2 the loop at 22.05 kHz, 4 at 11.025 kHz, 0 it does not fit */
static uint32_t looper_fit(uint32_t frames)
{
    return frames / 2u <= LOOP_CAP - 4u ? 2u : frames / 4u <= LOOP_CAP - 4u ? 4u : 0u;
}

static inline float loop_at(uint32_t blk, uint32_t k, uint32_t len, float fr)   /* the loop at sample k and fr on */
{
    float a = (float)loop_m[blk][k], b = (float)loop_m[blk][k + 1u < len ? k + 1u : 0u];
    return (a + (b - a) * fr) * (1.0f / 16384.0f);
}

static inline void loop_turn(void)           /* the loop's start: a take that was recording is whole, one that waited begins */
{
    if (lp.state == LP_REC) {                /* the take is whole: its block plays from this sample on */
        lp.cur ^= 1u;
        lp.prev_ok = lp.has;
        lp.has = 1;
        lp.undo = 1;
        lp.layers++;
        lp.state = LP_IDLE;
        lp.done++;
        lp.ver++;
    } else if (lp.state == LP_ARMED) {
        lp.state = LP_REC;
        lp.undo = 0;                         /* (the other block is the take's from here) */
        lp.acc = 0.0f;
        lp.accn = 0;
    }
}

static inline void loop_rec(uint32_t frame, float in, uint32_t len, uint32_t div)   /* a take: frame's share of a sample */
{
    const uint32_t k = frame / div;
    float g = 1.0f, left = (float)(lp.total - frame);
    if (frame < 220u)
        g = (float)frame * (1.0f / 220.0f);
    if (left < 441.0f)
        g *= left * (1.0f / 441.0f);
    lp.acc += in * g;
    lp.accn++;
    if (frame - k * div == div - 1u && k < len) {
        float s = lp.acc / (float)lp.accn * 16384.0f + (lp.has && !lp.fading ? (float)loop_m[lp.cur][k] : 0.0f);
        loop_m[lp.cur ^ 1u][k] = (int16_t)(s > 32767.0f ? 32767.0f : s < -32767.0f ? -32767.0f : s);
        lp.acc = 0.0f;
        lp.accn = 0;
    }
}

/* the audio interrupt, a block: what the main loop asked for, then the loop's n frames into out. bar_off: where in
 * this block the pattern's first step starts (-1: not in it); playing: the transport runs; pat: the pattern's frames;
 * phase: -1, the loop is set back at each bar (the FM-1 leads at 120: a bar is the loop's frames exactly); else the
 * place in the bar at the block's start, 0 .. 1, that the loop is steered to, dph more each frame */
static uint32_t loop_block(float *out, uint32_t n, int32_t bar_off, uint32_t playing, uint32_t pat, float phase, float dph)
{
    const uint32_t steer = phase >= 0.0f;
    uint32_t i, q = lp.req, len, div, sounding = 0;
    lp.req = LQ_NONE;
    if (q == LQ_ARM && lp.state == LP_IDLE && !lp.fading) {
        if (!lp.has) {                           /* a first take: the loop's length and rate are the pattern's now */
            lp.total = pat * lp.bars;
            lp.div = (uint8_t)looper_fit(lp.total);
            lp.bar = (uint8_t)(lp.bars - 1u);    /* (it starts at the next bar's start) */
        }
        if (lp.div)
            lp.state = LP_ARMED;
    } else if (q == LQ_CANCEL) {
        lp.state = LP_IDLE;
    } else if (q == LQ_UNDO && lp.state == LP_IDLE && lp.undo && !lp.fading) {
        lp.fading = 1;
        lp.fade = 0.0f;
        lp.undo = 0;
    } else if (q == LQ_CLEAR) {
        lp.state = LP_IDLE;
        lp.undo = 0;
        if (lp.has) {
            lp.fading = 2;
            lp.fade = 0.0f;
        } else {
            lp.layers = 0;
        }
    }
    if (!playing && lp.state != LP_IDLE)         /* the transport stopped under a take: the take is let go */
        lp.state = LP_IDLE;
    div = lp.div ? lp.div : 2u;
    len = lp.total / div;
    if (!steer) {
        lp.fr = 0.0f;
        lp.wrapped = 0;
        lp.drift = 0;
    } else if (lp.resync && lp.total) {          /* the transport started: to the bar's place (the loop was silent) */
        lp.frame = (uint32_t)(phase * (float)lp.total) % lp.total;
        lp.fr = 0.0f;
        lp.wrapped = 0;
    }
    lp.resync = 0;
    for (i = 0; i < n; i++) {
        uint32_t k, sub, step = 1;
        float v = 0.0f, fr, nud = 0.0f;
        if (steer) {
            if (lp.wrapped) {                    /* the loop turned round: its start is here */
                lp.wrapped = 0;
                loop_turn();
            } else if ((int32_t)i == bar_off && !lp.has && lp.state == LP_ARMED) {
                lp.frame = 0;                    /* a first take: the loop's start is this bar's */
                lp.fr = 0.0f;
                loop_turn();
            }
            if (lp.total && playing) {           /* steered to the bar: where it should be, and a little that way */
                const float tot = (float)lp.total;
                float want = (phase + (float)i * dph) * tot, e;
                want -= tot * (float)(uint32_t)(want / tot);
                e = want - ((float)lp.frame + lp.fr);
                if (e > tot * 0.5f)
                    e -= tot;
                if (e < -tot * 0.5f)
                    e += tot;
                if (e > 441.0f || e < -441.0f)
                    lp.drift = 1;
                else if (e < 44.0f && e > -44.0f)
                    lp.drift = 0;
                e *= 0.0005f;
                e = e > 0.001f ? 0.001f : e < -0.001f ? -0.001f : e;
                if (e > lp.nudge || -e > lp.nudge)
                    lp.nudge = e < 0.0f ? -e : e;
                nud = e;
            }
        } else if ((int32_t)i == bar_off) {      /* the pattern's first step starts here */
            lp.bar = (uint8_t)((lp.bar + 1u) % lp.bars);
            if (!lp.bar) {                       /* and so does the loop */
                lp.frame = 0;
                loop_turn();
            }
        }
        lp.run += playing ? (1.0f - lp.run) * 0.02f : -lp.run * 0.01f;      /* (in at once, out over ~15 ms) */
        if (lp.run < 0.0005f && !playing) {
            lp.run = 0.0f;
            out[i] = 0.0f;
            continue;                            /* stopped: the loop waits at silence */
        }
        k = lp.frame / div;
        sub = lp.frame - k * div;
        if (k >= len)
            k = len ? len - 1u : 0u;
        fr = ((float)sub + lp.fr) / (float)div;
        if (lp.has) {
            v = loop_at(lp.cur, k, len, fr);
            if (lp.fading == 1u) {               /* undo: across to the loop as it was, or to nothing */
                float o = lp.prev_ok ? loop_at(lp.cur ^ 1u, k, len, fr) : 0.0f;
                v += (o - v) * lp.fade;
                lp.fade += 1.0f / 441.0f;
                if (lp.fade >= 1.0f) {
                    lp.fading = 0;
                    lp.cur ^= 1u;
                    lp.has = lp.prev_ok;
                    lp.prev_ok = 0;
                    lp.ver++;
                    lp.layers = (uint8_t)(lp.layers ? lp.layers - 1u : 0u);
                }
            } else if (lp.fading == 2u) {        /* clear: out */
                v *= 1.0f - lp.fade;
                lp.fade += 1.0f / 661.0f;
                if (lp.fade >= 1.0f) {
                    lp.fading = 0;
                    lp.has = 0;
                    lp.prev_ok = 0;
                    lp.layers = 0;
                    lp.ver++;
                }
            }
        }
        if (lp.state == LP_REC)                  /* the take: the loop as it plays and the new playing, a sample */
            loop_rec(lp.frame, rec_in[i], len, div);
        if (steer) {
            lp.fr += nud;                        /* on by a frame and the little more or less: the place stays one line */
            if (lp.fr >= 1.0f) {
                lp.fr -= 1.0f;
                step = 2;
            } else if (lp.fr < 0.0f) {
                lp.fr += 1.0f;
                step = 0;
            }
            if (step == 2u && lp.state == LP_REC && lp.frame + 1u < lp.total)
                loop_rec(lp.frame + 1u, rec_in[i], len, div);    /* (the frame a steered loop steps over) */
            lp.frame += step;
            if (lp.total && lp.frame >= lp.total) {
                lp.frame -= lp.total;            /* a steered loop turns round by itself */
                lp.wrapped = 1;
            }
        } else if (lp.frame + 1u < lp.total || !lp.total) {
            lp.frame++;                          /* (the pattern's first step sets it back to 0: it holds here till then) */
        }
        out[i] = v * lp.run;
        sounding |= v != 0.0f;
    }
    return sounding || lp.has;
}
