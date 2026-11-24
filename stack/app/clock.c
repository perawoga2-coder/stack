/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the clock, out and in, over USB MIDI. Written for STACK. Part of the unity build: included by app.c.
 *
 * Who leads is not a setting: the FM-1 leads, and sends its clock (24 a quarter note, Start and Stop), until a clock
 * comes in; then it follows that one for as long as it comes, and sends none of its own (nothing that comes in is ever
 * sent on: no loop). When the clock in stops coming for CK_LOST_MS the FM-1 leads again, at the tempo it last followed.
 *
 * Out: the sequencer runs in the audio interrupt, a block ahead of what is heard, so a tick is not sent there: its time
 * is worked out (the block's time, from a steady count of blocks pulled slowly to the real clock, plus where in the
 * block it falls) and app_tick_fast, 10 000 times a second, sends it when that time comes (plat_midi_rt).
 *
 * In: every tick is stamped where it lands (plat_midi_in_t24, fm1.toml midi_stamp). The stamps of a tablet jitter by
 * milliseconds, so the sequencer does not step on them: a filter keeps the time the last tick should have come at and
 * the ticks' period (each new stamp moves both a little), and the sequencer's place is steered towards the place that
 * gives, at 2 % of its speed at most. Start, Continue and Song Position set the place; Stop stops. */

#define CK_T24_FRAME (24000000.0f / 44100.0f)        /* plat_ticks24's ticks a frame */
#define CK_LEAD ((uint32_t)((float)APP_BLOCK * CK_T24_FRAME))   /* a block: what is rendered now is heard about then */
#define CK_LOST_MS 500u
#define CK_BAR 96.0f                                 /* clock ticks in the pattern (16 steps of 6) */
#define SEQ_SF_120 (44100.0f * 60.0f / (120.0f * 4.0f))   /* frames a step at 120 */

/* --- time: plat_ticks24 on the FM-1; in the simulator its own count of app_tick_fast (that clock there is the
 * computer's, not the simulated FM-1's) --- */
#ifdef PLAT_HOST
static volatile uint32_t ck_fast;
static uint32_t ck_now(void) { return ck_fast * 2400u; }
#else
static uint32_t ck_now(void) { return plat_ticks24(); }
#endif

/* --- out: what is to be sent and when (the audio interrupt writes, the 10 kHz tick reads) --- */
static struct { uint32_t due; uint8_t st; } ck_q[16];
static volatile uint8_t ck_w, ck_r;
static volatile uint8_t ck_follow;                   /* 1: a clock comes in and the FM-1 follows it */
static struct { uint32_t last, ivmin, ivmax, clocks, starts, stops, fresh; } ck_sent;   /* (the simulator's checks) */

void app_tick_fast(void)
{
    uint32_t now;
#ifdef PLAT_HOST
    ck_fast++;
#endif
    if (ck_r == ck_w)
        return;
    now = ck_now();
    while (ck_r != ck_w && (int32_t)(now - ck_q[ck_r & 15u].due) >= 0) {
        uint32_t st = ck_q[ck_r & 15u].st;
        ck_r++;
        if (ck_follow) {                             /* (following since it was queued: nothing goes out) */
            ck_sent.fresh = 1;
            continue;
        }
        plat_midi_rt(0x0Fu | st << 8);
        if (st == 0xF8u) {
            if (ck_sent.clocks++ && !ck_sent.fresh) {
                uint32_t iv = now - ck_sent.last;
                ck_sent.ivmax = iv > ck_sent.ivmax ? iv : ck_sent.ivmax;
                ck_sent.ivmin = iv < ck_sent.ivmin || !ck_sent.ivmin ? iv : ck_sent.ivmin;
            }
            ck_sent.last = now;
            ck_sent.fresh = 0;
        } else if (st == 0xFAu) {
            ck_sent.starts++;
        } else {
            ck_sent.stops++;
        }
    }
}
static void ck_send(uint32_t st, uint32_t due)
{
    if ((uint8_t)(ck_w - ck_r) >= 16u)
        return;
    ck_q[ck_w & 15u].due = due;
    ck_q[ck_w & 15u].st = (uint8_t)st;
    ck_w++;
}

/* --- the block's time: blocks counted, pulled 1/32 of the way to the real clock each (the audio interrupt comes a
 * little early or late; the count does not) --- */
static struct { uint32_t t; float f; uint8_t ok; } ck_tb;
static uint32_t ck_block_time(void)
{
    uint32_t now = ck_now();
    int32_t e = (int32_t)(now - ck_tb.t);
    if (!ck_tb.ok || e > 480000 || e < -480000) {    /* (20 ms out: start again from here) */
        ck_tb.t = now;
        ck_tb.f = 0.0f;
        ck_tb.ok = 1;
    } else {
        ck_tb.t += (uint32_t)(e / 32);
    }
    return ck_tb.t;
}
static void ck_block_done(uint32_t n)
{
    uint32_t w;
    ck_tb.f += (float)n * CK_T24_FRAME;
    w = (uint32_t)ck_tb.f;
    ck_tb.t += w;
    ck_tb.f -= (float)w;
}

/* --- in --- */
static struct {
    uint8_t n;                           /* ticks taken since the clock began to come (it stops counting at 255) */
    uint8_t pend;                        /* Start (1) or Continue (2) came: the next tick is the place it starts at */
    uint8_t run;                         /* the leader's transport runs */
    uint8_t locate;                      /* for the sequencer: 1 start, 2 continue, at the latest tick */
    uint32_t pend_t;
    uint32_t tp;                         /* when the latest tick should have come (the filter's), and its fraction */
    float tpf;
    float p;                             /* the ticks' period, in plat_ticks24's ticks */
    uint32_t last;                       /* when the latest tick did come */
    uint32_t free_;                      /* ticks counted round the bar, whatever the transport does */
    uint32_t song;                       /* the leader's place: the tick the next one is, 0..95 */
    float base;                          /* free_ at the pattern's start (the sequencer's place is free_ - base) */
} cki;
static volatile uint8_t ck_ui;           /* for the screen: 1 following began, 2 the clock was lost, 4 Start, 8 Stop */
static float seq_sf = SEQ_SF_120;        /* frames a step while the FM-1 leads: 120, or the tempo it last followed */

static inline float ck_wrap(float x)     /* into 0 .. CK_BAR */
{
    while (x >= CK_BAR)
        x -= CK_BAR;
    while (x < 0.0f)
        x += CK_BAR;
    return x;
}
static float ck_tick_frames(void) { return ck_follow ? cki.p * (1.0f / CK_T24_FRAME) : seq_sf * (1.0f / 6.0f); }
static uint32_t pat_frames(void) { return (uint32_t)(ck_tick_frames() * 6.0f * (float)NSTEP); }

static void ck_tick_in(uint32_t s)       /* a clock tick in, stamped s */
{
    if (!ck_follow) {
        ck_follow = 1;
        cki.n = 0;
        ck_ui |= 1u;
    }
    if (cki.n >= 2u) {
        int32_t e;
        float w, g1 = cki.n < 12u ? 0.5f : 0.1f, g2 = cki.n < 12u ? 0.15f : 0.005f;
        cki.tpf += cki.p;
        w = (float)(uint32_t)cki.tpf;
        cki.tp += (uint32_t)w;
        cki.tpf -= w;
        e = (int32_t)(s - cki.tp);
        if ((float)e > 2.0f * cki.p || (float)e < -2.0f * cki.p) {
            cki.n = 1;                               /* ticks were missed, or it jumped: from this one again */
        } else {
            float m = g1 * (float)e;
            cki.tp += (uint32_t)(int32_t)m;
            cki.p += g2 * (float)e;
        }
    } else if (cki.n == 1u) {
        uint32_t iv = s - cki.tp;
        if (iv > 48000u && iv < 6000000u)            /* (2 .. 250 ms: 1250 down to 10 beats a minute) */
            cki.p = (float)iv;
    }
    if (cki.n < 2u) {
        cki.tp = s;
        cki.tpf = 0.0f;
        if (!cki.n && cki.p == 0.0f)
            cki.p = seq_sf * (1.0f / 6.0f) * CK_T24_FRAME;   /* one tick tells no tempo: the FM-1's own till the next */
    }
    if (cki.p < 192000.0f)                           /* 8 .. 100 ms a tick: 312 down to 25 beats a minute */
        cki.p = 192000.0f;
    if (cki.p > 2400000.0f)
        cki.p = 2400000.0f;
    cki.last = s;
    if (cki.n < 255u)
        cki.n++;
    cki.free_ = (cki.free_ + 1u) % 96u;
    if (cki.pend) {
        if (cki.pend == 1u)
            cki.song = 0;
        cki.base = ck_wrap((float)cki.free_ - (float)cki.song);
        cki.locate = cki.pend;
        cki.pend = 0;
        cki.run = 1;
        seq_play_req = 1;
        ck_ui |= 4u;
    } else if (cki.run) {
        cki.song = (cki.song + 1u) % 96u;
    }
}

static void ck_midi(uint32_t st, uint32_t d1, uint32_t d2, uint32_t stamp)   /* the audio interrupt: MIDI in, real-time */
{
    if (st == 0xF8u) {
        ck_tick_in(stamp);
    } else if (st == 0xFAu || st == 0xFBu) {
        cki.pend = (uint8_t)(st == 0xFAu ? 1u : 2u);
        cki.pend_t = stamp;
    } else if (st == 0xFCu) {
        cki.pend = 0;
        if (ck_follow && cki.run) {
            cki.run = 0;
            cki.song = (cki.song + 1u) % 96u;        /* (the next tick's place, for a Continue) */
            seq_play_req = 0;
            ck_ui |= 8u;
        }
    } else if (st == 0xF2u && !cki.run) {            /* Song Position, in sixteenths: where a Continue starts */
        cki.song = (d1 | d2 << 7) * 6u % 96u;
    }
}

/* MIDI in for the audio interrupt, with its stamp. In the simulator the script's messages are taken each millisecond
 * (app_host_ms) and stamped then, as the FM-1 stamps them where they land */
#ifdef PLAT_HOST
static struct { uint32_t pkt, t; int port; } ck_hq[64];
static uint32_t ck_hw, ck_hr;
void app_host_ms(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        ck_hq[ck_hw % 64u].pkt = pkt;
        ck_hq[ck_hw % 64u].port = port;
        ck_hq[ck_hw++ % 64u].t = ck_now();
    }
}
static int ck_midi_in(uint32_t *pkt, uint32_t *stamp, int *port)
{
    if (ck_hr == ck_hw)
        return 0;
    *pkt = ck_hq[ck_hr % 64u].pkt;
    *port = ck_hq[ck_hr % 64u].port;
    *stamp = ck_hq[ck_hr++ % 64u].t;
    return 1;
}
#else
static int ck_midi_in(uint32_t *pkt, uint32_t *stamp, int *port)
{
    if (!plat_midi_in(pkt, port))
        return 0;
    *stamp = plat_midi_in_t24();
    return 1;
}
#endif
