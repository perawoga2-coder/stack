# STACK: messages from the Burger Studio app

Two things go over USB MIDI: the app's own messages, as SysEx (this part and MUTE below), and the clock, as the
standard MIDI real-time bytes (the last part, CLOCK). Every SysEx message, both ways, starts the same way:

    F0 7D 46 4C <command> <data ...> F7

All data bytes are 0..7F. STACK answers every message it knows with a message of the same command number.
Messages with other command numbers get no answer.

| Command | Hex | Name | What it does |
|---|---|---|---|
| 11 | 0B | BEGIN | start sending a sample into a MY slot |
| 12 | 0C | WRITE | a piece of that sample |
| 13 | 0D | END | the sample is whole: check it and keep it |
| 14 | 0E | ERASE | empty a MY slot |
| 15 | 0F | INFO | what the three MY slots hold |
| 20 | 14 | MUTE | the FM-1's own sound off or on (since 0.4.1-test) |
| 30 | 1E | SONG | a piece of the song as it is now (since 4.0-beta) |
| 31 | 1F | LOOP INFO | is there a loop (on track 8 or 7), and its size |
| 32 | 20 | LOOP DATA | a piece of the loop's audio |
| 112 | 70 | SHOW | is the sample in this slot there |
| 114 | 72 | HELLO | STACK's name and version |

Commands 11 to 15, 112 and 114 are the sample messages the app already sends; `app/my_samples.c` is their reference.

## 20 (hex 14): MUTE

For the app's SOUND switch at MERGED IN BURGER: the app plays the FM-1's sound itself, from the FM-1's USB audio
input, so the FM-1's own speaker and 3.5 mm jack must be silent.

### The app sends

| Bytes | Meaning |
|---|---|
| `F0 7D 46 4C 14 01 F7` | MUTE ON: the FM-1's speaker and 3.5 mm jack go silent |
| `F0 7D 46 4C 14 00 F7` | MUTE OFF: they sound again |
| `F0 7D 46 4C 14 F7` | ask only: change nothing, just answer |

### STACK answers (every time)

    F0 7D 46 4C 14 <state> <keep> F7

- `<state>`: `01` the FM-1 is muted now, `00` it sounds.
- `<keep>`: how long a MUTE ON lasts without being sent again, in tenths of a second. Now `1E` (30 = 3.0 s).

So the answer to MUTE ON is `F0 7D 46 4C 14 01 1E F7`, and to MUTE OFF `F0 7D 46 4C 14 00 1E F7`.
A value other than 00 or 01 changes nothing and is answered with the state as it is.

### Keep-alive: the app must repeat MUTE ON

A MUTE ON lasts 3 seconds. **While the switch is at MERGED IN BURGER, send MUTE ON again once every second.**
Each one starts the 3 seconds afresh (two may be lost and the FM-1 stays silent).

### When the FM-1 sounds again by itself

The FM-1 must never stay silent by mistake. It unmutes by itself:

- 3 seconds after the last MUTE ON, if the app has stopped sending it (the app closed, crashed, in the background);
- at once when the USB connection goes or a new one starts (the cable out, another device);
- at every power-on and restart: it always starts unmuted.

After any of these the app has to send MUTE ON again. To leave the mode cleanly, send MUTE OFF once: the FM-1
sounds again at once (the change is a 6 ms fade, no click).

### What muting does not change

- **The USB audio input goes on as before.** The app still receives the FM-1's whole master (all tracks, the
  looper, the effects, the limiter), at full level, whatever the FM-1's MASTER knob says. The mute comes after the
  point where USB audio is taken.
- The keys, the sequencer, the looper and everything on the screen work as before.
- While muted the FM-1's screen shows `SOUND IN BURGER`, top right; it goes when the FM-1 sounds again.
- Nothing is saved: the mute is never remembered.

## CLOCK (since 0.4.2-test)

Standard MIDI real-time messages over USB MIDI, no SysEx. There is no setting on the FM-1 and no message that says
who leads: **whoever sends the clock leads.** The app's MASTER CLOCK switch decides by sending clock or not.

| Byte(s) | Name | Meaning |
|---|---|---|
| `F8` | Clock | one tick; 24 ticks a quarter note, so 6 ticks a step (a sixteenth), 96 ticks a 16-step pattern |
| `FA` | Start | play from step 1; **the next `F8` is the first step** |
| `FB` | Continue | play on from where it stopped (or from the Song Position sent since); the next `F8` is that place |
| `FC` | Stop | stop |
| `F2 ll mm` | Song Position | the place in sixteenths (steps), `ll` the low 7 bits, `mm` the high 7 bits |

### Switch at DEVICE: the FM-1 leads (this is how it starts)

The app sends **no** `F8`, `FA`, `FB`, `FC`. The FM-1 sends:

- `F8` all the time, playing or stopped, at its tempo (120 now: a tick every 20.83 ms);
- `FA` when PLAY is pressed, about 1.5 ms before the tick that is step 1;
- `FC` when it stops. It never sends `FB` or `F2`.

What the app should do with it:

- take the tempo from the average of several ticks, not from one gap;
- at `FA` start from the top, on the next `F8`. The gap between the last tick before `FA` and the first after it is
  shorter than a tick (the FM-1 starts the bar at once): leave that one gap out of the tempo;
- **never send the FM-1's clock back to it** (no MIDI thru of real-time bytes). The FM-1 would take it for a leader's
  clock and follow its own echo.

### Switch at BURGER: the app leads

The app sends `F8` at its tempo, and the FM-1 follows from the first tick:

- the FM-1 stops sending its own `F8`, `FA`, `FC` for as long as it follows, and never sends on what comes in;
- its screen shows `CLOCK: BURGER` (else `CLOCK: FM-1`);
- its tempo is the app's (25 to 312 beats a minute). Ticks a few milliseconds early or late are smoothed out;
- to start: `FA`, then the ticks; the first tick after `FA` is step 1. To stop: `FC`.
- to start somewhere else: while stopped, `F2 ll mm`, then `FB`, then the ticks. The pattern is 16 steps, so the
  place is taken modulo 16 steps. A Song Position sent while playing is ignored;
- **keep sending `F8` while stopped.** If no tick comes for 0.5 s the FM-1 takes the clock as gone: it leads again,
  at the tempo it last followed, shows `CLOCK LOST: FM-1 LEADS` and sends its own clock again. If it was playing it
  plays on. The next tick from the app makes it follow again.
- so when the switch goes back to DEVICE, the app just stops sending `F8`; half a second later the FM-1 leads.
- while it leads, the app must ignore the clock that comes from the FM-1 for its own transport.

PLAY on the FM-1 still works while it follows: it starts and stops the FM-1's own pattern at the app's tempo, and
sends nothing.

### The looper and the clock

A loop on the FM-1 is recorded sound: it is a whole number of bars long (since 4.02-beta; one pattern before) at
the tempo it was recorded at, and it does not stretch. Under the app's clock it is kept on the bar by playing up to 0.1 % faster or slower, which covers the small
drift between two devices. **A real change of tempo in the app while a loop exists puts the loop out of time**; the
FM-1 shows `TEMPO CHANGED: LOOP OUT OF TIME` and plays on. So the app should not change tempo under a loop without
telling the user (or the user clears the loop and records again).

## Bluetooth MIDI (since 0.6.0-test)

The FM-1 is a Bluetooth LE MIDI device (the standard BLE-MIDI service): a tablet or computer connects to it as to any
Bluetooth MIDI instrument. One link at a time. No pairing code. USB works as before, at the same time.

Bluetooth carries **MIDI only**. Samples (the SysEx messages above), MUTE and audio stay on the USB cable: STACK
answers no SysEx over Bluetooth.

| | Over USB | Over Bluetooth |
|---|---|---|
| Notes in (play the selected track) | yes | yes |
| Notes out (the FM-1's own keys, channel 1; since 0.6.1-test) | none | yes |
| Clock, Start, Stop out (the FM-1 leads) | yes | yes, while a link is up |
| Clock, Start, Continue, Stop, Song Position in | followed | followed |
| SysEx (samples, MUTE) | yes | no |

- The rules of CLOCK above hold on both: whoever sends `F8` leads. The FM-1 follows the clock from the port it comes
  in on first (USB or Bluetooth), and from that one only, until it stops for 0.5 s.
- The keys' notes go out over Bluetooth only: a note on with the key's velocity, a note off when it is let go
  (`90 nn vv`, `80 nn 00`), as the keys are pressed, whatever the ARP makes of them. **The app must not play them on
  its drum pads by itself** (that was the echo of 0.2.2: the FM-1's keys sounding as drums in the app).
- Bluetooth can be turned off on the FM-1 (GLO page): it then cannot be found or connected to. It is on at every
  power-on. GLO's DISCONNECT ends the link and lets the FM-1 be found again a moment later.
- **Nothing is passed from one to the other**: what comes in over USB never goes out over Bluetooth, nor the other
  way. And nothing that comes in is sent back.
- While it follows a clock over Bluetooth the FM-1 shows `CLOCK: BT` and, once, `BT CLOCK: CABLE IS STEADIER`.
  A Bluetooth clock arrives in bunches; the FM-1 smooths it, but a loop on the FM-1 holds the bar best on the cable.
  **For the looper, send the clock over USB.**
- The app should connect either by USB or by Bluetooth for the clock, not send it on both.
- The FM-1's screen shows `BT` (second line of the header) while a link is up.

## VACUUM: the song and the loop (since 4.0-beta)

The app pulls the whole project off the FM-1 over the USB cable. The app asks piece by piece and STACK answers each
piece. There is no mode and no button on the FM-1: it answers on any page, playing or stopped. A dump only reads:
nothing is written to the flash, and nothing on the FM-1 changes. While it runs the FM-1's screen shows
`SENDING TO BURGER`.

**Pacing:** ask for one piece, wait for its answer, then ask for the next. STACK holds at most one answer. If the
USB queue has no room, the answer waits until it has (the sound goes on meanwhile). If an answer does not come
within about half a second, ask for the same piece again. A firmware older than 4.0-beta answers none of these
messages: that is how the app knows.

Data bytes are packed 7 to 8, as in the sample messages: for every 7 bytes (or what is left), first one byte with
their top bits (bit 0 for the first), then the bytes with the top bit cleared. A piece is 128 bytes before packing.
Numbers of more than 7 bits are sent low 7 bits first.

### 30 (hex 1E): SONG

    ask:     F0 7D 46 4C 1E <piece lo> <piece hi> F7
    answer:  F0 7D 46 4C 1E <piece lo> <piece hi> <pieces lo> <pieces hi> <data, packed> F7

**Asking for piece 0 makes the record** from the FM-1's state at that moment (NOT from what SAVE last kept); the
other pieces are of that same record. Ask for piece 0 first, then 1 to pieces − 1. A piece past the end is answered
with no data. The record is 800 bytes (7 pieces). All numbers in it are little-endian.

The head, 32 bytes:

| Offset | Size | What |
|---|---|---|
| 0 | 4 | `STKD` |
| 4 | 1 | the record's version: 1 |
| 5 | 1 | tracks: 8 |
| 6 | 1 | bytes a track: 96 |
| 7 | 1 | flags: bit 0 there is a loop, bit 1 the pattern plays, bit 2 the FM-1 follows a clock, bit 3 the looper is on track 7 (since 4.04-beta; 0: track 8) |
| 8 | 2 | the record's length in bytes (800) |
| 10 | 4 | CRC-32 (the usual one, as zlib's) of the bytes from offset 16 to the end |
| 16 | 1 | tempo, BPM |
| 17 | 1 | time signature: 0 2/4, 1 3/4, 2 4/4, 3 6/8, 4 7/8 |
| 18 | 1 | steps in the pattern: 8, 12, 14 or 16 |
| 19 | 1 | swing, 50 (straight) to 75 |
| 20 | 1 | the selected track, 0 to 7 |
| 21 | 1 | the loop's layers |
| 22 | 10 | the firmware's version as text, e.g. `4.0-BETA` |

Then eight tracks of 96 bytes, T1 first:

| Offset | Size | What |
|---|---|---|
| 0 | 1 | kind: 0 EMPTY, 1 a built-in sound, 2 a sample of the app's (MY 1 to 3), 3 the looper (T8L, or T7L since 4.04-beta) |
| 1 | 1 | category: 0 DRUMS, 1 BASS, 2 KEYS, 3 PAD, 4 LEAD, 5 FX; 255 none |
| 2 | 1 | the MY slot, 1 to 3 (kind 2) |
| 3 | 1 | bit 0: a hit (plays through, its own pitch on every key); bit 1: a held sound with a loop |
| 4 | 4 | a built-in sound's stable ID: FNV-1a (32 bits) of its name |
| 8 | 4 | kind 2: the CRC-32 the sample's header names for its data (as the app sent it) |
| 12 | 16 | the sound's name as text, zero-padded: `KICK ROCK`, a sample's own name, `LOOP` |
| 28 | 25 | the settings, one byte each (below) |
| 56 | 16 | steps 1 to 16: 1 on, 0 off (only the first "steps in the pattern" play) |
| 72 | 16 | each step's MIDI note (60 is middle C; a hit ignores it) |
| 88 | 8 | the category as text, zero-padded: `DRUMS`, `BASS`, `KEYS`, `PAD`, `LEAD`, `FX`, `MY`, `LOOP`, or empty |

**Which track holds the loop (since 4.04-beta).** The loop's first layer is track 8's; each time a layer is in, the
loop moves to the other of tracks 8 and 7 (8, 7, 8, ...). An undo or a cancelled take does not move it; a clear
ends it, and the next first layer is track 8's again. The track that holds it is the one of kind 3 (`LOOP`); head
flag bit 3 says the same (1: track 7). Up to 4.03-beta it was always track 8, and bit 3 was 0, so an app that
reads kind 3 track by track reads both. While a track is kind 3 its own sound sleeps under the loop and is not in
the record (its steps and the other settings are its own, as SAVE keeps them), except LEVEL (setting 3): that is
the loop's own level, which goes with the loop from track to track (80 when a loop starts). The other track of the
two is an ordinary track with its own sound, playing. The record is of the moment piece 0 was asked for: a layer
that comes in later moves the loop, and changes `<ver>` (LOOP INFO); then ask for the song again.

The 25 settings, in order (signed ones are two's complement):

| # | Setting | Range |
|---|---|---|
| 0 | PITCH, semitones | −24 to 24 |
| 1 | FILTER: negative a low-pass closing, positive a high-pass opening, −3 to 3 is off | −53 to 53 |
| 2 | DECAY | 0 to 100 |
| 3 | LEVEL (80 is the starting level; the gain is its square) | 0 to 100 |
| 4 | ENV ATTACK | 0 to 100 |
| 5 | ENV RELEASE (−1 is AUTO) | −1 to 100 |
| 6 | ENV SUSTAIN | 0 to 100 |
| 7 | ENV VELOCITY | 0 to 100 |
| 8 | LFO AMOUNT (0 off) | 0 to 100 |
| 9 | LFO RATE: 0 4 BARS, 2 BARS, 1 BAR, 1/2, 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 10 1/32 | 0 to 10 |
| 10 | LFO TARGET: 0 PITCH, 1 FILTER, 2 LEVEL, 3 PAN | 0 to 3 |
| 11 | LFO SHAPE: 0 SINE, 1 TRIANGLE, 2 SQUARE, 3 SAW UP, 4 SAW DOWN | 0 to 4 |
| 12 | EDIT START, per cent | 0 to 99 |
| 13 | EDIT REVERSE | 0 or 1 |
| 14 | EDIT LOOP: 0 AUTO, 1 OFF, 2 ON | 0 to 2 |
| 15 | EDIT LENGTH, per cent | 1 to 100 |
| 16 | FIT: 0 OFF, 1 1/4 bar, 2 1/2, 3 1, 4 2, 5 4 bars | 0 to 5 |
| 17 | ARP RATE: 0 OFF, 1 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 7 1/32 | 0 to 7 |
| 18 | ARP MODE: 0 UP, 1 DOWN, 2 UP-DOWN, 3 ORDER | 0 to 3 |
| 19 | ARP OCTAVE | 1 to 3 |
| 20 | ARP GATE, per cent | 10 to 100 |
| 21 | FX slot A: 0 OFF, 1 DELAY, 2 ECHO, 3 REVERB, 4 TAPE SPEED, 5 ROBOT, 6 FLANGER, 7 OVERDRIVE, 8 CHORUS, 9 LO-FI | 0 to 9 |
| 22 | FX slot B, the same | 0 to 9 |
| 23 | FX A's knob: −3 to 3 off; left and right are the effect's two flavours | −53 to 53 |
| 24 | FX B's knob | −53 to 53 |

### 31 (hex 1F): LOOP INFO

    ask:     F0 7D 46 4C 1F F7
    answer:  F0 7D 46 4C 1F <has> <rate> <n lo> <n mid> <n hi> <layers> <ver lo> <ver hi> <CRC, 4 bytes packed> F7

- `<has>`: 1 a loop plays (on track 8 or 7: the SONG record says which), 0 none (the rest is then 0).
- `<rate>`: 0 the loop is at 22 050 Hz, 1 at 11 025 Hz. Mono. (Since 4.03-beta it is always 0.)
- `<n>`: its samples (21 bits). Its length in seconds is n / rate. Up to 4.01-beta it is one pattern long at the
  tempo it was recorded at. **Since 4.02-beta it is a whole number of bars (patterns) long**, one or more: the
  number of bars is not sent; it is n / rate divided by the pattern's length at the song's tempo and time
  signature, rounded. An app that fits the loop to one bar must fit it to that many. Since 4.03-beta a loop can be
  8 s long (176 400 samples: 352 800 data bytes, 2 757 pieces), four times what 4.02 could send.
- Since 4.03-beta a loop on the FM-1 starts where REC was pressed, not on a bar line. **The dump does not show
  that: its first sample is the loop at the start of a bar**, so an app lays it on its own bars as before.
  The FM-1 keeps the loop as 4-bit ADPCM (with the coder's noise); the dump is that, decoded to 16 bits.
- `<ver>`: a number (14 bits) that changes whenever the loop that plays is another one: a layer added, an undo, a
  clear.
- CRC: CRC-32 of the loop's 2 × n data bytes.

### 32 (hex 20): LOOP DATA

    ask:     F0 7D 46 4C 20 <piece lo> <piece hi> F7
    answer:  F0 7D 46 4C 20 <piece lo> <piece hi> <ver lo> <ver hi> <data, packed> F7

128 bytes a piece: 64 samples, 16 bits signed, low byte first. **Full scale is 16384, not 32768** (two layers at
full level fit): divide by 16384 for −1 to 1. A piece past the end has no data. Ask LOOP INFO, then pieces 0 to
(2 × n − 1) / 128. If a piece's `<ver>` is not the one LOOP INFO gave, the loop changed while it was read: start
again. Check the CRC at the end.

A loop of the longest kind (2 s at 22 kHz) is 88 208 bytes: 690 pieces.
