/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the face view (since 4.11-beta: it took the city view's place; its name in the code stays "city").
 * Written for STACK. Part of the unity build: included by app.c.
 *
 * Maker Hip's own face drawn in characters on a black ground with an olive dot grid (face_data.h, from the owner's
 * photos): the hair and the shoulders as an outline of small marks, the face in letters of STACK's words and #. It
 * talks with the sound: the master's level picks one of five mouths, closed to wide open (the loudest with the eyes
 * wide too). Around it six little equalizers, one for each group of sounds in the group's own colour (KICK sky blue,
 * SNARE pink, HI-HAT red, BASS green, MELODY yellow, SOUND purple): the louder, the taller, and a new hit flashes
 * white. Across the top: STACK, the version, the tempo.
 *
 * It is drawn once when the view opens (three times, brighter each: it fades in); after that only the six equalizers,
 * the face when its mouth changes and the tempo when it changes, each in a small canvas of its own, twenty times a
 * second. It runs in the main loop, where the screen always is: the sound and the keys are not held up by it (the
 * tracks' levels come from the audio interrupt as one number each). */

#include "face_data.h"
enum { G_KICK, G_SNARE, G_HAT, G_BASS, G_MELODY, G_SOUND, NGRP };
#define CITY_EDGE 14u                        /* the master's level in so many steps */
static struct {
    uint8_t stage;                           /* 0 closed; 1 .. 3 fading in; 4 up */
    uint8_t manual;                          /* opened by HOME (it stays while stopped) */
    uint32_t t;                              /* the last frame's time */
    float lv[NGRP];                          /* each group's level as its lamp shows it */
    uint32_t flash_t[NGRP];                  /* when its lamp last flashed white (0: not) */
    uint32_t flashes;                        /* (the simulator's checks) */
    float mv[2];                             /* the master's, left and right */
    uint8_t edge[2];                         /* the master's level, left and right, in 14 steps (255: not yet) */
    uint8_t mouth;                           /* the mouth drawn (255: not yet) */
    uint16_t bpm;                            /* the tempo drawn */
} city;

/* the groups' colours, at full: sky blue, pink, red, bright green, yellow, purple */
static const uint8_t CITY_RGB[NGRP][3] = {{80, 190, 255}, {255, 110, 190}, {255, 40, 40}, {60, 255, 70}, {255, 225, 40}, {180, 80, 255}};
/* where each group's equalizer stands (its top left, px): three left of the face, three right */
static const uint8_t CITY_EQ[NGRP][2] = {{6, 46}, {222, 46}, {6, 127}, {222, 118}, {12, 190}, {216, 190}};

/* a colour at brightness f, 0 .. 256 (the fade-in) */
static inline uint16_t city_rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t f) { return RGB(r * f >> 8, g * f >> 8, b * f >> 8); }
#define FACE_YEL(f) city_rgb(236, 204, 70, f)
#define FACE_DIM(f) city_rgb(170, 146, 52, f)
static inline uint32_t city_hash(uint32_t a) { a ^= a >> 7; a *= 0x9E3779B1u; return a ^ a >> 15; }

/* the group a track's sound belongs to (-1: none, an EMPTY track). The drums by their names: KICK and the toms (the low
 * skins) with the kick, SNARE, CLAP and RIM with the snare, HAT, OPEN, CRASH and RIDE with the hi-hat, the rest of the
 * percussion with SOUND; BASS is the BASS list; MELODY is KEYS, PAD and LEAD; SOUND is FX, the app's samples and the loop */
static int32_t city_group(uint32_t t)
{
    const uint32_t li = trk[t].sound;
    const sound_t *s;
    const char *n;
    if (t == lp.trk && lp.own)
        return G_SOUND;
    if (li > NSOUNDS)
        return li < NLIST && my[li - NSOUNDS - 1u].valid ? G_SOUND : -1;
    if (!(s = list_sound(li)))
        return -1;
    if (s->cat != CAT_DRUMS)
        return s->cat == CAT_BASS ? G_BASS : s->cat == CAT_FX ? G_SOUND : G_MELODY;
    for (n = s->name; *n; n++) {
        if ((n[0] == 'K' && n[1] == 'I' && n[2] == 'C') || (n[0] == 'T' && n[1] == 'O' && n[2] == 'M'))
            return G_KICK;
        if ((n[0] == 'S' && n[1] == 'N' && n[2] == 'A') || (n[0] == 'C' && n[1] == 'L' && n[2] == 'A') ||
            (n[0] == 'R' && n[1] == 'I' && n[2] == 'M'))
            return G_SNARE;
        if ((n[0] == 'H' && n[1] == 'A' && n[2] == 'T') || (n[0] == 'O' && n[1] == 'P' && n[2] == 'E') ||
            (n[0] == 'C' && n[1] == 'R' && n[2] == 'A') || (n[0] == 'R' && n[1] == 'I' && n[2] == 'D'))
            return G_HAT;
    }
    return G_SOUND;
}

/* the small letters the outline is drawn in: o c e i n s k t, 3 x 4 px each, a bit a pixel (rows of 3, the lowest
 * bit the leftmost) */
static const uint16_t FACE_MARK[8] = {0x56A, 0xC4E, 0xC7A, 0x482, 0xB58, 0x79E, 0xAE9, 0xCBA};
static const char FACE_WORDS[] = "clock*anything*stack*rec*play*one*loop*eight*tracks*maker*hip*sync*tape*";
static const char FACE_SPECK[4] = {'.', ',', '\'', ':'};

/* the dot grid, the box x0, y0, w, h of it, into a canvas whose top left is ox, oy on the screen */
static void face_dots(int32_t x0, int32_t y0, int32_t w, int32_t h, int32_t ox, int32_t oy, uint32_t f)
{
    int32_t x, y;
    for (y = 16 + (y0 > 16 ? (y0 - 16 + 8) / 9 * 9 : 0); y < y0 + h; y += 9)     /* on the screen's grid: 16 + 9k, 3 + 6k */
        for (x = 3 + (x0 > 3 ? (x0 - 3 + 5) / 6 * 6 : 0); x < x0 + w; x += 6)
            if (city_hash((uint32_t)(y / 18 * 64 + x / 30)) % 6u && city_hash((uint32_t)(y * 256 + x)) % 14u)
                cv_rect(x - ox, y - oy, 1, 1, city_rgb(92, 84, 36, f));
}

/* the face, mouth m, into a canvas whose top left is ox, oy on the screen */
static void face_cells(uint32_t m, int32_t ox, int32_t oy, uint32_t f)
{
    uint32_t i, k = 0;
    char c[2] = {0, 0};
    for (i = 0; i < FACE_CW * FACE_CH; i++) {
        const uint32_t v = FACE_CELLS[m][i >> 2] >> ((i & 3u) * 2u) & 3u, h = city_hash(i * 31u + m);
        const int32_t x = 78 - ox + (int32_t)(i % FACE_CW) * 6, y = 90 + (int32_t)(i / FACE_CW) * 9 - oy;
        if (!v)
            continue;
        c[0] = v == 1u ? '#' : v == 2u ? FACE_WORDS[k++ % (sizeof FACE_WORDS - 1u)] : FACE_SPECK[h & 3u];
        cv_rect(x, y, 6, 9, C_BLACK);
        cv_text(x, y - 3, &FONT_XS, c, v == 3u || (v == 2u && h & 16u) ? FACE_DIM(f) : FACE_YEL(f));
    }
}

static void face_top(int32_t oy, uint32_t f)  /* STACK, the version, the tempo */
{
    char t[12];
    const char *v = APP_VERSION;
    int32_t x = cv_text(4, 1 - oy, &FONT_T, APP_NAME, city_rgb(238, 230, 198, f)), bpm = city.bpm, i;
    for (i = 0; i < 11 && v[i] && v[i] != '-'; i++)      /* the version's number: 4.11, not -beta */
        t[i] = v[i];
    t[i] = 0;
    cv_text(x + 6, 1 - oy, &FONT_T, t, city_rgb(238, 78, 62, f));
    t[0] = (char)(bpm >= 100 ? '0' + bpm / 100 : ' ');
    t[1] = (char)('0' + bpm / 10 % 10);
    t[2] = (char)('0' + bpm % 10);
    t[3] = 0;
    text_cat(t, " BPM");
    cv_text(236 - text_w(&FONT_T, t), 1 - oy, &FONT_T, t, city_rgb(238, 230, 198, f));
}

static void city_scene(uint32_t f)           /* the whole picture: three bands */
{
    int32_t y0, x, y;
    city.bpm = (uint16_t)(44100.0f * 15.0f / seq_sf + 0.5f);
    for (y0 = 0; y0 < 240; y0 += 80) {
        uint32_t k = (uint32_t)y0 * 7u;
        cv_begin(240, 80, C_BLACK);
        face_dots(0, 12, 240, 228, 0, y0, f);
        for (y = 0; y < FACE_OH; y++)        /* the outline */
            for (x = 0; x < FACE_OW; x++)
                if (FACE_OUT[y][x >> 3] >> (x & 7) & 1u) {
                    const uint32_t mk = FACE_MARK[city_hash((uint32_t)(y * 64 + x)) & 7u], h = city_hash(k++);
                    const int32_t px = 24 + x * 3, py = 9 + y * 5 - y0;
                    uint32_t b;
                    if (py < -5 || py > 80)
                        continue;
                    cv_rect(px, py, 3, 5, C_BLACK);
                    for (b = 0; b < 12u; b++)
                        if (mk >> b & 1u)
                            cv_rect(px + (int32_t)(b % 3u), py + (int32_t)(b / 3u), 1, 1, h & 8u ? FACE_DIM(f) : FACE_YEL(f));
                }
        face_cells(0, 0, y0, f);
        if (!y0)
            face_top(0, f);
        cv_blit(0, (uint32_t)y0);
    }
    city.mouth = 0;
}

static void city_lights(uint32_t now)        /* the six equalizers, the mouth and the tempo, as it sounds now */
{
    float g[NGRP] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const int32_t mine = city_group(ui.sel);
    uint32_t t, side;
    for (t = 0; t < NTRK; t++) {             /* the tracks into their groups: several in one add as powers do */
        const int32_t k = city_group(t);
        const float v = trk_pk[t];
        trk_pk[t] = 0.0f;
        if (k >= 0)
            g[k] += v * v;
    }
    for (t = 0; t < NGRP; t++) {             /* the six equalizers */
        int32_t lit, k;
        uint32_t br, white;
        float db;
        const float v = fm_sqrtf(g[t]);
        if (v > 0.03f && v > city.lv[t] * 1.6f) {    /* a hit, well over what it showed: it flashes white */
            city.flash_t[t] = now | 1u;
            city.flashes++;
        }
        city.lv[t] = v > city.lv[t] ? v : city.lv[t] * 0.78f;        /* up at once, down over a few frames */
        white = city.flash_t[t] && now - city.flash_t[t] < 90u;
        db = city.lv[t] > 1e-5f ? 6.0206f * fm_log2f(city.lv[t]) : -100.0f;
        lit = (int32_t)((db + 45.0f) * 12.0f / 36.0f + 0.5f);        /* -45 dB .. -9 dB: 0 .. 12 px */
        lit = lit < 0 ? 0 : lit > 12 ? 12 : lit;
        br = (int32_t)t == mine ? 256u : 200u;
        cv_begin(10, 12, C_BLACK);
        for (k = 0; k < 3; k++) {            /* three bars, the middle one the level, the others a little less, by turns */
            int32_t h = k == 1 ? lit : lit * (int32_t)(5u + (city_hash(now / 50u * 3u + (uint32_t)k + t * 8u) & 3u)) / 8;
            cv_rect(k * 4, 0, 2, 12, RGB(CITY_RGB[t][0] / 9u, CITY_RGB[t][1] / 9u, CITY_RGB[t][2] / 9u));
            if (h > 0)
                cv_rect(k * 4, 12 - h, 2, h, white ? C_WHITE : RGB(CITY_RGB[t][0] * br >> 8, CITY_RGB[t][1] * br >> 8, CITY_RGB[t][2] * br >> 8));
        }
        cv_blit(CITY_EQ[t][0], CITY_EQ[t][1]);
    }
    for (side = 0; side < 2u; side++) {      /* the master, left and right, in 14 steps */
        const float v = mst_pk[side];
        float db;
        uint32_t lit;
        mst_pk[side] = 0.0f;
        city.mv[side] = v > city.mv[side] ? v : city.mv[side] * 0.80f;
        db = city.mv[side] > 1e-5f ? 6.0206f * fm_log2f(city.mv[side]) : -100.0f;
        lit = db <= -42.0f ? 0u : (uint32_t)((db + 42.0f) * (float)CITY_EDGE / 41.0f + 0.5f);    /* -42 dB .. -1 dB */
        city.edge[side] = (uint8_t)(lit > CITY_EDGE ? CITY_EDGE : lit);
    }
    {   /* the mouth, from the louder side: closed when silent, wide open (the eyes too) near the top */
        const uint32_t e = city.edge[0] > city.edge[1] ? city.edge[0] : city.edge[1];
        const uint32_t m = !e ? 0u : e < 6u ? 1u : e < 9u ? 2u : e < 12u ? 3u : 4u;
        const uint16_t bpm = (uint16_t)(44100.0f * 15.0f / seq_sf + 0.5f);
        if (m != city.mouth) {
            city.mouth = (uint8_t)m;
            cv_begin(96, 117, C_BLACK);
            face_dots(78, 90, 96, 117, 78, 90, 256);
            face_cells(m, 78, 90, 256);
            cv_blit(78, 90);
        }
        if (bpm != city.bpm) {
            city.bpm = bpm;
            cv_begin(240, 12, C_BLACK);
            face_top(0, 256);
            cv_blit(0, 0);
        }
    }
}

static void city_open(uint32_t manual)
{
    uint32_t t;
    if (city.stage)
        return;
    for (t = 0; t < NTRK; t++)
        trk_pk[t] = 0.0f;
    for (t = 0; t < NGRP; t++) {
        city.lv[t] = 0.0f;
        city.flash_t[t] = 0;
    }
    city.mv[0] = city.mv[1] = mst_pk[0] = mst_pk[1] = 0.0f;
    city.edge[0] = city.edge[1] = 255;
    city.manual = (uint8_t)manual;
    city.stage = 1;
    city.t = 0;
    city_on = 1;
}

static void city_close(void)
{
    city.stage = 0;
    city_on = 0;
    ui.redraw = 1;                           /* the page that was there, whole */
}

static void city_frame(uint32_t now)         /* a frame of the view (the main loop) */
{
    if (city.stage < 4u) {                   /* fading in: the city a third brighter each frame */
        if (now - city.t < 90u && city.t)
            return;
        city.t = now | 1u;
        city_scene(city.stage == 1u ? 80u : city.stage == 2u ? 168u : 256u);
        city.stage++;
        return;
    }
    if (now - city.t < 50u)
        return;
    city.t = now;
    city_lights(now);
}
