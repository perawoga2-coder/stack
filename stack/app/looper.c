/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the looper on track 8. Written for STACK. Part of the unity build: included by app.c.
 *
 * A loop is a whole number of bars (passes of the pattern) long, so it stays in time with the steps; it need not start
 * on a bar line. It holds what was played by hand (render_part's rec_in: the live voices, before the effects, mono),
 * a layer at a time. Nothing here touches the flash.
 *
 * A take starts the moment REC goes down (since 4.03: nothing waits for a bar).
 *  - The first layer: recorded from the press on. REC again: the loop is the recorded time rounded to the nearest
 *    whole number of bars at the tempo and time signature (one at least; no more than a block holds). Rounded up, it
 *    records on until that long; rounded down, it is cut there. The loop's start is where REC went down, and lp.off
 *    remembers where in the bar that is, so the loop goes round with the bars from then on.
 *  - A layer on top: the loop that plays plus the new playing, into the other of the two blocks, from the place the
 *    loop is at, for one pass at most or until REC again. Its first half second waits in a small queue (loop_fifo)
 *    until REC is let go: a press that turns into a hold (undo, clear) has then written nothing, and the undo that
 *    sleeps in the other block is whole. When the take ends the blocks change places between two samples, and the one
 *    left is the undo.
 *
 * No seams: the new playing fades in over the take's first 5 ms and out over its last 10 ms. An undo crosses from one
 * block to the other over 10 ms at the same place in the loop; a clear and a stop of the transport fade out over 15 ms.
 *
 * With a clock coming in (clock.c), or a tempo other than 120, a bar is not a whole number of frames, nor quite the
 * loop's own length: the loop is then steered to its place in the bars (loop_block's phase): it plays up to 0.1 %
 * faster or slower, a frame doubled or left out here and there, which is not heard, and turns round by itself where
 * it ends. More than that is a change of tempo: the loop cannot follow it (it is sound, it does not stretch), it goes
 * out of time and lp.drift says so.
 *
 * The memory (since 4.03): a block is IMA ADPCM, 4 bits a sample at 22.05 kHz, in records of LB samples, each with
 * the coder's state before its first sample (4 bytes: its value, its step index), so a record decodes by itself: the
 * loop can be entered anywhere, and records of the two blocks can stand next to each other. 16-bit samples at this
 * size held 2 s (4 s at 11 kHz); this holds 8 s at 22 kHz. The price is the coder's noise, some 30 to 40 dB under
 * the sound, and a little more of it in whatever a later layer's take passes over (it is decoded and coded again).
 * 1.0 is 16384, so two layers at full level fit. */

#define LB 256u                          /* samples a record */
#define LREC 132u                        /* bytes a record: the coder's state (4), then LB samples of 4 bits */
#define LOOP_NB 690u                     /* records a block */
#define LOOP_CAP (LOOP_NB * LB)          /* samples a block: 176 640, 8.01 s (four bars of 16 steps at 120) */
#define LOOP_PCM 44104u                  /* PERFORM borrows the second block as this many 16-bit samples (perform.c) */
static int16_t loop_m[2][LOOP_NB * LREC / 2u] __attribute__((section(".pool")));     /* 2 x 91 080 B */
#define LOOP_B(b) ((uint8_t *)loop_m[b])
#define PRE_N 13312u                     /* the queue between the playing and the coder: 0.6 s of samples */
static int16_t loop_fifo[PRE_N];

enum { LP_IDLE, LP_PRE, LP_REC };        /* LP_PRE: a layer on top whose REC is still down (nothing written yet) */
enum { LQ_NONE, LQ_ARM, LQ_CANCEL, LQ_UNDO, LQ_CLEAR, LQ_STOP };
typedef struct {                         /* the coder at a place in a block */
    uint8_t *m;
    uint32_t k;                          /* the sample that comes next */
    int32_t pred, idx;
} lcodec_t;
typedef struct {                         /* a block being played: two samples next to each other, and the coder after them */
    lcodec_t c;
    float s0, s1;
    uint32_t at;                         /* the sample s0 is (~0: none) */
} lplay_t;
static struct {
    volatile uint8_t own;                /* track 8 is the looper's: a loop there, or a take running */
    volatile uint8_t has;                /* a loop plays */
    volatile uint8_t state;              /* LP_* */
    volatile uint8_t layers;
    volatile uint8_t undo;               /* the other block holds the loop as it was a layer ago */
    volatile uint8_t req;                /* LQ_*: the main loop asks, the audio interrupt does */
    uint8_t hold;                        /* PERFORM still has the second block (perform.c: it lets go within a few ms): the coder waits */
    volatile uint8_t trk;                /* the track the loop lives on: 7 (T8), or 6 (T7): it changes places each time a take is in (4.04) */
    volatile uint8_t swap;               /* a take is in: the loop moves to the other track at the next block's start (render_part) */
    uint8_t armw;                        /* REC went down with the transport just started: blocks it has waited for it to run */
    volatile uint8_t go;                 /* REC was let go in time: the layer on top that waits in the queue is taken */
    volatile uint8_t div;                /* frames of the mix a sample: 2 (22.05 kHz) */
    volatile uint8_t bars;               /* the bars (passes of the pattern) the loop is long */
    volatile uint32_t done;              /* takes finished (the screen says so) */
    volatile uint32_t ver;               /* goes up whenever the loop that plays is another (a take in, an undo, a clear): a dump knows */
    volatile uint32_t frame;             /* frames since the loop's start */
    volatile uint32_t total;             /* frames in the loop */
    uint8_t cur;                         /* the block that plays */
    uint8_t prev_ok;                     /* the other block is a whole loop (else: nothing was there) */
    uint8_t bar;                         /* the bar of the loop's bars that the pattern is in */
    uint8_t fading;                      /* 1 across to the other block (undo); 2 out (clear) */
    float fade;
    float run;                           /* 1 while the transport plays, down to 0 over 15 ms when it stops */
    float acc;                           /* a take: the frames of the sample being made */
    uint8_t accn;                        /* and how many */
    uint8_t resync;                      /* the transport started: a steered loop goes to the bar's place at once */
    uint8_t wrapped;                     /* a steered loop turned round at the last frame: its start is this one */
    volatile uint8_t drift;              /* a steered loop is over 10 ms from the bar: the tempo changed */
    float fr;                            /* a steered loop: the part of a frame it is on from lp.frame */
    float nudge;                         /* (the simulator's checks: the most it was sped or slowed) */
    uint32_t off;                        /* the loop's frame at the start of its bars' first bar (it starts where REC went down) */
    lplay_t pl[2];                       /* the block that plays, and the one an undo crosses to */
    /* a take */
    volatile uint8_t first;              /* it is the loop's first layer */
    volatile uint8_t stop;               /* REC was pressed again: it ends (a first layer: when it is lp.want long) */
    volatile uint8_t cap;                /* the playing is being taken */
    volatile uint8_t full;               /* a first layer ended by itself: the memory holds no bar more (the screen says so) */
    uint8_t fin;                         /* a layer on top, after its end: 1 its last record is filled, 2 the rest are copied */
    uint8_t barseen;                     /* a first layer: a bar began under it */
    volatile uint32_t tk;                /* frames since the take began */
    volatile uint32_t maxf;              /* a first layer: the frames a block holds, in whole bars */
    volatile uint32_t want;              /* a first layer, REC pressed again: the frames it will be */
    uint32_t bartk;                      /* a first layer: lp.tk when the last bar began */
    float end_in;                        /* frames until the playing is no longer taken (very many: not known yet) */
    uint32_t capk;                       /* the loop's sample the playing is being gathered for */
    uint32_t ncap, nmax;                 /* a layer on top: samples taken, and the most (once round, less its first record's start) */
    uint32_t b0, k0;                     /* a layer on top: the record and the sample it began in */
    uint32_t cb;                         /* fin 2: the record to copy next */
    uint32_t fw, fq, fn;                 /* the queue: where it is written, read, and how many wait */
    lcodec_t rd, we;                     /* the take's coder: the loop that plays read, the other block written */
} lp = {.div = 2, .bars = 1, .trk = 7};

static float rec_in[APP_BLOCK];              /* what is played by hand this block (render_part) */

/* a pattern of `frames` frames: 2 a bar of it fits a block (at 22.05 kHz), 0 it does not */
static uint32_t looper_fit(uint32_t frames)
{
    return frames / 2u <= LOOP_CAP ? 2u : 0u;
}

static inline uint8_t *lc_rec(const lcodec_t *c) { return c->m + (c->k / LB) * LREC; }

static inline void lc_head_get(lcodec_t *c)  /* the state a record begins with */
{
    const uint8_t *h = lc_rec(c);
    c->pred = (int16_t)(uint16_t)(h[0] | (uint32_t)h[1] << 8);
    c->idx = h[2] > 88u ? 88 : h[2];
}

static inline int32_t lc_dec(lcodec_t *c, uint32_t len)      /* the next sample of a loop of len samples (it goes round) */
{
    uint32_t j;
    int32_t r;
    if (c->k >= len)
        c->k = 0;
    j = c->k & (LB - 1u);
    if (!j)
        lc_head_get(c);
    r = ima_next(lc_rec(c) + 4u, j, &c->pred, &c->idx);
    c->k++;
    return r;
}

static inline void lc_enc(lcodec_t *c, int32_t s, uint32_t len)      /* the next sample written (len 0: it does not go round) */
{
    uint32_t j, code = 0;
    int32_t step, diff;
    uint8_t *h, *d;
    if (len && c->k >= len)
        c->k = 0;
    if (c->k >= LOOP_CAP)
        return;
    j = c->k & (LB - 1u);
    h = lc_rec(c);
    d = h + 4u + (j >> 1);
    if (!j) {
        h[0] = (uint8_t)c->pred;
        h[1] = (uint8_t)((uint32_t)c->pred >> 8);
        h[2] = (uint8_t)c->idx;
        h[3] = 0;
    }
    s = s > 32767 ? 32767 : s < -32767 ? -32767 : s;
    step = IMA_STEP[c->idx];
    diff = s - c->pred;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4u;
        diff -= step;
    }
    if (diff >= step >> 1) {
        code |= 2u;
        diff -= step >> 1;
    }
    if (diff >= step >> 2)
        code |= 1u;
    *d = (uint8_t)(j & 1u ? (*d & 0x0Fu) | code << 4 : (*d & 0xF0u) | code);
    ima_next(h + 4u, j, &c->pred, &c->idx);  /* (the state goes on as the decoder's will) */
    c->k++;
}

/* block blk of a loop of len samples, at sample k and fr on. It is read forwards, a sample at a time; anywhere else
 * is found from its record's start (256 samples at most) */
static float loop_at(lplay_t *p, uint32_t blk, uint32_t k, uint32_t len, float fr)
{
    if (!len)
        return 0.0f;
    if (p->c.m != LOOP_B(blk) || p->at != k) {
        uint32_t n1 = p->at + 1u < len ? p->at + 1u : 0u;
        if (p->c.m == LOOP_B(blk) && p->at != ~0u && k == n1) {
            p->s0 = p->s1;
        } else {
            uint32_t n;
            p->c.m = LOOP_B(blk);
            p->c.k = k & ~(LB - 1u);
            for (n = (k & (LB - 1u)) + 1u; n; n--)
                p->s0 = (float)lc_dec(&p->c, len);
        }
        p->at = k;
        p->s1 = (float)lc_dec(&p->c, len);
    }
    return (p->s0 + (p->s1 - p->s0) * fr) * (1.0f / 16384.0f);
}

/* the main loop (a dump): n samples of block blk, a loop of len samples, 16 bits each, low byte first; from sample k
 * counted from where the loop's first bar begins (a dump is a loop that starts on its bar line, whatever the place
 * REC went down at: an app lays it on its own bars as it is) */
static void loop_pcm(uint32_t blk, uint32_t k, uint32_t n, uint8_t *dst, uint32_t len)
{
    lcodec_t c;
    uint32_t j;
    if (!len)
        return;
    k = (k + lp.off / 2u) % len;
    c.m = LOOP_B(blk);
    c.k = k & ~(LB - 1u);
    c.pred = c.idx = 0;
    for (j = k & (LB - 1u); j; j--)
        lc_dec(&c, len);
    for (j = 0; j < n; j++) {
        const int32_t v = lc_dec(&c, len);
        dst[2u * j] = (uint8_t)v;
        dst[2u * j + 1u] = (uint8_t)((uint32_t)v >> 8);
    }
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
    lp.cap = 0;
    lp.fin = 0;
    lp.pl[0].at = ~0u;
    lp.swap = 1;
    lp.done++;
    lp.ver++;
}

static void loop_drop(void)                  /* the take is let go: nothing of it stays */
{
    lp.state = LP_IDLE;
    lp.first = 0;
    lp.stop = 0;
    lp.cap = 0;
    lp.fin = 0;
    lp.fn = 0;
}

/* the take's coder, a block of the audio: the samples that wait in the queue, each on top of the loop's own there
 * (a layer on top), into the other block; then, for a layer on top that has ended, the rest of its last record as
 * the loop is, the records it never came to copied as they are, and the blocks change places */
static void loop_work(void)
{
    const uint32_t len = lp.first ? 0u : lp.total / 2u;
    uint32_t budget = 512u;
    if (lp.state != LP_REC || lp.hold)
        return;
    while (budget && lp.fn) {
        int32_t s = loop_fifo[lp.fq];
        lp.fq = lp.fq + 1u < PRE_N ? lp.fq + 1u : 0u;
        lp.fn--;
        if (!lp.first)
            s += lc_dec(&lp.rd, len);
        lc_enc(&lp.we, s, len);
        budget--;
    }
    if (lp.first || lp.cap || lp.fn || !len)
        return;
    if (lp.fin < 2u) {
        while (budget && (lp.we.k & (LB - 1u)) && lp.we.k < len) {
            lc_enc(&lp.we, lc_dec(&lp.rd, len), len);
            budget--;
        }
        if ((lp.we.k & (LB - 1u)) && lp.we.k < len)
            return;
        lp.cb = lp.we.k < len ? lp.we.k / LB : 0u;
        lp.fin = 2;
    }
    for (budget = 64u; budget && lp.cb != lp.b0; budget--) {
        memcpy(LOOP_B(lp.cur ^ 1u) + lp.cb * LREC, LOOP_B(lp.cur) + lp.cb * LREC, LREC);
        lp.cb = (lp.cb + 1u) * LB < len ? lp.cb + 1u : 0u;
    }
    if (lp.cb == lp.b0)
        loop_take_in();
}

/* a first layer is whole at `want` frames: what waits is coded, its last 10 ms fade out where they lie (decoded and
 * coded again in place: 476 samples at most), and it plays from where it is now. p: frames into the bar now */
static void loop_first_close(uint32_t want, uint32_t bars, uint32_t p)
{
    const uint32_t len = want / 2u, n = len < 220u ? len : 220u, a = len - n;
    lcodec_t d, e;
    uint32_t k;
    while (lp.fn) {
        lc_enc(&lp.we, loop_fifo[lp.fq], 0);
        lp.fq = lp.fq + 1u < PRE_N ? lp.fq + 1u : 0u;
        lp.fn--;
    }
    d.m = e.m = LOOP_B(lp.cur ^ 1u);
    d.k = e.k = a & ~(LB - 1u);
    d.pred = d.idx = 0;
    lc_head_get(&e);
    for (k = d.k; k < len; k++) {
        int32_t s = lc_dec(&d, LOOP_CAP);
        if (k >= a)
            s = s * (int32_t)(len - 1u - k) / (int32_t)n;
        lc_enc(&e, s, 0);
    }
    lp.total = want;
    lp.bars = (uint8_t)bars;
    loop_take_in();
    lp.frame = lp.tk >= want ? (lp.tk - want) % want : 0u;
    lp.fr = 0.0f;
    lp.bar = 0;
    lp.off = (lp.frame + want - p % want) % want;
}

/* the audio interrupt, a block: what the main loop asked for, then the loop's n frames into out. bar_off: where in
 * this block the pattern's first step starts (-1: not in it); playing: the transport runs; pat: the pattern's frames;
 * to_bar: frames from the block's start to the next bar's; phase: -1, the loop goes by its own count (the FM-1
 * leads at 120: a bar is a whole number of frames); else the place in the bar at the block's start, 0 .. 1, that the
 * loop is steered to, dph more each frame */
static uint32_t loop_block(float *out, uint32_t n, int32_t bar_off, uint32_t playing, uint32_t pat, float to_bar, float phase, float dph)
{
    const uint32_t steer = phase >= 0.0f;
    const float barf = steer && dph > 0.0f ? 1.0f / dph : (float)pat;       /* a bar's frames */
    const uint32_t maxb = (uint32_t)((float)(LOOP_CAP * 2u) / barf);
    uint32_t i, q = lp.req, len, sounding = 0, rs;
    lp.req = LQ_NONE;
    lp.maxf = steer ? (uint32_t)((float)maxb * barf) : maxb * pat;
    if (q == LQ_ARM && !playing && lp.armw < 40u) {
        lp.armw++;                               /* (REC started the transport: it runs within a block or two) */
        lp.req = LQ_ARM;
    } else if (q == LQ_ARM && lp.state == LP_IDLE && !lp.fading && playing) {
        lp.armw = 0;
        lp.fw = lp.fq = lp.fn = 0;
        lp.acc = 0.0f;
        lp.accn = 0;
        lp.tk = 0;
        lp.stop = 0;
        lp.fin = 0;
        lp.full = 0;
        if (!lp.has) {                           /* a first layer: from nothing, from now */
            if (maxb) {
                lp.state = LP_REC;
                lp.cap = 1;
                lp.first = 1;
                lp.undo = 0;
                lp.prev_ok = 0;
                lp.frame = 0;
                lp.fr = 0.0f;
                lp.total = 0;
                lp.bars = 1;
                lp.barseen = 0;
                lp.want = 0;
                lp.end_in = 1e9f;
                lp.capk = 0;
                lp.we.m = LOOP_B(lp.cur ^ 1u);
                lp.we.k = 0;
                lp.we.pred = lp.we.idx = 0;
            }
        } else {                                 /* a layer on top: gathered from here; written once REC is let go */
            lp.state = LP_PRE;
            lp.cap = 1;
            lp.first = 0;
            lp.k0 = lp.capk = lp.frame / 2u < lp.total / 2u ? lp.frame / 2u : 0u;
            lp.b0 = lp.k0 / LB;
            lp.ncap = 0;
            lp.nmax = lp.total / 2u - (lp.k0 & (LB - 1u));
            lp.end_in = (float)(lp.nmax * 2u);
        }
    } else if (q == LQ_CANCEL) {
        loop_drop();
    } else if (q == LQ_STOP && lp.state == LP_REC && !lp.stop) {
        lp.stop = 1;
        if (lp.first) {                          /* the nearest whole number of bars: one at least, no more than fit */
            uint32_t b = (uint32_t)((float)lp.tk / barf + 0.5f);
            b = b < 1u ? 1u : b > maxb ? maxb : b;
            lp.bars = (uint8_t)b;
            lp.want = steer ? (uint32_t)((float)b * barf + 0.5f) & ~1u : b * pat;
        } else if (lp.end_in > 441.0f) {
            lp.end_in = 441.0f;
        }
    } else if (q == LQ_UNDO && lp.state == LP_IDLE && lp.undo && !lp.fading) {
        lp.fading = 1;
        lp.fade = 0.0f;
        lp.undo = 0;
        lp.pl[1].at = ~0u;
    } else if (q == LQ_CLEAR) {
        loop_drop();
        lp.undo = 0;
        if (lp.has) {
            lp.fading = 2;
            lp.fade = 0.0f;
        } else {
            lp.layers = 0;
        }
    }
    if (lp.state == LP_PRE && lp.go && !lp.hold) {           /* REC let go: the layer on top is taken. Its first record, up to where
                                                  * it began, is the loop as it is; the other block is its own from here */
        const uint32_t l = lp.total / 2u;
        lp.state = LP_REC;
        lp.undo = 0;
        lp.prev_ok = 0;
        lp.rd.m = LOOP_B(lp.cur);
        lp.we.m = LOOP_B(lp.cur ^ 1u);
        lp.rd.k = lp.we.k = lp.b0 * LB;
        lp.rd.pred = lp.rd.idx = 0;
        lc_head_get(&lp.rd);
        lp.we.pred = lp.rd.pred;
        lp.we.idx = lp.rd.idx;
        while (lp.we.k < lp.k0)
            lc_enc(&lp.we, lc_dec(&lp.rd, l), l);
    }
    if (!playing && lp.state != LP_IDLE)         /* the transport stopped under a take: the take is let go */
        loop_drop();
    if (lp.state == LP_IDLE && !lp.has && !lp.fading) {      /* no loop: the next first layer starts from nothing */
        lp.total = 0;
        lp.bars = 1;
        lp.off = 0;
    }
    rs = steer && lp.resync && lp.total && lp.has;
    if (!steer) {
        lp.fr = 0.0f;
        lp.wrapped = 0;
        lp.drift = 0;
    } else if (rs) {                             /* the transport started: to the bar's place (the loop was silent) */
        lp.bar = (uint8_t)(lp.bars - 1u);        /* (the bar before the first: the next bar's start is the first's) */
        lp.frame = (lp.off + (uint32_t)(((float)lp.bar + phase) / (float)lp.bars * (float)lp.total)) % lp.total;
        lp.fr = 0.0f;
    }
    lp.resync = 0;
    for (i = 0; i < n; i++) {
        uint32_t k, sub, step = 1;
        float v = 0.0f, fr, nud = 0.0f;
        len = lp.total / 2u;
        if ((int32_t)i == bar_off) {             /* a bar starts here */
            if (lp.state == LP_REC && lp.first) {
                lp.bartk = lp.tk;
                lp.barseen = 1;
            } else if (lp.has && lp.total) {
                lp.bar = (uint8_t)((lp.bar + 1u) % lp.bars);
                if (!steer) {                    /* the loop's place at this bar (it is there already, but after a start) */
                    lp.frame = (lp.off + lp.bar * pat) % lp.total;
                    lp.fr = 0.0f;
                }
            }
        }
        if (steer && lp.total && lp.has && playing) {        /* steered to its place in its bars: a little that way */
            const float tot = (float)lp.total;
            float p = phase + (float)i * dph, want, e;
            p -= (float)(uint32_t)p;
            want = (float)lp.off + ((float)lp.bar + p) / (float)lp.bars * tot;
            if (want >= tot)
                want -= tot;
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
        k = lp.frame / 2u;
        sub = lp.frame - k * 2u;
        if (k >= len)
            k = len ? len - 1u : 0u;
        fr = ((float)sub + lp.fr) * 0.5f;
        if (lp.has) {
            v = loop_at(&lp.pl[0], lp.cur, k, len, fr);
            if (lp.fading == 1u) {               /* undo: across to the loop as it was, or to nothing */
                float o = lp.prev_ok ? loop_at(&lp.pl[1], lp.cur ^ 1u, k, len, fr) : 0.0f;
                v += (o - v) * lp.fade;
                lp.fade += 1.0f / 441.0f;
                if (lp.fade >= 1.0f) {
                    lp.fading = 0;
                    lp.cur ^= 1u;
                    lp.pl[0] = lp.pl[1];
                    lp.pl[1].at = ~0u;
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
        if (lp.cap) {                            /* the take: the playing, faded in over its first 5 ms and out over its last 10 */
            float g = lp.tk < 220u ? (float)lp.tk * (1.0f / 220.0f) : 1.0f;
            if (lp.end_in < 441.0f)
                g *= lp.end_in > 0.0f ? lp.end_in * (1.0f / 441.0f) : 0.0f;
            lp.acc += rec_in[i] * g;
            lp.accn++;
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
            lp.frame += step;
            if (lp.frame >= lp.total) {
                lp.frame -= lp.total;            /* a steered loop turns round by itself */
                lp.wrapped = 1;
            }
        } else {
            lp.frame++;
            if (lp.total && lp.frame >= lp.total)
                lp.frame = 0;                    /* (its length is its bars': it turns round where they do) */
        }
        if (lp.cap) {
            const uint32_t nk = lp.frame / 2u;
            lp.tk++;
            lp.end_in -= 1.0f;
            if (nk != lp.capk) {                 /* the loop's sample is done: into the queue */
                const float s = lp.acc / (float)lp.accn * 16384.0f;
                lp.capk = nk;
                lp.acc = 0.0f;
                lp.accn = 0;
                if (lp.fn >= PRE_N) {            /* (REC held, and never taken nor let go of: the main loop stood still) */
                    loop_drop();
                } else {
                    loop_fifo[lp.fw] = (int16_t)(s > 32767.0f ? 32767.0f : s < -32767.0f ? -32767.0f : s);
                    lp.fw = lp.fw + 1u < PRE_N ? lp.fw + 1u : 0u;
                    lp.fn++;
                    lp.ncap++;
                }
            }
            if (lp.state == LP_REC && lp.first) {
                if (lp.stop ? lp.tk >= lp.want : lp.tk >= lp.maxf) {
                    const uint32_t tb = to_bar > (float)i ? (uint32_t)(to_bar - (float)i) : 0u;
                    if (!lp.stop) {              /* the memory is full: as many bars as fit */
                        lp.want = lp.maxf & ~1u;
                        lp.bars = (uint8_t)maxb;
                        lp.full = 1;
                    }
                    loop_first_close(lp.want, lp.bars, lp.barseen ? lp.tk - lp.bartk : (uint32_t)barf > tb ? (uint32_t)barf - tb : 0u);
                }
            } else if (lp.cap && (lp.end_in <= 0.0f || lp.ncap >= lp.nmax)) {
                lp.cap = 0;                      /* a layer on top has what it takes: loop_work brings it in */
            }
        }
        out[i] = v * lp.run;
        sounding |= v != 0.0f;
    }
    loop_work();
    return sounding || lp.has;
}
