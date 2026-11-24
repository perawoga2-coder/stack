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

A loop on the FM-1 is recorded sound: it is as long as one pattern at the tempo it was recorded at, and it does not
stretch. Under the app's clock it is kept on the bar by playing up to 0.1 % faster or slower, which covers the small
drift between two devices. **A real change of tempo in the app while a loop exists puts the loop out of time**; the
FM-1 shows `TEMPO CHANGED: LOOP OUT OF TIME` and plays on. So the app should not change tempo under a loop without
telling the user (or the user clears the loop and records again).
