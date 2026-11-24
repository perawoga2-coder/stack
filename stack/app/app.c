/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip
 * Started from fm1-base's template/app.c, Copyright (C) 2026 Charles Vestal (MPL-2.0): the unity build's shape,
 * app_say, the key scan with velocity and the text helpers follow it. Nothing here comes from Felucca or from the
 * Burger Studio firmware (bs19); the sounds are audio only (app/SOUNDS.md). */
/* STACK. Eight tracks, each with one sound picked from a single list; the keys play the selected track.
 *   ALGORITHM  the track, 1..8 (it stops at the ends)
 *   PRESETS    the selected track's sound, from one list that stops at its ends; nothing else changes
 *   REC        UNDO SOUND: the sound the track had before the last turn of PRESETS (lit while there is one)
 *   KNOB 1..4  the selected track's PITCH, FILTER (left low-pass, the middle off, right high-pass), DECAY, LEVEL
 *   OCT- OCT+  the keys' octave, on release; both together are kept for the looper (not in this version)
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
} sound_t;
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

static void cat_text(uint32_t i, char *b)    /* "KEYS 3/15": the category and the sound's place in it; "MY SAMPLES" */
{
    const sound_t *s = list_sound(i);
    uint32_t k, at = 0, of = 0;
    char n[8];
    b[0] = 0;
    if (i < 1u || i > NSOUNDS) {
        text_cat(b, i ? "MY SAMPLES" : "");
        return;
    }
    for (k = 0; k < NSOUNDS; k++)
        if (SOUNDS[k].cat == s->cat) {
            of++;
            if (&SOUNDS[k] <= s)
                at++;
        }
    text_cat(b, CAT_NAME[s->cat]);
    text_cat(b, " ");
    int_text((int32_t)at, n);
    text_cat(b, n);
    text_cat(b, "/");
    int_text((int32_t)of, n);
    text_cat(b, n);
}

/* ------------------------------------------------------------------ tracks --- */
#define NTRK 8u
#define NSTEP 16u
#define FILT_DEAD 3                      /* FILTER: -FILT_DEAD .. +FILT_DEAD is off (seven detents wide) */
#define FILT_MAX (50 + FILT_DEAD)
typedef struct {
    volatile uint8_t sound;              /* its place in the list */
    volatile int8_t pitch;               /* semitones, -24 .. +24 */
    volatile int8_t filt;                /* -FILT_MAX (low-pass, closed) .. 0 off .. +FILT_MAX (high-pass) */
    volatile uint8_t decay, level;       /* 0 .. 100 */
    volatile uint8_t fx[2];              /* the effect in slot A and in slot B (fx.c FX_*) */
    volatile int8_t fxk[2];              /* their knobs, -FX_MAX .. 0 off .. +FX_MAX */
} track_t;
static track_t trk[NTRK];
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
    uint8_t my;                          /* 1 + the app's slot it plays (0: a built-in sound) */
    float gain;                          /* a sample from the app: what brings it to the built-in sounds' loudness */
    uint32_t delay;                      /* frames into this block before it starts */
    uint32_t life;                       /* a preview: frames until it lets go (0: none) */
    uint32_t age;
    uint8_t state, track, note, seq;
} voice_t;
static voice_t voice[NVOICE];
static uint32_t voice_age;
static volatile uint32_t voices_on;      /* for the screen and the tests */

/* what each track's bus keeps between blocks */
static struct {
    float lp1, lp2, hp1, hp2, a, b, gain;
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
static float voice_fetch(voice_t *v)
{
    const sound_t *s = v->s;
    uint32_t i = v->ni;
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
        return (float)((const int16_t *)s->d)[i];
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
        return (float)p;
    }
}

static void voice_start(uint32_t t, uint32_t note, uint32_t vel, uint32_t delay, uint32_t seq, uint32_t life)
{
    uint32_t li = trk[t].sound, i, k = 0, oldest = 0xFFFFFFFFu;
    float g = 1.0f, st;
    const sound_t *s = li > NSOUNDS ? my_sound(li - NSOUNDS - 1u, note, &g) : list_sound(li);
    voice_t *v;
    if (!s)
        return;                          /* an empty track is silent */
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
    v->s0 = voice_fetch(v);
    v->s1 = voice_fetch(v);
    v->env = 1.0f;
    v->att = 0.0f;
    v->vel = 0.25f + 0.75f * (float)vel * (1.0f / 127.0f);
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
        if (v->state == V_HELD && v->track == t && v->seq == seq && (seq || v->note == note) &&
            !(v->s->flags & SF_DRUM))
            v->state = V_REL;
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

static void midi_in(void)                    /* audio interrupt: MIDI in plays the selected track */
{
    uint32_t pkt;
    int port;
    extern volatile uint8_t ui_sel_isr;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if ((st & 0xF0u) == 0x90u && d2)
            voice_start(ui_sel_isr, d1, d2, 0, 0, 0);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            voice_release(ui_sel_isr, d1, 0);
    }
}
volatile uint8_t ui_sel_isr;                 /* the selected track, as the audio interrupt sees it */

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

static void seq_block(uint32_t n)
{
    const float step_frames = 44100.0f * 60.0f / (SEQ_BPM * 4.0f);
    if (seq_play_req && !seq_running) {      /* PLAY: from step 1, now */
        seq_running = 1;
        seq_next = 0;
        seq_left = 0.0f;
    } else if (!seq_play_req && seq_running) {
        uint32_t t;
        seq_running = 0;
        for (t = 0; t < NTRK; t++)
            voice_release(t, 0, 1);
    }
    if (!seq_running)
        return;
    while (seq_left < (float)n) {
        seq_fire(seq_next, (uint32_t)seq_left);
        seq_next = (seq_next + 1u) % NSTEP;
        seq_left += step_frames;
    }
    seq_left -= (float)n;
}

#define VOICE_AMP 1.0f                   /* a voice at full velocity, before its track's LEVEL */
static float tb[APP_BLOCK];                  /* one track's bus */
static float mixl[APP_BLOCK], mixr[APP_BLOCK];
#include "fx.c"                          /* the effects: two slots a track */
_Static_assert(NTRK == 8u, "fx.c's per-track state is for eight tracks");

static void voice_render(voice_t *v, uint32_t n, float dcoef, float rcoef, float rate)
{
    uint32_t i = v->delay;
    float frac = v->frac, inc = v->inc * rate, env = v->env, att = v->att;
    /* (TAPE SPEED: the track's play rate; near a standstill the sound fades, as a stopped tape gives none) */
    float amp = v->vel * v->gain * (VOICE_AMP / 32768.0f) * (rate * 12.0f < 1.0f ? rate * 12.0f : 1.0f);
    float s0 = v->s0, s1 = v->s1;
    if (inc > INC_MAX)
        inc = INC_MAX;
    v->delay = 0;
    for (; i < n; i++) {
        if (att < 1.0f)
            att += 1.0f / 48.0f;             /* ~1 ms in: no click on a sound that starts away from zero */
        env *= v->state == V_REL ? rcoef : dcoef;
        tb[i] += (s0 + (s1 - s0) * frac) * amp * env * (att < 1.0f ? att : 1.0f);
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

static void render_part(int32_t *lr, uint32_t n, float gain)
{
    uint32_t t, i, k, on = 0;
    uint32_t r = evq_r;
    if (my_kill) {                           /* a slot of the app's is about to be erased: its voices end here */
        for (k = 0; k < NVOICE; k++)
            if (voice[k].my && (my_kill >> (voice[k].my - 1u) & 1u))
                voice[k].state = V_OFF;
        my_kill = 0;
    }
    while (r != evq_w) {                     /* the keys' notes since the last block */
        uint32_t e = evq[r], kind = e >> 24, tr = e & 0xFFu, note = (e >> 8) & 0x7Fu, vel = (e >> 16) & 0x7Fu;
        if (kind == QE_ON)
            voice_start(tr, note, vel, 0, 0, 0);
        else if (kind == QE_PING)
            voice_start(tr, note, vel, 0, 0, 8820);   /* a step's note heard for 0.2 s */
        else
            voice_release(tr, note, 0);
        r = (r + 1u) % EVQ;
    }
    evq_r = r;
    midi_in();
    seq_block(n);
    for (i = 0; i < n; i++)
        mixl[i] = mixr[i] = 0.0f;
    for (t = 0; t < NTRK; t++) {
        const sound_t *s = list_sound(trk[t].sound);
        int32_t f = trk[t].filt;
        float kd = (float)trk[t].decay, lv = (float)trk[t].level * 0.01f;
        float dcoef = 1.0f, rcoef, a_t = 1.0f, b_t = 0.0f, g_t = lv * lv, a = bus[t].a, b = bus[t].b, g = bus[t].gain;
        float lp1 = bus[t].lp1, lp2 = bus[t].lp2, hp1 = bus[t].hp1, hp2 = bus[t].hp2;
        float fa[2], rate_t = 1.0f, dry = 1.0f;
        uint32_t any = 0, ft[2], fs[2], q;
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
        /* FILTER: left of the middle a low-pass closing from 16 kHz to 125 Hz, right a high-pass opening 30 Hz .. 5.4 kHz */
        if (f < -FILT_DEAD)
            a_t = 1.0f - fm_expf(-6.2831853f * 16000.0f * fm_exp2f(-(float)(-f - FILT_DEAD) * 0.14f) * (1.0f / 44100.0f));
        if (f > FILT_DEAD)
            b_t = 1.0f - fm_expf(-6.2831853f * 30.0f * fm_exp2f((float)(f - FILT_DEAD) * 0.15f) * (1.0f / 44100.0f));
        for (i = 0; i < n; i++)
            tb[i] = 0.0f;
        for (k = 0; k < NVOICE; k++)
            if (voice[k].state != V_OFF && voice[k].track == t) {
                voice_render(&voice[k], n, dcoef, rcoef, tape_rate[t]);
                any = 1;
                on++;
            }
        if (!any && fm_fabsf(lp2) < 1e-6f && fm_fabsf(hp2) < 1e-6f) {
            bus[t].a = a_t;                  /* silent: nothing to filter, the knobs' values taken at once */
            bus[t].b = b_t;
            bus[t].gain = g_t;
            bus[t].lp1 = bus[t].lp2 = bus[t].hp1 = bus[t].hp2 = 0.0f;
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
        }
        for (q = 0; q < 2u; q++)                 /* OVERDRIVE and LO-FI: on the track itself, slot A then B */
            if (ft[q] == FX_DRIVE || ft[q] == FX_LOFI)
                fx_inline(ft[q], fs[q], fa[q], &fx_slot[t][q], tb, n);
        for (i = 0; i < n; i++) {                /* LEVEL */
            g += (g_t - g) * 0.004f;
            tb[i] *= g;
        }
        for (q = 0; q < 2u; q++)                 /* the shared effects: this track's part into their buses */
            if (FX_BUS[ft[q]] >= 0)
                dry *= fx_send(ft[q], fs[q], fa[q], tb, n);
        for (i = 0; i < n; i++) {
            float x = tb[i] * dry;
            mixl[i] += x;
            mixr[i] += x;
        }
        bus[t].a = a;
        bus[t].b = b;
        bus[t].gain = g;
        bus[t].lp1 = fm_flush(lp1);
        bus[t].lp2 = fm_flush(lp2);
        bus[t].hp1 = fm_flush(hp1);
        bus[t].hp2 = fm_flush(hp2);
    }
    fx_buses(mixl, mixr, n, 44100.0f * 60.0f / (SEQ_BPM * 4.0f));
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
        trk[t].fx[0] = trk[t].fx[1] = FX_OFF;
        trk[t].fxk[0] = trk[t].fxk[1] = 0;
        for (i = 0; i < NSTEP; i++) {
            step_on[t][i] = 0;
            step_note[t][i] = 60;
        }
    }
}

static uint8_t my_again;                     /* the scans after the start done so far (my_frame) */
static uint32_t my_t0;                       /* plat_ms at the start */
void app_init(void)
{
    fx_reset();
    memset(my, 0, sizeof my);                /* (a start is from nothing: the simulator's restart keeps the RAM) */
    ms.slot = -1;
    mc.slot = -1;
    rx.on = 0;
    my_again = 0;
    my_load_all();                           /* the samples the app sent before (they stay in the flash) */
    defaults();
}

/* --------------------------------------------------------------------- UI --- */
enum { PAGE_PLAY, PAGE_STEP, PAGE_FX };
static struct {
    char msg[2][28];                     /* a short message over the header */
    uint32_t msg_until;
    uint32_t keys, btn;                  /* held at the last input */
    uint8_t sel;                         /* the selected track */
    uint8_t page;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t oct_chord;                   /* both OCT buttons went down together: neither release shifts */
    uint8_t undo_ok, undo_trk, undo_sound;   /* UNDO SOUND: the track and the sound to put back */
    uint32_t undo_t;                     /* the last turn of PRESETS (a pause starts a new undo) */
    int8_t held_step;                    /* the step page: the step whose key is down (-1 none) */
    uint8_t held_turned;                 /* KNOB 1 turned while it was held: letting go does not toggle it */
    uint8_t note_of[NKEYS];              /* the note each held key started (the octave may change under it) */
    uint8_t trk_of[NKEYS];
    int8_t hot;                          /* the knob turned last (0..3), lit for a moment */
    uint32_t hot_t;
    uint8_t title;                       /* the title is up (the first TITLE_MS after a start) */
    uint8_t cpu;                         /* plat_cpu_pct, as drawn */
    uint32_t cpu_t;
    uint8_t redraw;
} ui;

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
 * the knobs' values, the steps and the other tracks stay. The screen says the old and the new */
static void sound_turn(int32_t e)
{
    uint32_t t = ui.sel, old = trk[t].sound, now = plat_ms();
    int32_t to = clamp((int32_t)old + e, 0, (int32_t)NLIST - 1);
    char a[28], b[28], m[28];
    if ((uint32_t)to == old) {
        app_say(e < 0 ? "START OF LIST" : "END OF LIST", "");
        return;
    }
    if (!(ui.undo_ok && ui.undo_trk == t && now - ui.undo_t < 1500u)) {   /* a new turn: what REC puts back */
        ui.undo_ok = 1;
        ui.undo_trk = (uint8_t)t;
        ui.undo_sound = (uint8_t)old;
    }
    ui.undo_t = now;
    trk[t].sound = (uint8_t)to;
    list_name(ui.undo_sound, a);             /* (the sound before the turn began: what REC puts back) */
    list_name((uint32_t)to, b);
    m[0] = 0;
    text_cat(m, "> ");
    text_cat(m, b);
    app_say(a, m);
}

static void sound_undo(void)                 /* REC: UNDO SOUND */
{
    char a[28], b[28], m[28];
    uint32_t t;
    if (!ui.undo_ok) {
        app_say("NOTHING TO UNDO", "");
        return;
    }
    t = ui.undo_trk;
    list_name(trk[t].sound, a);
    list_name(ui.undo_sound, b);
    trk[t].sound = ui.undo_sound;
    ui.undo_ok = 0;
    ui.sel = (uint8_t)t;                     /* (the track it was on: shown, so the undo is seen) */
    ui_sel_isr = ui.sel;
    m[0] = 0;
    text_cat(m, "UNDO T");
    b[27] = 0;
    {
        char n[4];
        int_text((int32_t)t + 1, n);
        text_cat(m, n);
        text_cat(m, ": ");
        text_cat(m, b);
    }
    (void)a;
    app_say(m, "");
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
        t->level = (uint8_t)clamp(t->level + e, 0, 100);
    }
    ui.redraw = 1;
}

static void input(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    uint32_t keys, btn = plat_buttons(), k, pressed, released, lit_keys = 0, lit_btn;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    pressed = btn & ~ui.btn;
    released = ~btn & ui.btn;

    /* the keys: on the play page they play the selected track; on the step page the 16 white keys are the steps */
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k, down = (keys & bit) && !(ui.keys & bit), up = !(keys & bit) && (ui.keys & bit);
        if (ui.page == PAGE_STEP) {
            uint32_t s;
            for (s = 0; s < 16u && WK[s] != k; s++)
                ;
            if (s == 16u)
                continue;                    /* (the black keys have no job on the step page) */
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
            /* (the keys send no MIDI notes: up to 0.2.2-test they did, and the Burger Studio app plays its own drum
             * pads on any note that comes in, pad = note % 32, which came back through the FM-1's USB audio and
             * sounded like drums on some of the keys. Notes out return with a setting of their own) */
        } else if (up) {
            ev_put(QE_OFF, ui.trk_of[k], ui.note_of[k], 0);
        }
    }

    /* OCT- and OCT+: the octave moves when the button is let go. Both down together is the looper's chord (kept for
     * a later version): then neither release moves the octave, whichever finger came first */
    if ((btn >> B_OCTDN & 1u) && (btn >> B_OCTUP & 1u) && !ui.oct_chord) {
        ui.oct_chord = 1;
        app_say("LOOPER", "NOT IN THIS VERSION");
    }
    if (released >> B_OCTDN & 1u) {
        if (!ui.oct_chord)
            octave_step(-1);
    }
    if (released >> B_OCTUP & 1u) {
        if (!ui.oct_chord)
            octave_step(1);
    }
    if (ui.oct_chord && !(btn >> B_OCTDN & 1u) && !(btn >> B_OCTUP & 1u))
        ui.oct_chord = 0;

    for (k = 0; k < NB; k++) {
        static const char *const BTN_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                                 "PLAY", "REC", "OCT-", "OCT+"};
        if (!(pressed >> k & 1u) || k == B_OCTDN || k == B_OCTUP)
            continue;
        if (k == B_REC) {
            sound_undo();
        } else if (k == B_PLAY) {
            seq_play_req = (uint8_t)!seq_play_req;
            app_say(seq_play_req ? "PLAY" : "STOP", "");
        } else if (k == B_SEQ || k == B_FX) {    /* SEQ: the steps and back; FX: the effects and back */
            uint32_t to = k == B_SEQ ? PAGE_STEP : PAGE_FX;
            ui.page = (uint8_t)(ui.page == to ? PAGE_PLAY : to);
            ui.held_step = -1;
            for (e = 0; e < (int32_t)NKEYS; e++)        /* keys held across the change let go */
                if (ui.keys >> e & 1u)
                    ev_put(QE_OFF, ui.trk_of[e], ui.note_of[e], 0);
            app_say(ui.page == PAGE_STEP ? "STEP PAGE" : ui.page == PAGE_FX ? "FX PAGE" : "TRACKS PAGE", "");
        } else {
            app_say(BTN_NAME[k], "NOT IN THIS VERSION");   /* (never a silent press) */
        }
    }

    if ((e = plat_enc(EN_ALGO)) != 0) {      /* ALGORITHM: the track; one detent a track, it stops at 1 and 8 */
        int32_t to = clamp((int32_t)ui.sel + e, 0, (int32_t)NTRK - 1);
        if ((uint32_t)to != ui.sel) {
            ui.sel = (uint8_t)to;
            ui_sel_isr = ui.sel;
            ui.held_step = -1;
            ui.redraw = 1;
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0)
        sound_turn(e);
    for (k = 0; k < 4u; k++)
        if ((e = plat_enc((int)(EN_K1 + k))) != 0)
            knob_turn(k, e);
    plat_enc(EN_SELECT);                     /* (no job in this version: its turns are not kept) */

    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;

    /* the lights: REC only while there is a sound to undo; SEQ on the step page; PLAY while playing */
    lit_btn = (ui.undo_ok ? 1u << B_REC : 0u) | (ui.page == PAGE_STEP ? 1u << B_SEQ : 0u) |
              (ui.page == PAGE_FX ? 1u << B_FX : 0u) | (seq_play_req ? 1u << B_PLAY : 0u);
    if (ui.page == PAGE_STEP) {
        for (k = 0; k < 16u; k++)
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

static void draw_header(void)
{
    char t[28];
    uint32_t now = plat_ms();
    cv_begin(240, 28, C_BLACK);
    /* two lines: the first is the name row or a message; the second, small, is the version, always there (the build's
     * APP_VERSION: stack/VERSION, the one place it is written) */
    if (now < ui.msg_until) {
        int32_t x = cv_text(6, 0, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 6, 0, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(6, 0, &FONT_B, APP_NAME, C_ORANGE);
        cv_text(x + 8, 2, &FONT_XS, ui.page == PAGE_STEP ? "STEPS" : ui.page == PAGE_FX ? "FX" : "TRACKS", C_GRAY);
        t[0] = 0;
        text_cat(t, "OCT ");
        t[4] = ui.octave < 0 ? '-' : '+';
        t[5] = (char)('0' + (ui.octave < 0 ? -ui.octave : ui.octave));
        t[6] = 0;
        cv_text(150, 2, &FONT_XS, t, C_GRAY);
        t[0] = 0;                            /* the audio's share of the processor (a test version's readout) */
        text_cat(t, "CPU ");
        int_text((int32_t)ui.cpu, t + 4);
        cv_text(104, 5, &FONT_T, t, ui.cpu >= 70u ? C_WHITE : C_DIM);
        cv_text(200, 2, &FONT_XS, seq_play_req ? "PLAY" : "STOP", seq_play_req ? C_HI : C_DIM);
    }
    cv_text(6, 15, &FONT_T, APP_VERSION, C_GRAY);
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
    cv_text(6, 28, &FONT_XS, s ? "WHITE KEYS 1-16: STEP ON / OFF" : "EMPTY TRACK: PICK A SOUND FIRST", C_GRAY);
    cv_text(6, 46, &FONT_XS, drum ? "A DRUM PLAYS ITS OWN PITCH" : "HOLD A STEP + KNOB 1: ITS NOTE", C_GRAY);
    cv_blit(0, 28);
    cv_begin(240, 80, C_BLACK);
    for (i = 0; i < NSTEP; i++) {
        int32_t x = 4 + (int32_t)(i % 8u) * 29, y = 2 + (int32_t)(i / 8u) * 38;
        uint32_t on = step_on[ui.sel][i], now = seq_play_req && seq_step_now == i, held = ui.held_step == (int8_t)i;
        uint16_t c = on ? C_ORANGE : C_LINE;
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

static void draw_knobs(void)                 /* the four knobs of the selected track, and what the controls do */
{
    static const char *const NAME[4] = {"PITCH", "FILTER", "DECAY", "LEVEL"};
    const track_t *t = &trk[ui.sel];
    uint32_t k, now = plat_ms();
    char v[12];
    cv_begin(240, 52, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    if (ui.page == PAGE_FX) {                /* the effects' page: what its four knobs are */
        static const char *const FN[4] = {"K1 EFFECT", "K2 KNOB", "K3 EFFECT", "K4 KNOB"};
        for (k = 0; k < 4u; k++) {
            int32_t x = 4 + (int32_t)k * 59;
            int hot = ui.hot == (int8_t)k && now - ui.hot_t < 900u;
            cv_text(x, 3, &FONT_T, FN[k], hot ? C_WHITE : C_GRAY);
            cv_text(x, 16, &FONT_B, k < 2u ? "A" : "B", hot ? C_WHITE : C_HI);
        }
        cv_text(4, 40, &FONT_T, "FX: BACK TO THE TRACKS", C_DIM);
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
            int_text(k == 2u ? t->decay : t->level, v);
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
    cv_text(4, 40, &FONT_T, ui.undo_ok ? "REC: UNDO SOUND" : "ALGO: TRACK  PRESETS: SOUND", ui.undo_ok ? C_WHITE : C_DIM);
    {                                        /* where the track's sound is in the list: its category and place there */
        char c[20];
        cat_text(t->sound, c);
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

#define TITLE_MS 2000u                   /* the title after a start: the name and the version, large */
static void draw_title(void)                 /* in the tracks' place; the keys, the knobs and the header work meanwhile */
{
    cv_begin(240, 80, C_BLACK);
    cv_text((240 - text_w(&FONT_L, APP_NAME)) / 2, 40, &FONT_L, APP_NAME, C_ORANGE);
    cv_blit(0, 28);
    cv_begin(240, 80, C_BLACK);
    cv_text((240 - text_w(&FONT_M, APP_VERSION)) / 2, 8, &FONT_M, APP_VERSION, C_WHITE);
    cv_text((240 - text_w(&FONT_XS, "MAKER HIP")) / 2, 44, &FONT_XS, "MAKER HIP", C_GRAY);
    cv_blit(0, 108);
}

static void draw(void)
{
    draw_header();
    if (ui.title)
        draw_title();
    else if (ui.page == PAGE_FX)
        draw_fx();
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
        for (t = 6; t < NTRK && trk[t].sound; t++)
            ;
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
            if (++my_again == 2u) {                 /* then once: how many samples of the app's are here */
                char c[4];
                for (k = 0, n_ok = 0; k < NMY; k++)
                    n_ok += my[k].valid;
                int_text((int32_t)n_ok, c);
                app_say("MY SAMPLES:", c);
            }
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
    touch_on(1);                             /* velocity: the keys' readings on */
    plat_usb_audio_full(1);                  /* USB audio at full level, whatever MASTER says */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.held_step = -1;
    ui.hot = -1;
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
    speak();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.hot >= 0 && now - ui.hot_t >= 900u) {
        ui.hot = -1;
        ui.redraw = 1;
    }
    if (ui.undo_ok && now - ui.undo_t >= 1500u && now - ui.undo_t < 1600u)
        ui.redraw = 1;
    if (ui.title && now - my_t0 >= TITLE_MS) {
        ui.title = 0;
        ui.redraw = 1;
    }
    if (now - ui.cpu_t >= 500u) {            /* the CPU readout, twice a second */
        uint32_t c = plat_cpu_pct();
        ui.cpu_t = now;
        if (c != ui.cpu) {
            ui.cpu = (uint8_t)(c > 255u ? 255u : c);
            ui.redraw = 1;
        }
    }
    if (seq_play_req != last_play || (ui.page == PAGE_STEP && seq_play_req && seq_step_now != last_step)) {
        last_play = seq_play_req;
        last_step = seq_step_now;
        ui.redraw = 1;
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
    else if (!strcmp(what, "undo"))
        *got = ui.undo_ok;
    else if (!strcmp(what, "pitch"))
        *got = trk[ui.sel].pitch;
    else if (!strcmp(what, "filt"))
        *got = trk[ui.sel].filt;
    else if (!strcmp(what, "decay"))
        *got = trk[ui.sel].decay;
    else if (!strcmp(what, "level"))
        *got = trk[ui.sel].level;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "playing"))
        *got = seq_play_req;
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
    else if (!strcmp(what, "fxa") || !strcmp(what, "fxb"))       /* the selected track's effect in slot A, B */
        *got = trk[ui.sel].fx[what[2] - 'a'];
    else if (!strcmp(what, "fxak") || !strcmp(what, "fxbk"))     /* and its knob */
        *got = trk[ui.sel].fxk[what[2] - 'a'];
    else if (!strcmp(what, "fx_awake")) {    /* the effects' buses that run now */
        uint32_t b;
        for (*got = 0, b = 0; b < NBUS; b++)
            *got += fxb[b].awake;
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
