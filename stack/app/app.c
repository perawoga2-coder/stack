/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip
 * Started from fm1-base's template/app.c, Copyright (C) 2026 Charles Vestal (MPL-2.0): the unity build's shape,
 * app_say, the key scan with velocity and the text helpers follow it. Nothing here comes from Felucca or from the
 * Burger Studio firmware (bs19); the sounds are audio only (app/SOUNDS.md). */
/* STACK. Eight tracks, each with one sound picked from a single list; the keys play the selected track.
 *   ALGORITHM  the track, 1..8 (it stops at the ends)
 *   PRESETS    the selected track's sound, from one list that stops at its ends; nothing else changes
 *   REC        the looper (looper.c): a layer is recorded from the moment it goes down; again: it ends (a first layer
 *              at the nearest whole number of bars). Held and let go after 3 s: the last layer undone; held 5 s: the
 *              loop cleared. (On GLO: disconnect.)
 *   SELECT     the tempo, 1 BPM a detent, 40..240, on every page (the same value as EDIT page 2's K4)
 *   KNOB 1..4  the selected track's PITCH, FILTER (left low-pass, the middle off, right high-pass), DECAY, LEVEL
 *   OCT- OCT+  the keys' octave, on release
 *   PLAY       play / stop the steps;  SEQ  the step page and back (16 steps a track, the white keys)
 * Samples sent by the Burger Studio app come into MY 1 .. MY 3, at the end of the list (my_samples.c).
 * No randomness anywhere: the same press gives the same result. The tracks and steps are not saved; the flash is
 * written only while the app sends a sample, and only inside the three slots.
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"

/* ------------------------------------------------------------------ sounds --- */
enum { CAT_DRUMS, CAT_BASS, CAT_KEYS, CAT_PAD, CAT_LEAD, CAT_FX, NCAT };
static const char *const CAT_NAME[NCAT] = {"DRUMS", "BASS", "KEYS", "PAD", "LEAD", "FX"};
typedef struct {
    const char *name;
    const void *d;                       /* IMA ADPCM, 4 bits a sample, low nibble first; SF_PCM: int16_t */
    uint32_t n, ls, le;                  /* samples; the loop [ls, le) when SF_LOOP */
    float rate, hz;                      /* its sample rate, and the pitch it sounds at that rate */
    uint16_t flags;
    uint8_t cat;
    int16_t lpred;                       /* ADPCM: the decoder's state as it comes to the loop's first sample */
    uint8_t lidx;
    const uint8_t *ck;                   /* ADPCM: the decoder's state before every SND_CK'th sample, 3 bytes each (its
                                          * value, low byte first, and its step index); 0: none (PCM, or not made yet) */
} sound_t;
#define SND_CK 512u                      /* (tools/make_sounds.py CK) */
#define SF_LOOP 1u                       /* holds while the key is down */
#define SF_DRUM 2u                       /* a hit: plays through, at its own pitch on every key */
#define SF_PCM 4u                        /* 16-bit PCM (a single cycle) */
#include "sounds_data.h"

/* IMA ADPCM, the standard's two tables (IMA Digital Audio Focus and Technical Working Groups, "Recommended Practices
 * for Enhancing Digital Audio Compatibility in Multimedia Systems", 1992) */
static const int16_t IMA_STEP[89] = {       /* the standard's 89 step sizes, ten a row */
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* the one list PRESETS walks: EMPTY, the built-in sounds, then the three slots for samples from the app (empty in
 * this version: the app's samples come in a later one) */
#define NMY 3u
#define NLIST (1u + NSOUNDS + NMY)
#include "my_samples.c"                  /* MY 1 .. MY 3: the samples the Burger Studio app sends */
/* the sound at a place in the list (a sample from the app: its first zone), 0 = none there */
static const sound_t *list_sound(uint32_t i)
{
    if (i >= 1u && i <= NSOUNDS)
        return &SOUNDS[i - 1u];
    return i > NSOUNDS && i < NLIST && my[i - NSOUNDS - 1u].valid ? &my[i - NSOUNDS - 1u].z[0] : 0;
}

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void list_name(uint32_t i, char *b)   /* "EMPTY", "KICK ROCK", "MY 1 SPOONS", "MY 2 (EMPTY)", "MY 2 (IN MY 1)" */
{
    const sound_t *s = list_sound(i);
    char n[4];
    b[0] = 0;
    if (i >= 1u && i <= NSOUNDS) {
        text_cat(b, s->name);
    } else if (!i) {
        text_cat(b, "EMPTY");
    } else {
        const my_slot_t *m = &my[i - NSOUNDS - 1u];
        int_text((int32_t)(i - NSOUNDS), n);
        text_cat(b, "MY ");
        text_cat(b, n);
        text_cat(b, " ");
        if (m->valid) {
            text_cat(b, m->name[0] ? m->name : "SAMPLE");
            if (m->crc == 2u)
                text_cat(b, " ?");               /* (its data is not what its header's CRC says) */
        } else if (m->why > MY_BLANK) {          /* something is there and was refused: say what (a test version) */
            static const char HEX[] = "0123456789ABCDEF";
            char h[9];
            uint32_t q;
            for (q = 0; q < 8u; q++)
                h[q] = HEX[(m->head >> (28u - 4u * q)) & 15u];
            h[8] = 0;
            int_text(m->why, n);
            text_cat(b, m->why == MY_NOFLASH ? "(NO FLASH" : "(BAD ");
            if (m->why != MY_NOFLASH) {
                text_cat(b, n);
                text_cat(b, " ");
                text_cat(b, h);
            }
            text_cat(b, ")");
        } else if (m->cov) {
            int_text(m->cov, n);
            text_cat(b, "(IN MY ");
            text_cat(b, n);
            text_cat(b, ")");
        } else {
            text_cat(b, "(EMPTY)");
        }
    }
}

static void cat_text(uint32_t i, char *b)    /* "KEYS 3/15": the group and the place in it; "MY 2/3"; EMPTY: "" */
{
    uint32_t k, at = i - NSOUNDS, of = NMY;
    const char *g = "MY";
    char n[8];
    b[0] = 0;
    if (!i)
        return;
    if (i <= NSOUNDS) {
        const uint32_t c = SOUNDS[i - 1u].cat;
        g = CAT_NAME[c];
        for (k = at = of = 0; k < NSOUNDS; k++)
            if (SOUNDS[k].cat == c) {
                of++;
                if (k < i)
                    at++;
            }
    }
    text_cat(b, g);
    text_cat(b, " ");
    int_text((int32_t)at, n);
    text_cat(b, n);
    text_cat(b, "/");
    int_text((int32_t)of, n);
    text_cat(b, n);
}

/* ------------------------------------------------------------------ tracks --- */
#define ENV_AUTO (-1)                    /* RELEASE as before the ENV page: a held sound lets go at DECAY's pace, a hit
                                          * plays through */
#define ENV_V_START 75                   /* VELOCITY as before: a quarter of the loudness fixed, the rest the finger's */
#define REC_SHORT_MS 500u                /* REC let go before this: a short press (a layer starts, or ends) */
#define REC_UNDO_MS 3000u                /* REC let go after this: the last layer goes */
#define REC_CLEAR_MS 5000u               /* REC held this long: the whole loop goes */
/* the header says what REC is doing: it is held past a short press, or a take waits or runs */
#define REC_HELD (ui.rec_t && !ui.rec_fired && (int32_t)(plat_ms() - ui.rec_t) >= (int32_t)REC_SHORT_MS)
#define LP_TAKE (lp.state != LP_IDLE || lp.req == LQ_ARM)   /* a take waits or runs */
#define LP_TAKE_LIVE (LP_TAKE && lp.req != LQ_CANCEL)          /* and is not being let go (REC's hold line, its undo) */
#define REC_UP (REC_HELD || LP_TAKE)
#define NTRK 8u
#define NSTEP 16u
#define FILT_DEAD 3                      /* FILTER: -FILT_DEAD .. +FILT_DEAD is off (seven detents wide) */
#define FILT_MAX (50 + FILT_DEAD)
enum { ED_LOOP_AUTO, ED_LOOP_OFF, ED_LOOP_ON };
typedef struct {
    volatile uint8_t sound;              /* its place in the list */
    volatile int8_t pitch;               /* semitones, -24 .. +24 */
    volatile int8_t filt;                /* -FILT_MAX (low-pass, closed) .. 0 off .. +FILT_MAX (high-pass) */
    volatile uint8_t decay, level;       /* 0 .. 100 */
    /* the envelope (the ENV page). As they start, a track sounds as it did before there was one */
    volatile uint8_t env_a;              /* ATTACK 0 .. 100: at once (1 ms) .. a fade in of about 2 s */
    volatile int8_t env_r;               /* RELEASE ENV_AUTO, or 0 .. 100: stops at once (4 ms) .. a tail of about 1 s */
    volatile uint8_t env_s;              /* SUSTAIN 0 .. 100: the level a held sound settles at, at DECAY's pace */
    volatile uint8_t env_v;              /* VELOCITY 0 .. 100: how much the finger's weight changes the loudness */
    /* the EDIT page: where the sound starts, how much of it plays, looped or not, backwards or not. As they start
     * (0, off, AUTO, 100) the sound is as it is */
    volatile uint8_t ed_start;           /* START 0 .. 99: per cent of the sound */
    volatile uint8_t ed_rev;             /* REVERSE 0 off, 1 on */
    volatile uint8_t ed_loop;            /* LOOP: ED_LOOP_AUTO as the sound is, ED_LOOP_OFF, ED_LOOP_ON */
    volatile uint8_t ed_len;             /* LENGTH 1 .. 100: per cent of what is left from START (100: to the end) */
    volatile uint8_t fit;                /* EDIT 2's FIT: 0 off, else the sound's length made FIT_BARS bars of the tempo */
    /* the ARP page: the track's keys played one after another while held. RATE 0 is off: the keys as they are */
    volatile uint8_t arp_r;              /* RATE: 0 off, else ARP_R_NAME */
    volatile uint8_t arp_m;              /* MODE: ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_ORDER */
    volatile uint8_t arp_o;              /* OCTAVE 1 .. 3 */
    volatile uint8_t arp_g;              /* GATE 10 .. 100: per cent of a step a note is held */
    /* the LFO (the LFO page): one a track, on the whole track. AMOUNT 0 is off: the track is as without one */
    volatile uint8_t lfo_a;              /* AMOUNT 0 .. 100 */
    volatile uint8_t lfo_r;              /* RATE: a division of the tempo, LFO_RATE_NAME */
    volatile uint8_t lfo_t;              /* TARGET: LFO_PITCH, LFO_FILTER, LFO_LEVEL, LFO_PAN */
    volatile uint8_t lfo_s;              /* SHAPE: LFO_SHAPE_NAME */
    volatile uint8_t fx[2];              /* the effect in slot A and in slot B (fx.c FX_*) */
    volatile int8_t fxk[2];              /* their knobs, -FX_MAX .. 0 off .. +FX_MAX */
    /* the EQ (FX page 2, 4.06): LOW (a shelf under EQ_LOW_HZ), MID (a bell at MID FREQ), HIGH (a shelf over
     * EQ_HIGH_HZ), each in dB, EQ_MIN .. EQ_MAX, 0 off; MID FREQ -EQ_F .. EQ_F: 1 kHz x 2^(n/6). All 0: no EQ at all */
    volatile int8_t eq[4];               /* LOW, MID, MID FREQ, HIGH: the knobs' order */
} track_t;
#define EQ_MIN (-15)
#define EQ_MAX 6
#define EQ_F 16                          /* MID FREQ: 157 Hz .. 6.3 kHz in sixths of an octave */
#define EQ_LOW_HZ 150.0f
#define EQ_HIGH_HZ 5000.0f
static track_t trk[NTRK];

/* --- the LFO: a slow wave in time with the tempo that moves one thing of its track. PITCH (vibrato) starts again at
 * each note of the track, from the middle and upwards; FILTER (wah), LEVEL (tremolo) and PAN keep to the bar while
 * the pattern plays, so they fall on the beat the same way every time round. No shape is random --- */
enum { LFO_PITCH, LFO_FILTER, LFO_LEVEL, LFO_PAN, NLFO_T };
enum { LFO_SINE, LFO_TRI, LFO_SQUARE, LFO_SAWUP, LFO_SAWDN, NLFO_S };
#define NLFO_R 11u
#define LFO_R_START 6u                   /* 1/8 */
static const char *const LFO_T_NAME[NLFO_T] = {"PITCH", "FILTER", "LEVEL", "PAN"};
static const char *const LFO_T_WORD[NLFO_T] = {"VIBRATO", "WAH", "TREMOLO", "LEFT-RIGHT"};
static const char *const LFO_S_NAME[NLFO_S] = {"SINE", "TRIANGLE", "SQUARE", "SAW UP", "SAW DOWN"};
static const char *const LFO_R_NAME[NLFO_R] = {"4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16",
                                               "1/16T", "1/32"};
/* a cycle's length in steps (sixteenths); T: three in the time of two */
static const float LFO_R_STEPS[NLFO_R] = {64.0f, 32.0f, 16.0f, 8.0f, 4.0f, 8.0f / 3.0f, 2.0f, 4.0f / 3.0f, 1.0f,
                                          2.0f / 3.0f, 0.5f};
static float lfo_ph[NTRK];               /* where in its cycle each track's is, 0 .. 1 (the audio interrupt) */
static uint8_t lfo_trig;                 /* tracks a note has just started on (PITCH starts again) */
static float lfo_wave(uint32_t shape, float p)   /* -1 .. 1 at p of a cycle (any p >= 0) */
{
    p -= (float)(uint32_t)p;
    switch (shape) {
    case LFO_TRI:
        return p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f;
    case LFO_SQUARE:
        return p < 0.5f ? 1.0f : -1.0f;
    case LFO_SAWUP:
        return 2.0f * p - 1.0f;
    case LFO_SAWDN:
        return 1.0f - 2.0f * p;
    default:
        return fm_sinf(6.2831853f * p);
    }
}
/* the pattern, for all the tracks: its steps (the time signature: SIG_*), how late the off-beat sixteenths come
 * (SWING 50 straight .. 75), and the tempo while the FM-1 leads (seq_sf, clock.c). As they start: 4/4, 50, 120 */
static volatile uint8_t seq_len = NSTEP;     /* steps in the pattern: 8, 12, 14 or 16 */
static volatile uint8_t seq_sig = 2;         /* SIG_NAME's */
static volatile uint8_t seq_swing = 50;
static volatile float trk_pk[NTRK];          /* each track's highest sample since the screen last looked (the city view: city.c) */
static volatile float mst_pk[2];              /* and the mix's, left and right */
static volatile uint8_t city_on;             /* the view is up: the audio interrupt keeps trk_pk and mst_pk */
static uint8_t pf_latch;                     /* GLO's PERFORM LATCH: a key turns its effect on, and again off */
static void perform_off(void);               /* (every effect of PERFORM's off: below, with the keys) */
static uint8_t bt_on = 1;                    /* GLO's BLUETOOTH: on at every start (nothing is saved yet) */
static uint32_t bt_again;                    /* GLO's DISCONNECT: when it was turned off, to be turned on again (0: not) */
#define NSIG 5u
static const char *const SIG_NAME[NSIG] = {"2/4", "3/4", "4/4", "6/8", "7/8"};
static const uint8_t SIG_LEN[NSIG] = {8, 12, 16, 12, 14};
/* where a beat starts (the screen's grouping, the step keys' accents): in quarter notes; 6/8 in two dotted quarters;
 * 7/8 as 2 + 2 + 3 eighths */
static const uint16_t SIG_BEATS[NSIG] = {0x0011, 0x0111, 0x1111, 0x0041, 0x0111};
#define NFIT 6u
static const char *const FIT_NAME[NFIT] = {"OFF", "1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS", "4 BARS"};
static const float FIT_BARS[NFIT] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static volatile uint8_t step_on[NTRK][NSTEP];
static volatile uint8_t step_note[NTRK][NSTEP];   /* MIDI note; a drum plays its own pitch */

/* the transport: the main loop asks, the audio interrupt runs it */
#define SEQ_BPM 120.0f                   /* fixed in this version */
static volatile uint8_t seq_play_req;    /* 1 playing */
static volatile uint8_t seq_step_now;    /* the step sounding (for the screen and the lights) */
static uint8_t seq_running;
static float seq_left;                   /* frames to the next step */
static uint32_t seq_next;                /* the step that starts then */

/* ------------------------------------------------------------------ voices --- */
#define NVOICE 12u
enum { V_OFF, V_HELD, V_REL };
typedef struct {
    const sound_t *s;
    float frac, inc, env, att, vel;      /* frac: how far between s0 and s1 */
    float s0, s1;                        /* the two samples it is between */
    uint32_t ni;                         /* the next sample to fetch */
    int32_t pred;                        /* ADPCM: the decoder */
    int32_t idx;
    uint8_t done;                        /* a one-shot: its last sample has been fetched */
    /* the EDIT page's part of it (ed 0: the sound as it is, none of this used) */
    uint8_t ed;                          /* ED_* */
    int8_t rb;                           /* backwards, ADPCM: its block's buffer (rev_buf), -1 none */
    uint8_t aidx;                        /* the decoder at ea (a loop of the page's goes back to it) */
    int16_t apred;
    uint32_t ea, eb;                     /* the part that plays, [ea, eb) */
    uint32_t rbase;                      /* backwards: the block in the buffer starts at this sample */
    uint8_t my;                          /* 1 + the app's slot it plays (0: a built-in sound) */
    float gain;                          /* a sample from the app: what brings it to the built-in sounds' loudness */
    uint32_t delay;                      /* frames into this block before it starts */
    uint32_t life;                       /* a preview: frames until it lets go (0: none) */
    /* a held key on a sound with no loop of its own (not a hit): it holds on a loop near the part's end (4.03) */
    uint32_t su_a, su_b, su_x;           /* the loop [su_a, su_b) and the samples its end crosses into its start (su_b 0: none) */
    uint32_t su_j;                       /* crossing: the next sample of the second reader (before su_a) */
    int32_t su_pred, su_idx;             /* and its decoder */
    uint8_t su_on;                       /* crossing now */
    uint32_t age;
    uint8_t state, track, note, seq;
} voice_t;
static voice_t voice[NVOICE];
static uint32_t voice_age;
static volatile uint32_t voices_on;      /* for the screen and the tests */

/* what each track's bus keeps between blocks */
static struct {
    float lp1, lp2, hp1, hp2, a, b, gain;
    float pan;                           /* the LFO's PAN as it glides: -1 left .. 0 .. 1 right */
    float eq[2][3][2];                   /* the EQ's three filters' state: the track, and a take's part of it (rb) */
} bus[NTRK];
static float gain_now;

/* the master's limiter (render_part) */
#define LIM_AHEAD 48u                    /* frames the mix is held back: 1.1 ms */
#define LIM_T 0.89f                      /* -1 dBFS: nothing above it goes out */
#define LIM_ATK 0.2f                     /* the gain comes down within the look-ahead (0.8^48: 0.00002 left) */
#define LIM_UP 0.0004f                   /* and back up over ~60 ms once the level has fallen */
#define LIM_REL 0.99991f                 /* the level's follower: ~0.25 s to fall */
static struct {
    float d[LIM_AHEAD], e[LIM_AHEAD];    /* left, right */
    uint32_t w;
    float env, g;
    float g_min, peak;                   /* since the last look (the tests): the lowest gain, the highest sample out */
    uint32_t clips;
} lim = {.g = 1.0f, .g_min = 1.0f};

/* notes from the main loop to the audio interrupt: track | note << 8 | velocity << 16 | kind << 24 */
enum { QE_ON = 1, QE_OFF, QE_PING };
#define EVQ 32u
static volatile uint32_t evq[EVQ];
static volatile uint32_t evq_w, evq_r;
static void ev_put(uint32_t kind, uint32_t t, uint32_t note, uint32_t vel)
{
    uint32_t w = evq_w;
    if (((w + 1u) % EVQ) == evq_r)
        return;                          /* full: the note is dropped (32 between two blocks never happens by hand) */
    evq[w] = (t & 0xFFu) | (note & 0x7Fu) << 8 | (vel & 0x7Fu) << 16 | kind << 24;
    evq_w = (w + 1u) % EVQ;
}

static float semis(float st) { return fm_exp2f(st * (1.0f / 12.0f)); }

#define INC_MAX 4.0f                     /* at most four samples of a sound a frame: two octaves above its own rate */

/* the voice's next sample (ni), decoded if it is ADPCM; a loop goes round, a one-shot ends on zeros */
/* --- the EDIT page: a sound started part way in, cut short, looped over that part, or played backwards.
 * ADPCM only decodes forwards, from a known state: a sound has one kept every SND_CK samples (sound_t.ck). Starting
 * part way decodes from the nearest one (511 samples at most, once, at the note's start). Backwards, the block of
 * SND_CK samples the place is in is decoded forwards into a buffer and read from its end; there are NREV buffers, so
 * NREV notes play backwards at a time (a fifth takes the oldest one's). Where the part ends away from the sound's own
 * end, and where a loop of the page's turns round, 32 samples fade: no click --- */
enum { ED_ON = 1, ED_REV = 2, ED_LOOP = 4, ED_BAKED = 8 };
#define NREV 4u
#define ED_FADE 32u
static int16_t rev_buf[NREV][SND_CK] __attribute__((section(".pool")));   /* 4 KB */
static int8_t rev_of[NREV] = {-1, -1, -1, -1};   /* the voice each buffer is with, -1 free */

static inline int32_t ima_next(const uint8_t *d, uint32_t i, int32_t *pred, int32_t *idx)
{
    uint32_t code = (d[i >> 1] >> ((i & 1u) << 2)) & 15u;
    int32_t step = IMA_STEP[*idx], q = step >> 3, p = *pred;
    if (code & 4u)
        q += step;
    if (code & 2u)
        q += step >> 1;
    if (code & 1u)
        q += step >> 2;
    p = code & 8u ? p - q : p + q;
    p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    *pred = p;
    q = *idx + IMA_INDEX[code & 7u];
    *idx = q < 0 ? 0 : q > 88 ? 88 : q;
    return p;
}
static void ck_state(const sound_t *s, uint32_t block, int32_t *pred, int32_t *idx)
{
    const uint8_t *c = s->ck + 3u * block;
    *pred = (int16_t)((uint32_t)c[0] | (uint32_t)c[1] << 8);
    *idx = c[2];
}

/* SUSTAIN LOOP (4.03). x is sample i of a part played forwards, once. With the key still down when the part comes to
 * its last su_x samples before su_b, a second reader starts su_x samples before su_a and the two cross, linearly; at
 * su_b the first goes on from su_a, where the second is: no step. Round and round while the key is down; let go, the
 * part plays out to its end as it always did (and fades at RELEASE's pace meanwhile) */
static void ck_state(const sound_t *s, uint32_t block, int32_t *pred, int32_t *idx);
static inline float voice_sus(voice_t *v, float x, uint32_t i)
{
    const sound_t *s = v->s;
    float y;
    if (i + v->su_x < v->su_b)
        return x;
    if (!v->su_on) {
        if (v->state != V_HELD || i >= v->su_b) {
            v->su_b = 0;                     /* let go before the loop's end: no more rounds */
            return x;
        }
        v->su_on = 1;
        v->su_j = v->su_a - v->su_x;
        if (!(s->flags & SF_PCM)) {          /* the second reader's decoder: from the kept state before it */
            uint32_t q = v->su_j / SND_CK * SND_CK;
            ck_state(s, v->su_j / SND_CK, &v->su_pred, &v->su_idx);
            for (; q < v->su_j; q++)
                ima_next((const uint8_t *)s->d, q, &v->su_pred, &v->su_idx);
        }
    }
    y = s->flags & SF_PCM ? (float)((const int16_t *)s->d)[v->su_j]
                          : (float)ima_next((const uint8_t *)s->d, v->su_j, &v->su_pred, &v->su_idx);
    v->su_j++;
    x += (y - x) * ((float)(i + v->su_x + 1u - v->su_b) / (float)v->su_x);
    if (i + 1u >= v->su_b) {                 /* round: the first reader is the second from here */
        v->ni = v->su_j;
        v->pred = v->su_pred;
        v->idx = v->su_idx;
        v->su_on = 0;
    }
    return x;
}

static float voice_fetch_ed(voice_t *v)
{
    const sound_t *s = v->s;
    uint32_t i = v->ni, lo = v->ea, hi = v->eb, edge;
    float x;
    if (v->ed & ED_BAKED) {                  /* the sound's own loop: it holds there (no fades: it is seamless) */
        lo = s->ls;
        hi = s->le;
    }
    if (v->ed & ED_REV) {
        if ((int32_t)i < (int32_t)lo) {      /* past the part's start */
            if (!(v->ed & ED_LOOP)) {
                v->done = 1;
                return 0.0f;
            }
            i = hi - 1u;
        }
        v->ni = i - 1u;                      /* (below 0: 0xFFFFFFFF, read as -1 above) */
        if (s->flags & SF_PCM) {
            x = (float)((const int16_t *)s->d)[i];
        } else {
            if (v->rb < 0)
                return 0.0f;
            if (i - v->rbase >= SND_CK) {    /* another block: decoded forwards, from its kept state */
                uint32_t b = i / SND_CK, k, n = s->n - b * SND_CK < SND_CK ? s->n - b * SND_CK : SND_CK;
                int32_t pred, idx;
                int16_t *o = rev_buf[v->rb];
                ck_state(s, b, &pred, &idx);
                for (k = 0; k < n; k++)
                    o[k] = (int16_t)ima_next((const uint8_t *)s->d, b * SND_CK + k, &pred, &idx);
                v->rbase = b * SND_CK;
            }
            x = (float)rev_buf[v->rb][i - v->rbase];
        }
    } else {
        if (v->ed & ED_BAKED) {
            if (i >= hi) {
                i = lo;
                v->pred = s->lpred;
                v->idx = s->lidx;
            }
        } else if (i >= hi) {
            if (!(v->ed & ED_LOOP)) {
                v->done = 1;
                return 0.0f;
            }
            i = lo;
            v->pred = v->apred;
            v->idx = v->aidx;
        }
        v->ni = i + 1u;
        x = s->flags & SF_PCM ? (float)((const int16_t *)s->d)[i] : (float)ima_next((const uint8_t *)s->d, i, &v->pred, &v->idx);
        if (v->su_b) {
            x = voice_sus(v, x, i);
            if (v->su_on || v->ni != i + 1u)
                return x;                    /* (crossing into the loop's start: this is no end, nothing fades) */
        }
    }
    if (v->ed & ED_BAKED)
        return x;
    /* the fades: where it will stop (unless that is the sound's own end, forwards), and both ends of a loop */
    edge = v->ed & ED_REV ? i - lo : hi - 1u - i;
    if (edge < ED_FADE && (v->ed & (ED_LOOP | ED_REV) || hi < s->n || (s->flags & SF_LOOP)))
        x *= (float)edge * (1.0f / (float)ED_FADE);
    if (v->ed & ED_LOOP) {
        edge = v->ed & ED_REV ? hi - 1u - i : i - lo;
        if (edge < ED_FADE)
            x *= (float)edge * (1.0f / (float)ED_FADE);
    }
    return x;
}

static float voice_fetch(voice_t *v)
{
    const sound_t *s = v->s;
    uint32_t i = v->ni;
    if (v->ed)
        return voice_fetch_ed(v);
    if (s->flags & SF_LOOP) {
        if (i >= s->le) {
            i = s->ls;
            v->pred = s->lpred;
            v->idx = s->lidx;
        }
    } else if (i >= s->n) {
        v->done = 1;
        return 0.0f;
    }
    v->ni = i + 1u;
    if (s->flags & SF_PCM)
        return v->su_b ? voice_sus(v, (float)((const int16_t *)s->d)[i], i) : (float)((const int16_t *)s->d)[i];
    {
        uint32_t code = (((const uint8_t *)s->d)[i >> 1] >> ((i & 1u) << 2)) & 15u;
        int32_t step = IMA_STEP[v->idx], d = step >> 3, p = v->pred;
        if (code & 4u)
            d += step;
        if (code & 2u)
            d += step >> 1;
        if (code & 1u)
            d += step >> 2;
        p = code & 8u ? p - d : p + d;
        p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
        v->pred = p;
        d = v->idx + IMA_INDEX[code & 7u];
        v->idx = d < 0 ? 0 : d > 88 ? 88 : d;
        return v->su_b ? voice_sus(v, (float)p, i) : (float)p;
    }
}

static float ck_tick_frames(void);           /* (clock.c: frames a clock tick, at the tempo now) */
static void voice_start(uint32_t t, uint32_t note, uint32_t vel, uint32_t delay, uint32_t seq, uint32_t life)
{
    uint32_t li = trk[t].sound, i, k = 0, oldest = 0xFFFFFFFFu;
    float g = 1.0f, st;
    const sound_t *s = li > NSOUNDS ? my_sound(li - NSOUNDS - 1u, note, &g) : list_sound(li);
    voice_t *v;
    if (!s)
        return;                          /* an empty track is silent */
    lfo_trig = (uint8_t)(lfo_trig | 1u << t);
    for (i = 0; i < NVOICE; i++) {       /* a free voice, or else the oldest */
        if (voice[i].state == V_OFF) {
            k = i;
            break;
        }
        if (voice[i].age < oldest) {
            oldest = voice[i].age;
            k = i;
        }
    }
    v = &voice[k];
    st = (float)trk[t].pitch;
    if (!(s->flags & SF_DRUM))           /* a key's pitch against the sound's own */
        st += 12.0f * fm_log2f(440.0f / s->hz) + (float)((int32_t)note - 69);
    v->s = s;
    v->inc = s->rate * (1.0f / 44100.0f) * semis(st);
    if (v->inc > INC_MAX)                /* (the decoder's work a frame is bounded: no higher than this) */
        v->inc = INC_MAX;
    v->ni = 0;
    v->pred = 0;
    v->idx = 0;
    v->done = 0;
    v->frac = 0.0f;
    for (i = 0; i < NREV; i++)               /* (this voice's last note may have had a buffer) */
        if (rev_of[i] == (int8_t)k)
            rev_of[i] = -1;
    v->ed = 0;
    v->rb = -1;
    if ((trk[t].ed_start || trk[t].ed_len < 100u || trk[t].ed_rev || trk[t].ed_loop != ED_LOOP_AUTO) &&
        ((s->flags & SF_PCM) || s->ck) && s->n > 4u * ED_FADE) {
        /* the EDIT page has changed something: the part that plays, and how */
        const uint32_t n = s->n, loop = trk[t].ed_loop == ED_LOOP_ON || (trk[t].ed_loop == ED_LOOP_AUTO && (s->flags & SF_LOOP));
        uint32_t a = (uint32_t)((uint64_t)n * trk[t].ed_start / 100u), b;
        if (a > n - 4u * ED_FADE)
            a = n - 4u * ED_FADE;
        b = trk[t].ed_len >= 100u ? n : a + (uint32_t)((uint64_t)(n - a) * trk[t].ed_len / 100u);
        if (b < a + 4u * ED_FADE)
            b = a + 4u * ED_FADE;
        v->ed = (uint8_t)(ED_ON | (loop ? ED_LOOP : 0u) | (trk[t].ed_rev ? ED_REV : 0u));
        if (loop && (s->flags & SF_LOOP)) {  /* its own loop is the loop; it starts at START if that is before its end */
            v->ed |= ED_BAKED;
            if (a >= s->le)
                a = s->ls;
        }
        v->ea = a;
        v->eb = b;
        if (v->ed & ED_REV) {
            v->ni = (v->ed & ED_BAKED ? s->le : b) - 1u;
            v->rbase = 0xFFFFFFFFu - SND_CK;     /* (no block in the buffer yet) */
            if (!(s->flags & SF_PCM)) {
                uint32_t q, best = 0, age = 0xFFFFFFFFu;
                for (q = 0; q < NREV && rev_of[q] >= 0 && voice[rev_of[q]].state != V_OFF; q++)
                    if (voice[rev_of[q]].age < age) {
                        age = voice[rev_of[q]].age;
                        best = q;
                    }
                if (q == NREV) {             /* none free: the oldest backwards note gives its up, and ends */
                    q = best;
                    voice[rev_of[q]].state = V_OFF;
                    voice[rev_of[q]].rb = -1;
                }
                rev_of[q] = (int8_t)k;
                v->rb = (int8_t)q;
            }
        } else {
            if (!(s->flags & SF_PCM)) {      /* to START: from the kept state before it */
                uint32_t q = a / SND_CK * SND_CK;
                ck_state(s, a / SND_CK, &v->pred, &v->idx);
                for (; q < a; q++)
                    ima_next((const uint8_t *)s->d, q, &v->pred, &v->idx);
            }
            v->ni = a;
            v->apred = (int16_t)v->pred;
            v->aidx = (uint8_t)v->idx;
        }
    }
    v->su_b = 0;
    v->su_on = 0;
    if (!seq && !(s->flags & (SF_DRUM | SF_LOOP)) && trk[t].ed_loop == ED_LOOP_AUTO && !(v->ed & ED_REV) &&
        !(trk[t].fit && trk[t].fit < NFIT) && ((s->flags & SF_PCM) || s->ck)) {   /* (FIT: its length is the point) */
        /* a key (or the ARP) on a sound that ends by itself, LOOP on AUTO: held, it holds on a short loop that starts
         * 65 % into the part that plays: 2048 samples (93 ms at 22 kHz; a quarter of the part at most), brought to a
         * whole number of the sound's pitch's periods, half of it crossed. Short, so the sound's own fading does not
         * pulse in it; what comes after the loop is the sound's tail, which plays when the key is let go */
        const uint32_t a = v->ed ? v->ea : 0u, b = v->ed ? v->eb : s->n, len = b - a;
        if (len >= 512u) {
            const float per = s->hz > 20.0f ? s->rate / s->hz : 0.0f;
            uint32_t w = 2048u, x, at = b - len * 35u / 100u;
            if (w > len / 4u)
                w = len / 4u;
            if (per >= 2.0f && per < (float)w)
                w = (uint32_t)((float)(uint32_t)((float)w / per + 0.5f) * per + 0.5f);
            if (at + w > b)
                w = b - at;
            x = w / 2u;
            if (x >= 16u) {
                v->su_b = at + w;
                v->su_a = at;
                v->su_x = x;
            }
        }
    }
    if (trk[t].fit && trk[t].fit < NFIT) {
        /* FIT: the part that plays lasts so many bars of the tempo as it is now: its speed is set for that (the pitch
         * goes with it, as a sampler's). Middle C plays it at that speed, the other keys and PITCH around it */
        const float len = v->ed && !(v->ed & ED_BAKED) ? (float)(v->eb - v->ea) : (float)s->n;
        v->inc = len / (FIT_BARS[trk[t].fit] * ck_tick_frames() * (float)seq_len * 6.0f) *
                 semis((float)trk[t].pitch + (s->flags & SF_DRUM ? 0.0f : (float)((int32_t)note - 60)));
        if (v->inc > INC_MAX)
            v->inc = INC_MAX;
    }
    v->s0 = voice_fetch(v);
    v->s1 = voice_fetch(v);
    v->env = 1.0f;
    v->att = 0.0f;
    {   /* VELOCITY: 0 every press as loud as the hardest; 100 the finger's weight alone (75: as before the ENV page) */
        float va = (float)trk[t].env_v / 100.0f;
        v->vel = (1.0f - va) + va * (float)vel * (1.0f / 127.0f);
    }
    v->gain = g;
    v->my = (uint8_t)(li > NSOUNDS ? li - NSOUNDS : 0u);
    v->delay = delay;
    v->life = life;
    v->age = ++voice_age;
    v->track = (uint8_t)t;
    v->note = (uint8_t)note;
    v->seq = (uint8_t)seq;
    v->state = V_HELD;
}

static void voice_release(uint32_t t, uint32_t note, uint32_t seq)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &voice[i];
        /* (a hit plays through, unless the track's RELEASE is set, under 100: then letting go fades it too) */
        if (v->state == V_HELD && v->track == t && v->seq == seq && (seq || v->note == note) &&
            (!(v->s->flags & SF_DRUM) || (trk[t].env_r >= 0 && trk[t].env_r < 100) || (v->ed & ED_LOOP)))
            v->state = V_REL;                /* (and a hit the EDIT page loops: it would never end) */
    }
}

/* the voices playing these slots of the app's end before the main loop goes on (my_samples.c: before an erase) */
static void my_voices_end(uint32_t mask)
{
#ifdef PLAT_HOST
    uint32_t k;                              /* (the simulator renders between the script's lines: ended here) */
    for (k = 0; k < NVOICE; k++)
        if (voice[k].my && (mask >> (voice[k].my - 1u) & 1u))
            voice[k].state = V_OFF;
#else
    uint32_t t0 = plat_ms();
    my_kill = (uint8_t)(my_kill | mask);
    while (my_kill && plat_ms() - t0 < 60u)  /* the audio interrupt takes it within a block (5.8 ms) */
        ;
#endif
}

#include "clock.c"                        /* the clock out and in */
static void midi_in(void)                    /* audio interrupt: MIDI in plays the selected track */
{
    uint32_t pkt;
    int port;
    extern volatile uint8_t ui_sel_isr;
    uint32_t stamp;
    while (ck_midi_in(&pkt, &stamp, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if (st >= 0xF0u) {                   /* the clock, Start, Continue, Stop, Song Position: from USB or Bluetooth */
            if (port == PORT_USB || port == PORT_BLE)
                ck_midi(st, d1, d2 & 0x7Fu, stamp, port);
        } else if ((st & 0xF0u) == 0x90u && d2)
            voice_start(ui_sel_isr, d1, d2, 0, 0, 0);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            voice_release(ui_sel_isr, d1, 0);
    }
}
volatile uint8_t ui_sel_isr;                 /* the selected track, as the audio interrupt sees it */
volatile uint8_t ui_step_isr;                /* the step page is up */


static void seq_fire(uint32_t step, uint32_t delay)
{
    uint32_t t;
    for (t = 0; t < NTRK; t++) {
        voice_release(t, 0, 1);              /* the step before lets go */
        if (step_on[t][step])
            voice_start(t, step_note[t][step], 100, delay, 1, 0);
    }
    seq_step_now = (uint8_t)step;
}

static int32_t seq_bar_off;                  /* where in this block the pattern's first step starts (-1: not in it) */
static void seq_start_hook(void);            /* (the looper: its loop starts with the pattern) */
static float seq_xpos;                       /* following: the place in the pattern, in clock ticks (0 .. 96) */
static uint8_t seq_ext;                      /* the last block followed a clock in */
static float ck_left;                        /* leading: frames to the next clock tick out */
static uint32_t seq_bars;                    /* the bars since the pattern started (the LFOs keep to four of them) */
static float seq_phase, seq_dphase;          /* the place in the pattern at the block's start (0 .. 1) and a frame's share */
static struct { float e; } seq_dbg;          /* (the simulator's checks: the furthest the place was from the clock's) */

static void seq_stop_now(void)
{
    uint32_t t;
    seq_running = 0;
    for (t = 0; t < NTRK; t++)
        voice_release(t, 0, 1);
}

/* following a clock in: the place the clock gives for when this block is heard, and the sequencer steered to it */
static void seq_block_ext(uint32_t n, uint32_t tblock)
{
    const float pf = cki.p * (1.0f / CK_T24_FRAME);              /* frames a tick */
    const float raw = ck_wrap((float)cki.free_ + (float)(int32_t)(tblock + CK_LEAD - cki.tp) / cki.p);
    float tgt, e, adv, b, lim;
    uint32_t placed = 0;
    if (!seq_ext) {                          /* it led till now: the place stays, the tempo is the clock's from here */
        seq_ext = 1;
        if (seq_running && !cki.locate) {
            seq_xpos = ck_wrap((float)seq_next * 6.0f - seq_left * 6.0f / seq_sf);
            cki.base = ck_wrap(raw - seq_xpos);
        }
    }
    if (seq_play_req && (!seq_running || cki.locate)) {
        if (!cki.locate)                     /* PLAY on the FM-1 while it follows: from step 1 now, at the clock's tempo */
            cki.base = raw;
        seq_running = 1;
        seq_xpos = ck_wrap(raw - cki.base);
        cki.locate = 0;
        placed = 1;
        seq_start_hook();
    } else if (!seq_play_req && seq_running) {
        seq_stop_now();
    }
    cki.locate = 0;
    seq_phase = seq_xpos * (1.0f / CK_BAR);
    seq_dphase = 0.0f;
    if (!seq_running)
        return;
    if (placed) {                            /* the step it lands on, if it has only just begun, sounds now */
        uint32_t st = (uint32_t)(seq_xpos * (1.0f / 6.0f));
        if (seq_xpos - (float)st * 6.0f < 1.5f) {
            if (!(st % seq_len))
                seq_bar_off = 0;
            seq_fire(st % seq_len, 0);
        }
    }
    tgt = ck_wrap(raw - cki.base);
    e = tgt - seq_xpos;
    if (e > CK_BAR * 0.5f)
        e -= CK_BAR;
    if (e < -CK_BAR * 0.5f)
        e += CK_BAR;
    if (e > seq_dbg.e || -e > seq_dbg.e)
        seq_dbg.e = e < 0.0f ? -e : e;
    adv = (float)n / pf;
    lim = adv * 0.02f;
    e *= 0.1f;
    adv += e > lim ? lim : e < -lim ? -lim : e;
    seq_dphase = adv * (1.0f / CK_BAR) / (float)n;
    if (seq_swing > 50u) {                   /* SWING: the off-beat sixteenths come late, by so much of a step */
        const float sh = 6.0f * ((float)seq_swing * 0.02f - 1.0f);
        uint32_t k = (uint32_t)(seq_xpos * (1.0f / 6.0f)), kk;
        for (kk = k; kk <= k + 1u; kk++) {
            b = 6.0f * (float)kk + (kk & 1u ? sh : 0.0f);
            if (b > seq_xpos && b <= seq_xpos + adv) {
                uint32_t off = (uint32_t)((b - seq_xpos) / adv * (float)n), st = kk % seq_len;
                if (off >= n)
                    off = n - 1u;
                if (!st)
                    seq_bar_off = (int32_t)off;
                seq_fire(st, off);
            }
        }
    } else
    for (b = 6.0f * (float)((uint32_t)(seq_xpos * (1.0f / 6.0f)) + 1u); b <= seq_xpos + adv; b += 6.0f) {
        uint32_t off = (uint32_t)((b - seq_xpos) / adv * (float)n), st = (uint32_t)(b * (1.0f / 6.0f) + 0.5f) % seq_len;
        if (off >= n)
            off = n - 1u;
        if (!st)
            seq_bar_off = (int32_t)off;
        seq_fire(st, off);
    }
    seq_xpos = ck_wrap(seq_xpos + adv);
}

static void seq_block(uint32_t n)
{
    const uint32_t tblock = ck_block_time();
    seq_bar_off = -1;
    ck_block_done(n);
    if (ck_follow && (int32_t)(ck_now() - cki.last) > (int32_t)(CK_LOST_MS * 24000u)) {
        ck_follow = 0;                       /* the clock stopped coming: the FM-1 leads again, at that tempo */
        cki.run = 0;
        cki.pend = 0;
        ck_ui |= 2u;
        ck_sent.fresh = 1;
    }
    if (cki.pend && (int32_t)(ck_now() - cki.pend_t) > (int32_t)(CK_LOST_MS * 24000u))
        cki.pend = 0;                        /* (a Start with no clock after it) */
    if (ck_follow) {
        seq_block_ext(n, tblock);
        return;
    }
    if (seq_ext) {                           /* it followed till now: on from this place, at the tempo it had */
        const float pf = cki.p * (1.0f / CK_T24_FRAME);
        uint32_t nx = (uint32_t)(seq_xpos * (1.0f / 6.0f)) + 1u;
        seq_ext = 0;
        seq_sf = 6.0f * pf;
        seq_next = nx % seq_len;
        seq_left = ((float)nx * 6.0f - seq_xpos) * pf;
        ck_left = seq_left;
        while (ck_left >= pf)
            ck_left -= pf;
    }
    {
    const float step_frames = seq_sf;
    if (seq_play_req && !seq_running) {      /* PLAY: from step 1, now */
        seq_running = 1;
        seq_next = 0;
        seq_left = 0.0f;
        seq_start_hook();
        ck_send(0xFAu, tblock + CK_LEAD - 36000u);   /* Start, 1.5 ms before the tick that is its first */
        ck_left = 0.0f;
    } else if (!seq_play_req && seq_running) {
        seq_stop_now();
        ck_send(0xFCu, tblock + CK_LEAD);
    }
    while (ck_left < (float)n) {             /* the clock out: 24 a quarter note, playing or not */
        ck_send(0xF8u, tblock + CK_LEAD + (uint32_t)(ck_left * CK_T24_FRAME));
        ck_left += step_frames * (1.0f / 6.0f);
    }
    ck_left -= (float)n;
    if (seq_next >= seq_len)                 /* (the pattern was made shorter under it) */
        seq_next = 0;
    if (seq_swing > 50u)                     /* (the place in the bar as the clock has it: straight) */
        seq_phase = seq_running ? ck_wrap((float)seq_next * 6.0f + (seq_next & 1u ? 6.0f * ((float)seq_swing * 0.02f - 1.0f) : 0.0f) -
                                          seq_left * 6.0f / step_frames) * (1.0f / CK_BAR) : 0.0f;
    else
    seq_phase = seq_running ? ck_wrap((float)seq_next * 6.0f - seq_left * 6.0f / step_frames) * (1.0f / CK_BAR) : 0.0f;
    seq_dphase = 1.0f / (step_frames * (float)seq_len);
    if (!seq_running)
        return;
    while (seq_left < (float)n) {
        if (!seq_next)
            seq_bar_off = (int32_t)seq_left;
        seq_fire(seq_next, (uint32_t)seq_left);
        /* SWING: an on-beat sixteenth is so much longer, the off-beat one after it so much shorter */
        if (seq_swing > 50u)
            seq_left += step_frames * (seq_next & 1u ? 2.0f - (float)seq_swing * 0.02f : (float)seq_swing * 0.02f);
        else
            seq_left += step_frames;
        seq_next = (seq_next + 1u) % seq_len;
    }
    seq_left -= (float)n;
    }
}

/* --- the ARP page: the keys held on a track played one after another, in time with the sequencer's clock (its own,
 * or the one it follows). It lives in the audio interrupt: the keys' events come to it instead of to the voices while
 * its track's RATE is not OFF. While the pattern plays its steps fall on the bar's grid; stopped, it keeps its own
 * time from the first key. Its notes are the keys' own (not the sequencer's), so a take of the looper records them --- */
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_ORDER, NARP_M };
#define NARP_R 8u
#define ARP_KEYS 16u
static const char *const ARP_M_NAME[NARP_M] = {"UP", "DOWN", "UP-DOWN", "ORDER"};
static const char *const ARP_R_NAME[NARP_R] = {"OFF", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const uint8_t ARP_R_TICKS[NARP_R] = {0, 24, 16, 12, 8, 6, 4, 3};   /* a step in clock ticks (24 a quarter note) */
static struct {
    uint8_t n, trk;                      /* the keys held, on this track */
    uint8_t note[ARP_KEYS], vel[ARP_KEYS];       /* in the order they were pressed */
    uint8_t on, cur;                     /* a note of its sounds: which */
    uint32_t i;                          /* the step it is at */
    float since;                         /* frames since its last step */
    float off;                           /* frames until the sounding note is let go (GATE) */
} arp;

static void arp_key(uint32_t tr, uint32_t note, uint32_t vel, uint32_t down)
{
    uint32_t k;
    if (down) {
        if (arp.n && arp.trk != tr)              /* (keys of another track: it starts over with them) */
            arp.n = 0;
        if (!arp.n) {
            arp.trk = (uint8_t)tr;
            arp.i = 0;
            arp.since = 1e9f;                    /* stopped: the first key sounds at once */
        }
        for (k = 0; k < arp.n && arp.note[k] != note; k++)
            ;
        if (k == arp.n && arp.n < ARP_KEYS) {
            arp.note[arp.n] = (uint8_t)note;
            arp.vel[arp.n++] = (uint8_t)vel;
        }
    } else {
        for (k = 0; k < arp.n && !(arp.trk == tr && arp.note[k] == note); k++)
            ;
        if (k == arp.n) {
            voice_release(tr, note, 0);          /* (a key that went down before the ARP was on) */
            return;
        }
        for (arp.n--; k < arp.n; k++) {
            arp.note[k] = arp.note[k + 1u];
            arp.vel[k] = arp.vel[k + 1u];
        }
    }
}

static void arp_block(uint32_t n)
{
    const track_t *t = &trk[arp.trk];
    const uint32_t r = t->arp_r < NARP_R ? t->arp_r : 0u;
    float per, left;
    if (arp.on) {                                /* GATE: the note's time is up (to the block: 5.8 ms) */
        arp.off -= (float)n;
        if (arp.off <= 0.0f || !arp.n || !r) {
            voice_release(arp.trk, arp.cur, 0);
            arp.on = 0;
        }
    }
    if (!arp.n)
        return;
    if (!r) {                                    /* RATE went to OFF under held keys: they are let go */
        arp.n = 0;
        return;
    }
    per = (float)ARP_R_TICKS[r] * ck_tick_frames();
    if (seq_running) {                           /* on the bar's grid: the next multiple of the step from here */
        float at = seq_phase * CK_BAR / (float)ARP_R_TICKS[r], dt = seq_dphase * CK_BAR / (float)ARP_R_TICKS[r], to;
        if (seq_swing > 50u && ARP_R_TICKS[r] == 6u) {           /* SWING, at 1/16: its off-beat steps late with the pattern's */
            const float sh = (float)seq_swing * 0.02f - 1.0f, m2 = 2.0f * (float)((uint32_t)at / 2u), u = at - m2;
            at = u < 1.0f + sh ? m2 + u / (1.0f + sh) : m2 + 1.0f + (u - 1.0f - sh) / (1.0f - sh);
            dt /= u < 1.0f + sh ? 1.0f + sh : 1.0f - sh;
        }
        to = (float)((uint32_t)at + 1u) - at;                    /* steps of the ARP to its next one */
        left = at - (float)(uint32_t)at < 0.0005f && arp.since > per * 0.5f ? 0.0f : dt > 0.0f ? to / dt : 1e9f;
    } else {
        if (seq_swing > 50u && ARP_R_TICKS[r] == 6u)             /* (stopped: long and short by turns) */
            per *= arp.i & 1u ? (float)seq_swing * 0.02f : 2.0f - (float)seq_swing * 0.02f;
        left = per - arp.since;
    }
    if (left < (float)n && arp.since + (left > 0.0f ? left : 0.0f) > per * 0.4f) {
        uint8_t seq[ARP_KEYS];
        const uint32_t oct = t->arp_o >= 1u && t->arp_o <= 3u ? t->arp_o : 1u, m = t->arp_m, len = arp.n * oct;
        uint32_t a, b, cyc, pos, note, off = left > 0.0f ? (uint32_t)left : 0u;
        for (a = 0; a < arp.n; a++)
            seq[a] = (uint8_t)a;
        if (m != ARP_ORDER)                      /* low to high */
            for (a = 1; a < arp.n; a++)
                for (b = a; b && arp.note[seq[b]] < arp.note[seq[b - 1u]]; b--) {
                    uint8_t x = seq[b];
                    seq[b] = seq[b - 1u];
                    seq[b - 1u] = x;
                }
        cyc = m == ARP_UPDOWN && len > 1u ? 2u * len - 2u : len;
        pos = arp.i % cyc;
        if (m == ARP_UPDOWN && pos >= len)
            pos = cyc - pos;
        if (m == ARP_DOWN)
            pos = len - 1u - pos;
        note = arp.note[seq[pos % arp.n]] + 12u * (pos / arp.n);
        if (arp.on)
            voice_release(arp.trk, arp.cur, 0);
        if (note > 127u)
            note = 127u;
        voice_start(arp.trk, note, arp.vel[seq[pos % arp.n]], off, 0, 0);
        arp.cur = (uint8_t)note;
        arp.on = 1;
        arp.off = (float)off + per * (float)(t->arp_g < 10u ? 10u : t->arp_g > 100u ? 100u : t->arp_g) * 0.01f;
        arp.i++;
        arp.since = (float)(n - off);
    } else {
        arp.since += (float)n;
    }
}

#define VOICE_AMP 1.0f                   /* a voice at full velocity, before its track's LEVEL */
static float tb[APP_BLOCK];                  /* one track's bus */
static float mixl[APP_BLOCK], mixr[APP_BLOCK];
#include "fx.c"                          /* the effects: two slots a track */
_Static_assert(NTRK == 8u, "fx.c's per-track state is for eight tracks");
#include "looper.c"                      /* the looper on track 8 */
#include "perform.c"                     /* PERFORM: SEL and a white key, an effect on everything */
static float rb[APP_BLOCK];                  /* a track's voices that a take records (played by hand) */
static float recf[NTRK][4];                  /* and the track's FILTER on them alone */
static fx_slot_t recx[NTRK][2];              /* and its OVERDRIVE, LO-FI (4.05: the take hears the effects) */
static void seq_start_hook(void) { lp.bar = (uint8_t)(lp.bars - 1u); lp.resync = 1; }
static uint8_t *lvl_of(uint32_t t) { return t == lp.trk && lp.own ? &lp.lvl : &trk[t].level; }   /* LEVEL: the loop has its own */

/* what a track's envelope is this block (render_part works it out from the track's knobs) */
typedef struct {
    float ainc;                          /* ATTACK: the fade in's step a frame */
    float dcoef;                         /* a hit's DECAY (1: none) */
    float kd, sus;                       /* a held sound: to SUSTAIN at DECAY's pace (sus 1: stays as it is) */
    float rcoef;                         /* RELEASE */
    float rhit;                          /* and a hit's that the EDIT page loops: RELEASE, or at AUTO about 0.1 s */
} env_t;

static void voice_render(voice_t *v, float *out, uint32_t n, const env_t *e, float rate)
{
    uint32_t i = v->delay;
    float frac = v->frac, inc = v->inc * rate, env = v->env, att = v->att;
    /* (TAPE SPEED: the track's play rate; near a standstill the sound fades, as a stopped tape gives none) */
    float amp = v->vel * v->gain * (VOICE_AMP / 32768.0f) * (rate * 12.0f < 1.0f ? rate * 12.0f : 1.0f);
    float s0 = v->s0, s1 = v->s1, ainc = e->ainc, dcoef = e->dcoef, rcoef = e->rcoef, kd = e->kd, sus = e->sus;
    uint32_t settle = !(v->s->flags & SF_DRUM) && sus < 1.0f;   /* a held sound with SUSTAIN under 100 */
    if (inc > INC_MAX)
        inc = INC_MAX;
    v->delay = 0;
    for (; i < n; i++) {
        if (att < 1.0f)
            att += ainc;                     /* ATTACK (at 0: ~1 ms, no click on a sound that starts away from zero) */
        if (v->state == V_REL)
            env *= (v->ed & ED_LOOP) && (v->s->flags & SF_DRUM) ? e->rhit : rcoef;
        else if (settle)
            env += (sus - env) * kd;
        else
            env *= dcoef;
        out[i] += (s0 + (s1 - s0) * frac) * amp * env * (att < 1.0f ? att : 1.0f);
        frac += inc;
        while (frac >= 1.0f) {               /* on to the next samples (INC_MAX of them at most) */
            frac -= 1.0f;
            s0 = s1;
            s1 = voice_fetch(v);
        }
        if (v->life && !--v->life && v->state == V_HELD)
            v->state = V_REL;
        if (env < 0.0005f || (v->done && s0 == 0.0f && s1 == 0.0f)) {
            v->state = V_OFF;
            break;
        }
    }
    v->frac = frac;
    v->s0 = s0;
    v->s1 = s1;
    v->env = env;
    v->att = att;
}

/* the EQ (FX page 2): band 0 LOW, 1 MID, 2 HIGH, at db dB; the RBJ cookbook's shelves (S 1) and bell (Q 1) at 44.1
 * kHz (Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients"). A band at 0 dB is not
 * run: the track is then bit for bit as without it. (Small in the flash: whole steps of dB and of sixths of an octave
 * are powers by multiplying, and sin and cos of an angle under 1 are their series) */
static float eq_pow(float r, int32_t n)      /* r to the n */
{
    float v = 1.0f;
    if (n < 0) {
        r = 1.0f / r;
        n = -n;
    }
    while (n--)
        v *= r;
    return v;
}
static float eq_hz(int32_t n) { return 1000.0f * eq_pow(1.1224620f, n); }   /* MID FREQ: 1 kHz x 2^(n/6) */
static void eq_band(float *x, uint32_t n, float *z, uint32_t band, int32_t db, float hz)
{
    const float sA = eq_pow(1.0292005f, db), A = sA * sA;   /* (10^(1/80): A is 10^(db/40)) */
    const float w = hz * (FM_TWO_PI / 44100.0f), w2 = w * w;   /* (under 0.91: 6.3 kHz at most) */
    const float sn = w * (1.0f - w2 * (1.0f / 6.0f) * (1.0f - w2 * (1.0f / 20.0f) * (1.0f - w2 * (1.0f / 42.0f))));   /* (1e-6) */
    const float cs = 1.0f - w2 * 0.5f * (1.0f - w2 * (1.0f / 12.0f) * (1.0f - w2 * (1.0f / 30.0f) * (1.0f - w2 * (1.0f / 56.0f))));
    float b0, b1, b2, a0, a1, a2, z1 = z[0], z2 = z[1];
    uint32_t i;
    if (band == 1u) {                        /* a bell: Q 1 */
        const float al = sn * 0.5f;
        b0 = 1.0f + al * A;
        b2 = 1.0f - al * A;
        a0 = 1.0f + al / A;
        a2 = 1.0f - al / A;
        b1 = a1 = -2.0f * cs;
    } else {                                 /* a shelf: the high one is the low one with cos and the odd terms turned */
        const float c = band ? -cs : cs, sa = 1.4142136f * sA * sn, s = band ? -1.0f : 1.0f;
        b0 = A * ((A + 1.0f) - (A - 1.0f) * c + sa);
        b1 = s * 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * c - sa);
        a0 = (A + 1.0f) + (A - 1.0f) * c + sa;
        a1 = s * -2.0f * ((A - 1.0f) + (A + 1.0f) * c);
        a2 = (A + 1.0f) + (A - 1.0f) * c - sa;
    }
    a0 = 1.0f / a0;
    b0 *= a0, b1 *= a0, b2 *= a0, a1 *= a0, a2 *= a0;
    for (i = 0; i < n; i++) {                /* (transposed direct form II) */
        const float v = x[i], y = b0 * v + z1;
        z1 = b1 * v - a1 * y + z2;
        z2 = b2 * v - a2 * y;
        x[i] = y;
    }
    z[0] = fm_flush(z1);
    z[1] = fm_flush(z2);
}

#ifdef PLAT_HOST
static uint32_t host_sounded;                /* (the simulator's checks: the tracks whose own voices sounded, a bit each) */
static uint32_t host_eq_runs;                /* (and the EQ's bands run, a block each) */
#endif
static void render_part(int32_t *lr, uint32_t n, float gain)
{
    uint32_t t, i, k, on = 0;
    uint32_t r = evq_r, take = lp.state != LP_IDLE || lp.req == LQ_ARM || lp.cap;   /* a take is armed or runs (or its tail, 4.14) */
    if (my_kill) {                           /* a slot of the app's is about to be erased: its voices end here */
        for (k = 0; k < NVOICE; k++)
            if (voice[k].my && (my_kill >> (voice[k].my - 1u) & 1u))
                voice[k].state = V_OFF;
        my_kill = 0;
    }
    while (r != evq_w) {                     /* the keys' notes since the last block */
        uint32_t e = evq[r], kind = e >> 24, tr = e & 0xFFu, note = (e >> 8) & 0x7Fu, vel = (e >> 16) & 0x7Fu;
        if (kind == QE_ON && trk[tr].arp_r)
            arp_key(tr, note, vel, 1);           /* the ARP page: the key joins the ones it plays */
        else if (kind == QE_ON)
            voice_start(tr, note, vel, 0, 0, 0);
        else if (kind == QE_PING)
            voice_start(tr, note, vel, 0, 0, 8820);   /* a step's note heard for 0.2 s */
        else if (arp.n)
            arp_key(tr, note, 0, 0);
        else
            voice_release(tr, note, 0);
        r = (r + 1u) % EVQ;
    }
    evq_r = r;
    midi_in();
    seq_block(n);
    {   /* (for the LFOs: the bars since the pattern started, counted where its place goes round) */
        static float last;
        if (!seq_running)
            seq_bars = 0;
        else if (seq_phase < last - 0.5f)
            seq_bars++;
        last = seq_running ? seq_phase : 0.0f;
    }
    perform_begin();
    if (lp.swap) {                           /* a take came in last block: the loop lives on the other of tracks 7 and 8
                                              * from this block on (4.04). Its bus goes with it as it stands (the filters'
                                              * and the level's state): no step, no gap; the track it left starts clean */
        const uint32_t fr = lp.trk, to = fr ^ 1u;    /* (7 and 6: tracks 8 and 7) */
        lp.swap = 0;
        bus[to] = bus[fr];
        bus[fr].lp1 = bus[fr].lp2 = bus[fr].hp1 = bus[fr].hp2 = 0.0f;
        lp.trk = (uint8_t)to;
    }
    if (arp.n || arp.on)
        arp_block(n);
    for (i = 0; i < n; i++)
        mixl[i] = mixr[i] = rec_in[i] = 0.0f;
    fx_rec = (uint8_t)take;
    for (t = 0; t < NTRK; t++) {
        const sound_t *s = list_sound(trk[t].sound);
        int32_t f = trk[t].filt;
        float kd = (float)trk[t].decay, lv = (float)*lvl_of(t) * 0.01f;    /* (the loop's track: the loop's own) */
        float dcoef = 1.0f, rcoef, a_t = 1.0f, b_t = 0.0f, g_t = lv * lv, a = bus[t].a, b = bus[t].b, g = bus[t].gain;
        float lp1 = bus[t].lp1, lp2 = bus[t].lp2, hp1 = bus[t].hp1, hp2 = bus[t].hp2;
        float fa[2], rate_t = 1.0f, dry = 1.0f, dfx;
        float lfo_p0 = 0.0f, lfo_dp = 0.0f, lfo_d = 0.0f, lfo_v = 0.0f, pan_t = 0.0f;   /* the LFO this block */
        uint32_t lfo_on = trk[t].lfo_a != 0u, lfo_to = trk[t].lfo_t, lfo_sh = trk[t].lfo_s;
        env_t ev;
        uint32_t any = 0, anyr = 0, ft[2], fs[2], q, loop8 = t == lp.trk && lp.own;
        for (q = 0; q < 2u; q++) {               /* the two slots: what, which side, how much */
            ft[q] = trk[t].fx[q] < NFX ? trk[t].fx[q] : FX_OFF;
            fa[q] = fx_amount(trk[t].fxk[q], &fs[q]);
            if (fa[q] <= 0.0f)
                ft[q] = FX_OFF;
            if (ft[q] == FX_TAPE)
                rate_t *= fx_tape_target(fs[q], fa[q]);
        }
        tape_rate[t] += (rate_t - tape_rate[t]) * 0.04f;     /* a tape's weight: ~0.15 s to get there */
        if (rate_t == 0.0f && tape_rate[t] < 0.002f)
            tape_rate[t] = 0.0f;
        /* DECAY: a hit fades over 20 ms .. 1.9 s (100: not at all); a held sound lets go over 10 ms .. 1 s */
        if (s && (s->flags & SF_DRUM) && kd < 100.0f)
            dcoef = fm_expf(-1.0f / (0.020f * fm_exp2f(kd * 0.066f) * 44100.0f));
        rcoef = fm_expf(-1.0f / (0.010f * fm_exp2f(kd * 0.066f) * 44100.0f));
        {   /* the ENV page's four, into what the voices use */
            int32_t ea = trk[t].env_a, er = trk[t].env_r;
            ev.ainc = ea ? (1.0f / 48.0f) * fm_exp2f(-0.11f * (float)ea) : 1.0f / 48.0f;
            ev.dcoef = dcoef;
            ev.sus = (float)trk[t].env_s / 100.0f;
            ev.kd = 1.0f - fm_expf(-1.0f / (0.020f * fm_exp2f(kd * 0.066f) * 44100.0f));
            ev.rcoef = er < 0 ? rcoef : fm_expf(-1.0f / (0.004f * fm_exp2f((float)er * 0.08f) * 44100.0f));
            ev.rhit = er < 0 ? 0.99849f : ev.rcoef;
        }
        if (lfo_on) {                            /* the LFO: where it is in this block, and what it moves */
            const uint32_t r = trk[t].lfo_r < NLFO_R ? trk[t].lfo_r : LFO_R_START;
            /* (4, 2, 1 BARS: of the pattern as the time signature has it; the note values are what they are) */
            const float lsteps = r < 3u ? (float)(4u >> r) * (float)seq_len : LFO_R_STEPS[r];
            lfo_dp = 1.0f / (lsteps * ck_tick_frames() * 6.0f);
            if (lfo_to == LFO_PITCH) {
                if (lfo_trig >> t & 1u)
                    lfo_ph[t] = 0.0f;            /* a note: the vibrato from its start */
            } else if (seq_running) {            /* the pattern plays: on the bar */
                const float at = ((float)(seq_bars & 3u) + seq_phase) * (float)seq_len / lsteps;
                lfo_ph[t] = at - (float)(uint32_t)at;
            }
            lfo_p0 = lfo_ph[t];
            lfo_ph[t] = lfo_p0 + lfo_dp * (float)n;
            lfo_ph[t] -= (float)(uint32_t)lfo_ph[t];
            lfo_d = (float)trk[t].lfo_a * 0.01f;
            lfo_v = lfo_wave(lfo_sh, lfo_p0 + lfo_dp * (float)n * 0.5f);
            if (lfo_to == LFO_LEVEL)             /* tremolo: down from the track's level, to silence at 100 */
                g_t *= 1.0f - lfo_d * (1.0f - lfo_v) * 0.5f;
            else if (lfo_to == LFO_PAN)
                pan_t = lfo_v * lfo_d;
        }
        lfo_trig = (uint8_t)(lfo_trig & ~(1u << t));
        /* FILTER: left of the middle a low-pass closing from 16 kHz to 125 Hz, right a high-pass opening 30 Hz .. 5.4 kHz */
        if (lfo_on && lfo_to == LFO_FILTER) {
            /* wah: the LFO moves FILTER's place, up to 36 of its detents (5 octaves of the low-pass) each way, on the
             * side the knob is on; with the knob at OFF it is a low-pass that closes from open and comes back */
            const float w = lfo_d * 36.0f;
            float ff = (float)f;
            if (f < -FILT_DEAD) {
                ff = fm_clampf(ff + lfo_v * w, -(float)FILT_MAX, -(float)FILT_DEAD);
            } else if (f > FILT_DEAD) {
                ff = fm_clampf(ff + lfo_v * w, (float)FILT_DEAD, (float)FILT_MAX);
            } else {
                ff = -(float)FILT_DEAD - w * (1.0f - lfo_v) * 0.5f;
            }
            if (ff < 0.0f)
                a_t = 1.0f - fm_expf(-6.2831853f * 16000.0f * fm_exp2f(-(-ff - (float)FILT_DEAD) * 0.14f) * (1.0f / 44100.0f));
            else
                b_t = 1.0f - fm_expf(-6.2831853f * 30.0f * fm_exp2f((ff - (float)FILT_DEAD) * 0.15f) * (1.0f / 44100.0f));
        } else {
        if (f < -FILT_DEAD)
            a_t = 1.0f - fm_expf(-6.2831853f * 16000.0f * fm_exp2f(-(float)(-f - FILT_DEAD) * 0.14f) * (1.0f / 44100.0f));
        if (f > FILT_DEAD)
            b_t = 1.0f - fm_expf(-6.2831853f * 30.0f * fm_exp2f((float)(f - FILT_DEAD) * 0.15f) * (1.0f / 44100.0f));
        }
        for (i = 0; i < n; i++)
            tb[i] = 0.0f;
        if (loop8) {                             /* the looper's track (8 or 7): the loop is its sound (no voices) */
            {   /* the loop holds to the bar by its own count while the FM-1 leads at 120 and the loop is a pattern of
                 * that long; else (a clock in, or a tempo taken from one) it is steered to the bar: looper.c */
                const uint32_t pat = pat_frames();
                const int steer = ck_follow || seq_sf != SEQ_SF_120 || (lp.total && lp.total != pat * lp.bars);
                any = loop_block(tb, n, seq_bar_off, seq_running, pat,
                                 seq_dphase > 0.0f ? (1.0f - seq_phase) / seq_dphase : (float)pat, steer ? seq_phase : -1.0f, seq_dphase);
                for (k = 0; k < NVOICE; k++)         /* (the track's own sound sleeps under the loop: a note that
                                                      * sounded as the loop came fades in ~8 ms, a new one never starts) */
                    if (voice[k].state != V_OFF && voice[k].track == t) {
                        env_t fe = ev;
                        /* (a take's tail hears it fade, as it is heard; over ~30 ms, not 1: the seam's click, 4.14) */
                        const uint32_t mine = take && (!voice[k].seq || (ui_step_isr && t == ui_sel_isr));
                        voice[k].state = voice[k].att > 0.0f ? V_REL : V_OFF;
                        fe.rcoef = fe.rhit = 0.9992f;
                        if (voice[k].state) {
                            if (mine && !anyr) {
                                for (i = 0; i < n; i++)
                                    rb[i] = 0.0f;
                                anyr = 1;
                            }
                            voice_render(&voice[k], mine ? rb : tb, n, &fe, 1.0f);
                            any = 1;
                        }
                    }
            }
            g_t *= 1.0f / 0.64f;                 /* (LEVEL at its 80: the loop as it was played) */
            if (pf_rate * 12.0f < 1.0f)          /* (PERFORM's TAPE STOP: the loop is no tape, it fades as it stops) */
                g_t *= pf_rate * 12.0f;
        } else {
            for (k = 0; k < NVOICE; k++)
                if (voice[k].state != V_OFF && voice[k].track == t) {
                    /* what a take records: the keys' own notes; on the step page the steps of the track on it */
                    uint32_t mine = take && (!voice[k].seq || (ui_step_isr && t == ui_sel_isr));
                    if (mine && !anyr) {
                        for (i = 0; i < n; i++)
                            rb[i] = 0.0f;
                        anyr = 1;
                    }
                    if (lfo_on && lfo_to == LFO_PITCH) {
                        /* vibrato: up to 2 semitones each way (the square of AMOUNT: fine at the low end), the pitch
                         * set anew every 64 frames (1.5 ms) so that it bends and does not step */
                        uint32_t c0;
                        for (c0 = 0; c0 < n && voice[k].state != V_OFF; c0 += 64u) {
                            const uint32_t m = n - c0 < 64u ? n - c0 : 64u;
                            const float lv = lfo_wave(lfo_sh, lfo_p0 + lfo_dp * ((float)c0 + (float)m * 0.5f));
                            if (voice[k].delay >= c0 + m)
                                continue;        /* (it starts later in the block) */
                            voice[k].delay = voice[k].delay > c0 ? voice[k].delay - c0 : 0u;
                            voice_render(&voice[k], (mine ? rb : tb) + c0, m, &ev,
                                         tape_rate[t] * pf_rate * fm_exp2f(lv * lfo_d * lfo_d * (2.0f / 12.0f)));
                        }
                    } else {
                        voice_render(&voice[k], mine ? rb : tb, n, &ev, tape_rate[t] * pf_rate);
                    }
                    any = 1;
                    on++;
#ifdef PLAT_HOST
                    host_sounded |= 1u << t;
#endif
                }
        }
        if (!anyr) {
            recf[t][0] = recf[t][1] = recf[t][2] = recf[t][3] = 0.0f;
            memset(recx[t], 0, sizeof recx[t]);
            memset(bus[t].eq[1], 0, sizeof bus[t].eq[1]);
        }
        if (!any && fm_fabsf(lp2) < 1e-6f && fm_fabsf(hp2) < 1e-6f) {
            bus[t].a = a_t;                  /* silent: nothing to filter, the knobs' values taken at once */
            bus[t].b = b_t;
            bus[t].gain = g_t;
            bus[t].pan = pan_t;
            bus[t].lp1 = bus[t].lp2 = bus[t].hp1 = bus[t].hp2 = 0.0f;
            memset(bus[t].eq, 0, sizeof bus[t].eq);
            continue;
        }
        for (i = 0; i < n; i++) {
            float x = tb[i];
            a += (a_t - a) * 0.004f;         /* the knobs glide: no step at a block's edge */
            b += (b_t - b) * 0.004f;
            lp1 += a * (x - lp1);
            lp2 += a * (lp1 - lp2);
            x = lp2;
            hp1 += b * (x - hp1);
            x -= hp1;
            hp2 += b * (x - hp2);
            tb[i] = x - hp2;
            if (anyr) {                          /* the same FILTER on the take's part alone; then it joins the track */
                float y = rb[i];
                recf[t][0] += a * (y - recf[t][0]);
                recf[t][1] += a * (recf[t][0] - recf[t][1]);
                y = recf[t][1];
                recf[t][2] += b * (y - recf[t][2]);
                y -= recf[t][2];
                recf[t][3] += b * (y - recf[t][3]);
                rb[i] = y - recf[t][3];
            }
        }
        if (anyr)                                /* (the two filters are the one filter on the sum: it is linear) */
            for (i = 0; i < n; i++)
                tb[i] += rb[i];
        for (q = 0; q < 3u; q++) {               /* the EQ: LOW, MID, HIGH (a band at 0 dB does nothing, not even run) */
            const int32_t db = trk[t].eq[q + (q >> 1)];
            const float hz = q == 0u ? EQ_LOW_HZ : q == 2u ? EQ_HIGH_HZ : eq_hz(trk[t].eq[2]);
            if (!db) {
                bus[t].eq[0][q][0] = bus[t].eq[0][q][1] = bus[t].eq[1][q][0] = bus[t].eq[1][q][1] = 0.0f;
                continue;
            }
#ifdef PLAT_HOST
            host_eq_runs++;
#endif
            eq_band(tb, n, bus[t].eq[0][q], q, db, hz);
            if (anyr)                            /* (the take's part alone too: what it hears) */
                eq_band(rb, n, bus[t].eq[1][q], q, db, hz);
        }
        for (q = 0; q < 2u; q++)                 /* OVERDRIVE and LO-FI: on the track itself, slot A then B */
            if (ft[q] == FX_DRIVE || ft[q] == FX_LOFI) {
                fx_inline(ft[q], fs[q], fa[q], &fx_slot[t][q], tb, n);
                if (anyr)                        /* (and on the take's part alone: they are not linear) */
                    fx_inline(ft[q], fs[q], fa[q], &recx[t][q], rb, n);
            }
        for (i = 0; i < n; i++) {                /* LEVEL */
            g += (g_t - g) * 0.004f;
            tb[i] *= g;
            if (anyr)
                rb[i] *= g;
        }
        if (city_on) {                           /* the city view is up: this track's level for its sign */
            float pk = 0.0f;
            for (i = 0; i < n; i++)
                pk = fm_fabsf(tb[i]) > pk ? fm_fabsf(tb[i]) : pk;
            if (pk > trk_pk[t])
                trk_pk[t] = pk;
        }
        for (q = 0; q < 2u; q++)                 /* the shared effects: this track's part into their buses */
            if (FX_BUS[ft[q]] >= 0)
                dry *= fx_send(ft[q], fs[q], fa[q], tb, n, anyr ? rb : 0);
        dfx = dry;                               /* (the take hears the track's effects, not PERFORM's) */
        if (pf_run)                              /* PERFORM's sends: every track's, as much as its keys say */
            dry *= perform_sends(tb, n);
        if (pan_t != 0.0f || bus[t].pan != 0.0f) {   /* the LFO's PAN: the side it leaves is turned down, to nothing at 100 */
            float pn = bus[t].pan;
            for (i = 0; i < n; i++) {
                float x = tb[i] * dry;
                pn += (pan_t - pn) * 0.004f;
                mixl[i] += pn > 0.0f ? x * (1.0f - pn) : x;
                mixr[i] += pn < 0.0f ? x * (1.0f + pn) : x;
                if (anyr)                        /* (the take: left and right as one) */
                    rec_in[i] += rb[i] * dfx * (pn > 0.0f ? 1.0f - 0.5f * pn : 1.0f + 0.5f * pn);
            }
            bus[t].pan = pan_t == 0.0f && pn < 0.0005f && pn > -0.0005f ? 0.0f : pn;
        } else {
        for (i = 0; i < n; i++) {
            float x = tb[i] * dry;
            mixl[i] += x;
            mixr[i] += x;
        }
        for (i = 0; anyr && i < n; i++)
            rec_in[i] += rb[i] * dfx;
        }
        bus[t].a = a;
        bus[t].b = b;
        bus[t].gain = g;
        bus[t].lp1 = fm_flush(lp1);
        bus[t].lp2 = fm_flush(lp2);
        bus[t].hp1 = fm_flush(hp1);
        bus[t].hp2 = fm_flush(hp2);
    }
    {   /* the echoes' tempo: the clock's, taken when it has moved by 0.3 % (not at every tick's wobble) */
        static float fx_sf = SEQ_SF_120;
        const float sf = ck_tick_frames() * 6.0f, d = sf - fx_sf;
        if (d > fx_sf * 0.003f || -d > fx_sf * 0.003f)
            fx_sf = sf;
        fx_buses(mixl, mixr, n, fx_sf, take ? rec_in : 0, tb);
    }
    /* what a take hears is whole only now (the effects are shared): the looper takes this block's in the next (4.05) */
    memcpy(rec_pv, rec_in, sizeof rec_pv);
    if (pf_run) {                                /* PERFORM: its effects on the whole mix */
        int32_t boff = -1;
        if (seq_running && seq_dphase > 0.0f) {  /* where in this block a step starts (the straight grid) */
            const float at = seq_phase * (float)seq_len, to = ((float)((uint32_t)at + 1u) - at) / (seq_dphase * (float)seq_len);
            boff = at - (float)(uint32_t)at < 0.0005f ? 0 : to < (float)n ? (int32_t)to : -1;
        }
        perform_mix(mixl, mixr, n, boff, ck_tick_frames() * 6.0f);
    }
    if (city_on) {                               /* the city view is up: the mix's level, left and right, for the kerbs */
        float pl = 0.0f, pr = 0.0f;
        for (i = 0; i < n; i++) {
            pl = fm_fabsf(mixl[i]) > pl ? fm_fabsf(mixl[i]) : pl;
            pr = fm_fabsf(mixr[i]) > pr ? fm_fabsf(mixr[i]) : pr;
        }
        if (pl > mst_pk[0])
            mst_pk[0] = pl;
        if (pr > mst_pk[1])
            mst_pk[1] = pr;
    }
    voices_on = on;
    /* the master's limiter: the mix is held back LIM_AHEAD frames, so the gain is already down when a peak comes out.
     * The level's follower rises at once and falls over ~0.25 s; under LIM_T nothing is touched (gain 1) */
    for (i = 0; i < n; i++) {                    /* (left and right share the one gain: the picture stays put) */
        float xl = mixl[i], xr = mixr[i], a = fm_fabsf(xl) > fm_fabsf(xr) ? fm_fabsf(xl) : fm_fabsf(xr), want, sl, sr;
        lim.env = a > lim.env ? a : lim.env * LIM_REL;
        want = lim.env > LIM_T ? LIM_T / lim.env : 1.0f;
        lim.g += (want - lim.g) * (want < lim.g ? LIM_ATK : LIM_UP);
        sl = lim.d[lim.w];
        sr = lim.e[lim.w];
        lim.d[lim.w] = xl;
        lim.e[lim.w] = xr;
        lim.w = (lim.w + 1u) % LIM_AHEAD;
        sl *= lim.g;
        sr *= lim.g;
        if (lim.g < lim.g_min)
            lim.g_min = lim.g;
        if (sl > 1.0f || sl < -1.0f || sr > 1.0f || sr < -1.0f) {   /* (never, with the look-ahead: counted if it ever is) */
            lim.clips++;
            sl = fm_clampf(sl, -1.0f, 1.0f);
            sr = fm_clampf(sr, -1.0f, 1.0f);
        }
        a = fm_fabsf(sl) > fm_fabsf(sr) ? fm_fabsf(sl) : fm_fabsf(sr);
        if (a > lim.peak)
            lim.peak = a;
        gain_now += (gain - gain_now) * 0.004f;      /* ~5 ms: turning MASTER does not click */
        lr[2 * i] = (int32_t)(sl * gain_now);
        lr[2 * i + 1] = (int32_t)(sr * gain_now);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;
    while (n) {                              /* (a block is APP_BLOCK frames; longer ones in parts) */
        uint32_t m = n > APP_BLOCK ? APP_BLOCK : n;
        render_part(lr, m, gain);
        lr += 2u * m;
        n -= m;
    }
}

void app_panic(void)
{
    uint32_t i;
    seq_play_req = 0;
    for (i = 0; i < NVOICE; i++)
        voice[i].state = V_OFF;
}

/* ------------------------------------------------------------------- state --- */
static void defaults(void)
{
    /* tracks 1..6 start with a built-in sound, 7 and 8 empty; the names are looked up, so the list may change */
    static const char *const START[NTRK] = {"KICK ROCK", "SNARE ROCK", "HAT ROCK", "SUB BASS", "PIANO", "SAW LEAD", 0, 0};
    uint32_t t, i, k;
    for (t = 0; t < NTRK; t++) {
        trk[t].sound = 0;
        for (i = 0; START[t] && i < NSOUNDS; i++) {
            for (k = 0; START[t][k] && START[t][k] == SOUNDS[i].name[k]; k++)
                ;
            if (!START[t][k] && !SOUNDS[i].name[k])
                trk[t].sound = (uint8_t)(i + 1u);
        }
        trk[t].pitch = 0;
        trk[t].filt = 0;
        trk[t].decay = list_sound(trk[t].sound) && !(list_sound(trk[t].sound)->flags & SF_DRUM) ? 50 : 100;
        trk[t].level = 80;
        trk[t].env_a = 0;
        trk[t].env_r = ENV_AUTO;
        trk[t].env_s = 100;
        trk[t].env_v = ENV_V_START;
        trk[t].fit = 0;
        trk[t].ed_start = 0;
        trk[t].ed_rev = 0;
        trk[t].ed_loop = ED_LOOP_AUTO;
        trk[t].ed_len = 100;
        trk[t].arp_r = 0;
        trk[t].arp_m = ARP_UP;
        trk[t].arp_o = 1;
        trk[t].arp_g = 50;
        trk[t].lfo_a = 0;
        trk[t].lfo_r = LFO_R_START;
        trk[t].lfo_t = LFO_PITCH;
        trk[t].lfo_s = LFO_SINE;
        lfo_ph[t] = 0.0f;
        trk[t].fx[0] = trk[t].fx[1] = FX_OFF;
        trk[t].fxk[0] = trk[t].fxk[1] = 0;
        trk[t].eq[0] = trk[t].eq[1] = trk[t].eq[2] = trk[t].eq[3] = 0;
        for (i = 0; i < NSTEP; i++) {
            step_on[t][i] = 0;
            step_note[t][i] = 60;
        }
    }
}

static uint8_t my_again;                     /* the scans after the start done so far (my_frame) */
static uint32_t my_t0;                       /* plat_ms at the start */
/* --- SAVE: the song kept in the flash (fm1-base's store, the object OBJ_SETTINGS: two sectors, a CRC, safe against
 * a write cut short), when SAVE is pressed and only then. Kept: each track's sound, knobs, effects, envelope, LFO,
 * EDIT pages, ARP and steps; the tempo, the time signature, the swing; Bluetooth on or off. Not kept: the loop (it
 * is sound in the RAM) and the app's samples themselves. A built-in sound is kept by its name's hash, so the list may
 * change between versions; a sample of the app's by its slot and its data's CRC: if that slot is empty or holds
 * another sample at the next start, the track is EMPTY. A record of another version, or a damaged one, is left
 * alone and the FM-1 starts as new. Writing the flash stops the sound for a moment: SAVE works while stopped --- */
#define SONG_MAGIC 0x314B5453u               /* "STK1" */
#define SONG_VER 1u
#define SONG_TRK 68u                         /* bytes a track: its sound 8, its settings 25, its steps 16 and their notes 16;
                                              * since 4.06 the EQ in the three left (33, 66, 67: eq_word) */
#define SONG_LEN (16u + NTRK * SONG_TRK)
static uint8_t song_buf[SONG_LEN];
static struct { uint8_t slot; uint32_t crc; } song_my[NTRK];   /* a track waits for its sample (slot 1 + k; 0: none) */
static uint8_t song_drawn = 1;               /* 0: a track got its sample, the screen is to show it */

static uint32_t name_hash(const char *n)     /* FNV-1a, never 0 or a slot's mark */
{
    uint32_t h = 0x811C9DC5u;
    while (*n)
        h = (h ^ (uint8_t)*n++) * 0x01000193u;
    return h < 16u ? h + 16u : h;
}
static uint32_t my_crc_of(uint32_t k)        /* the CRC a slot's header names for its data */
{
    const uint8_t *p = plat_data(MY_BASE + k * MY_SIZE);
    return p && my[k].valid ? rd32(p + 20) : 0u;
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
/* a track's settings, in the record's order: each one byte, with its least and its most (a record that says more is
 * brought back inside) */
#define SONG_FIELDS(T, F) \
    F(T.pitch, -24, 24) F(T.filt, -FILT_MAX, FILT_MAX) F(T.decay, 0, 100) F(T.level, 0, 100) \
    F(T.env_a, 0, 100) F(T.env_r, ENV_AUTO, 100) F(T.env_s, 0, 100) F(T.env_v, 0, 100) \
    F(T.lfo_a, 0, 100) F(T.lfo_r, 0, (int32_t)NLFO_R - 1) F(T.lfo_t, 0, (int32_t)NLFO_T - 1) F(T.lfo_s, 0, (int32_t)NLFO_S - 1) \
    F(T.ed_start, 0, 99) F(T.ed_rev, 0, 1) F(T.ed_loop, 0, 2) F(T.ed_len, 1, 100) F(T.fit, 0, (int32_t)NFIT - 1) \
    F(T.arp_r, 0, (int32_t)NARP_R - 1) F(T.arp_m, 0, (int32_t)NARP_M - 1) F(T.arp_o, 1, 3) F(T.arp_g, 10, 100) \
    F(T.fx[0], 0, (int32_t)NFX - 1) F(T.fx[1], 0, (int32_t)NFX - 1) F(T.fxk[0], -FX_MAX, FX_MAX) F(T.fxk[1], -FX_MAX, FX_MAX)

/* the EQ in three bytes, one 24-bit number (the record's and the dump's bytes that were 0 till 4.06: an older song,
 * all 0, is no EQ): bits 0-4 LOW, 5-9 MID, 10-15 MID FREQ, 16-20 HIGH, each two's complement */
static const uint8_t EQ_SH[5] = {0, 5, 10, 16, 21};
static uint32_t eq_word(uint32_t t)
{
    uint32_t q, w = 0;
    for (q = 0; q < 4u; q++)
        w |= ((uint32_t)trk[t].eq[q] & ((1u << (EQ_SH[q + 1u] - EQ_SH[q])) - 1u)) << EQ_SH[q];
    return w;
}
static void eq_unword(uint32_t t, uint32_t w)
{
    uint32_t q;
    for (q = 0; q < 4u; q++) {
        const uint32_t up = 32u - EQ_SH[q + 1u], b = EQ_SH[q + 1u] - EQ_SH[q];
        trk[t].eq[q] = (int8_t)clamp((int32_t)(w << up) >> (32u - b), q == 2u ? -EQ_F : EQ_MIN, q == 2u ? EQ_F : EQ_MAX);
    }
}

static int song_save(void)                   /* 0: kept */
{
    uint32_t t, i;
    memset(song_buf, 0, sizeof song_buf);
    put32(song_buf, SONG_MAGIC);
    song_buf[4] = SONG_VER;
    song_buf[5] = (uint8_t)NTRK;
    song_buf[6] = (uint8_t)SONG_TRK;
    song_buf[8] = (uint8_t)clamp((int32_t)(44100.0f * 15.0f / seq_sf + 0.5f), 40, 240);
    song_buf[9] = seq_sig;
    song_buf[10] = seq_swing;
    song_buf[11] = bt_on;
    song_buf[12] = pf_latch;                 /* (3.99: a byte that was 0 before: an older song says OFF) */
    for (t = 0; t < NTRK; t++) {
        uint8_t *p = song_buf + 16u + t * SONG_TRK, *q = p + 8;
        const uint32_t li = trk[t].sound;
        if (li >= 1u && li <= NSOUNDS) {
            put32(p, name_hash(SOUNDS[li - 1u].name));
        } else if (li > NSOUNDS && li < NLIST && my[li - NSOUNDS - 1u].valid) {
            put32(p, li - NSOUNDS);              /* 1 .. 3: the app's slot, and whose sample it was */
            put32(p + 4, my_crc_of(li - NSOUNDS - 1u));
        }
#define F(x, lo, hi) *q++ = (uint8_t)(x);
        SONG_FIELDS(trk[t], F)
#undef F
        for (i = 0; i < NSTEP; i++) {
            p[34u + i] = step_on[t][i] ? 1u : 0u;
            p[50u + i] = step_note[t][i];
        }
        i = eq_word(t);                      /* the EQ (4.06): in the three bytes left over */
        p[33] = (uint8_t)i;
        p[66] = (uint8_t)(i >> 8);
        p[67] = (uint8_t)(i >> 16);
    }
    return plat_store_save(OBJ_SETTINGS, song_buf, SONG_LEN);
}

static void song_my_place(void)              /* the tracks that wait for a sample of the app's: is it there, the same? */
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        if (song_my[t].slot && my[song_my[t].slot - 1u].valid && my_crc_of(song_my[t].slot - 1u) == song_my[t].crc &&
            !trk[t].sound && !(t == lp.trk && lp.own)) {
            trk[t].sound = (uint8_t)(NSOUNDS + song_my[t].slot);
            song_my[t].slot = 0;
            song_drawn = 0;
        }
}

static int song_load(void)                   /* 1: a song was there and is in place; 0: none (or not one of ours) */
{
    uint32_t t, i;
    memset(song_my, 0, sizeof song_my);
    if (plat_store_load(OBJ_SETTINGS, song_buf, SONG_LEN) != (int)SONG_LEN || rd32(song_buf) != SONG_MAGIC ||
        song_buf[4] != SONG_VER || song_buf[5] != NTRK || song_buf[6] != SONG_TRK)
        return 0;
    seq_sf = 44100.0f * 60.0f / ((float)clamp(song_buf[8], 40, 240) * 4.0f);
    seq_sig = (uint8_t)(song_buf[9] < NSIG ? song_buf[9] : 2u);
    seq_len = SIG_LEN[seq_sig];
    seq_swing = (uint8_t)clamp(song_buf[10], 50, 75);
    bt_on = (uint8_t)(song_buf[11] != 0u);
    pf_latch = (uint8_t)(song_buf[12] == 1u);
    for (t = 0; t < NTRK; t++) {
        const uint8_t *p = song_buf + 16u + t * SONG_TRK, *q = p + 8;
        const uint32_t id = rd32(p);
        trk[t].sound = 0;
        if (id >= 1u && id <= NMY) {             /* a sample of the app's: placed when (if) it is found */
            song_my[t].slot = (uint8_t)id;
            song_my[t].crc = rd32(p + 4);
        } else if (id) {
            for (i = 0; i < NSOUNDS && name_hash(SOUNDS[i].name) != id; i++)
                ;
            trk[t].sound = (uint8_t)(i < NSOUNDS ? i + 1u : 0u);     /* (a sound no longer in the list: EMPTY) */
        }
#define F(x, lo, hi) x = (__typeof__(x))clamp((lo) < 0 ? (int32_t)(int8_t)*q : (int32_t)*q, lo, hi); q++;
        SONG_FIELDS(trk[t], F)
#undef F
        for (i = 0; i < NSTEP; i++) {
            step_on[t][i] = (uint8_t)(p[34u + i] != 0u);
            step_note[t][i] = (uint8_t)(p[50u + i] & 127u);
        }
        eq_unword(t, p[33] | (uint32_t)p[66] << 8 | (uint32_t)p[67] << 16);   /* (before 4.06: 0, no EQ) */
    }
    song_my_place();
    return 1;
}

static void song_clear(void)                 /* the saved song gone, and the FM-1 as new */
{
    uint32_t t;
    memset(song_buf, 0, 16);
    plat_store_save(OBJ_SETTINGS, song_buf, 16);
    memset(song_my, 0, sizeof song_my);
    defaults();
    for (t = 0; t < NVOICE; t++)
        voice[t].state = V_OFF;
    seq_sf = SEQ_SF_120;
    seq_sig = 2;
    seq_len = NSTEP;
    seq_swing = 50;
    pf_latch = 0;
    perform_off();
    if (!bt_on) {
        bt_on = 1;
        plat_ble_enable(1);
    }
}

#include "dump.c"                         /* the dump for the app's VACUUM: the song and the loop, read only */

void app_init(void)
{
    fx_reset();
    memset(&lp, 0, sizeof lp);               /* no loop at a start: it was never kept */
    lp.div = 2;
    lp.bars = 1;
    stack_mute = 0;                          /* a start always sounds */
    seq_len = NSTEP;                         /* 4/4, straight */
    seq_sig = 2;
    seq_swing = 50;
    bt_on = 1;
    bt_again = 0;
    pf_latch = 0;
    memset(&arp, 0, sizeof arp);
    memset(rev_of, -1, sizeof rev_of);
    perform_reset();
    memset(&dump, 0, sizeof dump);
    memset(&cki, 0, sizeof cki);             /* and leads, at 120 */
    memset(&ck_tb, 0, sizeof ck_tb);
    memset(&ck_sent, 0, sizeof ck_sent);
    ck_follow = 0;
    ck_src = PORT_USB;
    ck_ble = 0;
    ck_ui = 0;
    ck_r = ck_w;
    seq_sf = SEQ_SF_120;
    seq_ext = 0;
    ck_left = 0.0f;
    memset(&mute, 0, sizeof mute);
    memset(my, 0, sizeof my);                /* (a start is from nothing: the simulator's restart keeps the RAM) */
    ms.slot = -1;
    mc.slot = -1;
    rx.on = 0;
    my_again = 0;
    my_load_all();                           /* the samples the app sent before (they stay in the flash) */
    defaults();
    song_load();                             /* the song SAVE kept, if there is one */
}

/* --------------------------------------------------------------------- UI --- */
enum { PAGE_PLAY, PAGE_STEP, PAGE_FX, PAGE_ENV, PAGE_LFO, PAGE_EDIT, PAGE_ARP, PAGE_EDIT2, PAGE_GLO, PAGE_EQ };
#define PG4(p) ((p) == PAGE_EDIT ? 0u : (p) == PAGE_ARP ? 1u : (p) == PAGE_EDIT2 ? 2u : (p) == PAGE_EQ ? 4u : 3u)   /* page_words' pages */
static struct {
    char msg[2][28];                     /* a short message over the header */
    uint32_t msg_until;
    uint8_t msg_low;                     /* the message is on the bottom line instead (a take's header stays: say_t8l) */
    uint32_t keys, btn;                  /* held at the last input */
    uint8_t sel;                         /* the selected track */
    uint8_t page;
    int8_t octave;                       /* -2 .. +2 */
    int8_t held_step;                    /* the step page: the step whose key is down (-1 none) */
    uint8_t held_turned;                 /* KNOB 1 turned while it was held: letting go does not toggle it */
    uint8_t note_of[NKEYS];              /* the note each held key started (the octave may change under it) */
    uint8_t trk_of[NKEYS];
    int8_t hot;                          /* the knob turned last (0..3), lit for a moment */
    uint32_t hot_t;
    uint8_t title;                       /* the title is up (the first TITLE_MS after a start) */
    uint8_t rec_take;                    /* this press of REC began a take (a hold drops it; let go in time, it stays) */
    uint8_t rec_fired;                   /* REC held long enough: it cleared (letting go does nothing more) */
    uint32_t rec_t;                      /* REC went down (0: not held) */
    uint32_t loop_seen;                  /* takes finished that the screen has said */
    uint32_t loop_drawn;                 /* the looper's state as the screen shows it */
    uint8_t redraw;
    uint8_t perform;                     /* SEL is held: the keys are PERFORM's */
    uint8_t pn, pstack[5];               /* PERFORM's buffer effects that are on, in the order they came (the last one plays) */
    uint32_t pkeys;                      /* keys that went down for PERFORM (their letting go is no note's end) */
    uint8_t sv_fired;                    /* SAVE held long enough: it cleared (letting go is no save) */
    uint32_t sv_t;                       /* SAVE went down (0: not held) */
    uint32_t deaf_t;                     /* the knobs do nothing until they have rested 0.3 s (a turn closed the city view) */
    uint32_t home_t;                     /* HOME went down (0: not held) */
    uint32_t idle_t;                     /* the last button or knob */
    uint32_t wake;                       /* buttons down when a touch closed the city view: they do nothing until let go */
} ui;

#ifdef PLAT_HOST                             /* what REC's hold line and the bottom line's corner said (expect scr_) */
static char host_hold[48], host_corner[24];
#define HOST_KEEP(b, s) (strncpy(b, s, sizeof b - 1u))
#define HOST_DROP(b) ((b)[0] = 0)
#else
#define HOST_KEEP(b, s) ((void)0)
#define HOST_DROP(b) ((void)0)
#endif

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[64];
    for (i = 0; i < 27u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 27u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1600u;
    ui.msg_low = 0;
    ui.redraw = 1;
    if (ann_on()) {                          /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void note_name(int32_t note, char *b)
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char o[12];
    b[0] = 0;
    text_cat(b, N[note % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}

static void trk_text(uint32_t t, char *b)    /* "T3" */
{
    b[0] = 'T';
    b[1] = (char)('1' + t);
    b[2] = 0;
}

static void filt_text(int32_t f, char *b)    /* "OFF", "LP 12", "HP 40" */
{
    char n[8];
    b[0] = 0;
    if (f >= -FILT_DEAD && f <= FILT_DEAD) {
        text_cat(b, "OFF");
        return;
    }
    text_cat(b, f < 0 ? "LP " : "HP ");
    int_text(f < 0 ? -f - FILT_DEAD : f - FILT_DEAD, n);
    text_cat(b, n);
}

static void speak(void)                      /* a screen reader that asks hears where we are */
{
    char t[96], n[28];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". TRACK ");
        int_text(ui.sel + 1, n);
        text_cat(t, n);
        text_cat(t, ". ");
        list_name(trk[ui.sel].sound, n);
        text_cat(t, n);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* PRESETS: the selected track's sound, one place along the list; the list stops at its ends. Only the sound changes:
 * the knobs' values, the steps and the other tracks stay. The screen says where the turn is now: the group and the
 * place, and the sound */
/* ------------------------------------------------------------------ the looper's side of the panel --- */
static int on_t8l(void) { return ui.sel == lp.trk && lp.own; }   /* the looper's track (T8L or T7L) is selected */
static void say_t8l(const char *b)           /* why the press does nothing; over a take, on the bottom line (the top */
{                                            /* of the screen shows the recording) */
    app_say("THE LOOP'S TRACK:", b);
    ui.msg_low = (uint8_t)LP_TAKE;
}

static void bars_cat(char *a)                /* + "2 BARS": the loop's length */
{
    char n[8];
    int_text(lp.bars, n);
    text_cat(a, n);
    text_cat(a, lp.bars == 1u ? " BAR" : " BARS");
}

static void layer_text(char *a, uint32_t n)  /* "LAYER 3" */
{
    char c[8];
    a[0] = 0;
    text_cat(a, "LAYER ");
    int_text((int32_t)n, c);
    text_cat(a, c);
}

/* REC goes down: a layer is recorded from this moment, or the one recording ends: a first layer at the nearest whole
 * number of bars, one on top where it is. Stopped, the FM-1 leading, the press starts the transport too (4.04);
 * following a clock it is the clock's to start. Nothing is said: the top of the screen shows the take at once. The
 * loop lives on track 8 or 7 (looper.c): that track is T8L or T7L */
static void looper_press(void)
{
    if (lp.state == LP_REC) {
        if (!lp.stop)
            lp.req = LQ_STOP;
    } else if (LP_TAKE) {
        return;
    } else if (!seq_play_req && ck_follow) {
        app_say("REC: THE CLOCK", "MUST RUN FIRST");     /* (the transport is the clock's to start) */
    } else if (lp.fading) {
        app_say("WAIT", "");
    } else if (!lp.has && !looper_fit(pat_frames())) {
        app_say("PATTERN TOO LONG", "TO LOOP");
    } else {
        if (!lp.has)
            lp.lvl = 80;                     /* (a new loop: at the level it is played at) */
        lp.own = 1;                          /* (its track is the looper's from here: the audio interrupt takes the take) */
        seq_play_req = 1;                    /* stopped, the FM-1 leading: the pattern starts with the take (bar 1 is here) */
        lp.armw = 0;
        lp.go = 0;
        lp.req = LQ_ARM;
        ui.rec_take = 1;
        ui.msg_until = 0;                    /* (whatever was said makes room: RECORDING shows now) */
        ui.redraw = 1;
    }
}

static void looper_undo(void)                /* REC held 3 to 5 s: the take under way is dropped, or the newest layer goes */
{
    char a[20];
    if (LP_TAKE_LIVE) {                      /* (one whose cancel is asked for already, at half a second, is not: the
                                              * undo, as the hold's line says; looper.c lets that take go first) */
        lp.req = LQ_CANCEL;
        app_say("TAKE", "CANCELLED");
    } else if (lp.undo) {
        layer_text(a, lp.layers);
        lp.req = LQ_UNDO;
        app_say(a, "UNDONE");
    } else if (lp.has && pf_undo_gone) {    /* PERFORM's buffer took the block the undo slept in (perform.c) */
        app_say("NO UNDO:", "PERFORM USED IT");
    } else if (lp.has) {
        app_say("ONLY 1 LAYER", "CAN BE UNDONE");
    } else {
        app_say("NO LAYER TO UNDO", "");
    }
}

static void looper_clear(void)               /* REC held 5 s: the whole loop goes; tracks 7 and 8 are their own again */
{
    if (lp.has || LP_TAKE_LIVE) {
        lp.req = LQ_CLEAR;
        app_say(lp.has ? "LOOP CLEARED" : "TAKE", lp.has ? "" : "CANCELLED");  /* (no loop: only the take went) */
    } else {
        app_say("NOTHING TO CLEAR", "");
    }
}

static void sound_turn(int32_t e)
{
    uint32_t t = ui.sel, old = trk[t].sound;
    int32_t to = clamp((int32_t)old + e, 0, (int32_t)NLIST - 1);
    char a[28], b[28];
    if (on_t8l()) {                          /* the looper's track has no sound to pick */
        say_t8l("SELECT ANOTHER");
        return;
    }
    if ((uint32_t)to == old) {
        app_say(e < 0 ? "START OF LIST" : "END OF LIST", "");
        return;
    }
    trk[t].sound = (uint8_t)to;
    cat_text((uint32_t)to, a);               /* where the turn is now: the group and the place ("FX 2/5"), the sound */
    list_name((uint32_t)to, b);
    app_say(a[0] ? a : b, a[0] ? b : "");    /* (EMPTY has no group: its name, with no space before it) */
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(ui.octave + d, -2, 2);
    char t[4];
    if (o == ui.octave) {
        app_say("OCTAVE", d < 0 ? "LOWEST" : "HIGHEST");
        return;
    }
    ui.octave = (int8_t)o;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
}

static void knob_turn(uint32_t k, int32_t e)
{
    track_t *t = &trk[ui.sel];
    char a[28], n[12];
    a[0] = 0;
    if (ui.page == PAGE_EDIT) {              /* the EDIT page: START, REVERSE, LOOP, LENGTH */
        if (on_t8l()) {                      /* the looper's track has no sound to edit */
            say_t8l("NOTHING TO EDIT");
            return;
        }
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        if (k == 0u)
            t->ed_start = (uint8_t)clamp(t->ed_start + e, 0, 99);
        else if (k == 1u)
            t->ed_rev = (uint8_t)clamp(t->ed_rev + e, 0, 1);
        else if (k == 2u)
            t->ed_loop = (uint8_t)clamp(t->ed_loop + e, 0, 2);
        else
            t->ed_len = (uint8_t)clamp(t->ed_len + e, 1, 100);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_EDIT2) {             /* EDIT 2: SWING, FIT, TIME SIG, TEMPO (FIT the track's; the rest all tracks') */
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        if (k == 0u) {
            seq_swing = (uint8_t)clamp(seq_swing + e, 50, 75);
        } else if (k == 1u) {
            if (on_t8l())
                say_t8l("NOTHING TO FIT");
            else
                t->fit = (uint8_t)clamp(t->fit + e, 0, (int32_t)NFIT - 1);
        } else if (k == 2u) {
            int32_t to = clamp(seq_sig + e, 0, (int32_t)NSIG - 1);
            if (to != seq_sig) {
                seq_sig = (uint8_t)to;
                seq_len = SIG_LEN[to];
                app_say("TIME SIG (ALL TRACKS):", SIG_NAME[to]);
            }
        } else if (ck_follow) {
            app_say("TEMPO:", "THE CLOCK IN SETS IT");
        } else {
            int32_t bpm = clamp((int32_t)(44100.0f * 15.0f / seq_sf + 0.5f) + e, 40, 240);
            seq_sf = 44100.0f * 60.0f / ((float)bpm * 4.0f);
        }
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_EQ) {                /* FX page 2, the EQ: LOW, MID, MID FREQ, HIGH */
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        t->eq[k] = (int8_t)clamp(t->eq[k] + e, k == 2u ? -EQ_F : EQ_MIN, k == 2u ? EQ_F : EQ_MAX);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_GLO) {               /* GLO: KNOB 1 Bluetooth on or off; KNOB 4 PERFORM's LATCH */
        if (k == 3u) {
            uint32_t to = e > 0;
            ui.hot = 3;
            ui.hot_t = plat_ms();
            if (to != pf_latch) {
                pf_latch = (uint8_t)to;
                if (!to && !ui.perform)
                    perform_off();           /* (LATCH off: what it held on goes off) */
                app_say("PERFORM LATCH:", to ? "ON" : "OFF");
            }
        }
        if (k == 0u) {
            uint32_t to = e > 0;
            ui.hot = 0;
            ui.hot_t = plat_ms();
            if (to != bt_on) {
                bt_on = (uint8_t)to;
                bt_again = 0;
                plat_ble_enable((int)to);
                app_say("BLUETOOTH:", to ? "ON" : "OFF");
            }
        }
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_ARP) {               /* the ARP page: RATE, MODE, OCTAVE, GATE */
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        if (k == 0u)
            t->arp_r = (uint8_t)clamp(t->arp_r + e, 0, (int32_t)NARP_R - 1);
        else if (k == 1u)
            t->arp_m = (uint8_t)clamp(t->arp_m + e, 0, (int32_t)NARP_M - 1);
        else if (k == 2u)
            t->arp_o = (uint8_t)clamp(t->arp_o + e, 1, 3);
        else
            t->arp_g = (uint8_t)clamp(t->arp_g + e, 10, 100);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_LFO) {               /* the LFO's page: AMOUNT, RATE, TARGET, SHAPE */
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        if (k == 0u)
            t->lfo_a = (uint8_t)clamp(t->lfo_a + e, 0, 100);
        else if (k == 1u)
            t->lfo_r = (uint8_t)clamp(t->lfo_r + e, 0, (int32_t)NLFO_R - 1);
        else if (k == 2u)
            t->lfo_t = (uint8_t)clamp(t->lfo_t + e, 0, (int32_t)NLFO_T - 1);
        else
            t->lfo_s = (uint8_t)clamp(t->lfo_s + e, 0, (int32_t)NLFO_S - 1);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_ENV) {               /* the envelope's page: ATTACK, RELEASE, SUSTAIN, VELOCITY */
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        if (k == 0u)
            t->env_a = (uint8_t)clamp(t->env_a + e, 0, 100);
        else if (k == 1u)
            t->env_r = (int8_t)clamp(t->env_r + e, ENV_AUTO, 100);
        else if (k == 2u)
            t->env_s = (uint8_t)clamp(t->env_s + e, 0, 100);
        else
            t->env_v = (uint8_t)clamp(t->env_v + e, 0, 100);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_FX) {                /* the effects' page: KNOB 1, 3 the effect of slot A, B; KNOB 2, 4 its knob */
        uint32_t q = k >> 1;
        ui.hot = (int8_t)k;
        ui.hot_t = plat_ms();
        if (k & 1u) {
            t->fxk[q] = (int8_t)clamp(t->fxk[q] + e, -FX_MAX, FX_MAX);
        } else {                             /* the list stops at its ends; the screen says the old and the new */
            int32_t old = t->fx[q], to = clamp(old + e, 0, (int32_t)NFX - 1);
            if (to != old) {
                t->fx[q] = (uint8_t)to;
                text_cat(a, q ? "B: " : "A: ");
                text_cat(a, FX_NAME[old]);
                {
                    char m[20];
                    m[0] = 0;
                    text_cat(m, "> ");
                    text_cat(m, FX_NAME[to]);
                    app_say(a, m);
                }
            } else {
                app_say(e < 0 ? "START OF LIST" : "END OF LIST", "");
            }
        }
        ui.redraw = 1;
        return;
    }
    if (ui.page == PAGE_STEP && ui.held_step >= 0) {
        if (k == 0u) {                       /* a step held, KNOB 1: its note; the step is set */
            uint32_t s = (uint32_t)ui.held_step;
            int32_t old = step_note[ui.sel][s], to = clamp(old + e, 24, 96);
            const sound_t *snd = list_sound(t->sound);
            step_note[ui.sel][s] = (uint8_t)to;
            step_on[ui.sel][s] = 1;
            ui.held_turned = 1;
            if (to != old)
                ev_put(QE_PING, ui.sel, (uint32_t)to, 100);
            text_cat(a, "STEP ");
            int_text((int32_t)s + 1, n);
            text_cat(a, n);
            text_cat(a, ": ");
            note_name(to, n);
            text_cat(a, snd && (snd->flags & SF_DRUM) ? "DRUM" : n);
            app_say(a, "");
        }
        return;
    }
    ui.hot = (int8_t)k;
    ui.hot_t = plat_ms();
    if (k == 0u) {
        t->pitch = (int8_t)clamp(t->pitch + e, -24, 24);
    } else if (k == 1u) {
        t->filt = (int8_t)clamp(t->filt + e, -FILT_MAX, FILT_MAX);
    } else if (k == 2u) {
        t->decay = (uint8_t)clamp(t->decay + e, 0, 100);
    } else {
        uint8_t *l = lvl_of(ui.sel);         /* (the loop's track: the loop's own level) */
        *l = (uint8_t)clamp(*l + e, 0, 100);
    }
    ui.redraw = 1;
}

#include "city.c"                         /* the city view: the sounds' levels as a street's lights */

static const char *const PK_NAME[NPK][2] = {
    {"REPEAT", "1/8"}, {"REPEAT", "1/16"}, {"REPEAT", "1/32"}, {"REVERSE", ""}, {"FREEZE", ""}, {"TAPE", "STOP"},
    {"LOW-", "PASS"}, {"HIGH-", "PASS"}, {"FLANGER", ""}, {"PHASER", ""}, {"ROBOT", ""}, {"LO-FI", ""},
    {"REVERB", "THROW"}, {"ECHO", "THROW"}, {"OCT", "DOWN"}, {"GATE", ""}};
static uint32_t perform_lit(void)            /* the effects that are on, a bit each (PK_*) */
{
    uint32_t m = pf_on, i;
    for (i = 0; i < ui.pn; i++)
        m |= 1u << ui.pstack[i];
    return m;
}
static uint32_t perform_take(void) { return lp.state != LP_IDLE || lp.req == LQ_ARM; }   /* a take: some keys wait */
static void perform_set(uint32_t k, uint32_t on)     /* effect k on or off */
{
    uint32_t i, j;
    if (k < 5u) {                            /* the buffer's: kept in the order they came; the last one plays */
        for (i = j = 0; i < ui.pn; i++)
            if (ui.pstack[i] != k)
                ui.pstack[j++] = ui.pstack[i];
        ui.pn = (uint8_t)j;
        if (on && ui.pn < 5u)
            ui.pstack[ui.pn++] = (uint8_t)k;
        pf_buf = (uint8_t)(ui.pn ? 1u + ui.pstack[ui.pn - 1u] : 0u);
    } else {
        pf_on = (uint16_t)(on ? pf_on | 1u << k : pf_on & ~(1u << k));
    }
    ui.redraw = 1;
}
static void perform_off(void)                /* every effect off */
{
    pf_on = 0;
    pf_buf = 0;
    ui.pn = 0;
    ui.redraw = 1;
}

#define SAVE_HOLD_MS 2000u                /* SAVE held this long clears the saved song */
#define HOME_HOLD_MS 500u                 /* HOME held this long opens the city view */
static void input(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    uint32_t keys, btn = plat_buttons(), k, pressed, released, lit_keys = 0, lit_btn, pskip = 0;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    pressed = btn & ~ui.btn;
    released = ~btn & ui.btn;
    if (city.stage) {
        /* the city view is up: the first button or knob only brings the page back, and does nothing else (a key
         * too, where the keys are not the track's notes). The buttons down then are deaf until let go */
        uint32_t any = pressed != 0u;
        for (k = 0; k < NE; k++)
            any |= plat_enc((int)k) != 0;
        if (ui.page != PAGE_PLAY)
            any |= (keys & ~ui.keys) != 0u;
        if (any && pressed == 1u << B_REC && ui.page != PAGE_GLO) {
            city_close();                    /* REC is the looper's and must fall on its bar: it closes the view and counts */
            ui.idle_t = plat_ms();
        } else if (any) {
            city_close();
            ui.wake = btn;
            ui.idle_t = plat_ms();
            ui.btn = btn;
            ui.keys = ui.page != PAGE_PLAY ? keys : ui.keys;
            ui.home_t = ui.sv_t = 0;
            ui.deaf_t = plat_ms() | 1u;
            return;
        }
    }
    if (ui.deaf_t) {                         /* the turn that closed the view goes on: nothing of it counts */
        uint32_t any = 0;
        for (k = 0; k < NE; k++)
            any |= plat_enc((int)k) != 0;
        if (any)
            ui.deaf_t = plat_ms() | 1u;
        else if ((int32_t)(plat_ms() - ui.deaf_t) > 300)
            ui.deaf_t = 0;
    }
    pressed &= ~ui.wake;
    released &= ~ui.wake;
    ui.wake &= btn;
    if (pressed)
        ui.idle_t = plat_ms();
    /* PERFORM: while SEL is held the white keys are its sixteen effects (no notes, no MIDI, nothing for the looper or
     * the steps). Held, an effect is on; with GLO's LATCH a press turns it on and the next one off, and it stays when
     * SEL is let go. SEL with OCT-: every effect off */
    if (pressed >> B_SEL & 1u) {
        for (k = 0; k < NKEYS; k++)          /* the notes the keys held: let go */
            if ((ui.keys >> k & 1u) && !(ui.pkeys >> k & 1u) && ui.page != PAGE_STEP && !on_t8l()) {
                ev_put(QE_OFF, ui.trk_of[k], ui.note_of[k], 0);
                if (ck_ble)
                    plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)ui.note_of[k] << 16, TO_BLE);
            }
        ui.pkeys |= ui.keys;
        ui.held_step = -1;
        ui.perform = 1;
        ui.msg_until = 0;                    /* (a message before makes room: the header is PERFORM's at once) */
        ui.redraw = 1;
    }
    if ((released >> B_SEL & 1u) && ui.perform) {
        ui.perform = 0;
        if (!pf_latch)
            perform_off();
        ui.redraw = 1;
    }
    if (ui.perform && (pressed >> B_OCTDN & 1u)) {
        perform_off();
        app_say("PERFORM:", "ALL OFF");
        pressed &= ~(1u << B_OCTDN);
        ui.wake |= 1u << B_OCTDN;            /* (letting it go is no octave, no undo) */
    }
    for (k = 0; k < NKEYS && (ui.perform || ui.pkeys); k++) {
        const uint32_t bit = 1u << k, down = (keys & bit) && !(ui.keys & bit), up = !(keys & bit) && (ui.keys & bit);
        uint32_t s2;
        for (s2 = 0; s2 < 16u && WK[s2] != k; s2++)
            ;
        if (up && (ui.pkeys & bit)) {        /* a key of PERFORM's let go (SEL held still or not) */
            ui.pkeys &= ~bit;
            if (s2 < 16u && !pf_latch)
                perform_set(s2, 0);
            pskip |= bit;                    /* (taken: the notes' part below does not see it) */
        }
        if (down && ui.perform) {
            ui.pkeys |= bit;
            if (s2 < 16u) {
                if ((PK_NOTAKE >> s2 & 1u) && perform_take())
                    app_say(PK_NAME[s2][0], "WAITS FOR THE TAKE");
                else
                    perform_set(s2, pf_latch ? !(perform_lit() >> s2 & 1u) : 1u);
            }
        }
    }

    /* the keys: on the play page they play the selected track; on the step page the 16 white keys are the steps */
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k, down = (keys & bit) && !(ui.keys & bit), up = !(keys & bit) && (ui.keys & bit);
        if ((ui.pkeys | pskip) & bit)        /* PERFORM's: no note, no step */
            continue;
        if (on_t8l()) {                      /* the looper's track: the keys are silent, and the screen says why */
            if (down)
                say_t8l("SELECT ANOTHER");
            continue;
        }
        if (ui.page == PAGE_STEP) {
            uint32_t s;
            for (s = 0; s < 16u && WK[s] != k; s++)
                ;
            if (s == 16u)
                continue;                    /* (the black keys have no job on the step page) */
            if (s >= seq_len) {              /* a key past the pattern's end, as the time signature has it */
                if (down)
                    app_say("NO STEP THERE IN", SIG_NAME[seq_sig]);
                continue;
            }
            if (down && ui.held_step < 0) {
                ui.held_step = (int8_t)s;
                ui.held_turned = 0;
                ui.redraw = 1;
            } else if (up && ui.held_step == (int8_t)s) {
                if (!ui.held_turned) {       /* a tap: the step on or off */
                    char a[28], n[8];
                    step_on[ui.sel][s] = (uint8_t)!step_on[ui.sel][s];
                    a[0] = 0;
                    text_cat(a, "STEP ");
                    int_text((int32_t)s + 1, n);
                    text_cat(a, n);
                    app_say(a, step_on[ui.sel][s] ? "ON" : "OFF");
                }
                ui.held_step = -1;
                ui.redraw = 1;
            }
            continue;
        }
        if (down) {
            uint32_t v = touch_vel(k);
            int32_t note = clamp(53 + (int32_t)k + 12 * ui.octave, 0, 127);
            ui.note_of[k] = (uint8_t)note;
            ui.trk_of[k] = ui.sel;
            ev_put(QE_ON, ui.sel, (uint32_t)note, v ? v : 100u);
            if (ck_ble)                      /* over Bluetooth the keys' notes go out (0.6.1); over USB none, as below */
                plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | (v ? v : 100u) << 24, TO_BLE);
            /* (the keys send no MIDI notes: up to 0.2.2-test they did, and the Burger Studio app plays its own drum
             * pads on any note that comes in, pad = note % 32, which came back through the FM-1's USB audio and
             * sounded like drums on some of the keys. Notes out return with a setting of their own) */
        } else if (up) {
            ev_put(QE_OFF, ui.trk_of[k], ui.note_of[k], 0);
            if (ck_ble)
                plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)ui.note_of[k] << 16, TO_BLE);
        }
    }

    /* OCT- and OCT+ move the keys' octave, when the button is let go */
    if (released >> B_OCTDN & 1u)
        octave_step(-1);
    if (released >> B_OCTUP & 1u)
        octave_step(1);
    /* REC is the looper's (but on the GLO page, where it is DISCONNECT). It acts when it goes down: a layer is recorded
     * from there, or the one recording ends. Held: let go between REC_UNDO_MS and REC_CLEAR_MS, the last layer goes;
     * at REC_CLEAR_MS the whole loop goes, there and then. Let go between a short press and REC_UNDO_MS: nothing */
    if (ui.page != PAGE_GLO) {
        const uint32_t now = plat_ms();
        if (pressed >> B_REC & 1u) {
            ui.rec_t = now ? now : 1u;
            ui.rec_fired = 0;
            HOST_DROP(host_hold);
            ui.rec_take = 0;
            looper_press();
        }
        if (ui.rec_take && ui.rec_t && lp.state == LP_PRE && (int32_t)(now - ui.rec_t) >= (int32_t)REC_SHORT_MS) {
            ui.rec_take = 0;                 /* a hold, not a take: the layer on top that the press began is dropped
                                              * (nothing of it was written: the undo is whole). A first layer records
                                              * on: there is nothing a hold could be for until 3 s */
            lp.req = LQ_CANCEL;
        }
        if (ui.rec_t && (btn >> B_REC & 1u) && !ui.rec_fired && (int32_t)(now - ui.rec_t) >= (int32_t)REC_CLEAR_MS) {
            ui.rec_fired = 1;
            looper_clear();
        }
        if (released >> B_REC & 1u) {
            if (ui.rec_t && !ui.rec_fired) {
                const int32_t held = (int32_t)(now - ui.rec_t);
                if (held < (int32_t)REC_SHORT_MS)
                    lp.go = ui.rec_take;     /* (a layer on top waited for this: it is taken) */
                else if (held >= (int32_t)REC_UNDO_MS)
                    looper_undo();
                else if (LP_TAKE_LIVE)       /* (as the hold's line: 3 s lets the take go; 5 s clears a loop there is) */
                    app_say(lp.has ? "REC: HOLD 3 S CANCEL," : "REC: HOLD 3 S", lp.has ? "5 S CLEAR" : "CANCEL");
                else if (!lp.has)
                    app_say("NO LOOP:", "NOTHING TO UNDO");
                else
                    app_say("REC: HOLD 3 S UNDO,", "5 S CLEAR");
            }
            ui.rec_t = 0;
            ui.rec_take = 0;
            ui.redraw = 1;
        }
        if (ui.rec_t && (int32_t)(now - ui.rec_t) >= (int32_t)REC_SHORT_MS)
            ui.redraw = 1;                   /* (the hold's bar grows) */
    } else {
        ui.rec_t = 0;
    }

    for (k = 0; k < NB; k++) {
        static const char *const BTN_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                                 "PLAY", "REC", "OCT-", "OCT+"};
        if (!(pressed >> k & 1u) || k == B_OCTDN || k == B_OCTUP || k == B_SAVE || k == B_HOME || k == B_SEL)
            continue;
        if (k == B_REC && ui.page == PAGE_GLO) {     /* GLO's REC: the Bluetooth link dropped (it may come back) */
            if (!bt_on) {
                app_say("BLUETOOTH IS OFF", "");
            } else if (!plat_ble_linked()) {
                app_say("BLUETOOTH:", "NOTHING CONNECTED");
            } else {                         /* off, and on again in a moment: the link ends, the FM-1 can be found again */
                plat_ble_enable(0);
                bt_again = plat_ms() | 1u;
                app_say("BLUETOOTH:", "DISCONNECTED");
            }
        } else if (k == B_REC) {
            /* (the looper's: above) */
        } else if (k == B_PLAY) {
            seq_play_req = (uint8_t)!seq_play_req;
            if (!seq_play_req && LP_TAKE)
                app_say("STOP:", "TAKE CANCELLED");  /* (a take cannot outlast the transport) */
            else
                app_say(seq_play_req ? "PLAY" : "STOP", "");
        } else if (k == B_SEQ && on_t8l()) {     /* the looper's track has no steps */
            say_t8l("SELECT ANOTHER");
        } else if (k == B_SEQ || k == B_FX || k == B_ENV || k == B_LFO || k == B_EDIT || k == B_ARP || k == B_GLO) {
            /* SEQ: the steps and back; ENV, LFO, ARP, GLO: their pages and back; EDIT: its page 1, its page 2, back;
             * FX the same: its page 1 (the effects), its page 2 (the EQ, 4.06), back */
            uint32_t to = k == B_SEQ ? PAGE_STEP : k == B_FX ? PAGE_FX : k == B_ENV ? PAGE_ENV : k == B_LFO ? PAGE_LFO :
                          k == B_EDIT ? PAGE_EDIT : k == B_ARP ? PAGE_ARP : PAGE_GLO;
            if (k == B_EDIT && ui.page == PAGE_EDIT)
                ui.page = PAGE_EDIT2;
            else if ((k == B_EDIT && ui.page == PAGE_EDIT2) || (k == B_FX && ui.page == PAGE_EQ))
                ui.page = PAGE_PLAY;
            else if (k == B_FX && ui.page == PAGE_FX)
                ui.page = PAGE_EQ;
            else
            ui.page = (uint8_t)(ui.page == to ? PAGE_PLAY : to);
            ui.held_step = -1;
            for (e = 0; e < (int32_t)NKEYS; e++)        /* keys held across the change let go */
                if (ui.keys >> e & 1u)
                    ev_put(QE_OFF, ui.trk_of[e], ui.note_of[e], 0);
            app_say(ui.page == PAGE_STEP ? "STEP PAGE" : ui.page == PAGE_FX ? "FX PAGE" : ui.page == PAGE_ENV ? "ENV PAGE" :
                    ui.page == PAGE_LFO ? "LFO PAGE" : ui.page == PAGE_EDIT ? "EDIT PAGE 1" : ui.page == PAGE_ARP ? "ARP PAGE" :
                    ui.page == PAGE_EDIT2 ? "EDIT PAGE 2" : ui.page == PAGE_GLO ? "GLO PAGE" : ui.page == PAGE_EQ ? "FX PAGE 2: EQ" : "TRACKS PAGE", "");
            ui.msg_low = (uint8_t)LP_TAKE;   /* (over a take, on the bottom line: the header stays the take's) */
            if (ui.page == PAGE_EDIT && on_t8l())
                say_t8l("NOTHING TO EDIT");
        } else {
            app_say(BTN_NAME[k], "NOT IN THIS VERSION");   /* (never a silent press) */
        }
    }

    {   /* SAVE: let go, the song is kept; held SAVE_HOLD_MS, the saved song is cleared. Only while stopped: writing
         * the flash stops the sound for a moment */
        const uint32_t now = plat_ms();
        if (pressed >> B_SAVE & 1u) {
            ui.sv_t = now | 1u;
            ui.sv_fired = 0;
        }
        if (ui.sv_t && (btn >> B_SAVE & 1u) && !ui.sv_fired && (int32_t)(now - ui.sv_t) >= (int32_t)SAVE_HOLD_MS) {
            ui.sv_fired = 1;
            if (seq_play_req) {
                app_say("STOP TO CLEAR", "");
            } else {
                song_clear();
                app_say("SAVED SONG CLEARED", "");
            }
        }
        if (released >> B_SAVE & 1u) {
            if (ui.sv_t && !ui.sv_fired) {
                if (seq_play_req)
                    app_say("STOP TO SAVE", "");
                else if (song_save())
                    app_say("NOT SAVED:", "FLASH ERROR");
                else
                    app_say("SAVED", "");
            }
            ui.sv_t = 0;
            ui.redraw = 1;
        }
        if (ui.sv_t)
            ui.redraw = 1;                   /* (the hold's bar grows) */
        /* HOME held: the mushroom view (the city view till 4.10, a face in 4.11), at any time */
        if (pressed >> B_HOME & 1u)
            ui.home_t = now | 1u;
        if (ui.home_t && (btn >> B_HOME & 1u) && (int32_t)(now - ui.home_t) >= (int32_t)HOME_HOLD_MS) {
            ui.home_t = 0;
            ui.wake |= 1u << B_HOME;         /* (letting it go does not close the view again) */
            city_open(1);
        }
        if (released >> B_HOME & 1u) {
            if (ui.home_t)
                app_say("HOLD HOME:", "MUSHROOMS");
            ui.home_t = 0;
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {      /* ALGORITHM: the track; one detent a track, it stops at 1 and 8 */
        ui.idle_t = plat_ms();
        int32_t to = clamp((int32_t)ui.sel + e, 0, (int32_t)NTRK - 1);
        if ((uint32_t)to != ui.sel) {
            ui.sel = (uint8_t)to;
            ui_sel_isr = ui.sel;
            ui.held_step = -1;
            ui.redraw = 1;
            if (ui.msg[0][8] == '\'')        /* "THE LOOP'S TRACK: ..." was for the track left: it goes at once */
                ui.msg_until = 0;
            if (on_t8l() && ui.page == PAGE_STEP)    /* onto the looper's track: it has no steps to show (its LEVEL is */
                ui.page = PAGE_PLAY;         /* K4's; the keys are silent, said when one is played) */
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        ui.idle_t = plat_ms();
        sound_turn(e);
    }
    for (k = 0; k < 4u; k++)
        if ((e = plat_enc((int)(EN_K1 + k))) != 0) {
            ui.idle_t = plat_ms();
            knob_turn(k, e);
        }
    if ((e = plat_enc(EN_SELECT)) != 0) {    /* SELECT: the tempo, a beat a minute a click, on every page (EDIT 2's KNOB 4 is the same) */
        char n[8];
        ui.idle_t = plat_ms();
        if (ck_follow) {                     /* a clock in sets it: who leads is said instead */
            app_say("TEMPO:", ck_src == PORT_BLE ? "THE BT CLOCK LEADS" : "BURGER LEADS");
        } else {
            const int32_t bpm = clamp((int32_t)(44100.0f * 15.0f / seq_sf + 0.5f) + e, 40, 240);
            seq_sf = 44100.0f * 60.0f / ((float)bpm * 4.0f);
            int_text(bpm, n);
            app_say("TEMPO", n);
        }
    }

    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;

    /* the lights: REC through a take (blinking while it waits for the bar); SEQ on the step page; PLAY while playing */
    ui_step_isr = ui.page == PAGE_STEP;
    lit_btn = (lp.req == LQ_ARM || (lp.state != LP_IDLE && (!(lp.first && lp.stop) || (plat_ms() >> 7 & 1u))) ? 1u << B_REC : 0u) |
              (ui.page == PAGE_STEP ? 1u << B_SEQ : 0u) |
              (ui.page == PAGE_FX || ui.page == PAGE_EQ ? 1u << B_FX : 0u) | (ui.page == PAGE_ENV ? 1u << B_ENV : 0u) | (ui.page == PAGE_LFO ? 1u << B_LFO : 0u) | (ui.page == PAGE_EDIT || ui.page == PAGE_EDIT2 ? 1u << B_EDIT : 0u) | (ui.page == PAGE_ARP ? 1u << B_ARP : 0u) | (ui.page == PAGE_GLO ? 1u << B_GLO : 0u) | (seq_play_req ? 1u << B_PLAY : 0u);
    if (ui.page == PAGE_STEP) {
        for (k = 0; k < seq_len; k++)
            if ((step_on[ui.sel][k] != 0) != (seq_play_req && seq_step_now == k))
                lit_keys |= 1u << WK[k];
    } else {
        lit_keys = keys;
    }
    plat_glow(1);
    plat_leds(lit_btn, lit_keys);
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ screen --- */
#define C_ORANGE RGB(255, 122, 47)

/* the EDIT (pg 0) and ARP (pg 1) pages: each of the selected track's four values in words, and where its knob stands
 * (pos, 0 .. 226; 0 for none wanted) */
static void page_words(uint32_t pg, char w[4][28], int32_t *pos)
{
    static const char *const LOOPW[3] = {"AUTO", "OFF", "ON"};
    const track_t *t = &trk[ui.sel];
    int32_t p4[4];
    uint32_t q;
    for (q = 0; q < 4u; q++)
        w[q][0] = 0;
    if (!pg) {
        int_text(t->ed_start, w[0]);
        text_cat(w[0], "%");
        text_cat(w[1], t->ed_rev ? "ON" : "OFF");
        text_cat(w[2], LOOPW[t->ed_loop < 3u ? t->ed_loop : 0u]);
        int_text(t->ed_len, w[3]);
        text_cat(w[3], "%");
        p4[0] = t->ed_start * 226 / 99;
        p4[1] = t->ed_rev * 226;
        p4[2] = t->ed_loop * 113;
        p4[3] = (t->ed_len - 1) * 226 / 99;
    } else if (pg == 2u) {
        int_text(seq_swing, w[0]);
        text_cat(w[0], "%");
        text_cat(w[1], FIT_NAME[t->fit < NFIT ? t->fit : 0u]);
        text_cat(w[2], SIG_NAME[seq_sig < NSIG ? seq_sig : 2u]);
        int_text((int32_t)(44100.0f * 15.0f / (ck_tick_frames() * 6.0f) + 0.5f), w[3]);
        p4[0] = (seq_swing - 50) * 226 / 25;
        p4[1] = t->fit * 226 / ((int32_t)NFIT - 1);
        p4[2] = seq_sig * 226 / ((int32_t)NSIG - 1);
        p4[3] = clamp((int32_t)(44100.0f * 15.0f / (ck_tick_frames() * 6.0f) + 0.5f) - 40, 0, 200) * 226 / 200;
    } else if (pg == 4u) {                   /* the EQ: "+3 DB", "1300 HZ" */
        for (q = 0; q < 4u; q++) {
            const int32_t v = t->eq[q];
            if (q == 2u) {                   /* (two figures: 160 HZ .. 890 HZ, 1000 HZ .. 6400 HZ) */
                const int32_t hz = (int32_t)eq_hz(v), r = hz < 1000 ? 10 : 100;
                int_text((hz + r / 2) / r * r, w[2]);
                text_cat(w[2], " HZ");
                p4[2] = (v + EQ_F) * 226 / (2 * EQ_F);
            } else {
                if (v > 0)
                    text_cat(w[q], "+");
                int_text(v, w[q] + (v > 0));
                text_cat(w[q], " DB");
                p4[q] = (v - EQ_MIN) * 226 / (EQ_MAX - EQ_MIN);
            }
        }
    } else if (pg == 3u) {
        text_cat(w[0], bt_on ? "ON" : "OFF");
        text_cat(w[3], pf_latch ? "ON" : "OFF");
        p4[0] = bt_on ? 226 : 0;
        p4[1] = p4[2] = 0;
        p4[3] = pf_latch ? 226 : 0;
    } else {
        text_cat(w[0], ARP_R_NAME[t->arp_r < NARP_R ? t->arp_r : 0u]);
        text_cat(w[1], ARP_M_NAME[t->arp_m < NARP_M ? t->arp_m : 0u]);
        int_text(t->arp_o, w[2]);
        int_text(t->arp_g, w[3]);
        text_cat(w[3], "%");
        p4[0] = t->arp_r * 226 / ((int32_t)NARP_R - 1);
        p4[1] = t->arp_m * 226 / ((int32_t)NARP_M - 1);
        p4[2] = (t->arp_o - 1) * 113;
        p4[3] = (t->arp_g - 10) * 226 / 90;
    }
    for (q = 0; pos && q < 4u; q++)
        pos[q] = p4[q];
}

static int32_t tag(int32_t xr, const char *s, uint16_t c)   /* a box on the version's line ending at xr: its left */
{
    const int32_t x = xr - text_w(&FONT_T, s) - 8;
    cv_rect(x, 15, xr - x, 12, c);
    cv_text(x + 4, 15, &FONT_T, s, C_BLACK);
    return x - 4;
}

static void draw_header(void)
{
    char t[44];
    int32_t xr = 240;
    uint32_t now = plat_ms();
    const uint32_t svh = ui.sv_t && !ui.sv_fired && (int32_t)(now - ui.sv_t) > 400;
    /* a bar on the second line (SAVE held, or REC's): it has the line to itself, after the version */
    const uint32_t bar = svh || (REC_UP && (now >= ui.msg_until || ui.msg_low) && !ui.perform);
    HOST_DROP(host_hold);
    cv_begin(240, 28, C_BLACK);
    /* two lines: the first is the name row or a message; the second, small, is the version, always there (the build's
     * APP_VERSION: stack/VERSION, the one place it is written) */
    if (svh) {
        /* SAVE is being held: what it will do, and a bar that fills until it does */
        const uint32_t held = now - ui.sv_t;
        cv_rect(0, 0, 44, 16, C_ORANGE);
        cv_text(5, 0, &FONT_B, "SAVE", C_BLACK);
        cv_text(50, 2, &FONT_XS, "HOLD: CLEAR SAVED SONG", C_WHITE);
        cv_rect(50, 20, 184, 5, C_LINE);
        cv_rect(50, 20, (int32_t)(held >= SAVE_HOLD_MS ? 184u : held * 184u / SAVE_HOLD_MS), 5, C_WHITE);
    } else if (now < ui.msg_until && !ui.msg_low) {
        int32_t x = cv_text(6, 0, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 6, 0, &FONT_B, ui.msg[1], C_HI);
    } else if (ui.perform) {                 /* SEL is held: the keys are PERFORM's */
        cv_rect(0, 0, 76, 16, C_ORANGE);
        cv_text(5, 0, &FONT_B, "PERFORM", C_BLACK);
        cv_text(82, 2, &FONT_XS, pf_latch ? "LATCH: A KEY ON, AGAIN OFF" : "HOLD A WHITE KEY", C_HI);
    } else if (REC_UP) {
        /* REC is held, or a take waits or runs: said on a block, then where it stands */
        const int32_t held = ui.rec_t && !ui.rec_fired ? (int32_t)(now - ui.rec_t) : 0;
        /* (the press that began a first layer, still down: it records on; the hold's line shows, the take's bar stays
         * until 3 s) */
        const int keep = ui.rec_take && lp.state == LP_REC && lp.first && held < (int32_t)REC_UNDO_MS;
        const char *w;
        char c[8];
        t[0] = 0;
        cv_rect(0, 0, 36, 16, C_ORANGE);
        cv_text(5, 0, &FONT_B, "REC", C_BLACK);
        if (held >= (int32_t)REC_SHORT_MS) {
            /* what letting go at 3 s does (as looper_undo has it) and what 5 s does: no loop, nothing to either; no
             * undo (PERFORM took its block, one layer), letting go says which */
            const int u = held >= (int32_t)REC_UNDO_MS, take = LP_TAKE_LIVE;   /* (as looper_undo) */
            w = t;
            if (!lp.has && !take) {
                w = "NO LOOP: NOTHING TO UNDO OR CLEAR";
            } else {
                text_cat(t, u ? "LET GO: " : "HOLD 3 S: ");
                text_cat(t, take ? "CANCEL TAKE" : lp.undo ? "UNDO LAYER" : "NO UNDO");
                if (lp.has)                  /* (a first layer's take: at 5 s too it is only the take that goes) */
                    text_cat(t, u ? "  HOLD ON: CLEAR" : "  5 S: CLEAR LOOP");
            }
            HOST_KEEP(host_hold, w);
        } else if (lp.first && lp.stop) {    /* a first layer, REC pressed again: it records on to its last bar's end */
            text_cat(t, "RECORDING ON TO ");
            bars_cat(t);
            w = t;
        } else {
            text_cat(t, "RECORDING LAYER ");
            int_text(lp.layers + 1, c);
            text_cat(t, c);
            if (lp.first && lp.maxf > lp.tk) {           /* a first layer: the time the memory still holds */
                const uint32_t ds = (lp.maxf - lp.tk) / 4410u;
                text_cat(t, "   ");
                int_text((int32_t)(ds / 10u), c);
                text_cat(t, c);
                c[0] = '.';
                c[1] = (char)('0' + ds % 10u);
                c[2] = 0;
                text_cat(t, c);
                text_cat(t, " S LEFT");
            }
            w = t;
        }
        cv_text(42, 2, &FONT_XS, w, held < (int32_t)REC_SHORT_MS ? C_WHITE : C_HI);
        if (held >= (int32_t)REC_SHORT_MS && !keep) {    /* the hold's bar: a mark at 3 s, full at 5 s */
            cv_rect(50, 20, 184, 5, C_LINE);
            cv_rect(50, 20, (int32_t)((uint32_t)held >= REC_CLEAR_MS ? 184u : (uint32_t)held * 184u / REC_CLEAR_MS), 5,
                    held >= (int32_t)REC_UNDO_MS ? C_ORANGE : C_WHITE);
            cv_rect(50 + (int32_t)(REC_UNDO_MS * 184u / REC_CLEAR_MS), 18, 1, 9, C_WHITE);
        } else {
            /* the take's bar, running from the press: what is left, of the memory (a first layer; of its way to its
             * last bar's end once REC was pressed again) or of the pass round the loop (a layer on top) */
            const uint32_t of = lp.first ? (lp.stop ? lp.want : lp.maxf) : lp.total, tk = lp.tk;
            cv_rect(50, 20, 184, 5, C_LINE);
            if (of && tk < of)
                cv_rect(50, 20, (int32_t)((uint64_t)(of - tk) * 184u / of), 5, C_ORANGE);
        }
    } else {
        int32_t x = cv_text(6, 0, &FONT_B, APP_NAME, C_ORANGE);
        cv_text(x + 8, 2, &FONT_XS, ui.page == PAGE_STEP ? "STEPS" : ui.page == PAGE_FX ? "FX" : ui.page == PAGE_ENV ? "ENV" : ui.page == PAGE_LFO ? "LFO" : ui.page == PAGE_EDIT ? "EDIT 1" : ui.page == PAGE_ARP ? "ARP" : ui.page == PAGE_EDIT2 ? "EDIT 2" : ui.page == PAGE_GLO ? "GLO" : ui.page == PAGE_EQ ? "FX 2 EQ" : "TRACKS",
                C_GRAY);
        t[0] = 0;
        text_cat(t, "OCT ");
        t[4] = ui.octave < 0 ? '-' : '+';
        t[5] = (char)('0' + (ui.octave < 0 ? -ui.octave : ui.octave));
        t[6] = 0;
        cv_text(150, 2, &FONT_XS, t, C_GRAY);
        cv_text(200, 2, &FONT_XS, seq_play_req ? "PLAY" : "STOP", seq_play_req ? C_HI : C_DIM);
    }
    {
        int32_t x = cv_text(6, 15, &FONT_T, APP_VERSION, C_GRAY);
        if (!bar) {
            x = cv_text(x + 8, 15, &FONT_T, !ck_follow ? "CLOCK: FM-1" : ck_src == PORT_BLE ? "CLOCK: BT" : "CLOCK: BURGER",
                        ck_follow ? C_HI : C_GRAY);
            if (ck_ble)                      /* a Bluetooth link is up */
                cv_text(x + 6, 15, &FONT_T, "BT", C_WHITE);
        }
    }
    if (stack_mute && !bar)                  /* the app plays the FM-1's sound: said on the second line, on every page */
        xr = tag(xr, "SOUND IN BURGER", C_ORANGE);   /* (a bar there goes first) */
    if (!ui.perform && perform_lit()) {      /* effects of PERFORM's are on with SEL let go (LATCH): never silently */
        const uint32_t m = perform_lit();
        uint32_t k2, cnt = 0, first = 0;
        t[0] = 0;
        for (k2 = 0; k2 < NPK; k2++)
            if (m >> k2 & 1u) {
                if (!cnt++)
                    first = k2;
            }
        text_cat(t, PK_NAME[first][0]);
        if (PK_NAME[first][1][0]) {
            text_cat(t, " ");
            text_cat(t, PK_NAME[first][1]);
        }
        if (cnt > 1u) {
            char c2[4];
            text_cat(t, " +");
            int_text((int32_t)cnt - 1, c2);
            text_cat(t, c2);
        }
        tag(xr, t, C_WHITE);                 /* (right, as SOUND IN BURGER is; with that one up, left of it) */
    }
    cv_rect(0, 27, 240, 1, C_LINE);
    cv_blit(0, 0);
}

static void draw_tracks(void)                /* the play page: the eight tracks, the selected one bright */
{
    uint32_t half, r;
    char t[28], n[4];
    for (half = 0; half < 2u; half++) {
        cv_begin(240, 80, C_BLACK);
        for (r = 0; r < 4u; r++) {
            uint32_t k = half * 4u + r, sel = k == ui.sel;
            const sound_t *s = list_sound(trk[k].sound);
            int32_t y = (int32_t)r * 20;
            if (sel)
                cv_rect(0, y, 240, 19, C_ORANGE);
            trk_text(k, n);
            if (k == lp.trk && lp.own) {         /* the looper's track: T8L or T7L, and what its loop holds */
                char c[8];
                c[0] = 'T';
                c[1] = (char)('1' + k);
                c[2] = 'L';
                c[3] = 0;
                cv_text(6, y + 2, &FONT_B, c, sel ? C_BLACK : C_ORANGE);
                t[0] = 0;
                text_cat(t, "LOOP ");
                if (lp.has) {
                    int_text(lp.layers, c);
                    text_cat(t, c);
                    text_cat(t, lp.layers == 1u ? " LAYER" : " LAYERS");
                } else {
                    text_cat(t, "(EMPTY)");
                }
                cv_text(40, y + 2, sel ? &FONT_B : &FONT_S, t, sel ? C_BLACK : lp.has ? C_WHITE : C_DIM);
                if (lp.has) {
                    int32_t w = (int32_t)lp.lvl * 30 / 100;
                    cv_rect(204, y + 8, 30, 3, sel ? C_BLACK : C_LINE);
                    cv_rect(204, y + 8, w, 3, sel ? C_WHITE : C_AMB);
                }
                continue;
            }
            cv_text(6, y + 2, &FONT_B, n, sel ? C_BLACK : C_GRAY);
            list_name(trk[k].sound, t);
            cv_text(40, y + 2, sel ? &FONT_B : &FONT_S, t, sel ? C_BLACK : s ? C_WHITE : C_DIM);
            if (s) {                         /* its level, a short bar at the right */
                int32_t w = (int32_t)trk[k].level * 30 / 100;
                cv_rect(204, y + 8, 30, 3, sel ? C_BLACK : C_LINE);
                cv_rect(204, y + 8, w, 3, sel ? C_WHITE : C_AMB);
            }
        }
        cv_blit(0, 28 + half * 80u);
    }
}

static void draw_steps(void)                 /* the step page: the selected track's 16 steps, two rows of eight */
{
    uint32_t i, drum;
    char t[28], n[8];
    const sound_t *s = list_sound(trk[ui.sel].sound);
    drum = s && (s->flags & SF_DRUM);
    cv_begin(240, 80, C_BLACK);
    trk_text(ui.sel, n);
    cv_text(6, 4, &FONT_B, n, C_ORANGE);
    list_name(trk[ui.sel].sound, t);
    cv_text(40, 4, &FONT_B, t, s ? C_WHITE : C_DIM);
    t[0] = 0;                                /* (the pattern's steps, as the time signature has them) */
    text_cat(t, SIG_NAME[seq_sig]);
    text_cat(t, ": WHITE KEYS 1-");
    int_text(seq_len, n);
    text_cat(t, n);
    text_cat(t, ", STEP ON / OFF");
    cv_text(6, 28, &FONT_XS, s ? t : "EMPTY TRACK: PICK A SOUND FIRST", C_GRAY);
    cv_text(6, 46, &FONT_XS, drum ? "A DRUM PLAYS ITS OWN PITCH" : "HOLD A STEP + KNOB 1: ITS NOTE", C_GRAY);
    cv_blit(0, 28);
    cv_begin(240, 80, C_BLACK);
    for (i = 0; i < seq_len; i++) {
        int32_t x = 4 + (int32_t)(i % 8u) * 29, y = 2 + (int32_t)(i / 8u) * 38;
        uint32_t on = step_on[ui.sel][i], now = seq_play_req && seq_step_now == i, held = ui.held_step == (int8_t)i;
        uint16_t c = on ? C_ORANGE : C_LINE;
        if (SIG_BEATS[seq_sig] >> i & 1u)        /* a beat starts here: a mark under the step */
            cv_rect(x, y + 35, 27, 2, C_WHITE);
        cv_rect(x, y, 27, 34, held ? C_WHITE : now ? C_HI : c);
        if (!on || held || now)
            cv_rect(x + 2, y + 2, 23, 30, on ? C_ORANGE : C_BLACK);
        int_text((int32_t)i + 1, n);
        cv_text(x + 4, y + 3, &FONT_XS, n, on ? C_BLACK : C_DIM);
        if (on && !drum) {
            note_name(step_note[ui.sel][i], n);
            cv_text(x + 3, y + 17, &FONT_XS, n, C_BLACK);
        }
    }
    cv_blit(0, 108);
}

static int hint_line(const char *s)          /* the bottom line's hint, or a message put there (say_t8l over a take: */
{                                            /* 1, it has the line to itself) */
    if (ui.msg_low && plat_ms() < ui.msg_until) {
        cv_text(cv_text(4, 40, &FONT_T, ui.msg[0], C_WHITE) + 4, 40, &FONT_T, ui.msg[1], C_HI);
        return 1;
    }
    cv_text(4, 40, &FONT_T, s, C_DIM);
    return 0;
}

static void draw_knobs(void)                 /* the four knobs of the selected track, and what the controls do */
{
    static const char *const NAME[4] = {"PITCH", "FILTER", "DECAY", "LEVEL"};
    const track_t *t = &trk[ui.sel];
    uint32_t k, now = plat_ms();
    char v[12];
    HOST_DROP(host_corner);
    cv_begin(240, 52, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    if (ui.page >= PAGE_EDIT) {              /* the EDIT, ARP, GLO and EQ pages: their four knobs and their values */
        static const char *const N4[5][4] = {{"START", "REVERSE", "LOOP", "LENGTH"}, {"RATE", "MODE", "OCTAVE", "GATE"},
                                             {"SWING", "FIT", "TIME SIG", "TEMPO"}, {"BLUETOOTH", "", "", "LATCH"},
                                             {"LOW", "MID", "MID FREQ", "HIGH"}};
        static const char *const BACK[5] = {"EDIT: PAGE 2", "ARP: BACK TO THE TRACKS", "EDIT: BACK TO THE TRACKS",
                                            "GLO: BACK   REC: DISCONNECT", "FX: BACK TO THE TRACKS"};
        const uint32_t pg = PG4(ui.page);
        char w4[4][28];
        page_words(pg, w4, 0);
        for (k = 0; k < 4u; k++) {
            int32_t x = 4 + (int32_t)k * 59;
            int hot = ui.hot == (int8_t)k && now - ui.hot_t < 900u;
            cv_text(x, 3, &FONT_T, N4[pg][k], hot ? C_WHITE : C_GRAY);
            cv_text(x, 16, &FONT_XS, w4[k], hot ? C_WHITE : C_HI);
        }
        hint_line(BACK[pg]);
        cv_blit(0, 188);
        return;
    }
    if (ui.page == PAGE_LFO) {               /* the LFO's page: its four knobs and their values */
        static const char *const LN[4] = {"AMOUNT", "RATE", "TARGET", "SHAPE"};
        for (k = 0; k < 4u; k++) {
            int32_t x = 4 + (int32_t)k * 59;
            int hot = ui.hot == (int8_t)k && now - ui.hot_t < 900u;
            v[0] = 0;
            if (k == 0u && !t->lfo_a)
                text_cat(v, "OFF");
            else if (k == 0u)
                int_text(t->lfo_a, v);
            else
                text_cat(v, k == 1u ? LFO_R_NAME[t->lfo_r] : k == 2u ? LFO_T_NAME[t->lfo_t] : LFO_S_NAME[t->lfo_s]);
            cv_text(x, 3, &FONT_T, LN[k], hot ? C_WHITE : C_GRAY);
            cv_text(x, 16, k == 0u ? &FONT_B : &FONT_XS, v, hot ? C_WHITE : C_HI);
        }
        hint_line("LFO: BACK TO THE TRACKS");
        cv_blit(0, 188);
        return;
    }
    if (ui.page == PAGE_ENV) {               /* the envelope's page: its four knobs and their values */
        static const char *const EN[4] = {"ATTACK", "RELEASE", "SUSTAIN", "VELOCITY"};
        for (k = 0; k < 4u; k++) {
            int32_t x = 4 + (int32_t)k * 59;
            int hot = ui.hot == (int8_t)k && now - ui.hot_t < 900u;
            v[0] = 0;
            if (k == 1u && t->env_r < 0)
                text_cat(v, "AUTO");
            else
                int_text(k == 0u ? t->env_a : k == 1u ? t->env_r : k == 2u ? t->env_s : t->env_v, v);
            cv_text(x, 3, &FONT_T, EN[k], hot ? C_WHITE : C_GRAY);
            cv_text(x, 16, &FONT_B, v, hot ? C_WHITE : C_HI);
        }
        hint_line("ENV: BACK TO THE TRACKS");
        cv_blit(0, 188);
        return;
    }
    if (ui.page == PAGE_FX) {                /* the effects' page: what its four knobs are */
        static const char *const FN[4] = {"K1 EFFECT", "K2 KNOB", "K3 EFFECT", "K4 KNOB"};
        for (k = 0; k < 4u; k++) {
            int32_t x = 4 + (int32_t)k * 59;
            int hot = ui.hot == (int8_t)k && now - ui.hot_t < 900u;
            cv_text(x, 3, &FONT_T, FN[k], hot ? C_WHITE : C_GRAY);
            cv_text(x, 16, &FONT_B, k < 2u ? "A" : "B", hot ? C_WHITE : C_HI);
        }
        hint_line("FX: PAGE 2 (EQ)");
        cv_blit(0, 188);
        return;
    }
    for (k = 0; k < 4u; k++) {
        int32_t x = 4 + (int32_t)k * 59;
        int hot = ui.hot == (int8_t)k && now - ui.hot_t < 900u;
        v[0] = 0;
        if (k == 0u) {
            if (t->pitch > 0)
                text_cat(v, "+");
            int_text(t->pitch, v + (t->pitch > 0 ? 1 : 0));
        } else if (k == 1u) {
            filt_text(t->filt, v);
        } else {
            int_text(k == 2u ? t->decay : *lvl_of(ui.sel), v);
        }
        cv_text(x, 3, &FONT_XS, NAME[k], C_GRAY);
        cv_text(x, 16, &FONT_B, v, hot ? C_WHITE : C_HI);
        if (k == 1u) {                       /* FILTER: where it stands, the off zone marked in the middle */
            int32_t c = x + 27, p = c + (int32_t)t->filt * 26 / FILT_MAX;
            cv_rect(x, 35, 55, 1, C_DIM);
            cv_rect(c - 2, 33, 5, 5, C_LINE);
            cv_rect(p - 1, 32, 3, 7, t->filt >= -FILT_DEAD && t->filt <= FILT_DEAD ? C_GRAY : C_ORANGE);
        }
    }
    if (!hint_line(lp.own ? "REC: LAYER  HELD 3 S: UNDO  5 S: CLEAR" : "ALGO: TRACK  PRESETS: SOUND")) {
        char c[20];                          /* where the track's sound is in the list: its category and place there;
                                              * the loop's track: LOOP and its length (its own sound sleeps) */
        if (on_t8l()) {
            c[0] = 0;
            text_cat(c, lp.has ? "LOOP " : "LOOP");
            if (lp.has)
                bars_cat(c);
        } else {
            cat_text(t->sound, c);
        }
        HOST_KEEP(host_corner, c);
        cv_text(236 - text_w(&FONT_T, c), 40, &FONT_T, c, C_GRAY);
    }
    cv_blit(0, 188);
}

/* the effects' page: the selected track's two slots, each its effect and where its knob stands */
static void draw_fx(void)
{
    uint32_t q;
    char t[28], n[8];
    const track_t *tr = &trk[ui.sel];
    for (q = 0; q < 2u; q++) {
        uint32_t type = tr->fx[q] < NFX ? tr->fx[q] : FX_OFF, side;
        int32_t kv = tr->fxk[q], c = 120, p = c + kv * 104 / FX_MAX;
        float a = fx_amount(kv, &side);
        cv_begin(240, 80, C_BLACK);
        if (!q) {                                /* whose effects these are */
            trk_text(ui.sel, n);
            cv_text(6, 2, &FONT_B, n, C_ORANGE);
            list_name(tr->sound, t);
            cv_text(40, 2, &FONT_S, t, C_WHITE);
        }
        cv_text(6, 24, &FONT_M, q ? "B" : "A", type ? C_ORANGE : C_DIM);
        cv_text(30, 28, &FONT_B, FX_NAME[type], type ? C_WHITE : C_DIM);
        t[0] = 0;                                /* the knob in words: OFF, or its side and how far */
        if (!type || a <= 0.0f) {
            text_cat(t, "OFF");
        } else {
            text_cat(t, FX_SIDE[type][side]);
            text_cat(t, " ");
            int_text((int32_t)(a * 50.0f + 0.5f), n);
            text_cat(t, n);
        }
        cv_text(234 - text_w(&FONT_B, t), 28, &FONT_B, t, type && a > 0.0f ? C_HI : C_DIM);
        if (type) {                              /* the knob's line: the two sides named, the off zone in the middle */
            cv_text(8, 50, &FONT_T, FX_SIDE[type][0], C_GRAY);
            cv_text(232 - text_w(&FONT_T, FX_SIDE[type][1]), 50, &FONT_T, FX_SIDE[type][1], C_GRAY);
            cv_rect(16, 66, 208, 1, C_DIM);
            cv_rect(c - 7, 63, 14, 7, C_LINE);
            if (a > 0.0f)
                cv_rect(kv < 0 ? p : c, 65, kv < 0 ? c - p : p - c, 3, C_ORANGE);
            cv_rect(p - 2, 60, 4, 13, a > 0.0f ? C_WHITE : C_GRAY);
        }
        cv_blit(0, 28 + q * 80u);
    }
}

/* the envelope's page: the selected track's ATTACK, RELEASE, SUSTAIN and VELOCITY, each its name, its value (in
 * words where a number alone would not say it) and a line that shows where the knob stands */
static void draw_env(void)
{
    static const char *const NAME[4] = {"ATTACK", "RELEASE", "SUSTAIN", "VELOCITY"};
    const track_t *tr = &trk[ui.sel];
    const sound_t *snd = list_sound(tr->sound);
    uint32_t q, hit = snd && (snd->flags & SF_DRUM);
    char t[28], n[8];
    for (q = 0; q < 4u; q++) {
        int32_t val = q == 0u ? tr->env_a : q == 1u ? tr->env_r : q == 2u ? tr->env_s : tr->env_v;
        int32_t y = q < 2u ? 22 + (int32_t)q * 29 : 2 + (int32_t)(q - 2u) * 29;
        int32_t lo = q == 1u ? ENV_AUTO : 0, w = (val - lo) * 226 / (100 - lo);
        uint32_t off = q == 2u && hit;           /* (a hit is never held: SUSTAIN has nothing to hold) */
        int hot = ui.hot == (int8_t)q && plat_ms() - ui.hot_t < 900u;
        if (!(q & 1u)) {
            cv_begin(240, 80, C_BLACK);
            if (!q) {                            /* whose envelope this is */
                trk_text(ui.sel, n);
                cv_text(6, 2, &FONT_B, n, C_ORANGE);
                list_name(tr->sound, t);
                cv_text(40, 2, &FONT_S, t, C_WHITE);
            }
        }
        cv_text(6, y, &FONT_B, NAME[q], off ? C_DIM : hot ? C_ORANGE : C_WHITE);
        t[0] = 0;
        if (off) {
            text_cat(t, "A HIT: NOT USED");
        } else if (q == 1u && val < 0) {
            text_cat(t, hit ? "AUTO: PLAYS THROUGH" : "AUTO: AS DECAY");
        } else {
            int_text(val, t);
            if (q == 0u && !val)
                text_cat(t, " AT ONCE");
            else if (q == 1u && !val)
                text_cat(t, " STOPS AT ONCE");
            else if (q == 3u && !val)
                text_cat(t, " ALL THE SAME");
            else if (q == 3u && val == 100)
                text_cat(t, " BY THE FINGER");
        }
        cv_text(234 - text_w(&FONT_XS, t), y + 1, &FONT_XS, t, off ? C_DIM : C_HI);
        cv_rect(6, y + 20, 228, 1, C_DIM);       /* the knob's line */
        if (!off) {
            cv_rect(6, y + 19, w, 3, C_ORANGE);
            cv_rect(6 + w - 1, y + 16, 3, 9, C_WHITE);
        }
        if (q & 1u)
            cv_blit(0, 28 + (q >> 1) * 80u);
    }
}

/* the LFO's page: the selected track's AMOUNT, RATE, TARGET and SHAPE, each its name, its value in words and a line
 * that shows where the knob stands */
static void draw_lfo(void)
{
    static const char *const NAME[4] = {"AMOUNT", "RATE", "TARGET", "SHAPE"};
    const track_t *tr = &trk[ui.sel];
    uint32_t q;
    char t[28], n[8];
    for (q = 0; q < 4u; q++) {
        int32_t val = q == 0u ? tr->lfo_a : q == 1u ? tr->lfo_r : q == 2u ? tr->lfo_t : tr->lfo_s;
        int32_t hi = q == 0u ? 100 : q == 1u ? (int32_t)NLFO_R - 1 : q == 2u ? (int32_t)NLFO_T - 1 : (int32_t)NLFO_S - 1;
        int32_t y = q < 2u ? 22 + (int32_t)q * 29 : 2 + (int32_t)(q - 2u) * 29, w = val * 226 / hi;
        uint32_t off = q != 0u && !tr->lfo_a;    /* (AMOUNT at 0: the LFO is off, the rest waits) */
        int hot = ui.hot == (int8_t)q && plat_ms() - ui.hot_t < 900u;
        if (!(q & 1u)) {
            cv_begin(240, 80, C_BLACK);
            if (!q) {                            /* whose LFO this is */
                trk_text(ui.sel, n);
                cv_text(6, 2, &FONT_B, n, C_ORANGE);
                list_name(tr->sound, t);
                cv_text(40, 2, &FONT_S, t, C_WHITE);
            }
        }
        cv_text(6, y, &FONT_B, NAME[q], hot ? C_ORANGE : off ? C_GRAY : C_WHITE);
        t[0] = 0;
        if (q == 0u && !val) {
            text_cat(t, "OFF");
        } else if (q == 0u) {
            int_text(val, t);
        } else if (q == 1u) {
            text_cat(t, LFO_R_NAME[val]);
        } else if (q == 2u) {
            text_cat(t, LFO_T_NAME[val]);
            text_cat(t, ": ");
            text_cat(t, LFO_T_WORD[val]);
        } else {
            text_cat(t, LFO_S_NAME[val]);
        }
        cv_text(234 - text_w(&FONT_XS, t), y + 1, &FONT_XS, t, off ? C_GRAY : C_HI);
        cv_rect(6, y + 20, 228, 1, C_DIM);       /* the knob's line */
        cv_rect(6, y + 19, w, 3, off ? C_GRAY : C_ORANGE);
        cv_rect(6 + w - 1, y + 16, 3, 9, off ? C_GRAY : C_WHITE);
        if (q & 1u)
            cv_blit(0, 28 + (q >> 1) * 80u);
    }
}

/* the EDIT and ARP pages: as the ENV and LFO pages, the selected track's four values, each its name, its value in
 * words and a line that shows where the knob stands */
static void draw_page4(uint32_t pg)
{
    static const char *const NAME[5][4] = {{"START", "REVERSE", "LOOP", "LENGTH"}, {"RATE", "MODE", "OCTAVE", "GATE"},
                                           {"SWING", "FIT", "TIME SIG", "TEMPO"}, {"BLUETOOTH", "STATUS", "DISCONNECT", "PERFORM LATCH"},
                                           {"EQ LOW", "EQ MID", "MID FREQ", "EQ HIGH"}};
    const track_t *tr = &trk[ui.sel];
    const sound_t *snd = list_sound(tr->sound);
    const uint32_t t8 = !pg && on_t8l();
    uint32_t q;
    int32_t pos[4];
    char w[4][28], t[28], n[8];
    page_words(pg, w, pos);
    if (!pg) {                               /* (in words what the number alone would not say) */
        if (!tr->ed_start)
            text_cat(w[0], " FROM ITS START");
        if (tr->ed_loop == ED_LOOP_AUTO)
            text_cat(w[2], !snd ? "" : (snd->flags & SF_LOOP) ? ": ITS OWN LOOP" :      /* (since 4.03: a held key */
                           (snd->flags & SF_DRUM) || tr->ed_rev ? ": ONCE" : ": HOLDS WHILE HELD");   /* holds) */
        else if (tr->ed_loop == ED_LOOP_ON)
            text_cat(w[2], snd && (snd->flags & SF_LOOP) ? ": ITS OWN LOOP" : ": START TO LENGTH");
        if (tr->ed_len == 100u)
            text_cat(w[3], " TO ITS END");
    } else if (pg == 1u && !tr->arp_r) {
        w[0][0] = 0;
        text_cat(w[0], "OFF: THE KEYS AS THEY ARE");
    } else if (pg == 2u) {                   /* (which are the track's and which are everyone's) */
        text_cat(w[0], seq_swing == 50u ? " STRAIGHT (ALL TRACKS)" : " (ALL TRACKS)");
        if (on_t8l()) {
            w[1][0] = 0;
            text_cat(w[1], "LOOP: NOTHING TO FIT");
        } else if (tr->fit) {
            text_cat(w[1], " (THIS TRACK)");
        } else {
            text_cat(w[1], ": AS THE SOUND IS");
        }
        text_cat(w[2], " (ALL TRACKS)");
        text_cat(w[3], ck_follow ? " FROM THE CLOCK IN" : " BPM (ALL TRACKS)");
    } else if (pg == 4u) {                   /* (0 dB is no EQ at all) */
        for (q = 0; q < 4u; q++)
            if (q != 2u && !tr->eq[q])
                text_cat(w[q], ": OFF");
    } else if (pg == 3u) {
        text_cat(w[1], !bt_on ? "OFF" : plat_ble_connected() ? "CONNECTED" : plat_ble_linked() ? "CONNECTING" : "WAITING");
        text_cat(w[2], "PRESS REC");
    }
    for (q = 0; q < 4u; q++) {
        int32_t y = q < 2u ? 22 + (int32_t)q * 29 : 2 + (int32_t)(q - 2u) * 29;
        uint32_t off = t8 || (pg == 1u && q != 0u && !tr->arp_r) ||     /* (the ARP off: the rest waits) */
                       (pg == 2u && q == 3u && ck_follow) || (pg == 3u && q == 2u && !plat_ble_linked()) ||
                       (pg == 4u && q == 2u && !tr->eq[1]);  /* (MID off: its FREQ waits) */
        int hot = ui.hot == (int8_t)q && plat_ms() - ui.hot_t < 900u;
        if (!(q & 1u)) {
            cv_begin(240, 80, C_BLACK);
            if (!q) {                            /* whose page this is */
                trk_text(ui.sel, n);
                t[0] = 0;
                if (pg == 3u)
                    text_cat(t, "THE WHOLE FM-1");
                else if (t8)
                    text_cat(t, "THE LOOP: NOTHING TO EDIT");
                else
                    list_name(tr->sound, t);
                if (pg == 3u)
                    cv_text(6, 2, &FONT_B, "GLO", C_ORANGE);
                else
                    cv_text(6, 2, &FONT_B, n, C_ORANGE);
                cv_text(40, 2, &FONT_S, t, C_WHITE);
            }
        }
        cv_text(6, y, &FONT_B, NAME[pg][q], hot ? C_ORANGE : off ? C_GRAY : C_WHITE);
        cv_text(234 - text_w(&FONT_XS, w[q]), y + 1, &FONT_XS, w[q], off ? C_GRAY : C_HI);
        if (pg != 3u || !q || q == 3u) {
            cv_rect(6, y + 20, 228, 1, C_DIM);   /* the knob's line */
            cv_rect(6, y + 19, pos[q], 3, off ? C_GRAY : C_ORANGE);
            cv_rect(6 + (pos[q] ? pos[q] - 1 : 0), y + 16, 3, 9, off ? C_GRAY : C_WHITE);
        }
        if (q & 1u)
            cv_blit(0, 28 + (q >> 1) * 80u);
    }
}

/* PERFORM's overlay (SEL held): the sixteen white keys, left to right, four a row, each with its effect; the ones on
 * lit, the ones that wait for a take dim */
static void draw_perform(void)
{
    const uint32_t lit = perform_lit(), take = perform_take();
    uint32_t k;
    char n[4];
    for (k = 0; k < NPK; k++) {
        const int32_t x = 2 + (int32_t)(k % 4u) * 59, y = 2 + (int32_t)((k / 4u) % 2u) * 39;
        const uint32_t on = lit >> k & 1u, dim = (PK_NOTAKE >> k & 1u) && take;
        if (!(k % 8u))
            cv_begin(240, 80, C_BLACK);
        cv_rect(x, y, 57, 36, on ? C_ORANGE : dim ? C_LINE : C_GRAY);
        if (!on)
            cv_rect(x + 1, y + 1, 55, 34, C_BLACK);
        int_text((int32_t)k + 1, n);
        cv_text(x + 3, y + 1, &FONT_T, n, on ? C_BLACK : C_DIM);
        cv_text(x + 12, y + 3, &FONT_XS, PK_NAME[k][0], on ? C_BLACK : dim ? C_DIM : C_WHITE);
        cv_text(x + 12, y + 18, &FONT_XS, PK_NAME[k][1], on ? C_BLACK : dim ? C_DIM : C_HI);
        if (k % 8u == 7u)
            cv_blit(0, 28u + (k / 8u) * 80u);
    }
}

#define TITLE_MS 2000u                   /* the title after a start: the name and the version, large */
/* the cover: psychedelic mushrooms drawn in characters on an olive dot grid, as on STACK's cover (since 4.11-beta;
 * before, record crates): title_data.h, a cell of 6 x 9 px each. The caps' letters and outline in rings of the rainbow
 * from each cap's middle, the spots cream, the stems STACK's words. y0: the band's top in the picture (240 x 160) */
#include "title_data.h"
static void cover_band(int32_t y0)
{
    static const uint8_t BOW[7][3] = {{255, 60, 60}, {255, 150, 40}, {250, 230, 50}, {70, 230, 90}, {50, 200, 255}, {90, 110, 255}, {200, 80, 255}};
    static const char WORDS[] = "clock*anything*stack*rec*play*one*loop*eight*tracks*maker*hip*";
    uint32_t i, k = 0;
    char ch[2] = {0, 0};
    for (i = 0; i < TITLE_C * TITLE_R; i++) {
        const uint32_t v = TITLE_CELLS[i >> 1] >> ((i & 1u) * 4u) & 15u;
        const int32_t c = (int32_t)(i % TITLE_C), r = (int32_t)(i / TITLE_C), x = c * 6, y = r * 9 - y0;
        const int32_t dc = c - (c < 13 ? 8 : 16), dr = r - (c < 13 ? 8 : 10);
        const uint8_t *bw = BOW[(uint32_t)(dc * dc + 3 * dr * dr) / 10u % 7u];
        uint16_t col = RGB(240, 232, 204);
        if (v == 2u || v == 4u)
            k++;                                 /* (the words run on through every band) */
        if (y < -9 || y > 80)
            continue;
        if (!v) {
            if ((uint32_t)(c * 7 + r * 3) % 5u)
                cv_rect(x + 3, y + 4, 1, 1, RGB(92, 84, 36));                /* the dot grid */
            continue;
        }
        ch[0] = v == 1u ? '#' : v == 3u ? 'o' : v == 5u ? ':' : WORDS[(k - 1u) % (sizeof WORDS - 1u)];
        if (v <= 2u)
            col = RGB(bw[0], bw[1], bw[2]);
        else if (v == 5u)
            col = RGB(64, 200, 190);
        cv_text(x, y - 3, &FONT_XS, ch, col);
    }
}

static void draw_title(void)                 /* in the tracks' place; the keys, the knobs and the header work meanwhile */
{
    const felucca_font_t *big = text_w(&FONT_L, APP_NAME) <= 122 ? &FONT_L : &FONT_M;
    cv_begin(240, 80, C_BLACK);
    cover_band(0);
    cv_text(116 + (124 - text_w(big, APP_NAME)) / 2, 40, big, APP_NAME, C_ORANGE);
    cv_blit(0, 28);
    cv_begin(240, 80, C_BLACK);
    cover_band(80);
    cv_text(116 + (124 - text_w(&FONT_M, APP_VERSION)) / 2, 8, &FONT_M, APP_VERSION, C_WHITE);
    cv_text(116 + (124 - text_w(&FONT_XS, "MAKER HIP")) / 2, 44, &FONT_XS, "MAKER HIP", C_GRAY);
    cv_blit(0, 108);
}

/* 4.14: a take on the play page: big and plain. REC and the layer, the seconds left, and the graph of what is being
 * recorded: its loudness as it comes in, along the take's way (a first layer: the memory, or its last bar once REC
 * was pressed again; a layer on top: one pass), the part gone filled, a line at each bar */
#define RW 220u
static uint8_t rec_w[RW];                    /* the graph: a column's height, 0 .. 30 */
static uint32_t rec_x;                       /* the column it had come to (those between are filled too) */
static void draw_rec(void)
{
    const uint32_t of = lp.first ? (lp.stop ? lp.want : lp.maxf) : lp.total, tk = lp.tk, bf = pat_frames();
    const uint32_t gw = lp.first ? lp.maxf : lp.total;          /* (the graph's whole width: it keeps its scale) */
    const uint32_t x = gw ? (tk < gw ? (uint32_t)((uint64_t)tk * RW / gw) : RW - 1u) : 0u;
    char t[12], c[8];
    uint32_t i, h;
    float pk = lp.pk;
    lp.pk = 0.0f;
    if (lp.state == LP_IDLE || tk < 2205u || x < rec_x) {    /* a new take: a clean graph */
        for (i = 0; i < RW; i++)
            rec_w[i] = 0;
        rec_x = x;
    }
    h = (uint32_t)(pk * 60.0f);
    h = h > 30u ? 30u : h;
    for (i = rec_x; i <= x && i < RW; i++)
        if (h > rec_w[i])
            rec_w[i] = (uint8_t)h;
    rec_x = x;
    cv_begin(240, 80, C_BLACK);                  /* REC, the layer, the seconds left */
    cv_rect(10, 14, 16, 16, (plat_ms() >> 8 & 1u) ? RGB(255, 40, 40) : RGB(120, 20, 20));
    cv_text(34, 6, &FONT_L, "REC", RGB(255, 40, 40));
    t[0] = 0;
    text_cat(t, "LAYER ");
    int_text(lp.layers + 1, c);
    text_cat(t, c);
    cv_text(34, 46, &FONT_M, t, C_WHITE);
    if (of > tk) {
        const uint32_t ds = (of - tk) / 4410u;
        int_text((int32_t)(ds / 10u), t);
        c[0] = '.';
        c[1] = (char)('0' + ds % 10u);
        c[2] = 0;
        text_cat(t, c);
        cv_text(232 - text_w(&FONT_L, t), 6, &FONT_L, t, C_ORANGE);
        cv_text(232 - text_w(&FONT_M, "S LEFT"), 46, &FONT_M, "S LEFT", C_GRAY);
    }
    cv_blit(0, 28);
    cv_begin(240, 80, C_BLACK);                  /* the graph */
    cv_rect(10, 4, RW, 64, RGB(30, 12, 12));
    cv_rect(10, 4, (int32_t)x, 64, RGB(70, 20, 20));             /* the part gone */
    for (i = 1; bf && gw && i * bf < gw; i++)    /* a line at each bar */
        cv_rect(10 + (int32_t)((uint64_t)i * bf * RW / gw), 4, 1, 64, C_GRAY);
    if (lp.first && lp.stop && gw)               /* REC pressed again: where it ends (its last bar's end) */
        cv_rect(9 + (int32_t)((uint64_t)lp.want * RW / gw), 0, 3, 72, C_ORANGE);
    for (i = 0; i < RW; i++)                     /* what was recorded, about the middle line */
        if (rec_w[i])
            cv_rect(10 + (int32_t)i, 36 - rec_w[i], 1, rec_w[i] * 2u, RGB(255, 80, 60));
    cv_rect(10, 36, RW, 1, C_LINE);
    cv_rect(10 + (int32_t)x, 0, 2, 72, C_WHITE); /* where it is */
    cv_blit(0, 108);
}

static void draw(void)
{
    draw_header();
    if (ui.perform)
        draw_perform();
    else if (ui.title)
        draw_title();
    else if (ui.page == PAGE_PLAY && LP_TAKE_LIVE)
        draw_rec();
    else if (ui.page == PAGE_FX)
        draw_fx();
    else if (ui.page == PAGE_ENV)
        draw_env();
    else if (ui.page == PAGE_LFO)
        draw_lfo();
    else if (ui.page >= PAGE_EDIT)
        draw_page4(PG4(ui.page));
    else if (ui.page == PAGE_STEP)
        draw_steps();
    else
        draw_tracks();
    draw_knobs();
}

/* a sample from the app has come in whole: it goes onto track 7 or 8 if one is empty, and the screen says where it
 * is. A track that plays something is never changed; a track that had this slot already plays the new sample */
static void my_arrived(uint32_t k)
{
    uint32_t li = NSOUNDS + 1u + k, t;
    char a[28], b[12];
    list_name(li, a);
    b[0] = 0;
    for (t = 0; t < NTRK && trk[t].sound != li; t++)
        ;
    if (t < NTRK) {
        text_cat(b, "ON T");
    } else {
        for (t = 6; t < NTRK && (trk[t].sound || (t == lp.trk && lp.own)); t++)
            ;                                    /* (track 8 as the looper is not empty to it) */
        if (t < NTRK) {
            trk[t].sound = (uint8_t)li;
            text_cat(b, "> T");
        }
    }
    if (t < NTRK) {
        b[4] = 0;
        b[b[0] == '>' ? 3 : 4] = (char)('1' + t);
        b[b[0] == '>' ? 4 : 5] = 0;
    } else {
        text_cat(b, "IN LIST");
    }
    app_say(a, b);
}

static void my_progress(void)                /* "RECEIVING MY 1  24 KB" */
{
    char a[20], b[12], n[8];
    a[0] = b[0] = 0;
    text_cat(a, "RECEIVING MY ");
    int_text((int32_t)rx.slot + 1, n);
    text_cat(a, n);
    int_text((int32_t)((rx.next - MY_DATA) / 1024u), n);
    text_cat(b, n);
    text_cat(b, " KB");
    app_say(a, b);
}

static void my_frame(void)                   /* every frame: what the app's messages left for the screen to say */
{
    char a[20], n[4];
    if (rx.on && plat_ms() - rx.t > 5000u) { /* the app stopped half way: the slot stays empty */
        rx.on = 0;
        my_failed = (uint8_t)(rx.slot + 1u);
    }
    if (my_event) {
        uint32_t k = my_event - 1u;
        my_event = 0;
        my_arrived(k);
    }
    {   /* the scan again after the start: 1 s and 3 s on, while a slot is still not taken (and nothing comes in) */
        uint32_t now = plat_ms() - my_t0, k, n_ok = 0;
        if (my_again < 2u && !rx.on && now >= (my_again ? 3000u : 1000u)) {
            for (k = 0; k < NMY; k++)
                n_ok += my[k].valid || my[k].cov;
            if (n_ok < NMY)
                my_load_all();
            song_my_place();                        /* (a saved track's sample, found now) */
            if (++my_again == 2u)
                memset(song_my, 0, sizeof song_my); /* (not there at the last look: the track stays EMPTY) */
        }
    }
    if (my_failed) {
        a[0] = 0;
        text_cat(a, "MY ");
        int_text(my_failed, n);
        text_cat(a, n);
        my_failed = 0;
        app_say(a, "NOT RECEIVED");
    }
    my_measure(3000);
}

void app_ui_init(void)
{
    my_t0 = plat_ms();
    ui.title = 1;
    ui.rec_t = 0;
    ui.rec_fired = 0;
    touch_on(1);                             /* velocity: the keys' readings on */
    plat_usb_audio_full(1);                  /* USB audio at full level, whatever MASTER says */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.held_step = -1;
    ui.hot = -1;
    ui.sv_t = ui.home_t = 0;
    ui.wake = 0;
    ui.deaf_t = 0;
    ui.perform = 0;
    ui.pn = 0;
    ui.pkeys = 0;
    ui.idle_t = plat_ms();
    city.stage = 0;
    city_on = 0;
    if (!bt_on)                              /* (the saved song says Bluetooth off) */
        plat_ble_enable(0);
    ui_sel_isr = ui.sel;
    ui.redraw = 1;
    app_say("STACK", APP_VERSION);
}

void app_frame(void)
{
    static uint32_t last_step = 255, last_play = 2;
    uint32_t now = plat_ms();
    input();
    my_frame();
    if (mute_frame())
        ui.redraw = 1;
    if (!song_drawn) {
        song_drawn = 1;
        ui.redraw = 1;
    }
    dump_frame();
    if (dump.t && now - dump.t < 700u) {     /* a dump runs: said in the header, kept up while it goes on */
        static uint32_t said;
        if (now - said > 400u || !said) {
            app_say("SENDING TO BURGER", "");
            said = now | 1u;
        }
    }
    if (pf_undo_gone == 1u) {                /* a buffer effect of PERFORM's took the block the looper's undo was in: */
        pf_undo_gone = 2;                    /* said once; kept (2) for REC's undo to say why, until a new layer's undo */
        app_say("PERFORM USED", "THE UNDO LAYER");
    } else if (lp.undo && pf_undo_gone) {
        pf_undo_gone = 0;
    }
    {
        static uint8_t take_drawn;           /* (the overlay's dim keys follow the take) */
        if (ui.perform && take_drawn != (uint8_t)perform_take())
            ui.redraw = 1;
        if (perform_take() && !take_drawn && (perform_lit() & PK_NOTAKE)) {
            /* a take begins: the effects that must wait for it go off, and stay off until their keys are pressed
             * again (a REPEAT that came back by itself after the take would take the new undo's block) */
            pf_on = (uint16_t)(pf_on & ~PK_NOTAKE);
            pf_buf = 0;
            ui.pn = 0;
            ui.redraw = 1;
        }
        take_drawn = (uint8_t)perform_take();
    }
    if (bt_again && now - bt_again >= 400u) {    /* DISCONNECT: Bluetooth on again, to be found and connected to */
        bt_again = 0;
        if (bt_on)
            plat_ble_enable(1);
    }
    {
        const uint8_t bt = (uint8_t)(plat_ble_connected() != 0);
        if (bt != ck_ble) {                  /* a Bluetooth link came or went: the header says, the clock out follows */
            ck_ble = bt;
            app_say(bt ? "BLUETOOTH:" : "BLUETOOTH:", bt ? "CONNECTED" : "GONE");
            ui.redraw = 1;
        }
    }
    if (ck_ui) {                             /* the clock: who leads now, and what the leader's transport did */
        uint32_t ev = ck_ui;
        ck_ui = 0;
        if (ev & 2u)
            app_say("CLOCK LOST:", "FM-1 LEADS");
        else if (ev & 8u)
            app_say("STOP", "");
        else if (ev & 4u)
            app_say("PLAY", "");
        else if (ev & 1u)                    /* (over Bluetooth a clock wanders: for the looper, the cable) */
            app_say(ck_src == PORT_BLE ? "BT CLOCK:" : "CLOCK:", ck_src == PORT_BLE ? "CABLE IS STEADIER" : "BURGER");
        ui.redraw = 1;
    }
    {
        static uint8_t drift_said;
        if (lp.drift && !drift_said)         /* the tempo changed under a loop: it does not stretch */
            app_say("TEMPO CHANGED:", "LOOP OUT OF TIME");
        drift_said = lp.drift;
    }
    speak();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.hot >= 0 && now - ui.hot_t >= 900u) {
        ui.hot = -1;
        ui.redraw = 1;
    }
    /* track lp.trk is the looper's while a loop is there, or a take waits or runs; else it is its own track again */
    lp.own = (uint8_t)(lp.has || lp.state != LP_IDLE || lp.fading || lp.req == LQ_ARM);
    if (!lp.own && !lp.swap)
        lp.trk = NTRK - 1u;                  /* (no loop: the next first layer is track 8's again) */
    if (ui.page == PAGE_STEP && on_t8l()) {  /* the loop came onto the track whose steps were shown: it has none to show */
        ui.page = PAGE_PLAY;
        ui.redraw = 1;
    }
    if (lp.done != ui.loop_seen && !lp.swap) {   /* a take is in, and the loop has moved (a block after) */
        char a[24];
        ui.loop_seen = lp.done;
        a[0] = 0;
        if (lp.full) {                       /* a first layer that ended by itself */
            lp.full = 0;
            text_cat(a, "LOOP FULL: ");
            bars_cat(a);
        } else {
            layer_text(a, lp.layers);
            text_cat(a, " IN");
        }
        {
            char m[] = "LOOP -> T8";         /* (where it lives from here) */
            m[9] = (char)('1' + lp.trk);
            app_say(a, m);
        }
    }
    {   /* the looper's state changed, or a take moves on: the screen follows */
        uint32_t st = lp.own | lp.has << 1 | lp.state << 2 | lp.undo << 4 | (uint32_t)lp.layers << 5 |
                      (lp.state != LP_IDLE ? (lp.tk / 2205u & 0xFFu) << 13 : 0u) |      /* (its bar: 20 times a second) */
                      (lp.first && lp.stop ? (now >> 7 & 1u) << 21 | 1u << 22 : 0u) | (uint32_t)(lp.trk & 1u) << 23;
        if (st != ui.loop_drawn) {
            ui.loop_drawn = st;
            ui.redraw = 1;
        }
    }
    if (ui.title && now - my_t0 >= TITLE_MS) {
        ui.title = 0;
        ui.redraw = 1;
    }
    if (seq_play_req != last_play || (ui.page == PAGE_STEP && seq_play_req && seq_step_now != last_step)) {
        last_play = seq_play_req;
        last_step = seq_step_now;
        ui.redraw = 1;
    }
    /* the city view comes only when HOME is held (since 4.03: never by itself); its first touch only closes it */
    if (city.stage && !city.manual && !seq_play_req)
        city_close();
    if (city.stage) {
        city_frame(now);
        return;                              /* (the page under it is drawn again when the view closes) */
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "track"))
        *got = ui.sel + 1;
    else if (!strcmp(what, "sound"))         /* the selected track's place in the list */
        *got = trk[ui.sel].sound;
    else if (!strcmp(what, "nlist"))
        *got = (int)NLIST;
    else if (!strcmp(what, "octave"))
        *got = ui.octave;
    else if (!strcmp(what, "pitch"))
        *got = trk[ui.sel].pitch;
    else if (!strcmp(what, "filt"))
        *got = trk[ui.sel].filt;
    else if (!strcmp(what, "decay"))
        *got = trk[ui.sel].decay;
    else if (!strcmp(what, "level"))         /* (the loop's track: the loop's) */
        *got = *lvl_of(ui.sel);
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "playing"))
        *got = seq_play_req;
    else if (!strcmp(what, "notes_out"))     /* messages sent over USB, the clock, Start and Stop left out */
        *got = (int)plat_midi_count(PORT_USB, 1);
    else if (!strcmp(what, "bt"))            /* a Bluetooth link listens */
        *got = (int)ck_ble;
    else if (!strcmp(what, "clk_src"))       /* following: 0 USB, 1 Bluetooth; -1 the FM-1 leads */
        *got = ck_follow ? (int)ck_src : -1;
    else if (!strcmp(what, "clk_bt_out"))    /* clock ticks, Starts and Stops sent over Bluetooth */
        *got = (int)ck_sent.ble;
    else if (!strcmp(what, "bt_out"))        /* other messages sent over Bluetooth (notes): none, ever */
        *got = (int)plat_midi_count(PORT_BLE, 1);
    else if (!strcmp(what, "clk_follow"))
        *got = (int)ck_follow;
    else if (!strcmp(what, "clk_bpm10"))     /* the tempo now, in tenths of a beat a minute */
        *got = (int)(44100.0f * 60.0f * 10.0f / (ck_tick_frames() * 24.0f) + 0.5f);
    else if (!strcmp(what, "clk_out_span")) {    /* clock out: the longest gap less the shortest since the last look, in us */
        *got = (int)((ck_sent.ivmax - ck_sent.ivmin) / 24u);
        printf("  clock out: gaps %.2f .. %.2f ms\n", (double)ck_sent.ivmin / 24000.0, (double)ck_sent.ivmax / 24000.0);
        ck_sent.ivmin = ck_sent.ivmax = 0;
    } else if (!strcmp(what, "clk_out_gap"))     /* and the longest, in us */
        *got = (int)(ck_sent.ivmax / 24u);
    else if (!strcmp(what, "clk_ticks"))     /* clock ticks sent since the start */
        *got = (int)ck_sent.clocks;
    else if (!strcmp(what, "clk_starts"))
        *got = (int)ck_sent.starts;
    else if (!strcmp(what, "clk_stops"))
        *got = (int)ck_sent.stops;
    else if (!strcmp(what, "clk_err")) {     /* following: the furthest the place was from the clock's, in 1/1000 tick */
        *got = (int)(seq_dbg.e * 1000.0f);
        seq_dbg.e = 0.0f;
    } else if (!strcmp(what, "clk_step"))    /* the step that sounds */
        *got = (int)seq_step_now;
    else if (!strcmp(what, "loop_drift"))
        *got = (int)lp.drift;
    else if (!strcmp(what, "loop_nudge")) {  /* the most the loop was sped or slowed since the last look, in millionths */
        *got = (int)(lp.nudge * 1000000.0f + 0.5f);
        lp.nudge = 0.0f;
    } else if (!strcmp(what, "loop_total"))
        *got = (int)lp.total;
    else if (!strcmp(what, "mute"))
        *got = (int)stack_mute;
    else if (!strcmp(what, "voices"))
        *got = (int)voices_on;
    else if (!strcmp(what, "lim_gr")) {      /* the limiter's deepest gain reduction since the last look, in 0.1 dB */
        *got = (int)(-200.0f * fm_log2f(lim.g_min) * 0.30103f + 0.5f);
        printf("  limiter: deepest gain reduction %.1f dB\n", (double)(-20.0f * fm_log2f(lim.g_min) * 0.30103f));
        lim.g_min = 1.0f;
    } else if (!strcmp(what, "lim_peak")) {  /* the highest sample out since the last look, in 0.1 % of full scale */
        *got = (int)(lim.peak * 1000.0f + 0.5f);
        printf("  limiter: highest sample out %.3f of full scale\n", (double)lim.peak);
        lim.peak = 0.0f;
    } else if (!strcmp(what, "lim_clips"))   /* samples the limiter let past full scale (never) */
        *got = (int)lim.clips;
    else if (!strncmp(what, "my_gain", 7)) /* my_gainN: slot N's playback gain in 0.01 (0: no sample, or not measured) */
        *got = my[(atoi(what + 7) - 1) % (int)NMY].valid && my[(atoi(what + 7) - 1) % (int)NMY].measured
                   ? (int)(my[(atoi(what + 7) - 1) % (int)NMY].gain * 100.0f + 0.5f) : 0;
    else if (!strncmp(what, "my", 2) && what[2] >= '1' && what[2] <= '3')   /* myN: slot N's zones (0 empty) */
        *got = my[what[2] - '1'].valid ? my[what[2] - '1'].nz : 0;
    else if (!strncmp(what, "my_crc", 6))    /* my_crcN: slot N's data against its CRC: 0 not checked, 1 the same, 2 not */
        *got = my[(what[6] - '1') % (int)NMY].crc;
    else if (!strncmp(what, "my_why", 6))    /* my_whyN: why slot N is not taken (0 it is; 1 blank; 2, 5 refused; 9 no flash) */
        *got = my[(what[6] - '1') % (int)NMY].why;
    else if (!strcmp(what, "my_blind")) {    /* the next scans read nothing (a flash that cannot be read yet) ... */
        host_blind = 1;
        *got = 1;
    } else if (!strcmp(what, "my_sees")) {   /* ... and from now on they read again */
        host_blind = 0;
        *got = 1;
    }
    else if (!strcmp(what, "receiving"))
        *got = rx.on;
    else if (!strncmp(what, "trk", 3) && what[3] >= '1' && what[3] <= '8')  /* trkN: track N's place in the list */
        *got = trk[what[3] - '1'].sound;
    else if (!strcmp(what, "loop_own"))      /* the looper: its track is its, a loop there, its layers, */
        *got = lp.own;
    else if (!strcmp(what, "loop_trk"))      /* the track it lives on (or the next first layer will): 8 or 7 */
        *got = lp.trk + 1;
    else if (!strcmp(what, "loop_off"))      /* frames into the loop where its first bar begins */
        *got = (int)lp.off;
    else if (!strncmp(what, "sounded", 7)) { /* soundedN: track N's own voices sounded since the last look */
        *got = (int)(host_sounded >> ((what[7] - '1') & 7) & 1u);
        host_sounded &= ~(1u << ((what[7] - '1') & 7));
    }
    else if (!strcmp(what, "loop_has"))
        *got = lp.has;
    else if (!strcmp(what, "loop_seam") || !strcmp(what, "loop_tailseam")) {   /* (loop_tailseam: across where the last
                                              * take's tail began, a layer's end) */   /* 4.14: the loop's level across its seam (its last 1 ms and first 1 ms) in
                                              * 1/1000 of its whole level: about 1000 when the sound runs on round it */
        const uint32_t len = lp.total / 2u;
        lcodec_t c;
        double all = 0.0, seam = 0.0;
        uint32_t k;
        c.m = LOOP_B(lp.cur);
        c.k = 0;
        c.pred = c.idx = 0;
        for (k = 0; k < len; k++) {
            const double v = (double)lc_dec(&c, len);
            all += v * v;
            if (what[5] == 't' ? (k + 22u >= lp.tend && k < lp.tend + 22u) || (lp.tend < 22u && k + 22u >= len + lp.tend)
                               : k < 22u || k + 22u >= len)
                seam += v * v;
        }
        *got = len > 44u && all > 0.0 ? (int)(1000.0 * sqrt((seam / 44.0) / (all / (double)len))) : 0;
    } else if (!strcmp(what, "loop_layers"))
        *got = lp.layers;
    else if (!strncmp(what, "msg_", 4) || !strncmp(what, "scr_", 4)) {
        /* msg_TEXT: the header's message now has TEXT in it ('_' a space): 1, else 0. scr_TEXT: the same of what is
         * drawn: REC's hold line (held half a second on), the bottom line's corner (the track page) and a message
         * there (say_t8l over a take; not the header's) */
        char t[sizeof host_hold + sizeof host_corner + sizeof ui.msg + 8u], w[48];
        uint32_t i;
        const int msg = what[0] == 'm';
        for (i = 0; what[4u + i] && i < 47u; i++)
            w[i] = what[4u + i] == '_' ? ' ' : what[4u + i];
        w[i] = 0;
        t[0] = 0;
        if (msg && plat_ms() < ui.msg_until && !ui.msg_low) {
            text_cat(t, ui.msg[0]);
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        } else if (!msg) {
            text_cat(t, ui.rec_t && !ui.rec_fired ? host_hold : "");    /* (as drawn: cleared where each is drawn) */
            text_cat(t, " | ");
            text_cat(t, host_corner);
            if (ui.msg_low && plat_ms() < ui.msg_until) {   /* (a message on the bottom line: say_t8l over a take) */
                text_cat(t, " | ");
                text_cat(t, ui.msg[0]);
                text_cat(t, " ");
                text_cat(t, ui.msg[1]);
            }
        }
        *got = t[0] && strstr(t, w) != 0;
        printf(msg ? "  the header says: %s\n" : "  the screen says: %s\n", t[0] ? t : "(nothing)");
    }
    else if (!strcmp(what, "loop_undo"))     /* a layer can be taken off, */
        *got = lp.undo;
    else if (!strcmp(what, "loop_state"))    /* 0 idle, 1 waiting for the bar, 2 recording, */
        *got = lp.state;
    else if (!strcmp(what, "loop_khz"))      /* and its rate: 22 or 11 */
        *got = lp.div == 4u ? 11 : 22;
    else if (!strcmp(what, "loop_bars"))     /* the bars it is long */
        *got = lp.bars;
    else if (!strncmp(what, "loop_db", 7)) { /* loop_dbA_B: the loop's loudest from A to B ms after its first sample, in dB */
        unsigned a = 0, b = 0;
        uint32_t len = lp.has ? lp.total / 2u : 0u, k;
        int32_t pk = 0, v;
        lcodec_t c = {LOOP_B(lp.cur), 0, 0, 0};
        sscanf(what + 7, "%u_%u", &a, &b);
        for (k = 0; k < len; k++) {
            v = lc_dec(&c, len);
            v = v < 0 ? -v : v;
            if (k * 1000u >= a * 22050u && k * 1000u < b * 22050u && v > pk)
                pk = v;
        }
        *got = pk ? (int)(6.0206f * fm_log2f((float)pk / 32768.0f) - 0.5f) : -99;
        printf("  loop: loudest %d dBFS from %u to %u ms\n", *got, a, b);
    }
    else if (!strcmp(what, "env_a"))         /* the selected track's ATTACK, RELEASE (-1 AUTO), SUSTAIN, VELOCITY */
        *got = trk[ui.sel].env_a;
    else if (!strcmp(what, "env_r"))
        *got = trk[ui.sel].env_r;
    else if (!strcmp(what, "env_s"))
        *got = trk[ui.sel].env_s;
    else if (!strcmp(what, "env_v"))
        *got = trk[ui.sel].env_v;
    else if (!strcmp(what, "v_ni") || !strcmp(what, "v_ed")) {   /* the first sounding voice: its next sample, its ED_* */
        uint32_t k;
        *got = -1;
        for (k = 0; k < NVOICE && voice[k].state == V_OFF; k++)
            ;
        if (k < NVOICE)
            *got = what[2] == 'n' ? (int)voice[k].ni : (int)voice[k].ed;
    }
    else if (!strcmp(what, "dump_sent"))     /* the dump: answers sent, answers that had to wait for room, one waiting now */
        *got = (int)dump.sent;
    else if (!strcmp(what, "dump_held"))
        *got = (int)dump.held;
    else if (!strcmp(what, "dump_waits"))
        *got = dump.n != 0u;
    else if (!strcmp(what, "dump_loop_ok"))  /* the loop's pieces as they went, in order, are the loop: their CRC is its CRC */
        *got = lp.has && dump.next && dump.crc == dump_loop_crc(lp.total / 2u);
    else if (!strcmp(what, "dump_pieces"))   /* the loop's pieces sent since piece 0 */
        *got = (int)dump.next;
    else if (!strcmp(what, "loop_ver"))
        *got = (int)(lp.ver & 0x3FFFu);
    else if (!strcmp(what, "perform"))       /* PERFORM: SEL is held */
        *got = ui.perform;
    else if (!strcmp(what, "pf_lit"))        /* its effects that are on: a bit each, key 1 the lowest */
        *got = (int)perform_lit();
    else if (!strcmp(what, "pf_buf"))        /* the buffer effect that plays: 0 none, else its key 1 .. 5 */
        *got = pf_buf;
    else if (!strcmp(what, "pf_ph"))         /* the buffer: 0 waits, 1 records, 2 plays */
        *got = pb.cur ? pb.ph : -1;
    else if (!strcmp(what, "pf_run"))
        *got = pf_run;
    else if (!strcmp(what, "pf_latch"))
        *got = pf_latch;
    else if (!strcmp(what, "pf_rate"))       /* the voices' rate, in 1/1000 */
        *got = (int)(pf_rate * 1000.0f + 0.5f);
    else if (!strcmp(what, "bench_perform")) {   /* host only: as bench_mid, with PERFORM's mix effects and sends all on */
        static int32_t out[2 * APP_BLOCK];
        uint32_t i, k, save = trk[0].sound;
        clock_t t0;
        for (k = 1; k <= NSOUNDS && (SOUNDS[k - 1].flags & (SF_PCM | SF_DRUM)) != 0; k++)
            ;
        trk[0].sound = (uint8_t)k;
        pf_on = (uint16_t)(0xFFFFu & ~(PK_BUFFER | 1u << PK_TAPE | 1u << PK_OCT));
        pf_buf = 1u + PK_REP16;
        t0 = clock();
        for (i = 0; i < 4000u; i++) {
            for (k = 0; k < NVOICE; k++)
                if (voice[k].state == V_OFF)
                    voice_start(0, 60, 127, 0, 0, 0);
            render_part(out, APP_BLOCK, 8300000.0f);
        }
        *got = (int)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 4000.0 * 10.0);
        printf("  bench_perform: %.1f us a block of %u frames, %u voices, REPEAT and PERFORM's nine mix and send effects on (this computer, not an FM-1)\n",
               *got / 10.0, (unsigned)APP_BLOCK, (unsigned)NVOICE);
        for (i = 0; i < NVOICE; i++)
            voice[i].state = V_OFF;
        trk[0].sound = (uint8_t)save;
        perform_off();
        fx_reset();
    }
    else if (!strcmp(what, "city"))          /* the city view: 0 closed, 1 .. 3 fading in, 4 up */
        *got = city.stage;
    else if (!strcmp(what, "city_grp"))      /* the group the selected track's sound is in: 0 KICK, SNARE, HI-HAT, BASS, MELODY, 5 SOUND; -1 none */
        *got = (int)city_group(ui.sel);
    else if (!strncmp(what, "city_g", 6) && what[6] >= '0' && what[6] < '0' + (int)NGRP && !what[7])
        *got = (int)(city.lv[what[6] - '0'] * 1000.0f);      /* city_gN: group N's lamp, its level in 1/1000 */
    else if (!strcmp(what, "city_glow"))     /* the mushroom view's rainbow: 0 still .. 4 turning fastest */
        *got = city.glow;
    else if (!strcmp(what, "city_phase"))    /* its turn, 0 .. 6 */
        *got = city.phase;
    else if (!strcmp(what, "city_flashes"))  /* white flashes since the view opened first */
        *got = (int)city.flashes;
    else if (!strcmp(what, "city_el") || !strcmp(what, "city_er"))   /* the lights lit along the left, the right kerb */
        *got = city.edge[what[6] == 'r'] == 255u ? 0 : city.edge[what[6] == 'r'];
    else if (!strcmp(what, "song_junk")) {   /* host only: a record that is not a song put where the song is kept */
        static uint8_t junk[SONG_LEN];
        uint32_t i;
        for (i = 0; i < SONG_LEN; i++)
            junk[i] = (uint8_t)(i * 37u + 11u);
        *got = plat_store_save(OBJ_SETTINGS, junk, SONG_LEN);
    } else if (!strcmp(what, "song_old")) {  /* host only: a song of a version this one does not know */
        song_save();
        song_buf[4] = 99;
        *got = plat_store_save(OBJ_SETTINGS, song_buf, SONG_LEN);
    } else if (!strcmp(what, "song_wild")) { /* host only: a song whose every setting is out of range */
        uint32_t i;
        song_save();
        for (i = 8; i < SONG_LEN; i++)
            song_buf[i] = 0xEE;
        *got = plat_store_save(OBJ_SETTINGS, song_buf, SONG_LEN);
    } else if (!strcmp(what, "bench_city")) {    /* host only: microseconds to draw the city once, and its signs once */
        clock_t t0 = clock();
        uint32_t i;
        for (i = 0; i < 50u; i++)
            city_scene(256);
        *got = (int)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 50.0);
        t0 = clock();
        for (i = 0; i < 500u; i++)
            city_lights(i);
        printf("  bench_city: the whole city %d us, its lamps and kerb lights %.1f us (this computer, not an FM-1)\n", *got,
               (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 500.0);
        ui.redraw = 1;
    }
    else if (!strcmp(what, "swing"))         /* EDIT 2: SWING, the selected track's FIT, the time signature, the pattern's steps, the tempo */
        *got = seq_swing;
    else if (!strcmp(what, "fit"))
        *got = trk[ui.sel].fit;
    else if (!strcmp(what, "sig"))
        *got = seq_sig;
    else if (!strcmp(what, "seq_len"))
        *got = seq_len;
    else if (!strcmp(what, "bt_on"))         /* GLO's BLUETOOTH */
        *got = bt_on;
    else if (!strcmp(what, "ed_start"))      /* the selected track's EDIT page: START, REVERSE, LOOP (0 AUTO, 1 OFF, 2 ON), LENGTH */
        *got = trk[ui.sel].ed_start;
    else if (!strcmp(what, "ed_rev"))
        *got = trk[ui.sel].ed_rev;
    else if (!strcmp(what, "ed_loop"))
        *got = trk[ui.sel].ed_loop;
    else if (!strcmp(what, "ed_len"))
        *got = trk[ui.sel].ed_len;
    else if (!strcmp(what, "arp_r"))         /* its ARP page: RATE (0 OFF), MODE, OCTAVE, GATE */
        *got = trk[ui.sel].arp_r;
    else if (!strcmp(what, "arp_m"))
        *got = trk[ui.sel].arp_m;
    else if (!strcmp(what, "arp_o"))
        *got = trk[ui.sel].arp_o;
    else if (!strcmp(what, "arp_g"))
        *got = trk[ui.sel].arp_g;
    else if (!strcmp(what, "arp_note"))      /* the note the ARP sounds (-1: none) */
        *got = arp.on ? (int)arp.cur : -1;
    else if (!strcmp(what, "arp_keys"))
        *got = arp.n;
    else if (!strcmp(what, "arp_steps"))     /* the steps it has played since its keys went down */
        *got = (int)arp.i;
    else if (!strcmp(what, "lfo_a"))         /* the selected track's LFO: AMOUNT, RATE, TARGET, SHAPE */
        *got = trk[ui.sel].lfo_a;
    else if (!strcmp(what, "lfo_r"))
        *got = trk[ui.sel].lfo_r;
    else if (!strcmp(what, "lfo_t"))
        *got = trk[ui.sel].lfo_t;
    else if (!strcmp(what, "lfo_s"))
        *got = trk[ui.sel].lfo_s;
    else if (!strcmp(what, "fxa") || !strcmp(what, "fxb"))       /* the selected track's effect in slot A, B */
        *got = trk[ui.sel].fx[what[2] - 'a'];
    else if (!strcmp(what, "fxak") || !strcmp(what, "fxbk"))     /* and its knob */
        *got = trk[ui.sel].fxk[what[2] - 'a'];
    else if (!strncmp(what, "eq", 2) && what[2] >= '1' && what[2] <= '4' && !what[3])   /* eqN: the EQ page's knob N */
        *got = trk[ui.sel].eq[what[2] - '1'];
    else if (!strcmp(what, "eq_runs")) {     /* the EQ's bands run since the last look, a block each (0: no EQ at all) */
        *got = (int)host_eq_runs;
        host_eq_runs = 0;
    }
    else if (!strcmp(what, "fx_awake")) {    /* the effects' buses that run now */
        uint32_t b;
        for (*got = 0, b = 0; b < NBUS; b++)
            *got += fxb[b].awake;
    }
    else if (!strcmp(what, "bus_gain"))      /* the selected track's level as it is now (LEVEL and the LFO), in 1/1000 */
        *got = (int)(bus[ui.sel].gain * 1000.0f + 0.5f);
    else if (!strcmp(what, "bus_pan"))       /* its PAN as it is now, -1000 left .. 1000 right */
        *got = (int)(bus[ui.sel].pan * 1000.0f + (bus[ui.sel].pan < 0.0f ? -0.5f : 0.5f));
    else if (!strcmp(what, "bus_lp"))        /* its low-pass as it is now: 1000 open, less the further it is closed */
        *got = (int)(bus[ui.sel].a * 1000.0f + 0.5f);
    else if (!strcmp(what, "bench_lfo")) {   /* host only: as bench_mid, with the track's LFO on PITCH (the most work) */
        static int32_t out[2 * APP_BLOCK];
        uint32_t i, k, save = trk[0].sound, sa = trk[0].lfo_a, st = trk[0].lfo_t;
        clock_t t0;
        for (k = 1; k <= NSOUNDS && (SOUNDS[k - 1].flags & (SF_PCM | SF_DRUM)) != 0; k++)
            ;
        trk[0].sound = (uint8_t)k;
        trk[0].lfo_a = 100;
        trk[0].lfo_t = LFO_PITCH;
        t0 = clock();
        for (i = 0; i < 4000u; i++) {
            for (k = 0; k < NVOICE; k++)
                if (voice[k].state == V_OFF)
                    voice_start(0, 60, 127, 0, 0, 0);
            render_part(out, APP_BLOCK, 8300000.0f);
        }
        *got = (int)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 4000.0 * 10.0);
        printf("  bench_lfo: %.1f us a block of %u frames, %u voices with the LFO on PITCH (this computer, not an FM-1)\n",
               *got / 10.0, (unsigned)APP_BLOCK, (unsigned)NVOICE);
        for (i = 0; i < NVOICE; i++)
            voice[i].state = V_OFF;
        trk[0].sound = (uint8_t)save;
        trk[0].lfo_a = (uint8_t)sa;
        trk[0].lfo_t = (uint8_t)st;
    }
    else if (!strcmp(what, "bench_fx")) {    /* host only: microseconds a block, every voice busy and every bus fed */
        static int32_t out[2 * APP_BLOCK];
        static const uint8_t TA[8] = {FX_DELAY, FX_ECHO, FX_REVERB, FX_ROBOT, FX_FLANGER, FX_CHORUS, FX_DRIVE, FX_LOFI};
        uint32_t i, k, t;
        clock_t t0;
        track_t save[NTRK];
        memcpy(save, trk, sizeof save);
        for (t = 0; t < NTRK; t++) {             /* each track: one effect to the left in A, the same to the right in B */
            trk[t].sound = (uint8_t)(35u + t);
            trk[t].fx[0] = trk[t].fx[1] = TA[t];
            trk[t].fxk[0] = -FX_MAX;
            trk[t].fxk[1] = FX_MAX;
        }
        t0 = clock();
        for (i = 0; i < 4000u; i++) {
            for (k = 0; k < NVOICE; k++)
                if (voice[k].state == V_OFF)
                    voice_start(k % NTRK, 60, 127, 0, 0, 0);
            render_part(out, APP_BLOCK, 8300000.0f);
        }
        *got = (int)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 4000.0 * 10.0);
        printf("  bench_fx: %.1f us a block of %u frames, %u voices, 12 buses and 4 track effects (this computer, not an FM-1)\n",
               *got / 10.0, (unsigned)APP_BLOCK, (unsigned)NVOICE);
        for (i = 0; i < NVOICE; i++)
            voice[i].state = V_OFF;
        memcpy(trk, save, sizeof save);
        fx_reset();
    }
    else if (!strncmp(what, "snd_lo", 6)) {  /* host only: snd_loN: built-in sound N (its place in the list) as a key
                                              * held 1 s plays it at its own pitch, its part under ~150 Hz (four
                                              * one-pole low-passes), in dB of full scale (the RMS); 4.06 */
        const uint32_t li = (uint32_t)atoi(what + 6);
        const sound_t *sd = li >= 1u && li <= NSOUNDS ? &SOUNDS[li - 1u] : 0;
        const float a = 1.0f - fm_expf(-6.2831853f * 150.0f / (sd ? sd->rate : 1.0f));
        float f1 = 0.0f, f2 = 0.0f, f3 = 0.0f, f4 = 0.0f;
        double e = 0.0;
        int32_t pred = 0, idx = 0;
        uint32_t i, k, n = sd ? (uint32_t)sd->rate : 0u;
        for (i = k = 0; sd && i < n; i++, k++) {
            int32_t v;
            if (k >= sd->n) {                    /* the end: a held sound goes round its loop, a one-shot is silent */
                if (!(sd->flags & SF_LOOP))
                    break;
                k = sd->ls;
                pred = sd->lpred;
                idx = sd->lidx;
            }
            if (sd->flags & SF_PCM) {
                v = ((const int16_t *)sd->d)[k];
            } else {                             /* IMA ADPCM, low nibble first */
                const uint32_t nb = (((const uint8_t *)sd->d)[k >> 1] >> ((k & 1u) * 4u)) & 15u;
                int32_t st = IMA_STEP[idx], df = st >> 3;
                if (nb & 4u)
                    df += st;
                if (nb & 2u)
                    df += st >> 1;
                if (nb & 1u)
                    df += st >> 2;
                pred = clamp(nb & 8u ? pred - df : pred + df, -32768, 32767);
                idx = clamp(idx + IMA_INDEX[nb & 7u], 0, 88);
                v = pred;
            }
            f1 += a * ((float)v / 32768.0f - f1);
            f2 += a * (f1 - f2);
            f3 += a * (f2 - f3);
            f4 += a * (f3 - f4);
            e += (double)f4 * f4;
        }
        *got = sd && e > 0.0 ? (int)(10.0 * log10(e / n) - 0.5) : -99;
        printf("  %s: %s, %d dBFS under ~150 Hz\n", what, sd ? sd->name : "(none)", *got);
    }
    else if (!strcmp(what, "cat"))           /* the selected track's sound's category (0 DRUMS .. 5 FX), -1 none */
        *got = list_sound(trk[ui.sel].sound) ? list_sound(trk[ui.sel].sound)->cat : -1;
    else if (!strncmp(what, "bench", 5)) {   /* host only: microseconds a block with every voice busy: "bench" three
                                              * octaves up (the decoder's most work), "bench_mid" at the sound's own pitch */
        uint32_t bn = what[5] ? 60u : 96u;
        static int32_t out[2 * APP_BLOCK];
        uint32_t i, k, save = trk[0].sound;
        clock_t t0;
        for (k = 1; k <= NSOUNDS && (SOUNDS[k - 1].flags & (SF_PCM | SF_DRUM)) != 0; k++)
            ;
        trk[0].sound = (uint8_t)k;           /* the first ADPCM sound that is not a drum */
        for (i = 0; i < NVOICE; i++)
            voice_start(0, bn, 127, 0, 0, 0);
        t0 = clock();
        for (i = 0; i < 4000u; i++) {
            for (k = 0; k < NVOICE; k++)
                if (voice[k].state == V_OFF)
                    voice_start(0, bn, 127, 0, 0, 0);
            render_part(out, APP_BLOCK, 8300000.0f);
        }
        *got = (int)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 4000.0 * 10.0);   /* 0.1 us */
        printf("  %s: %.1f us a block of %u frames, %u voices (this computer, not an FM-1)\n", what, *got / 10.0, (unsigned)APP_BLOCK, (unsigned)NVOICE);
        for (i = 0; i < NVOICE; i++)
            voice[i].state = V_OFF;
        trk[0].sound = (uint8_t)save;
    }
    else if (!strncmp(what, "step", 4))      /* stepN: step N (1..16) of the selected track, 0 off or its note */
        *got = step_on[ui.sel][(atoi(what + 4) - 1) & 15] ? step_note[ui.sel][(atoi(what + 4) - 1) & 15] : 0;
    else
        return 0;
    return 1;
}
#endif
