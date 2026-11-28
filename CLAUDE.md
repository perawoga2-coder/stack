# STACK: start here

คู่มือสำหรับ Claude ที่เปิดหน้าใหม่มาทำงาน STACK ต่อ อ่านไฟล์นี้ก่อน แล้วอ่านอีก 2 ไฟล์ก่อนเริ่มงานทุกครั้ง:

1. `.claude/skills/makerhip-working-rules/SKILL.md`: how to work with the owner (Maker Hip). Follow it.
2. `.claude/skills/makerhip-working-rules/DECISIONS.md`: what is already decided. Do not ask again.

The owner is Thai, a designer, not a coder, and works on an Android tablet. Answer in Thai, short, step by step.
The owner designs the controls: do exactly what the owner designed ("ห้ามคิดเอง"); when it clashes with what
is there, say where and ask with ก/ข options. Talk an idea through, sum up the plan, wait for "โอเค".

## What this is

STACK is Maker Hip's own firmware for the M-VAVE FM-1 (C, a unity build on fm1-base): 8 tracks, 69 built-in sounds
plus MY 1-3 samples from the Burger Studio app, effects, EQ, ENV, LFO, EDIT, ARP, a step sequencer of up to four
16-step pages (4.16), a one-button looper on REC, PERFORM, SAVE, MIDI clock over USB and Bluetooth. It is on The
Firmware Hub (card: `fmhub.json`, image `stack/docs/card.png`). Identity `FM-1_34`: never change it.

- Work branch: `ccr-94587bba-xqg4fh` (it is the main line here). Never push to `main` without the owner's word.
- Version: `stack/VERSION` (the one place it is written; `4.17-beta` at the time of writing). New version every
  build that goes to the owner.
- Code: `stack/app/` (`app.c` the most; `looper.c`, `dump.c`, `perform.c`, `city.c` the mushroom view, `fx.c`,
  `clock.c`, `my_samples.c`). Manual: `stack/app/docs/MANUAL.md`. The protocol with Burger: `stack/docs/PROTOCOL.md`.
- Sizes: the device image must fit the app slot (581 564 B; 4.17 is about 566 KB). The big font is FONT_M (4.15).

## Set up (a new container)

    cd stack
    git clone -q https://github.com/charlesvestal/fm1-base base
    git -C base checkout -q $(sed -n 1p BASE_COMMITS.txt)
    git -C base submodule update --init -q
    git -C base/bsp checkout -q $(sed -n 2p BASE_COMMITS.txt)
    sh base/tools/setup_toolchain.sh --only uv,python,jieli,ac79-sdk,fm1-reader   # (as CI; the simulator may need less)
    base/.venv/bin/python base/tools/build.py app --host

(`stack/base/` is not in git.) For the web emulator also emsdk 3.1.74, then `build.py app --web`.

## The gates, in order (every change)

1. A bug: a failing test first.
2. **All simulator tests pass**, from `stack/`:
   `for t in basic limiter sounds receive keys fx env looper mute clock lfo edit arp ble edit2 glo save save_my city perform dump pages; do app/build/host/stack_host app/tests/$t.txt OUTDIR; done`
   (no line `FAIL`). When VERSION changes: `python3 tools/make_dump_test.py > app/tests/dump.txt`.
3. **WAV identity**: run the tests of the version before into another folder and compare the WAVs; only the ones
   the change means may differ (say why for each).
4. No `size_t` in the app's code.
5. `/code-review` (low).
6. Play it in the web emulator (Playwright, Chromium is installed) with screenshots, no page errors.
7. Commit, push. CI (`.github/workflows/stack-firmware.yml`) runs the tests, builds for the device and puts the
   branch on the site's `test/` (`https://perawoga2-coder.github.io/stack/test/install/`).
8. Report to the owner with the test/ links.

## Releases

Tag pushes from a session are refused: the owner makes the release on GitHub (Releases > new, tag `vX.YY-beta`,
target the work branch, not pre-release). `release.yml` builds and attests it (The Firmware Hub's shared build).
Then run `stack-firmware.yml` (workflow_dispatch) once so the site's front (`/stack/install/`, `/stack/emu/`,
the manual) is the new release, and set the STACK card's version on the owner's site (repo `Fm1sampa`,
`web/makerhip/index.html`, "Beta X.YY").

## Never

- Never tell the owner to install something older over something newer.
- Never put a token, password or key into a file, a commit or a message. Never send the owner's private data out.
- The owner's own recorded voice (MY slots on the device) is never part of the firmware.
- Pictures: the style of others' work only, never a copy.

## The controls as the owner designed them (4.17)

- TRACKS page K1-K4: PITCH, FILTER, DECAY, LEVEL. SEQ page: K4 picks the page of steps (1-4; a new page is a copy
  of the one before; the pattern is as long as the furthest page); OCT- held + K4 there is LEVEL.
- REC: records at once; held 5 s clears the whole loop. GLO held 3 s, then OCT-: the last layer goes.
- GLO page: K1 BLUETOOTH, K2 PLAY MODE (CHAIN / PAGE), K3 LENGTH, K4 PERFORM LATCH; REC there is DISCONNECT.
- FX opens on release; FX held + black keys 1-4: the page from the next bar (CHAIN). SEL held: PERFORM.
- HOME held: the mushroom view. SAVE: keeps the song (4 pages since 4.16); held 2 s: clears it.
- SELECT: tempo.

## Waiting

- See DECISIONS.md. Nothing of STACK waits at the time of writing.
