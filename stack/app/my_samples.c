/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip */
/* STACK: the three slots for samples sent by the Burger Studio app, MY 1 .. MY 3 (the app calls them USR1 .. USR3).
 * Written for STACK from the app's own side of the protocol (the app is Maker Hip's: Fm1SampleLink.kt says what it
 * sends, what it expects back and how a slot is laid out). Nothing here comes from Felucca or from the Burger Studio
 * firmware. Part of the unity build: included by app.c.
 *
 * A slot: 80 KiB of the flash at MY_BASE + slot * MY_SIZE (the same places the Burger Studio firmware uses, so a
 * sample stays through a change of firmware): a 480-byte header at its start, then IMA ADPCM (4 bits a sample, mono,
 * low nibble first, each zone from predictor 0 and step index 0) from byte 512. A sample may run on over the next one
 * or two slots (the header's span). The last 4 KiB of the last slot a sample uses is never written here.
 *   header: "FSMP" u32, version u16 (1), zones u8 (1..16), span u8 (0 or 1: one slot; 2, 3), name 8 chars,
 *           data bytes u32 at 16, the data's CRC-32 u32 at 20, then at 32 the zones, 28 bytes each:
 *           data offset u32, samples u32, loop start u32, loop end u32, rate u32 (x 65536 / 44100), root u16 (note x 16),
 *           predictor u16 and step index u8 at the loop start, lowest note u8, highest note u8, looped u8.
 * The app's messages over USB MIDI: F0 7D 46 4C cmd args F7, every byte 7 bits; data packed 7 to 8 (a byte of the
 * top bits, then up to seven bytes). Every message is answered with the same cmd:
 *   11 BEGIN  slot                      -> slot, rc      the slot is empty from now on (its first sector erased)
 *   12 WRITE  slot, offset (3 x 7 bits), data (<= 256 bytes, packed)  -> slot, offset, rc
 *                                                        offsets from 512, in order; a 4 KiB boundary erases its sector
 *   13 END    slot, header (480 bytes, packed)  -> slot, rc   checked (size, header, CRC, zones), then written: the commit
 *   14 ERASE  slot                      -> slot, rc      the slot empty
 *   15 INFO                             -> 3, 80, then a slot: zones (0 empty), name, 0, data KiB; then a slot: the
 *                                          slots its sample takes, or 0x40 + the slot whose sample covers it
 *   112 SHOW  slot, track, mode, flags  -> slot, rc (0 there, 2 empty)   (STACK places a sample by its own rule, at END)
 *   114 HELLO                           -> "STACK <version>", 0, capabilities (0)
 * rc of WRITE: 0 ok, 1 arguments, 2 erase, 3 write, 4 not begun. rc of END: 0 ok, 1 size, 2 header, 3 CRC, 4 flash,
 * 5 zones.
 *
 * The flash is only ever touched inside [MY_BASE, MY_BASE + 3 * MY_SIZE): my_erase and my_write refuse anything else.
 * The sound stops for each erase (fm1-base's plat_data_erase); a voice playing a slot is ended before the slot is erased. */

#define MY_BASE 0xA0000u
#define MY_SIZE 0x14000u
#define MY_END (MY_BASE + NMY * MY_SIZE)
#define MY_DATA 512u
#define MY_HDR 480u
#define MY_ZONES 16u
#define MY_MAGIC 0x504D5346u             /* "FSMP" */
#define MY_TARGET_DB (-15.5f)            /* the built-in sounds' loudness (tools/make_sounds.py TARGET_DB) */
#define MY_GAIN_FIRST 0.45f              /* until a slot's loudness has been measured */

typedef struct {
    volatile uint8_t valid;              /* a sample is here and may be played */
    uint8_t nz, span;
    uint8_t cov;                         /* 1 + the slot whose sample runs over this one (0: none) */
    uint8_t measured;
    uint8_t why;                         /* why it is not valid: MY_BLANK nothing there, MY_NOFLASH, or my_parse's 2 / 5 */
    uint8_t crc;                         /* its data against its header's CRC: 0 not checked yet, 1 the same, 2 not */
    uint32_t head;                       /* the header's first four bytes as found (shown when it is refused) */
    char name[9];
    uint32_t bytes;
    volatile float gain;                 /* brings it to the built-in sounds' loudness */
    sound_t z[MY_ZONES];
    uint8_t lo[MY_ZONES], hi[MY_ZONES];
    uint16_t ckz[MY_ZONES];              /* each zone's first decoder state in my_ck (made with the loudness) */
} my_slot_t;
/* the decoder's state every SND_CK samples of the app's samples, as the built-in sounds have theirs in the flash
 * (sound_t.ck: the EDIT page). Made while a slot's loudness is measured; until then the slot plays as it is.
 * A slot's states start at MY_CKN times its number; a sample over several slots has that many slots' room */
#define MY_CKN (MY_SIZE * 2u / SND_CK + MY_ZONES)
static uint8_t my_ck[NMY * MY_CKN * 3u] __attribute__((section(".pool")));   /* 3 KB */
static my_slot_t my[NMY];
static volatile uint8_t my_kill;         /* bit k: the audio interrupt ends slot k's voices (then clears it) */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static uint32_t my_cap(uint32_t k, uint32_t span)   /* the data bytes a sample from slot k over span slots may have */
{
    (void)k;
    return span * MY_SIZE - 0x1000u - MY_DATA;
}

/* the header at p read into slot k (the data is at data): 0 fine, 2 the header, 5 its zones (END's codes) */
static int my_parse(uint32_t k, const uint8_t *p, const uint8_t *data, my_slot_t *m)
{
    uint32_t nz = p[6], span = p[7] >= 2u ? p[7] : 1u, bytes = rd32(p + 16), i;
    if (rd32(p) != MY_MAGIC || rd16(p + 4) != 1u || nz < 1u || nz > MY_ZONES || k + span > NMY || !bytes ||
        bytes > my_cap(k, span))
        return 2;
    for (i = 0; i < nz; i++) {
        const uint8_t *z = p + 32u + 28u * i;
        uint32_t off = rd32(z), n = rd32(z + 4), ls = rd32(z + 8), le = rd32(z + 12), rate = rd32(z + 16);
        sound_t *s = &m->z[i];
        if (!n || off > bytes || (n + 1u) / 2u > bytes - off || rate < 1000u || rate > 65536u || z[25] > z[26] ||
            z[26] > 127u || z[24] > 88u)
            return 5;
        s->name = m->name;
        s->d = data + off;
        s->n = n;
        s->ls = s->le = 0;
        s->rate = (float)rate * (44100.0f / 65536.0f);
        s->hz = 440.0f * fm_exp2f(((float)rd16(z + 20) * (1.0f / 16.0f) - 69.0f) * (1.0f / 12.0f));
        s->flags = z[25] == z[26] ? SF_DRUM : 0u;       /* a zone of one key: a hit */
        s->cat = NCAT;
        s->lpred = 0;
        s->lidx = 0;
        s->ck = 0;
        if (z[27] && le > ls && le < n) {
            s->flags |= SF_LOOP;
            s->ls = ls;
            s->le = le + 1u;
            s->lpred = (int16_t)rd16(z + 22);
            s->lidx = z[24];
        }
        m->lo[i] = z[25];
        m->hi[i] = z[26];
    }
    for (i = 0; i < 8u; i++)
        m->name[i] = p[8 + i] >= 32u && p[8 + i] < 127u ? (char)p[8 + i] : 0;
    m->name[8] = 0;
    m->nz = (uint8_t)nz;
    m->span = (uint8_t)span;
    m->bytes = bytes;
    return 0;
}

/* what the flash holds, read again: the slots with a sample, and those another's sample runs over */
/* A slot is taken by its header alone (as the app's END left it): the data's CRC is not a condition here. It is
 * checked afterwards, a little each frame (my_measure), and a mismatch only marks the name ("?"). 0.2.1-test checked
 * the whole data's CRC at power-on, before anything else ran, and refused the slot on a mismatch: on the FM-1 every
 * slot was refused after a power cycle. The scan is also run again after the start (my_rescan). */
#define MY_BLANK 1u
#define MY_NOFLASH 9u
#ifdef PLAT_HOST
static uint8_t host_blind;                   /* a test: the next scans see no flash, as if it could not be read yet */
#endif
static void my_ck_set(uint32_t k)            /* a measured slot: its zones know their states */
{
    uint32_t i;
    for (i = 0; my[k].measured && i < my[k].nz; i++)
        my[k].z[i].ck = my_ck + 3u * my[k].ckz[i];
}

static void my_load_all(void)
{
    uint32_t k, j;
    for (k = 0; k < NMY; k++) {
        const uint8_t *p = plat_data(MY_BASE + k * MY_SIZE);
        uint8_t was = my[k].valid;
        float g = my[k].gain;
        uint32_t b = my[k].bytes;
        int rc;
#ifdef PLAT_HOST
        if (host_blind)
            p = 0;
#endif
        my[k].valid = 0;
        my[k].cov = 0;
        my[k].why = 0;
        for (j = 0; j < k; j++)
            if (my[j].valid && j + my[j].span > k)
                my[k].cov = (uint8_t)(j + 1u);
        if (my[k].cov)
            continue;
        if (!p) {
            my[k].why = MY_NOFLASH;
            continue;
        }
        my[k].head = rd32(p);
        for (j = 0; j < 32u && p[j] == 0xFFu; j++)
            ;
        if (j == 32u) {                          /* erased: nothing was ever committed here */
            my[k].why = MY_BLANK;
            continue;
        }
        if ((rc = my_parse(k, p, p + MY_DATA, &my[k])) != 0) {
            my[k].why = (uint8_t)rc;
            continue;
        }
        if (!was || b != my[k].bytes) {          /* a new sample: its loudness is measured again, its CRC checked */
            my[k].gain = MY_GAIN_FIRST;
            my[k].measured = 0;
            my[k].crc = 0;
        } else {
            my[k].gain = g;
            my_ck_set(k);                        /* (the same sample: its states are still there) */
        }
        my[k].valid = 1;
    }
}

/* the zone of slot k that note plays, its gain with it; 0 = none (an empty slot, or a kit's key with no hit) */
static const sound_t *my_sound(uint32_t k, uint32_t note, float *gain)
{
    uint32_t i;
    if (k >= NMY || !my[k].valid)
        return 0;
    for (i = 0; i < my[k].nz; i++)
        if (note >= my[k].lo[i] && note <= my[k].hi[i]) {
            *gain = my[k].gain;
            return &my[k].z[i];
        }
    return 0;
}

/* --- the flash: only ever inside the three slots --- */
static int my_erase(uint32_t off)
{
    if (off < MY_BASE || off >= MY_END || (off & 0xFFFu))
        return -1;
    return plat_data_erase(off);
}
static int my_write(uint32_t off, const void *src, uint32_t n)
{
    if (off < MY_BASE || n > MY_END - MY_BASE || off > MY_END - n)
        return -1;
    return plat_data_write(off, src, n);
}

static void my_voices_end(uint32_t mask);    /* (app.c: after the voices) */

/* slot k is about to change: no new voice takes it, and those playing it end before the flash under them goes */
static void my_let_go(uint32_t k)
{
    uint32_t mask = 1u << k, j;
    my[k].valid = 0;
    for (j = 0; j < NMY; j++)                    /* (and a sample that ran over it, or that it ran over) */
        if ((my[j].valid && j < k && j + my[j].span > k) || (j > k && my[j].cov == k + 1u)) {
            my[j].valid = 0;
            mask |= 1u << j;
        }
    my_voices_end(mask);
}

/* --- the app's messages --- */
static struct {
    uint8_t on, slot;
    uint32_t next;                       /* the next offset expected */
    uint32_t erased;                     /* the slot's sectors erased so far end here (an offset) */
    uint32_t t;                          /* the last message, plat_ms */
    uint32_t said;                       /* KiB at the last progress message */
} rx;
static volatile uint8_t my_event;        /* 1 + the slot whose sample has just come in (the UI places and names it) */
static uint8_t my_failed;                /* 1 + the slot whose transfer stopped half way (the UI says so) */

static struct { uint8_t b[96]; uint32_t n; } rp;
static void rp_begin(uint32_t cmd)
{
    rp.b[0] = 0xF0;
    rp.b[1] = 0x7D;
    rp.b[2] = 0x46;
    rp.b[3] = 0x4C;
    rp.b[4] = (uint8_t)cmd;
    rp.n = 5;
}
static void rp_b(uint32_t v)
{
    if (rp.n < sizeof rp.b - 1u)
        rp.b[rp.n++] = (uint8_t)(v & 0x7Fu);
}
static void rp_send(void)
{
    rp.b[rp.n++] = 0xF7;
    plat_sysex_send(rp.b, rp.n);
}

static uint32_t unpack7(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t max)
{
    uint32_t i = 0, o = 0, j;
    while (i < n) {
        uint32_t hi = s[i++];
        for (j = 0; j < 7u && i < n; j++, i++) {
            if (o >= max)
                return max + 1u;
            d[o++] = (uint8_t)(s[i] | ((hi >> j) & 1u) << 7);
        }
    }
    return o;
}

static uint32_t crc32_of(const uint8_t *p, uint32_t n)      /* the usual CRC-32 (reflected, 0xEDB88320) */
{
    static const uint32_t T[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                   0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                   0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        c = T[c & 15u] ^ (c >> 4);
        c = T[c & 15u] ^ (c >> 4);
    }
    return ~c;
}

/* --- MUTE: the FM-1's own output silent while the app plays its sound (app.h APP_GAIN). It must never stay silent
 * by mistake: it sounds again when the app says so, when the app has not asked again for MUTE_KEEP_MS, when the
 * computer is gone or another one comes (plat_usb_session), and at every start --- */
#define MUTE_KEEP_MS 3000u
volatile uint8_t stack_mute;
static struct { uint32_t t, session; uint8_t shown; } mute;
static void mute_set(uint32_t on)
{
    mute.t = plat_ms();
    mute.session = plat_usb_session();
    stack_mute = (uint8_t)(on && mute.session);
}
static uint32_t mute_frame(void)             /* each frame; 1: it changed (the screen says so) */
{
    if (stack_mute && (plat_usb_session() != mute.session || plat_ms() - mute.t >= MUTE_KEEP_MS))
        stack_mute = 0;
    if (mute.shown == stack_mute)
        return 0;
    mute.shown = stack_mute;
    return 1;
}

static uint8_t my_buf[MY_HDR + 32u];     /* a message's data, unpacked (main loop) */

static void my_progress(void);           /* (app.c: the screen says how far the sample has come) */

int app_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t cmd, k, base, na;
    const uint8_t *a;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x46u || p[2] != 0x4Cu)
        return 0;
    cmd = p[3];
    a = p + 4;
    na = n - 4u;
    k = na ? a[0] : 0;
    base = MY_BASE + k * MY_SIZE;
    if (cmd == 15u) {                            /* INFO */
        rp_begin(15);
        rp_b(NMY);
        rp_b(MY_SIZE / 1024u);
        for (k = 0; k < NMY; k++) {
            uint32_t i;
            rp_b(my[k].valid ? my[k].nz : 0u);
            for (i = 0; my[k].valid && my[k].name[i]; i++)
                rp_b((uint8_t)my[k].name[i]);
            rp_b(0);
            rp_b(my[k].valid ? (my[k].bytes / 1024u > 127u ? 127u : my[k].bytes / 1024u) : 0u);
        }
        for (k = 0; k < NMY; k++)
            rp_b(my[k].cov ? 0x40u | (uint32_t)(my[k].cov - 1u) : my[k].valid ? my[k].span : 1u);
        rp_send();
    } else if (cmd == 114u) {                    /* HELLO */
        static const char W[] = APP_NAME " " APP_VERSION;
        rp_begin(114);
        for (k = 0; W[k]; k++)
            rp_b((uint8_t)W[k]);
        rp_b(0);
        rp_b(0);
        rp_send();
    } else if (cmd == 11u || cmd == 14u) {       /* BEGIN, ERASE: the slot empty */
        uint32_t rc = 1;
        if (na >= 1u && k < NMY) {
            uint32_t owner = my[k].cov;          /* another slot's sample runs over this one: that sample goes too */
            my_let_go(k);
            rc = my_erase(base) ? 2u : 0u;
            if (owner && my_erase(MY_BASE + (owner - 1u) * MY_SIZE))
                rc = 2;
            rx.on = (uint8_t)(cmd == 11u && !rc);
            rx.slot = (uint8_t)k;
            rx.next = MY_DATA;
            rx.erased = 0x1000u;
            rx.t = plat_ms();
            rx.said = 0;
            my_load_all();
            if (rx.on)
                my_progress();
        }
        rp_begin(cmd);
        rp_b(k);
        rp_b(rc);
        rp_send();
    } else if (cmd == 12u) {                     /* WRITE */
        uint32_t rc = 1, off = 0, len;
        if (na >= 4u) {
            off = (uint32_t)a[1] | (uint32_t)a[2] << 7 | (uint32_t)a[3] << 14;
            len = unpack7(a + 4, na - 4u, my_buf, 256u);
            if (!rx.on || k != rx.slot)
                rc = 4;
            else if (!len || len > 256u || off != rx.next || (off & 255u) ||
                     off + len > (NMY - k) * MY_SIZE - 0x1000u)
                rc = 1;
            else {
                rc = 0;
                while (!rc && off + len > rx.erased) {       /* into a new sector: it is erased first */
                    if (rx.erased >= MY_SIZE && !(rx.erased % MY_SIZE))
                        my_let_go(k + rx.erased / MY_SIZE);   /* (running on over the next slot: its sample goes) */
                    rc = my_erase(base + rx.erased) ? 2u : 0u;
                    rx.erased += 0x1000u;
                }
                if (!rc)
                    rc = my_write(base + off, my_buf, len) ? 3u : 0u;
                if (!rc) {
                    rx.next = off + len;
                    rx.t = plat_ms();
                    if ((rx.next - MY_DATA) / 1024u >= rx.said + 8u) {
                        rx.said = (rx.next - MY_DATA) / 1024u;
                        my_progress();
                    }
                } else {
                    rx.on = 0;
                    my_failed = (uint8_t)(k + 1u);
                }
            }
        }
        rp_begin(12);
        rp_b(k);
        rp_b(off);
        rp_b(off >> 7);
        rp_b(off >> 14);
        rp_b(rc);
        rp_send();
    } else if (cmd == 13u) {                     /* END: the header checked against what came, then written */
        uint32_t rc = 1;
        if (na >= 1u && k < NMY && rx.on && k == rx.slot) {
            const uint8_t *data = plat_data(base + MY_DATA);
            static my_slot_t tmp;
            if (unpack7(a + 1, na - 1u, my_buf, MY_HDR) != MY_HDR)
                rc = 1;
            else if (!data || (rc = (uint32_t)my_parse(k, my_buf, data, &tmp)) != 0u)
                ;
            else if (tmp.bytes != rx.next - MY_DATA)
                rc = 1;
            else if (crc32_of(data, tmp.bytes) != rd32(my_buf + 20))
                rc = 3;
            else
                rc = my_write(base, my_buf, MY_HDR) ? 4u : 0u;
            rx.on = 0;
            my_load_all();
            if (!rc && my[k].valid)
                my_event = (uint8_t)(k + 1u);
            else
                my_failed = (uint8_t)(k + 1u);
        }
        rp_begin(13);
        rp_b(k);
        rp_b(rc);
        rp_send();
    } else if (cmd == 20u) {                     /* MUTE: 1 the FM-1's own output silent (asked again at least every
                                                  * MUTE_KEEP_MS, or it sounds again), 0 it sounds; else just asked */
        if (na >= 1u && a[0] <= 1u)
            mute_set(a[0]);
        rp_begin(20);
        rp_b(stack_mute);
        rp_b(MUTE_KEEP_MS / 100u);
        rp_send();
    } else if (cmd == 112u) {                    /* SHOW: STACK placed it already (END); say whether it is there */
        rp_begin(112);
        rp_b(k);
        rp_b(k < NMY && my[k].valid ? 0u : 2u);
        rp_send();
    }
    return 1;                                    /* (the editor's other commands are not STACK's: no answer) */
}

/* --- a slot's loudness, measured a little each frame: the ITU-R BS.1770 K-weighting, the loudest 100 ms, as
 * tools/make_sounds.py measures the built-in sounds; its gain then brings it to theirs --- */
static struct {
    int8_t slot;                         /* -1: idle */
    uint8_t z;
    uint32_t i;                          /* the zone's next sample */
    int32_t pred, idx;
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;        /* the shelf */
    float c1, c2, u1, u2, v1, v2;                    /* the high-pass (b: 1 -2 1) */
    float acc, ring[10], peak, best;
    uint32_t an, blk, ri, rn;
    uint32_t ck;                         /* the next decoder state's place in my_ck */
} ms = {.slot = -1};

static void ms_zone(const sound_t *s)        /* the filters for this zone's rate, from rest */
{
    float K = fm_tanf(3.14159265f * 1681.97445f / s->rate), Q = 0.70717524f, Vh = 1.58486470f, Vb = 1.25872093f, a0;
    a0 = 1.0f + K / Q + K * K;
    ms.b0 = (Vh + Vb * K / Q + K * K) / a0;
    ms.b1 = 2.0f * (K * K - Vh) / a0;
    ms.b2 = (Vh - Vb * K / Q + K * K) / a0;
    ms.a1 = 2.0f * (K * K - 1.0f) / a0;
    ms.a2 = (1.0f - K / Q + K * K) / a0;
    K = fm_tanf(3.14159265f * 38.1354709f / s->rate);
    Q = 0.50032704f;
    a0 = 1.0f + K / Q + K * K;
    ms.c1 = 2.0f * (K * K - 1.0f) / a0;
    ms.c2 = (1.0f - K / Q + K * K) / a0;
    ms.x1 = ms.x2 = ms.y1 = ms.y2 = ms.u1 = ms.u2 = ms.v1 = ms.v2 = 0.0f;
    ms.i = 0;
    ms.pred = 0;
    ms.idx = 0;
    ms.acc = 0.0f;
    ms.an = 0;
    ms.blk = (uint32_t)(s->rate * 0.01f);            /* 10 ms */
    ms.ri = ms.rn = 0;
}

/* a slot's data against its header's CRC, a piece each frame: it only marks the slot (my[k].crc) */
static struct { int8_t slot; uint32_t at, c; } mc = {.slot = -1};
static void my_crc_step(uint32_t budget)
{
    static const uint32_t T[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                   0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                   0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *p;
    uint32_t k, c;
    if (mc.slot < 0) {
        for (k = 0; k < NMY && !(my[k].valid && !my[k].crc); k++)
            ;
        if (k == NMY)
            return;
        mc.slot = (int8_t)k;
        mc.at = 0;
        mc.c = 0xFFFFFFFFu;
    }
    k = (uint32_t)mc.slot;
    p = plat_data(MY_BASE + k * MY_SIZE);
    if (!my[k].valid || my[k].crc || !p) {       /* (it went meanwhile) */
        mc.slot = -1;
        return;
    }
    c = mc.c;
    while (budget-- && mc.at < my[k].bytes) {
        c ^= p[MY_DATA + mc.at++];
        c = T[c & 15u] ^ (c >> 4);
        c = T[c & 15u] ^ (c >> 4);
    }
    mc.c = c;
    if (mc.at >= my[k].bytes) {
        my[k].crc = ~c == rd32(p + 20) ? 1u : 2u;
        mc.slot = -1;
    }
}

static void my_measure(uint32_t budget)      /* at most budget samples of work */
{
    uint32_t k;
    my_crc_step(budget);
    if (ms.slot < 0) {
        for (k = 0; k < NMY && !(my[k].valid && !my[k].measured); k++)
            ;
        if (k == NMY)
            return;
        ms.slot = (int8_t)k;
        ms.z = 0;
        ms.peak = 1.0f;
        ms.best = 0.0f;
        ms.ck = k * MY_CKN;
        my[k].ckz[0] = (uint16_t)ms.ck;
        ms_zone(&my[k].z[0]);
    }
    k = (uint32_t)ms.slot;
    if (!my[k].valid || my[k].measured) {        /* (it went meanwhile) */
        ms.slot = -1;
        return;
    }
    while (budget--) {
        const sound_t *s = &my[k].z[ms.z];
        uint32_t code;
        int32_t step, d, p;
        float x, y, w;
        if (ms.i >= s->n) {                      /* the zone is done: the next one, or the result */
            if (++ms.z < my[k].nz) {
                my[k].ckz[ms.z] = (uint16_t)ms.ck;
                ms_zone(&my[k].z[ms.z]);
                continue;
            }
            {
                float db = ms.best > 1e-12f ? 3.0103f * fm_log2f(ms.best) : -120.0f;
                float g = fm_exp2f((MY_TARGET_DB - db) * (1.0f / 6.0206f));
                float top = 0.89f * 32768.0f / ms.peak;          /* its own peak stays under -1 dBFS */
                my[k].gain = fm_clampf(g < top ? g : top, 0.05f, 4.0f);
                my[k].measured = 1;
                my_ck_set(k);
                ms.slot = -1;
            }
            return;
        }
        if (!(ms.i % SND_CK) && ms.ck < (k + my[k].span) * MY_CKN) {     /* the state before this sample is kept */
            my_ck[3u * ms.ck] = (uint8_t)ms.pred;
            my_ck[3u * ms.ck + 1u] = (uint8_t)((uint32_t)ms.pred >> 8);
            my_ck[3u * ms.ck + 2u] = (uint8_t)ms.idx;
            ms.ck++;
        }
        code = (((const uint8_t *)s->d)[ms.i >> 1] >> ((ms.i & 1u) << 2)) & 15u;
        step = IMA_STEP[ms.idx];
        d = step >> 3;
        if (code & 4u)
            d += step;
        if (code & 2u)
            d += step >> 1;
        if (code & 1u)
            d += step >> 2;
        p = code & 8u ? ms.pred - d : ms.pred + d;
        p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
        ms.pred = p;
        d = ms.idx + IMA_INDEX[code & 7u];
        ms.idx = d < 0 ? 0 : d > 88 ? 88 : d;
        ms.i++;
        x = (float)p * (1.0f / 32768.0f);
        if (fm_fabsf((float)p) > ms.peak)
            ms.peak = fm_fabsf((float)p);
        y = ms.b0 * x + ms.b1 * ms.x1 + ms.b2 * ms.x2 - ms.a1 * ms.y1 - ms.a2 * ms.y2;
        ms.x2 = ms.x1;
        ms.x1 = x;
        ms.y2 = ms.y1;
        ms.y1 = y;
        w = y - 2.0f * ms.u1 + ms.u2 - ms.c1 * ms.v1 - ms.c2 * ms.v2;
        ms.u2 = ms.u1;
        ms.u1 = y;
        ms.v2 = ms.v1;
        ms.v1 = w;
        ms.acc += w * w;
        if (++ms.an >= ms.blk || ms.i >= s->n) {     /* 10 ms more: the mean of the last 100 ms (or of what there is) */
            float sum = 0.0f;
            uint32_t j;
            ms.ring[ms.ri] = ms.acc / (float)ms.an;
            ms.ri = (ms.ri + 1u) % 10u;
            if (ms.rn < 10u)
                ms.rn++;
            if (ms.rn == 10u || ms.i >= s->n) {      /* (a zone under 100 ms: the mean of all of it, at its end) */
                for (j = 0; j < ms.rn; j++)
                    sum += ms.ring[j];
                sum /= (float)ms.rn;
                if (sum > ms.best)
                    ms.best = sum;
            }
            ms.acc = 0.0f;
            ms.an = 0;
        }
    }
}
