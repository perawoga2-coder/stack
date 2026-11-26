/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the mushroom view (since 4.12-beta; 4.11-beta showed a face, before it a city; its name in the code stays
 * "city"). Written for STACK. Part of the unity build: included by app.c.
 *
 * Psychedelic mushrooms drawn in characters on a black ground with an olive dot grid (view_data.h), as on STACK's
 * cover: the caps' letters and outlines in rings of the rainbow from each cap's middle, cream spots, the stems in
 * STACK's words. The rainbow turns with the sound: the louder the master, the faster its rings move outwards; silent,
 * it stands still. Around them six little equalizers, one for each group of sounds in the group's own colour (KICK sky
 * blue, SNARE pink, HI-HAT red, BASS green, MELODY yellow, SOUND purple): the louder, the taller, and a new hit flashes
 * white. Across the top: STACK, the version, the tempo.
 *
 * It is drawn once when the view opens (three times, brighter each: it fades in); after that the six equalizers, the
 * caps when the rainbow moves (ten times a second at most) and the tempo when it changes, each in a small canvas of its
 * own. It runs in the main loop, where the screen always is: the sound and the keys are not held up by it (the tracks'
 * levels come from the audio interrupt as one number each). */

#include "view_data.h"
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
    uint8_t glow;                            /* how fast the rainbow turns: 0 still .. 4 (the master's level) */
    uint8_t phase;                           /* the rainbow's turn, 0 .. 6 */
    uint32_t spin_t;                         /* when it last moved */
    uint16_t bpm;                            /* the tempo drawn */
} city;

/* the groups' colours, at full: sky blue, pink, red, bright green, yellow, purple */
static const uint8_t CITY_RGB[NGRP][3] = {{80, 190, 255}, {255, 110, 190}, {255, 40, 40}, {60, 255, 70}, {255, 225, 40}, {180, 80, 255}};
/* where each group's equalizer stands (its top left, px): three left of the face, three right */
static const uint8_t CITY_EQ[NGRP][2] = {{6, 46}, {222, 46}, {6, 127}, {222, 118}, {12, 190}, {216, 190}};

/* a colour at brightness f, 0 .. 256 (the fade-in) */
static inline uint16_t city_rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t f) { return RGB(r * f >> 8, g * f >> 8, b * f >> 8); }
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

static const char VIEW_WORDS[] = "clock*anything*stack*rec*play*one*loop*eight*tracks*maker*hip*sync*tape*";
static const uint8_t VIEW_BOW[7][3] = {{255, 60, 60}, {255, 150, 40}, {250, 230, 50}, {70, 230, 90}, {50, 200, 255}, {90, 110, 255}, {200, 80, 255}};

/* the dot grid, the box x0, y0, w, h of it, into a canvas whose top left is ox, oy on the screen */
static void face_dots(int32_t x0, int32_t y0, int32_t w, int32_t h, int32_t ox, int32_t oy, uint32_t f)
{
    int32_t x, y;
    for (y = 16 + (y0 > 16 ? (y0 - 16 + 8) / 9 * 9 : 0); y < y0 + h; y += 9)     /* on the screen's grid: 16 + 9k, 3 + 6k */
        for (x = 3 + (x0 > 3 ? (x0 - 3 + 5) / 6 * 6 : 0); x < x0 + w; x += 6)
            if (city_hash((uint32_t)(y / 18 * 64 + x / 30)) % 6u && city_hash((uint32_t)(y * 256 + x)) % 14u)
                cv_rect(x - ox, y - oy, 1, 1, city_rgb(92, 84, 36, f));
}

/* the mushrooms' rows r0 .. r1 - 1, the rainbow at turn ph, straight onto the screen in canvases of 8 rows */
static void view_rows(int32_t r0, int32_t r1, uint32_t ph, uint32_t f)
{
    int32_t b;
    for (b = r0; b < r1; b += 8) {
        const int32_t n = r1 - b < 8 ? r1 - b : 8, oy = 9 + b * 9;
        uint32_t i, k = 0;
        char ch[2] = {0, 0};
        cv_begin(192, (uint32_t)n * 9u, C_BLACK);
        face_dots(24, oy, 192, n * 9, 24, oy, f);
        for (i = 0; i < VIEW_C * (uint32_t)(b + n); i++) {
            const uint32_t v = VIEW_CELLS[i >> 1] >> ((i & 1u) * 4u) & 15u;
            const int32_t c = (int32_t)(i % VIEW_C), r = (int32_t)(i / VIEW_C), x = c * 6, y = (r - b) * 9;
            uint32_t j, d = 0xFFFFu;
            uint16_t col = city_rgb(240, 232, 204, f);
            if (v == 2u || v == 4u)
                k++;                             /* (the words run on from the top, whichever rows are drawn) */
            if (!v || r < b)
                continue;
            for (j = 0; j < 3u; j++) {           /* the nearest cap's middle: its rings */
                const int32_t dc = c - VIEW_CAP[j][0], dr = r - VIEW_CAP[j][1];
                const uint32_t e = (uint32_t)(dc * dc + 3 * dr * dr);
                d = e < d ? e : d;
            }
            if (v <= 2u) {
                const uint8_t *bw = VIEW_BOW[(d / 10u + 7u - ph) % 7u];
                col = city_rgb(bw[0], bw[1], bw[2], f);
            } else if (v == 5u) {
                col = city_rgb(64, 200, 190, f);
            }
            ch[0] = v == 1u ? '#' : v == 3u ? 'o' : v == 5u ? ':' : VIEW_WORDS[(k - 1u) % (sizeof VIEW_WORDS - 1u)];
            cv_rect(x, y, 6, 9, C_BLACK);
            cv_text(x, y - 3, &FONT_XS, ch, col);
        }
        cv_blit(24, (uint32_t)oy);
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

static void city_scene(uint32_t f)           /* the whole picture: the dots and the top in three bands, then the mushrooms */
{
    int32_t y0;
    city.bpm = (uint16_t)(44100.0f * 15.0f / seq_sf + 0.5f);
    for (y0 = 0; y0 < 240; y0 += 80) {
        cv_begin(240, 80, C_BLACK);
        face_dots(0, 12, 240, 228, 0, y0, f);
        if (!y0)
            face_top(0, f);
        cv_blit(0, (uint32_t)y0);
    }
    view_rows(1, VIEW_R, city.phase, f);     /* (row 0 is empty: the top line is over it) */
}

static void city_lights(uint32_t now)        /* the six equalizers, the rainbow and the tempo, as it sounds now */
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
    {   /* the rainbow turns with the louder side: still when silent, faster as it gets louder */
        const uint32_t e = city.edge[0] > city.edge[1] ? city.edge[0] : city.edge[1];
        const uint32_t g = !e ? 0u : e < 6u ? 1u : e < 9u ? 2u : e < 12u ? 3u : 4u;
        const uint16_t bpm = (uint16_t)(44100.0f * 15.0f / seq_sf + 0.5f);
        city.glow = (uint8_t)g;
        if (g && now - city.spin_t >= 500u / g) {        /* a step every 0.5 s .. 0.125 s */
            city.spin_t = now;
            city.phase = (uint8_t)((city.phase + 1u) % 7u);
            view_rows(1, VIEW_R, city.phase, 256);        /* every row: the small cap and the outlines are low down */
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
