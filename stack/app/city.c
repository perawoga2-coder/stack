/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the city view. Written for STACK. Part of the unity build: included by app.c.
 *
 * A city at night seen from the middle of its street, black and white, the buildings on both sides running to the
 * point where the street ends. The only colour is its eight signs, one a track: each a level meter (green, yellow,
 * red at the top; a dot that holds the peak), the selected track's a little brighter, the looper's purple.
 *
 * Nothing is stored: the city is worked out. A building's face is drawn two pixels at a time, its roof and its rows
 * of windows on lines that all meet at the street's end (240 x 240, the point at 120, 96). It is drawn once when the
 * view opens (three times, brighter each: it fades in); after that only the eight signs are drawn again, each in a
 * small canvas of its own, twenty times a second. It runs in the main loop, where the screen always is: the sound
 * and the keys are not held up by it (the tracks' levels come from the audio interrupt as one number each). */

#define CITY_VX 120
#define CITY_VY 96
static struct {
    uint8_t stage;                           /* 0 closed; 1 .. 3 fading in; 4 up */
    uint8_t manual;                          /* opened by HOME (it stays while stopped) */
    uint32_t t;                              /* the last frame's time */
    float lv[NTRK], hold[NTRK];
    uint32_t hold_t[NTRK];
} city;

/* the signs: T1 near left, T2 near right, T3 and T4 behind them, and so on down the street */
static const struct { uint8_t x, y, w, h; } CITY_SIGN[4] = {{20, 56, 16, 88}, {58, 69, 11, 60}, {82, 79, 8, 40}, {98, 85, 6, 24}};

static inline uint16_t city_gray(uint32_t v, uint32_t f) { v = v * f >> 8; return RGB(v, v, v); }
static inline uint32_t city_hash(uint32_t a) { a ^= a >> 7; a *= 0x9E3779B1u; return a ^ a >> 15; }

static void city_sign_box(uint32_t t, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    const uint32_t d = t >> 1;
    *w = CITY_SIGN[d].w;
    *h = CITY_SIGN[d].h;
    *y = CITY_SIGN[d].y;
    *x = t & 1u ? 240 - CITY_SIGN[d].x - *w : CITY_SIGN[d].x;
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
    for (t = 0; t < (int32_t)NTRK; t++) {                              /* the signs' frames, and whose each is */
        int32_t bx, by, bw, bh;
        char n[4];
        city_sign_box((uint32_t)t, &bx, &by, &bw, &bh);
        cv_rect(bx - 2, by - 2 - y0, bw + 4, bh + 4, city_gray(60, f));
        cv_rect(bx - 1, by - 1 - y0, bw + 2, bh + 2, C_BLACK);
        if (t < 4) {
            int_text(t + 1, n);
            cv_text(bx + (bw - text_w(&FONT_T, n)) / 2, by - 13 - y0, &FONT_T, n, city_gray(220, f));
        }
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

static void city_signs(uint32_t now)         /* the eight meters, each as its track sounds now */
{
    uint32_t t;
    for (t = 0; t < NTRK; t++) {
        int32_t bx, by, bw, bh, n, lit, pk, k;
        const uint32_t sel = t == ui.sel, loop = t == NTRK - 1u && lp.own;
        float v = trk_pk[t], db;
        trk_pk[t] = 0.0f;
        city.lv[t] = v > city.lv[t] ? v : city.lv[t] * 0.80f;        /* up at once, down over a few frames */
        if (city.lv[t] >= city.hold[t] || now - city.hold_t[t] > 900u) {
            city.hold[t] = city.lv[t];
            city.hold_t[t] = now;
        }
        city_sign_box(t, &bx, &by, &bw, &bh);
        n = bh / 4;                                                  /* segments of 3 px and a gap */
        db = city.lv[t] > 1e-5f ? 6.0206f * fm_log2f(city.lv[t]) : -100.0f;
        lit = (int32_t)((db + 45.0f) * (float)n / 42.0f);            /* -45 dB .. -3 dB up the sign */
        lit = lit < 0 ? 0 : lit > n ? n : lit;
        db = city.hold[t] > 1e-5f ? 6.0206f * fm_log2f(city.hold[t]) : -100.0f;
        pk = (int32_t)((db + 45.0f) * (float)n / 42.0f);
        pk = pk > n ? n : pk;
        cv_begin((uint32_t)bw, (uint32_t)bh, C_BLACK);
        for (k = 0; k < n; k++) {
            /* brighter the higher the level, and the selected track's brighter than the others */
            const uint32_t br = (sel ? 150u : 96u) + (uint32_t)lit * (sel ? 105u : 110u) / (uint32_t)n;
            uint16_t c = RGB(18, 18, 22);
            if (k < lit) {
                if (k >= n - n / 6 - 1)
                    c = RGB(br, br / 6u, br / 8u);                   /* red at the top */
                else if (k >= n * 2 / 3)
                    c = RGB(br, br * 4u / 5u, br / 10u);             /* yellow */
                else if (loop)
                    c = RGB(br * 3u / 4u, br / 4u, br);              /* the looper's: purple */
                else
                    c = RGB(br / 8u, br, br / 4u);                   /* green */
            }
            if (k == pk - 1 && pk > 0)
                c = C_WHITE;                                         /* the peak, held */
            cv_rect(1, bh - 4 * (k + 1) + 1, bw - 2, 3, c);
        }
        cv_blit((uint32_t)bx, (uint32_t)by);
    }
}

static void city_open(uint32_t manual)
{
    uint32_t t;
    if (city.stage)
        return;
    for (t = 0; t < NTRK; t++) {
        city.lv[t] = city.hold[t] = 0.0f;
        trk_pk[t] = 0.0f;
    }
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
    city_signs(now);
}
