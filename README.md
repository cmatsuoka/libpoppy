# libpoppy - a vintage drum machine engine library

A small (just a few dozen kilobytes), lightweight, target-agnostic **C,
integer-only, 32-bit fixed-point** audio library that implements the 15 voices
of a well-known vintage drum machine and its 20 built-in rhythm patterns.

The library renders a single stereo mix into a caller-supplied buffer. It does
no I/O and no floating point. Memory allocation happens only when a context is
created: the context itself, plus the shared sine table (built once per
process).

## Available rhythm patterns

There are two banks of preset rhythms. Bank A contains Waltz, Samba, Mambo, Rumba,
Bossa Nova, Rock 1, Rock 3, March 2/4, and Swing. Bank B has Jazz Waltz, Meringue,
Cha-Cha, Beguine, Habanera, Slow Rock, Rock 2, Rock 4, March 6/8 and Latin Swing.
Patterns in the same bankcan be combined by selecting multiple rhythms at the same
time.

## Build

```sh
make            # builds libpoppy.a
make check      # builds and runs the test harness
make play       # builds the OpenAL rhythm player example
```

`make play` additionally needs OpenAL (`pkg-config openal`, or `-lopenal`).
The player is written to `build/examples/play_rhythm`:

```sh
./build/examples/play_rhythm --tempo 140 --bank B --buttons=1,3
```

Requirements: a C99 compiler and `make`. The library is built with
`-std=c99 -Wall -Wextra -Werror`; the build also enables
`-Wfloat-conversion -Wdouble-promotion` when the compiler supports them, so a
floating-point operation in the library is a build error. Unwind tables are
disabled where the compiler supports the flags.

## Using it

```c
#include <mp7.h>

mp7_context ctx = mp7_create_context(44100);
if (ctx == NULL) { /* out of memory */ }

int16_t block[128 * 2];
mp7_process(ctx, block, 128);      /* from the audio ISR */

mp7_destroy_context(ctx);
```

Only the panel controls are exposed, plus a per-instrument gain: master
Volume, Balance, instrument gain, tempo and rhythm selection. The instrument
gain defaults to 0.5 for Tambourine, Guiro and Quijada (the panel's Additional
Volume faders) and to 1.0 for every other instrument. Synthesis parameters
(frequency, envelope times, filter settings and so on) are fixed constants
inside the engines. Gains are linear Q16 values: 1.0 is `65536`.

Rhythm selection mirrors the panel: a bank A/B switch and a 10-bit mask of
the pressed rhythm buttons. A button mask of 0 means no button is pressed.
Use `mp7_stop_rhythms()` to stop the player.

```c
mp7_set_volume(ctx, 65536);                 /* panel 1.0 -> 0.5x master gain */
mp7_set_balance(ctx, 32768);                /* center */
mp7_set_instr_gain(ctx, MP7_INSTR_QUIJADA, 65536);
mp7_set_tempo(ctx, 120);

/* Bank A, Waltz + Samba buttons latched. */
mp7_set_rhythm(ctx, MP7_BANK_A,
    mp7_rhythm_button_bit(MP7_RHYTHM_WALTZ) |
    mp7_rhythm_button_bit(MP7_RHYTHM_SAMBA));

/* Switch to bank B button 0 (Jazz Waltz) at the next cycle. */
mp7_queue_rhythm(ctx, mp7_rhythm_bank(MP7_RHYTHM_JAZZ_WALTZ),
    mp7_rhythm_button_bit(MP7_RHYTHM_JAZZ_WALTZ));

mp7_set_rhythm(ctx, MP7_BANK_A, 0);        /* no buttons: percussion only */
```

The panel state is available for a UI or lamp driver without callbacks:

```c
struct mp7_panel_state state;
mp7_get_panel_state(ctx, &state);

/* stopped: light the lens for state.bank; running: pulse it when
 * state.flash_count advances. */
```

`struct mp7_panel_state` carries the applied bank and button mask, any queued
selection, the transport state, the current grid column and a monotonic lamp
flash counter. The flash counter advances on two grid points: the first
enabled column at or after column 31, and the cycle wrap. The host owns the
visible pulse length and the library supplies the events.

The Guiro is the only gated voice. Its gate is shared between the pattern
player and the note you trigger yourself: the voice sounds while either one
holds it, so a note you hold is not cut off by the pattern, the pattern is not
cut off by your note-off, and a note that arrives while the gate is already
open does not restart the ratchet.

The library is not thread-safe: `mp7_process()` and the setters must not run at
the same time, and `mp7_create_context()` / `mp7_destroy_context()` must not run
at the same time as either.
