# STACK: the built-in sounds

Made by `tools/make_sounds.py` from `tools/sounds.json`. Every sound is audio only: no preset data and no code of
another firmware is in STACK. From Felucca only rendered audio is used, with credit: Felucca by Leo Kuroshita
(Hügelton Instruments).

69 sounds, 353.7 KiB: DRUMS 22, BASS 10, KEYS 15, PAD 7, LEAD 10, FX 5.
Stored as IMA ADPCM (4 bits a sample) or, the single cycles, 16-bit PCM; mono. Every sound is brought to the same
loudness (ITU-R BS.1770 K-weighting, the loudest 100 ms at -15.5 dB) with its own peak at -1.0 dBFS at most.

| # | Category | Name | Source | Licence | File | Rate | Length |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | DRUMS | KICK ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Kick.wav` | 22050 Hz | 0.40 s |
| 2 | DRUMS | KICK TIGHT | Maker Hip's own Rock kit | Maker Hip's own | `Rock Kick Tight.wav` | 22050 Hz | 0.30 s |
| 3 | DRUMS | SNARE ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Snare.wav` | 22050 Hz | 0.40 s |
| 4 | DRUMS | SNARE CRACK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Snare Crack.wav` | 22050 Hz | 0.35 s |
| 5 | DRUMS | HAT ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Hat Closed.wav` | 22050 Hz | 0.13 s |
| 6 | DRUMS | OPEN ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Hat Open.wav` | 22050 Hz | 0.40 s |
| 7 | DRUMS | TOM ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Tom Mid.wav` | 22050 Hz | 0.45 s |
| 8 | DRUMS | FLOOR TOM | Maker Hip's own Rock kit | Maker Hip's own | `Rock Tom Floor.wav` | 22050 Hz | 0.50 s |
| 9 | DRUMS | CRASH ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Crash.wav` | 22050 Hz | 0.80 s |
| 10 | DRUMS | RIDE ROCK | Maker Hip's own Rock kit | Maker Hip's own | `Rock Ride.wav` | 22050 Hz | 0.60 s |
| 11 | DRUMS | KICK HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `D KICK.wav` | 22050 Hz | 0.40 s |
| 12 | DRUMS | SNARE HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `D SNARE.wav` | 22050 Hz | 0.30 s |
| 13 | DRUMS | CLAP HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `D CLAP.wav` | 22050 Hz | 0.33 s |
| 14 | DRUMS | HAT HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `D CHAT.wav` | 22050 Hz | 0.20 s |
| 15 | DRUMS | COWBELL HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `D COWBELL.wav` | 22050 Hz | 0.35 s |
| 16 | DRUMS | RIM HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `D RIM.wav` | 22050 Hz | 0.08 s |
| 17 | DRUMS | KICK DEEP | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Deep Kit Kick.flac` | 22050 Hz | 0.45 s |
| 18 | DRUMS | SNARE DEEP | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Deep Kit Snare.flac` | 22050 Hz | 0.35 s |
| 19 | DRUMS | KICK STUDIO | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Studio Kit Kick.flac` | 22050 Hz | 0.35 s |
| 20 | DRUMS | CLAP STUDIO | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Studio Kit Clap.flac` | 22050 Hz | 0.30 s |
| 21 | DRUMS | BELL PERC | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Perc Kit Bell.flac` | 22050 Hz | 0.45 s |
| 22 | DRUMS | HAND DRUM | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Hand Drum C4.flac` | 22050 Hz | 0.40 s |
| 23 | BASS | SUB BASS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Sub Bass C3.flac` | 22050 Hz | 0.43 s |
| 24 | BASS | SQR BASS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Sqr Bass C3.flac` | 22050 Hz | 0.34 s |
| 25 | BASS | FAT BASS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Fat Bass C3.flac` | 22050 Hz | 0.34 s |
| 26 | BASS | WAVE BASS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Wave Bass C3.flac` | 22050 Hz | 0.41 s |
| 27 | BASS | WOW BASS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Wow Bass C3.flac` | 22050 Hz | 0.65 s |
| 28 | BASS | FM BASS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Fm Bass C3.flac` | 22050 Hz | 0.34 s |
| 29 | BASS | ACID | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Acid C4.flac` | 22050 Hz | 0.44 s |
| 30 | BASS | PUNCH BASS | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Bass Punch C2.wav` | 22050 Hz | 0.50 s |
| 31 | BASS | SLAP BASS | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Slap Bass C2.wav` | 22050 Hz | 0.50 s |
| 32 | BASS | GROWL BASS | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Growl Bass C2.wav` | 22050 Hz | 0.50 s |
| 33 | KEYS | PIANO | Versilian Studios VCSL by Sam Gossner | CC0 1.0 | `PIANO/03_GPiano_sus_C4_v2_rr1_Player.wav` | 22050 Hz | 1.00 s |
| 34 | KEYS | TINE EP | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Tine Ep C4.flac` | 22050 Hz | 0.90 s |
| 35 | KEYS | SOFT EP | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM E.Piano Soft C4.wav` | 22050 Hz | 0.50 s |
| 36 | KEYS | CLAV | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Clav C4.wav` | 22050 Hz | 0.45 s |
| 37 | KEYS | ORGAN | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Organ C4.flac` | 22050 Hz | 0.40 s |
| 38 | KEYS | FULL ORGAN | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Full Organ C4.flac` | 22050 Hz | 0.34 s |
| 39 | KEYS | MARIMBA | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Marimba C4.flac` | 22050 Hz | 0.50 s |
| 40 | KEYS | KALIMBA | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Kalimba C4.flac` | 22050 Hz | 0.60 s |
| 41 | KEYS | VIBES | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Vibes C5.wav` | 22050 Hz | 0.50 s |
| 42 | KEYS | HARP | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Harp C4.flac` | 22050 Hz | 0.80 s |
| 43 | KEYS | BELL | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Bell C4.flac` | 22050 Hz | 0.80 s |
| 44 | KEYS | FM BELL | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `FM6 Bell C4.flac` | 22050 Hz | 0.80 s |
| 45 | KEYS | PLUCK | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Pluck C4.flac` | 22050 Hz | 0.60 s |
| 46 | KEYS | PLUCK HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `P PLUCK C3.wav` | 22050 Hz | 0.80 s |
| 47 | KEYS | MUSIC BOX | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Music Box G5.wav` | 22050 Hz | 0.50 s |
| 48 | PAD | STRINGS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Strings C4.flac` | 11025 Hz | 1.15 s |
| 49 | PAD | PWM STRINGS | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Pwm Str C4.flac` | 11025 Hz | 1.43 s |
| 50 | PAD | CHOIR | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Choir Aah C4.flac` | 11025 Hz | 1.15 s |
| 51 | PAD | SHIMMER | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Shimmer C4.flac` | 11025 Hz | 1.20 s |
| 52 | PAD | FM PAD | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Pad C4.flac` | 11025 Hz | 1.51 s |
| 53 | PAD | VOX HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `W VOX.wav` | 32000 Hz | 0.01 s |
| 54 | PAD | GLASS HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `W GLASS.wav` | 32000 Hz | 0.01 s |
| 55 | LEAD | SAW LEAD | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Saw Lead C4.flac` | 22050 Hz | 0.44 s |
| 56 | LEAD | PULSE LEAD | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Pulse Ld C4.flac` | 22050 Hz | 0.41 s |
| 57 | LEAD | WAVE LEAD | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Wave Lead C4.flac` | 22050 Hz | 0.41 s |
| 58 | LEAD | VOX LEAD | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Vox Lead C4.flac` | 22050 Hz | 0.46 s |
| 59 | LEAD | RAVE | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Rave C4.flac` | 22050 Hz | 0.44 s |
| 60 | LEAD | FLUTE | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Soft Flute C4.flac` | 22050 Hz | 0.34 s |
| 61 | LEAD | ROCK ORGAN | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Rock Drive C4.flac` | 22050 Hz | 0.45 s |
| 62 | LEAD | BRASS HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `W BRASS.wav` | 32000 Hz | 0.01 s |
| 63 | LEAD | REED HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `W REED.wav` | 32000 Hz | 0.01 s |
| 64 | LEAD | ORGAN HG | The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments) | CC0 1.0 | `W ORGAN.wav` | 32000 Hz | 0.01 s |
| 65 | FX | WIND | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Wind.flac` | 11025 Hz | 1.60 s |
| 66 | FX | RAIN | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Rain.flac` | 22050 Hz | 1.20 s |
| 67 | FX | ARCADE | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Arcade.flac` | 22050 Hz | 0.90 s |
| 68 | FX | GLITCH | Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank) | rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments) | `Glitch C4.flac` | 22050 Hz | 0.80 s |
| 69 | FX | SPACE FX | Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds) | Maker Hip's own | `FM Space FX C4.wav` | 22050 Hz | 0.50 s |

Sources:

- **rock**: Maker Hip's own Rock kit. Maker Hip's own. Synthesized by Burger Studio's own script (the Burger Studio firmware's assets/burger-kit).
- **hugelton**: The Hügelton Sample Pack by Leo Kuroshita (Hügelton Instruments). CC0 1.0. Made by running its CC0 script, Felucca's tools/gen_waves.py.
- **vcsl**: Versilian Studios VCSL by Sam Gossner. CC0 1.0. Grand Piano, Kawai (github.com/sgossner/VCSL).
- **bank**: Audio rendered by Maker Hip from Felucca's presets (the Burger Studio app's Maker Bank). rendered audio, with credit to Felucca by Leo Kuroshita (Hügelton Instruments). Audio only: no preset data and no code.
- **fm**: Maker Hip's own FM bank (the Burger Studio app's MakerHip FM sounds). Maker Hip's own. Made by Maker Hip's tools/make_fm_bank.py from Maker Hip's own patches.
