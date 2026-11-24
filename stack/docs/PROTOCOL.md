# STACK: messages from the Burger Studio app

Everything here goes over USB MIDI as SysEx. Every message, both ways, starts the same way:

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
