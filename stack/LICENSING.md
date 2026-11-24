# STACK licensing

STACK is under the Mozilla Public License 2.0 (MPL-2.0), file by file: each source file says so in its first lines.

| What | Origin | Licence |
| --- | --- | --- |
| `app/app.c`, `app/app.h`, `app/fm1.toml`, `app/words.tsv`, `app/tests/`, `tools/` | Maker Hip. `app.c` and `app.h` were started from fm1-base's `template/` (Copyright (C) 2026 Charles Vestal) | MPL-2.0 |
| `base/` | [fm1-base](https://github.com/charlesvestal/fm1-base) at `2bb8e74` with [felucca-bsp](https://github.com/hugelton/felucca-bsp) at `2bd9dbe` (`BASE_COMMITS.txt`), unchanged. In the repository it is to be a git submodule | MPL-2.0; its own third-party parts in `base/LICENSING.md` |
| The built-in sounds (`app/sounds_data.h`, made by `tools/make_sounds.py` from `tools/sounds.json`) | each sound's source is in `app/SOUNDS.md` | Maker Hip's own, CC0 1.0, and audio rendered from Felucca's presets (credit: Felucca by Leo Kuroshita, Hügelton Instruments) |
| `app/my_samples.c`: the samples the Burger Studio app sends (the messages, the slots in the flash, their loudness) | written for STACK from the app's own side of the protocol (Maker Hip's `Fm1SampleLink.kt`); no code from Felucca or the Burger Studio firmware | MPL-2.0 |
| The IMA ADPCM decoder in `app/app.c` and the encoder in `tools/make_sounds.py` | written for STACK from the IMA standard (its two tables and its algorithm) | MPL-2.0 (the code) |

Rules this project keeps:

- No GPL code: nothing from Felucca or from the Burger Studio firmware (bs19). `base/tools/provenance.py` was run on
  the app's files against both: 0 files with Felucca's lines (it finds copies, not rewrites).
- From Felucca only rendered audio is ever shipped: never its preset data, patch tables or code.
- The JieLi SDK files the build needs are fetched by the build and never committed.
- fm1-base's fonts are under the SIL OFL 1.1 (`base/LICENSING.md`).

M-VAVE and FM-1 are trademarks of their owners. STACK is not affiliated with or endorsed by M-VAVE.
