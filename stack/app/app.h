/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Maker Hip
 * Started from fm1-base's template/app.h, Copyright (C) 2026 Charles Vestal (MPL-2.0). */
/* STACK 0.1.0-test: the app's half of the contract with fm1-base (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "STACK"
#define APP_SPLASH_RGB RGB(255, 122, 47)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */

/* the store: 0.1.0-test saves nothing of its own (no plat_store_save call in the app); the panel calibration is the
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
