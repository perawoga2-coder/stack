# STACK 0.1.0-test: the built-in sounds (TEMPORARY)

This set is temporary. The real set is chosen after a listening test. Every sound is audio only: no preset data and
no code of another firmware is in STACK. `tools/make_sounds.py` makes `app/sounds_data.h` from `tools/sounds_temp.json`.

| Sounds | Source | Licence |
| --- | --- | --- |
| ROCK KICK, SNARE, HAT, OPEN, TOM, CRASH | Maker Hip's own Rock kit (synthesized by Burger Studio's own script) | Maker Hip's own |
| HG KICK, SNARE, CLAP, HAT, COWBELL, ORGAN, BRASS, GLASS, VOX, PLUCK | the Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments), made by its script `tools/gen_waves.py` | CC0 1.0 |
| PIANO | Versilian Studios VCSL (Sam Gossner), Grand Piano, Kawai, C4 | CC0 1.0 |
| SUB BASS, BELL | audio rendered by Maker Hip from Felucca presets (credit: Felucca, Leo Kuroshita, Hügelton Instruments). TEMPORARY | rendered audio; see LICENSING.md |

All are stored as 16-bit PCM, mono, 22.05 kHz (the four single cycles at 32 kHz): 332.8 KiB in all.
