/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the city view. Written for STACK. Part of the unity build: included by app.c.
 *
 * A city at night seen from the middle of its street, black and white, the buildings on both sides running to the
 * point where the street ends. The only colour is its lights. Six poles stand along the kerbs, three a side, near to
 * far: each carries a lamp that is the level meter of one group of sounds, in the group's own colour (KICK sky blue,
 * SNARE pink, HI-HAT red, BASS green, MELODY yellow, SOUND purple): the louder, the brighter and the more of the lamp
 * is lit, and a new hit flashes it white. The lights along the two kerbs are the master's meters, left and right:
 * they run from the far end towards the viewer, green, then yellow, then red.
 *
 * Nothing is stored: the city is worked out. A building's face is drawn two pixels at a time, its roof and its rows
 * of windows on lines that all meet at the street's end (240 x 240, the point at 120, 96). It is drawn once when the
 * view opens (three times, brighter each: it fades in); after that only the six lamps and the kerbs' lights that have
 * changed are drawn again, each in a small canvas of its own, twenty times a second. It runs in the main loop, where the screen always is: the sound
 * and the keys are not held up by it (the tracks' levels come from the audio interrupt as one number each). */

#define CITY_VX 120
#define CITY_VY 96
enum { G_KICK, G_SNARE, G_HAT, G_BASS, G_MELODY, G_SOUND, NGRP };
#define CITY_EDGE 14u                        /* lights along a kerb */
static struct {
    uint8_t stage;                           /* 0 closed; 1 .. 3 fading in; 4 up */
    uint8_t manual;                          /* opened by HOME (it stays while stopped) */
    uint32_t t;                              /* the last frame's time */
    float lv[NGRP];                          /* each group's level as its lamp shows it */
    uint32_t flash_t[NGRP];                  /* when its lamp last flashed white (0: not) */
    uint32_t flashes;                        /* (the simulator's checks) */
    float mv[2];                             /* the master's, left and right */
    uint8_t edge[2];                         /* the lights lit along each kerb, as drawn (255: not drawn yet) */
} city;

/* the poles, near to far, a side each by turns: the kerb's x they stand at (the right ones mirrored) */
static const uint8_t CITY_POLE[NGRP] = {12, 30, 50, 66, 80, 92};
/* the groups' colours, at full: sky blue, pink, red, bright green, yellow, purple */
static const uint8_t CITY_RGB[NGRP][3] = {{80, 190, 255}, {255, 110, 190}, {255, 40, 40}, {60, 255, 70}, {255, 225, 40}, {180, 80, 255}};

static inline uint16_t city_gray(uint32_t v, uint32_t f) { v = v * f >> 8; return RGB(v, v, v); }
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

/* pole g: its lamp's box, and (if wanted) its foot and its top. It stands on the kerb; the lamp hangs towards the
 * street. Everything is as large as it is near: d is its distance from the street's end, 28 .. 108 of 120 */
static void city_pole(uint32_t g, int32_t *lx, int32_t *ly, int32_t *lw, int32_t *lh, int32_t *px, int32_t *foot, int32_t *top)
{
    const int32_t d = CITY_VX - CITY_POLE[g], right = (int32_t)(g & 1u);
    const int32_t x = right ? 240 - CITY_POLE[g] : CITY_POLE[g], f = CITY_VY + d * 6 / 5, t = f - d * 3 / 2;
    *lw = 3 + d / 9;
    *lh = 6 + d * 2 / 5;
    *lx = right ? x - d / 7 - *lw : x + d / 7;
    *ly = t + d / 12;
    if (px) {
        *px = x;
        *foot = f;
        *top = t;
    }
}

/* light j of a kerb (0 the farthest): where, and how large. The right kerb's mirrored */
static void city_edge_at(uint32_t side, uint32_t j, int32_t *x, int32_t *y, int32_t *w)
{
    static const uint8_t D[CITY_EDGE] = {9, 11, 13, 16, 19, 23, 28, 34, 41, 50, 60, 73, 88, 106};
    const int32_t d = D[j], cx = CITY_VX - d + d / 6;      /* a little in from the kerb, on the road */
    *w = 1 + d / 16;
    *x = side ? 240 - cx - *w : cx;
    *y = CITY_VY + d * 6 / 5 - d / 14;
}

/* the city itself into the canvas, the band that starts y0 down the picture; f: its brightness, 0 .. 256 */
static void city_band(int32_t y0, uint32_t f)
{
    static const uint8_t EDGE[6] = {0, 46, 74, 92, 104, 114};        /* where each building starts, from the kerb in */
    static const int8_t ROOF[2][5] = {{-60, 8, 34, 20, 58}, {-30, -6, 40, 50, 30}};   /* its roof at that edge */
    int32_t side, x, k, t;
    for (k = 0; k < 18; k++) {                                         /* the stars */
        const uint32_t h = city_hash((uint32_t)k + 7u);
        cv_rect((int32_t)(h % 240u), (int32_t)(h >> 8) % 90 - y0, 1, 1, city_gray(90u + (h >> 20) % 120u, f));
    }
    for (k = 0; k < 7; k++)                                            /* the towers at the street's end */
        cv_rect(107 + k * 4, CITY_VY - 6 - (int32_t)(city_hash((uint32_t)k) % 14u) - y0, 3, 20, city_gray(k & 1 ? 46 : 70, f));
    for (k = 1; k < 7; k++)                                            /* the street's middle line */
        cv_rect(CITY_VX - (k + 1) / 2, 98 + k * k * 3 - y0, k + 1, k * 3, city_gray(150, f));
    for (side = 0; side < 2; side++)
        for (x = 0; x < 114; x += 2) {
            const int32_t sx = side ? 238 - x : x, dx = CITY_VX - x;
            int32_t b = 0, top, bot;
            while (b < 4 && x >= EDGE[b + 1])
                b++;
            top = CITY_VY - (CITY_VY - ROOF[side][b]) * dx / (CITY_VX - EDGE[b]);
            bot = CITY_VY + dx * 6 / 5;
            cv_rect(sx, top - y0, 2, bot - top, city_gray(b & 1 ? 14 : 24, f));          /* the face */
            cv_rect(sx, top - y0, 2, 1, city_gray(200, f));                              /* the roof's line */
            cv_rect(sx, bot - 1 - y0, 2, 1, city_gray(120, f));                          /* the kerb */
            if (x == EDGE[b]) {                                                          /* its near corner */
                cv_rect(side ? sx + 1 : sx, top - y0, 1, bot - top, city_gray(170, f));
                continue;
            }
            if ((x >> 1) % 3 == 2)
                continue;                                                                /* (between two windows) */
            for (k = -9; k < 9; k++) {                                                   /* its windows, row by row */
                const int32_t wy = CITY_VY + (k * 13 + 6) * dx / CITY_VX;
                const uint32_t h = city_hash((uint32_t)(side * 4000 + b * 400 + (k + 9) * 20 + x / 6));
                if (wy <= top + 1 || wy >= bot - 4 || h % 5u > 1u)
                    continue;
                cv_rect(sx, wy - y0, 2, dx / 40 + 1, city_gray(h % 7u ? 96 : 230, f));
            }
        }
    for (t = NGRP - 1; t >= 0; t--) {                                  /* the poles, the far ones first */
        static const char *const INIT[NGRP] = {"K", "S", "H", "B", "M", "X"};
        int32_t lx, ly, lw, lh, px, foot, top;
        const int32_t d = CITY_VX - CITY_POLE[t], pw = 1 + d / 36;
        city_pole((uint32_t)t, &lx, &ly, &lw, &lh, &px, &foot, &top);
        cv_rect(px - pw / 2, top - y0, pw, foot - top, city_gray(210, f));           /* the pole */
        cv_rect(px - pw, foot - 1 - y0, pw * 2 + 1, 1 + d / 50, city_gray(150, f));    /* its foot */
        cv_rect(t & 1 ? lx : px, top + 1 - y0, t & 1 ? px - lx : lx + lw - px, 1 + d / 60, city_gray(210, f));   /* its arm */
        cv_rect(px - 2 - d / 30, top + d / 5 - y0, 5 + d / 15, 1, city_gray(120, f)); /* a cross-bar, for the wires */
        cv_rect(lx - 1, ly - 1 - y0, lw + 2, lh + 2, city_gray(110, f));              /* the lamp's housing */
        cv_rect(lx, ly - y0, lw, lh, C_BLACK);
        if (t < 4)                                                                   /* whose it is, where it can be read */
            cv_text(lx + (lw - text_w(&FONT_T, INIT[t])) / 2, ly + lh + 2 - y0, &FONT_T, INIT[t], city_gray(200, f));
    }
    for (t = 0; t < 2; t++)                                            /* the kerbs' lights, dark */
        for (k = 0; k < (int32_t)CITY_EDGE; k++) {
            int32_t ex, ey, ew;
            city_edge_at((uint32_t)t, (uint32_t)k, &ex, &ey, &ew);
            cv_rect(ex, ey - y0, ew, ew, city_gray(52, f));
        }
    {   /* a corner: the version and the tempo */
        char c[28], n[8];
        c[0] = 0;
        text_cat(c, APP_VERSION);
        text_cat(c, "  ");
        int_text((int32_t)(44100.0f * 15.0f / (ck_tick_frames() * 6.0f) + 0.5f), n);
        text_cat(c, n);
        text_cat(c, " BPM");
        cv_text(CITY_VX - text_w(&FONT_T, c) / 2, 229 - y0, &FONT_T, c, city_gray(150, f));
    }
}

static void city_scene(uint32_t f)           /* the whole picture: three bands */
{
    int32_t y0;
    for (y0 = 0; y0 < 240; y0 += 80) {
        cv_begin(240, 80, C_BLACK);
        city_band(y0, f);
        cv_blit(0, (uint32_t)y0);
    }
}

static void city_lights(uint32_t now)        /* the six lamps and the kerbs' lights, as it sounds now */
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
    for (t = 0; t < NGRP; t++) {
        int32_t lx, ly, lw, lh, n, lit, k;
        uint32_t br, white;
        float db;
        const float v = fm_sqrtf(g[t]);
        if (v > 0.03f && v > city.lv[t] * 1.6f) {    /* a hit, well over what the lamp showed: it flashes white */
            city.flash_t[t] = now | 1u;
            city.flashes++;
        }
        city.lv[t] = v > city.lv[t] ? v : city.lv[t] * 0.78f;        /* up at once, down over a few frames */
        white = city.flash_t[t] && now - city.flash_t[t] < 90u;
        city_pole(t, &lx, &ly, &lw, &lh, 0, 0, 0);
        n = lh / 3 > 1 ? lh / 3 : 1;                                 /* its lights: 2 px and a gap each */
        db = city.lv[t] > 1e-5f ? 6.0206f * fm_log2f(city.lv[t]) : -100.0f;
        lit = (int32_t)((db + 45.0f) * (float)n / 36.0f + 0.5f);     /* -45 dB .. -9 dB up the lamp */
        lit = lit < 0 ? 0 : lit > n ? n : lit;
        br = ((int32_t)t == mine ? 120u : 80u) + (uint32_t)lit * ((int32_t)t == mine ? 136u : 150u) / (uint32_t)n;
        cv_begin((uint32_t)lw, (uint32_t)lh, C_BLACK);
        for (k = 0; k < n; k++) {
            uint16_t c = RGB(CITY_RGB[t][0] / 9u, CITY_RGB[t][1] / 9u, CITY_RGB[t][2] / 9u);     /* unlit: its colour, dark */
            if (k < lit)
                c = white ? C_WHITE : RGB(CITY_RGB[t][0] * br >> 8, CITY_RGB[t][1] * br >> 8, CITY_RGB[t][2] * br >> 8);
            cv_rect(0, lh - 3 * (k + 1) + 1, lw, 2, c);
        }
        cv_blit((uint32_t)lx, (uint32_t)ly);
    }
    for (side = 0; side < 2u; side++) {      /* the master along the kerb: only the lights that changed */
        const float v = mst_pk[side];
        float db;
        uint32_t lit, j;
        mst_pk[side] = 0.0f;
        city.mv[side] = v > city.mv[side] ? v : city.mv[side] * 0.80f;
        db = city.mv[side] > 1e-5f ? 6.0206f * fm_log2f(city.mv[side]) : -100.0f;
        lit = db <= -42.0f ? 0u : (uint32_t)((db + 42.0f) * (float)CITY_EDGE / 41.0f + 0.5f);    /* -42 dB .. -1 dB */
        lit = lit > CITY_EDGE ? CITY_EDGE : lit;
        for (j = 0; j < CITY_EDGE; j++) {
            const uint32_t on = j < lit, was = city.edge[side] != 255u && j < city.edge[side];
            int32_t ex, ey, ew;
            if (on == was && city.edge[side] != 255u)
                continue;
            city_edge_at(side, j, &ex, &ey, &ew);
            /* green far off, yellow nearer, the last three red */
            cv_begin((uint32_t)ew, (uint32_t)ew, !on ? RGB(52, 52, 52) : j >= CITY_EDGE - 3u ? RGB(255, 40, 40) :
                     j >= CITY_EDGE - 7u ? RGB(255, 225, 40) : RGB(60, 255, 70));
            cv_blit((uint32_t)ex, (uint32_t)ey);
        }
        city.edge[side] = (uint8_t)lit;
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
