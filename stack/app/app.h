/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip
 * Started from fm1-base's template/app.h, Copyright (C) 2026 Charles Vestal (MPL-2.0). */
/* STACK: the app's half of the contract with fm1-base (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "STACK"
#include "stack_version.h"               /* STACK_VERSION: stack/VERSION, by tools/gen_version.py at every build */
#ifndef APP_VERSION                      /* (a release build passes the same, from the same file: --release) */
#define APP_VERSION STACK_VERSION
#endif
#define APP_SPLASH_RGB RGB(255, 122, 47)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */
#define APP_SYSEX                        /* the Burger Studio app's sample messages (my_samples.c) */
int app_sysex(const uint8_t *p, uint32_t n);

/* MUTE (the Burger Studio app's message 20, docs/PROTOCOL.md): the FM-1's own output (speaker and 3.5 mm jack) silent
 * while the app plays the FM-1's sound itself, from the USB audio input. The platform takes the USB audio input from
 * the render first (at full level: plat_usb_audio_full), then puts this gain on what goes to the speaker and the jack
 * (fm1-base device/uac_mix.h), sliding to it across a block: so 0 here is silence there, and the app hears it all */
extern volatile uint8_t stack_mute;
#define APP_GAIN(m) (stack_mute ? 0u : (m) * (m) >> 12)

/* the clock (clock.c): its ticks out leave from the 10 kHz tick, on time */
#define APP_TICK_FAST
void app_tick_fast(void);
#ifdef PLAT_HOST
#define APP_HOST_MS                      /* (the simulator: MIDI in taken and stamped each millisecond) */
void app_host_ms(void);
#endif

/* the store: STACK saves nothing yet of its own (no plat_store_save call in the app); the panel calibration is the
 * platform's. The offsets are fm1-base's template's */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);
void app_say(const char *a, const char *b);

/* the host simulator's checks (host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
