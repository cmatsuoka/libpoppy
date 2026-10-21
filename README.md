# The Poppy library

A small, target-agnostic **C, integer-only, 32-bit fixed-point** audio library
that implements the 15 voices of the Mini Pops percussion unit, their mixer and
the 20 built-in rhythm patterns.

The library renders a single stereo mix into a caller-supplied buffer. It does
no allocation, no I/O and no floating point, and it is meant to run on the
M-VAVE FM-1 (pi32v2) as well as on a desktop.

## Build

```sh
make            # builds libpoppy.a and mp7_test
make check      # builds and runs the test harness
make clean
```

Requirements: a C99 compiler and `make`. The library is built with
`-std=c99 -Wall -Wextra -Werror`; the build also enables
`-Wfloat-conversion -Wdouble-promotion` when the compiler supports them, so a
floating-point operation in the library is a build error.

## Using it

```c
#include <mp7.h>

mp7_context *ctx = malloc(mp7_context_size());
mp7_init(ctx, 44100);

int16_t block[128 * 2];
mp7_process(ctx, block, 128);      /* from the audio ISR */
```

Only the original unit's panel controls are exposed: master Volume, Balance,
the Quijada/Guiro/Tambourine Additional Volume faders, tempo and rhythm
selection. Synthesis parameters (frequency, envelope times, filter settings
and so on) are fixed constants inside the engines. Gains are linear Q16
values: 1.0 is `65536`.

Rhythm selection mirrors the panel: a bank lever (`MP7_BANK_A` / `MP7_BANK_B`)
plus a 10-bit mask of the pressed rhythm buttons. Only rhythms in the selected
bank can be combined. A button mask of 0 means no button is pressed: the main
rhythms stop but the fixed Tambourine/Guiro/Quijada layer keeps playing, as on
the original unit. Use `mp7_stop_rhythms()` for Stop.

```c
mp7_set_volume(ctx, 65536);                 /* panel 1.0 -> 0.5x master gain */
mp7_set_balance(ctx, 32768);                /* centre */
mp7_set_perc_gain(ctx, MP7_INSTR_QUIJADA, 65536);
mp7_set_tempo(ctx, 120);

/* Bank A, Waltz + Samba buttons latched. */
mp7_set_rhythm(ctx, MP7_BANK_A,
               mp7_rhythm_button_bit(MP7_RHYTHM_WALTZ) |
                   mp7_rhythm_button_bit(MP7_RHYTHM_SAMBA));

/* Switch to bank B button 0 (Jazz Waltz) at the next cycle. */
mp7_queue_rhythm(ctx, mp7_rhythm_bank(MP7_RHYTHM_JAZZ_WALTZ),
                 mp7_rhythm_button_bit(MP7_RHYTHM_JAZZ_WALTZ));

mp7_set_rhythm(ctx, MP7_BANK_A, 0);        /* no buttons: percussion only */
mp7_trigger_note(ctx, 36, 100);             /* bass drum */
mp7_trigger_note(ctx, 74, 0);               /* note-off (Guiro is the gated voice) */
```

The panel state is available for a UI or lamp driver without callbacks:

```c
mp7_panel_state state;
mp7_get_panel_state(ctx, &state);

/* stopped: light the lens for state.bank; running: pulse it when
 * state.flash_count advances. */
```

`mp7_panel_state` carries the applied bank and button mask, any queued
selection, the transport state, the current grid column and a monotonic lamp
flash counter. The flash counter advances at the same two grid points as the
reference unit (the first enabled column at or after column 31, and the cycle
wrap). The host owns the visible pulse length; the library supplies the events.

The Guiro is the only gated voice. Its gate is shared between the pattern
player and the note you trigger yourself: the voice sounds while either one
holds it, so a note you hold is not cut off by the pattern, the pattern is not
cut off by your note-off, and a note that arrives while the gate is already
open does not restart the ratchet.

The library is not thread-safe: `mp7_process()` and the setters must not run at
the same time. On the FM-1 the panel glue applies its changes while the audio
thread is blocked, or from the audio thread itself.

`examples/fm1_wrapper.c` is a small example of the ISR and panel glue for the FM-1.

## Test harness

```sh
./mp7_test --init             # context size, default sample rate, table size
./mp7_test --sine             # verify the shared sine table, index by index
./mp7_test --gain             # verify the Q16 gain multiply, including >unity
./mp7_test --silence out.wav  # render silence and write a 16-bit stereo WAV
./mp7_test --instruments dir  # render all 15 instruments to WAV files
./mp7_test --noise            # verify the noise voices are uncorrelated
./mp7_test --guiro            # verify Guiro gate hold and note-off release
./mp7_test --quijada          # verify Quijada frequencies, thresholds and comparator
./mp7_test --mixer            # verify pan, Volume, Balance and direct gains
./mp7_test --api              # verify triggers, MIDI map, rhythms, tempo
./mp7_test --panel            # verify the panel state snapshot and lamp counter
./mp7_test --pattern          # verify the 20 patterns, combination and timing
./mp7_test --rhythms dir      # render all 20 rhythms and two combinations
