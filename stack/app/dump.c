/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the dump for the Burger Studio app's VACUUM. Written for STACK. Part of the unity build: included by app.c.
 *
 * The app asks, piece by piece, and STACK answers each piece: there is no mode and no button, and it works on any
 * page, playing or not. Three messages (docs/PROTOCOL.md), each answered with the same command:
 *   30 SONG  piece        the song as it is NOW (not as SAVE last kept it): DUMP_PIECE bytes of a record made when
 *                         piece 0 is asked for, so all its pieces are of one moment
 *   31 LOOP INFO          is there a loop on track 8, its rate, its samples, its CRC, and a number that changes
 *                         whenever the loop does
 *   32 LOOP DATA piece    DUMP_PIECE bytes of the loop's samples (16 bits, low byte first), with that number
 * Nothing is written anywhere: a dump only reads. An answer goes out only when the USB queue has room for all of it;
 * else it waits its turn in the main loop (dump_frame) and the sound and the screen go on. One answer waits at most:
 * the app asks for the next piece when it has the last. */

#define DUMP_PIECE 128u                      /* bytes a piece (147 once packed 7 to 8) */
#define DUMP_VER 1u
#define DUMP_HEAD 32u
#define DUMP_TRK 96u
#define DUMP_LEN (DUMP_HEAD + NTRK * DUMP_TRK)
static uint8_t dump_song[DUMP_LEN];          /* the record, as of the last "piece 0" */
static struct {
    uint8_t out[16u + (DUMP_PIECE * 8u + 6u) / 7u + 2u];
    uint32_t n;                              /* an answer that waits for room (0: none) */
    uint32_t t;                              /* the last message of a dump (the screen says SENDING TO BURGER) */
    uint32_t held, sent;                     /* (the simulator's checks: answers that had to wait; answers sent) */
#ifdef PLAT_HOST
    uint32_t crc, next;                      /* (the simulator's check: the CRC of the loop's pieces as they went, in order) */
#endif
} dump;

static uint32_t dump_crc(uint32_t c, const uint8_t *p, uint32_t n)   /* the usual CRC-32, carried on (start: 0xFFFFFFFF) */
{
    static const uint32_t T[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                   0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                   0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) {
        c ^= *p++;
        c = T[c & 15u] ^ (c >> 4);
        c = T[c & 15u] ^ (c >> 4);
    }
    return c;
}

/* the song as it is now, into dump_song. Its layout is docs/PROTOCOL.md's: a head of 32 bytes, then 96 a track */
static void dump_make(void)
{
    static const char *const KIND[4] = {"", "", "MY", "LOOP"};
    uint32_t t, i;
    uint8_t *h = dump_song;
    memset(dump_song, 0, sizeof dump_song);
    put32(h, 0x444B5453u);                   /* "STKD" */
    h[4] = DUMP_VER;
    h[5] = (uint8_t)NTRK;
    h[6] = (uint8_t)DUMP_TRK;
    h[7] = (uint8_t)((lp.has ? 1u : 0u) | (seq_play_req ? 2u : 0u) | (ck_follow ? 4u : 0u));
    h[8] = (uint8_t)DUMP_LEN;
    h[9] = (uint8_t)(DUMP_LEN >> 8);
    h[16] = (uint8_t)clamp((int32_t)(44100.0f * 15.0f / (ck_tick_frames() * 6.0f) + 0.5f), 20, 255);
    h[17] = seq_sig;
    h[18] = seq_len;
    h[19] = seq_swing;
    h[20] = ui_sel_isr;
    h[21] = lp.layers;
    for (i = 0; APP_VERSION[i] && i < 10u; i++)
        h[22u + i] = (uint8_t)APP_VERSION[i];
    for (t = 0; t < NTRK; t++) {
        uint8_t *p = dump_song + DUMP_HEAD + t * DUMP_TRK, *q = p + 28;
        const uint32_t li = trk[t].sound;
        const sound_t *s = list_sound(li);
        const char *nm = "", *cn = "";
        uint32_t kind = 0;
        p[1] = 255;
        if (t == NTRK - 1u && lp.own) {
            kind = 3;
            nm = "LOOP";
        } else if (li >= 1u && li <= NSOUNDS) {
            kind = 1;
            p[1] = s->cat;
            put32(p + 4, name_hash(s->name));
            nm = s->name;
            cn = CAT_NAME[s->cat];
        } else if (s) {                      /* a sample of the app's that is there */
            kind = 2;
            p[2] = (uint8_t)(li - NSOUNDS);
            put32(p + 8, my_crc_of(li - NSOUNDS - 1u));
            nm = my[li - NSOUNDS - 1u].name;
        }
        p[0] = (uint8_t)kind;
        p[3] = (uint8_t)(s && kind != 3u ? ((s->flags & SF_DRUM) ? 1u : 0u) | ((s->flags & SF_LOOP) ? 2u : 0u) : 0u);
        if (kind >= 2u)
            cn = KIND[kind];
        for (i = 0; nm[i] && i < 15u; i++)
            p[12u + i] = (uint8_t)nm[i];
        for (i = 0; cn[i] && i < 7u; i++)
            p[88u + i] = (uint8_t)cn[i];
#define F(x, lo, hi) *q++ = (uint8_t)(x);
        SONG_FIELDS(trk[t], F)
#undef F
        for (i = 0; i < NSTEP; i++) {
            p[56u + i] = step_on[t][i] ? 1u : 0u;
            p[72u + i] = step_note[t][i];
        }
    }
    put32(h + 10, ~dump_crc(0xFFFFFFFFu, dump_song + 16, DUMP_LEN - 16u));
}

static void dump_put(uint32_t v) { dump.out[dump.n++] = (uint8_t)(v & 0x7Fu); }
static void dump_pack(const uint8_t *p, uint32_t n)      /* 8-bit bytes as the app's messages carry them: 7 to 8 */
{
    uint32_t i, j;
    for (i = 0; i < n; i += 7u) {
        uint32_t hi = 0, m = n - i < 7u ? n - i : 7u;
        for (j = 0; j < m; j++)
            hi |= (uint32_t)(p[i + j] >> 7) << j;
        dump_put(hi);
        for (j = 0; j < m; j++)
            dump_put(p[i + j]);
    }
}
static void dump_go(void)                    /* the answer in dump.out: out now if all of it fits, else it waits */
{
    dump.out[dump.n++] = 0xF7;
    if (plat_sysex_room() >= dump.n && !plat_sysex_send(dump.out, dump.n)) {
        dump.n = 0;
        dump.sent++;
    } else {
        dump.held++;
    }
}

static void dump_frame(void)                 /* every frame: an answer that waited goes when there is room */
{
    if (dump.n && plat_sysex_room() >= dump.n && !plat_sysex_send(dump.out, dump.n)) {
        dump.n = 0;
        dump.sent++;
    }
    if (dump.n && plat_ms() - dump.t > 3000u)
        dump.n = 0;                          /* (nobody reads: let go) */
}

static void dump_sysex(uint32_t cmd, const uint8_t *a, uint32_t na)
{
    const uint32_t piece = na >= 2u ? (uint32_t)a[0] | (uint32_t)a[1] << 7 : 0u;
    if (dump.n)                              /* (the last answer still waits: the app asks again when it has it) */
        return;
    dump.t = plat_ms() | 1u;
    dump.out[0] = 0xF0;
    dump.out[1] = 0x7D;
    dump.out[2] = 0x46;
    dump.out[3] = 0x4C;
    dump.out[4] = (uint8_t)cmd;
    dump.n = 5;
    if (cmd == 30u) {                        /* SONG piece: piece, pieces, then its bytes */
        const uint32_t pieces = (DUMP_LEN + DUMP_PIECE - 1u) / DUMP_PIECE, off = piece * DUMP_PIECE;
        if (!piece)
            dump_make();
        dump_put(piece);
        dump_put(piece >> 7);
        dump_put(pieces);
        dump_put(pieces >> 7);
        if (off < DUMP_LEN)
            dump_pack(dump_song + off, DUMP_LEN - off < DUMP_PIECE ? DUMP_LEN - off : DUMP_PIECE);
    } else {
        /* the loop that plays: the samples of its block, 16 bits each. Read here while the audio interrupt plays it:
         * the number lp.ver says if it was another by the time the app has all the pieces */
        const uint32_t has = lp.has && !lp.fading, len = has ? lp.total / (lp.div ? lp.div : 2u) : 0u, ver = lp.ver & 0x3FFFu;
        const uint8_t *d = (const uint8_t *)loop_m[lp.cur];
        if (cmd == 31u) {                    /* LOOP INFO: has, rate, samples, layers, its number, its CRC */
            const uint32_t crc = has ? ~dump_crc(0xFFFFFFFFu, d, len * 2u) : 0u;
            uint8_t c4[4];
            dump_put(has);
            dump_put(lp.div == 4u ? 1u : 0u);            /* 0: 22 050 Hz, 1: 11 025 Hz */
            dump_put(len);
            dump_put(len >> 7);
            dump_put(len >> 14);
            dump_put(lp.layers);
            dump_put(ver);
            dump_put(ver >> 7);
            put32(c4, crc);
            dump_pack(c4, 4);
        } else {                             /* LOOP DATA piece: piece, its number, then its bytes */
            const uint32_t off = piece * DUMP_PIECE, n = off < len * 2u ? (len * 2u - off < DUMP_PIECE ? len * 2u - off : DUMP_PIECE) : 0u;
            dump_put(piece);
            dump_put(piece >> 7);
            dump_put(ver);
            dump_put(ver >> 7);
            if (n)
                dump_pack(d + off, n);
#ifdef PLAT_HOST
            if (!piece) {
                dump.crc = 0xFFFFFFFFu;
                dump.next = 0;
            }
            if (piece == dump.next && n) {
                dump.crc = dump_crc(dump.crc, d + off, n);
                dump.next++;
            }
#endif
        }
    }
    dump_go();
}
