/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the looper on track 8. Written for STACK. Part of the unity build: included by app.c.
 *
 * A loop is a whole number of bars (passes of the pattern) long and runs with them: it starts where a bar does, every
 * time round, so it cannot drift from the steps. It holds what was played by hand (render_part's rec_in: the live
 * voices, before the effects, mono), a layer at a time. The first take sets the length (since 4.02): from the bar it
 * starts at to the end of the bar in which REC is pressed again, or to the last bar a block holds (loop_room: it
 * halves what it has to 11 kHz in place when the next bar would not fit at 22, and ends by itself when none would).
 * A later take records the loop that plays plus the new playing into the other of the two blocks, for one pass of the
 * loop at most, or to the end of the bar it is stopped in (the rest of that block is the loop as it was, copied at
 * the take's start); at the take's end the blocks change places between two samples, and the one left is the undo.
 * Nothing here touches the flash.
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
enum { LQ_NONE, LQ_ARM, LQ_CANCEL, LQ_UNDO, LQ_CLEAR, LQ_STOP };
static struct {
    volatile uint8_t own;                /* track 8 is the looper's: a loop there, or a take waiting or running */
    volatile uint8_t has;                /* a loop plays */
    volatile uint8_t state;              /* LP_* */
    volatile uint8_t layers;
    volatile uint8_t undo;               /* the other block holds the loop as it was a layer ago */
    volatile uint8_t req;                /* LQ_*: the main loop asks, the audio interrupt does */
    volatile uint8_t div;                /* frames of the mix a sample: 2 or 4 */
    volatile uint8_t bars;               /* the bars (passes of the pattern) the loop is long: as many as its first layer took */
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
    /* a take (4.02: REC starts it at the next bar, REC again ends it at the end of the bar it is in) */
    uint8_t first;                       /* it is the loop's first layer: it is as long as it is let run, in whole bars */
    uint8_t stop;                        /* it ends at the next bar's start (1), or the one after (2); 0: not asked yet */
    volatile uint8_t barsrec;            /* a first layer: its bars so far */
    volatile uint8_t full;               /* a first layer ended by itself: the memory holds no bar more (the screen says so) */
    volatile uint8_t low;                /* a first layer went to 11 kHz to hold another bar (the screen says so) */
    uint32_t tk;                         /* frames since the take began */
    float end_in;                        /* frames until it ends (very many: not known yet) */
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

static void loop_take_in(void)               /* the take is whole: its block plays from this sample on */
{
    lp.cur ^= 1u;
    lp.prev_ok = (uint8_t)(lp.has && !lp.first);
    lp.has = 1;
    lp.undo = 1;
    lp.layers++;
    lp.state = LP_IDLE;
    lp.first = 0;
    lp.stop = 0;
    lp.done++;
    lp.ver++;
}

/* a take: frame's share of a sample of the other block: the playing by hand, faded in over its first 5 ms and out
 * over its last 10, on top of the loop that plays there (cap: the samples the block may take) */
static inline void loop_rec(uint32_t frame, float in, uint32_t cap, uint32_t div)
{
    const uint32_t k = frame / div;
    float g = lp.tk < 220u ? (float)lp.tk * (1.0f / 220.0f) : 1.0f;
    if (lp.end_in < 441.0f)
        g *= lp.end_in > 0.0f ? lp.end_in * (1.0f / 441.0f) : 0.0f;
    lp.acc += in * g;
    lp.accn++;
    if (frame - k * div == div - 1u && k < cap) {
        float s = lp.acc / (float)lp.accn * 16384.0f + (lp.has && !lp.fading ? (float)loop_m[lp.cur][k] : 0.0f);
        loop_m[lp.cur ^ 1u][k] = (int16_t)(s > 32767.0f ? 32767.0f : s < -32767.0f ? -32767.0f : s);
        lp.acc = 0.0f;
        lp.accn = 0;
    }
}

/* a first layer, at a bar's start with barsrec bars in it: is there room for the bar that begins, and for one more?
 * At 22 kHz with no room for this one, what is there is halved to 11 kHz where it lies (two samples to one), and it
 * goes on at that; with no room for the one after, this bar is its last (lp.stop, as if REC had been pressed) */
static void loop_room(uint32_t pat)
{
    const uint32_t n = lp.barsrec;
    if ((n + 1u) * pat / lp.div > LOOP_CAP - 4u && lp.div == 2u) {
        int16_t *b = loop_m[lp.cur ^ 1u];
        uint32_t j, m = lp.frame / 4u;
        for (j = 0; j < m; j++)
            b[j] = (int16_t)(((int32_t)b[2u * j] + (int32_t)b[2u * j + 1u]) / 2);
        lp.div = 4;
        lp.low = 1;
        lp.acc = 0.0f;
        lp.accn = 0;
    }
    if ((n + 2u) * pat / 4u > LOOP_CAP - 4u && !lp.stop) {
        lp.stop = 1;
        lp.end_in = (float)pat;
        lp.full = 1;
    }
}

/* the audio interrupt, a block: what the main loop asked for, then the loop's n frames into out. bar_off: where in
 * this block the pattern's first step starts (-1: not in it); playing: the transport runs; pat: the pattern's frames;
 * to_bar: frames from the block's start to the next bar's; phase: -1, the loop is set back at its start (the FM-1
 * leads at 120: a bar is a whole number of frames); else the place in the bar at the block's start, 0 .. 1, that the
 * loop is steered to, dph more each frame */
static uint32_t loop_block(float *out, uint32_t n, int32_t bar_off, uint32_t playing, uint32_t pat, float to_bar, float phase, float dph)
{
    const uint32_t steer = phase >= 0.0f;
    uint32_t i, q = lp.req, len, div, sounding = 0, rs;
    lp.req = LQ_NONE;
    if (q == LQ_ARM && lp.state == LP_IDLE && !lp.fading) {
        if (lp.has || looper_fit(pat))           /* (a first layer needs room for a bar at least) */
            lp.state = LP_ARMED;
    } else if (q == LQ_CANCEL) {
        lp.state = LP_IDLE;
    } else if (q == LQ_STOP && lp.state == LP_REC && !lp.stop) {
        /* it ends where the bar it is in ends (too near to fade out by then: a bar later) */
        const float t = to_bar < 600.0f ? to_bar + (float)pat : to_bar;
        lp.stop = (uint8_t)(to_bar < 600.0f ? 2u : 1u);
        if (t < lp.end_in)
            lp.end_in = t;
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
    if (lp.state == LP_IDLE && !lp.has && !lp.fading) {      /* no loop: the next first layer starts from nothing */
        lp.total = 0;
        lp.bars = 1;
        lp.first = 0;
        lp.stop = 0;
    }
    rs = steer && lp.resync && lp.total && lp.has;
    if (!steer) {
        lp.fr = 0.0f;
        lp.wrapped = 0;
        lp.drift = 0;
    } else if (rs) {                             /* the transport started: to the bar's place (the loop was silent) */
        lp.bar = (uint8_t)(lp.bars - 1u);        /* (the bar before the loop's first: the next bar's start is its start) */
        lp.frame = (uint32_t)(((float)lp.bar + phase) / (float)lp.bars * (float)lp.total) % lp.total;
        lp.fr = 0.0f;
    }
    lp.resync = 0;
    for (i = 0; i < n; i++) {
        uint32_t k, sub, step = 1;
        float v = 0.0f, fr, nud = 0.0f;
        div = lp.div ? lp.div : 2u;
        len = lp.total / div;
        if ((int32_t)i == bar_off) {             /* a bar starts here */
            if (lp.has && !(lp.state == LP_REC && lp.first)) {
                lp.bar = (uint8_t)((lp.bar + 1u) % lp.bars);
                if (!lp.bar && (!steer || rs)) { /* and so does the loop (a steered one turns round by itself) */
                    lp.frame = 0;
                    lp.fr = 0.0f;
                }
            }
            if (lp.state == LP_REC && lp.first) {
                lp.barsrec++;
                if (lp.stop && !--lp.stop) {     /* the first layer is whole: the loop is as long as it ran */
                    lp.total = lp.frame;
                    lp.bars = lp.barsrec;
                    loop_take_in();
                    lp.frame = 0;
                    lp.fr = 0.0f;
                    lp.bar = 0;
                    len = lp.total / div;
                } else {
                    loop_room(pat);
                    div = lp.div;
                }
            } else if (lp.state == LP_REC) {
                if (lp.stop && !--lp.stop)       /* a layer on top, ended at this bar: its block plays on from here */
                    loop_take_in();
            } else if (lp.state == LP_ARMED) {   /* the take begins */
                lp.state = LP_REC;
                lp.undo = 0;                     /* (the other block is the take's from here) */
                lp.acc = 0.0f;
                lp.accn = 0;
                lp.tk = 0;
                lp.stop = 0;
                if (!lp.has) {                   /* a first layer: from nothing, for as many bars as it is let run */
                    lp.first = 1;
                    lp.frame = 0;
                    lp.fr = 0.0f;
                    lp.total = 0;
                    lp.barsrec = 0;
                    lp.full = lp.low = 0;
                    lp.div = (uint8_t)looper_fit(pat);
                    lp.end_in = 1e9f;
                    loop_room(pat);
                    div = lp.div;
                    len = 0;
                } else {                         /* a layer on top: the loop as it is, and the playing added for a turn at most */
                    lp.first = 0;
                    memcpy(loop_m[lp.cur ^ 1u], loop_m[lp.cur], (size_t)len * sizeof(int16_t));
                    lp.end_in = (float)lp.total;
                }
            }
        }
        if (steer && lp.total && lp.has && playing) {        /* steered to its place in its bars: a little that way */
            const float tot = (float)lp.total;
            float p = phase + (float)i * dph, want, e;
            p -= (float)(uint32_t)p;
            want = ((float)lp.bar + p) / (float)lp.bars * tot;
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
        if (lp.state == LP_REC) {                /* the take: the loop as it plays and the new playing, a sample */
            loop_rec(lp.frame, rec_in[i], lp.first ? LOOP_CAP - 4u : len, div);
            lp.tk++;
            lp.end_in -= 1.0f;
            if (!lp.first && lp.tk >= lp.total)  /* a layer on top has gone once round: it is whole */
                loop_take_in();
        }
        if (steer && lp.total) {
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
