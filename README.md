# libpoppy - a vintage drum machine engine library

*This library is under development and the API may be slightly unstable.*

A small (just a few dozen kilobytes), lightweight, target-agnostic **C,
integer-only, 32-bit fixed-point** audio library that implements the 15 voices
of a vintage Japanese drum machine and its 20 preset rhythm patterns. Check
the [demo](https://cmatsuoka.github.io/poppy-web) to hear what it sounds
like.

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

A host that wants patterns of its own has a second, host-writable set of 20
patterns in the library. See [User-defined patterns](#user-defined-patterns).

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

The library is built with the `LIBPOPPY_EXTRA` macro defined, so it carries
the user pattern slots. A minimal, preset-only library — the 20 factory
rhythms and nothing else — is one edit away: empty the `EXTRA_DEF` line in
the Makefile. `make check` builds and tests both configurations, whichever
one is the shipped one.

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
Volume faders) and to 1.0 for every other instrument. Gains are linear Q16
values: 1.0 is `65536`.

Synthesis parameters (frequency, envelope times, filter settings and so on)
are fixed constants inside the engines, so every instrument sounds as it does
on the original unit. The extended build adds a few run-time tweaks on top of
them: the accent lift of an accented pattern column, the Snare 1 decay time,
and the Snare 1 band-pass and Hihat high-pass cut-off frequencies. See
[Synthesis tweaks](#synthesis-tweaks).

Rhythm selection mirrors the panel: a bank A/B switch and a 10-bit mask of
the pressed rhythm buttons. A selection applies immediately by default; OR
`MP7_APPLY_NEXT_CYCLE` into the mask to defer it to the next pattern cycle. A
button mask of 0 means no button is pressed. Use `mp7_stop_rhythms()` to stop
the player.

```c
mp7_set_volume(ctx, 65536);                 /* panel 1.0 -> 0.5x master gain */
mp7_set_balance(ctx, 32768);                /* center */
mp7_set_instr_gain(ctx, MP7_INSTR_QUIJADA, 65536);
mp7_set_tempo(ctx, 120);

/* Bank A, Waltz + Samba buttons latched, applied now. */
mp7_select_rhythm(ctx, MP7_BANK_A,
    mp7_rhythm_button_bit(MP7_RHYTHM_WALTZ) |
    mp7_rhythm_button_bit(MP7_RHYTHM_SAMBA));

/* Switch to bank B button 0 (Jazz Waltz) at the next cycle. */
mp7_select_rhythm(ctx, mp7_rhythm_bank(MP7_RHYTHM_JAZZ_WALTZ),
    mp7_rhythm_button_bit(MP7_RHYTHM_JAZZ_WALTZ) | MP7_APPLY_NEXT_CYCLE);

/* no buttons: percussion only, applied now */
mp7_select_rhythm(ctx, MP7_BANK_A, 0);
```

The panel state is available for a UI or lamp driver without callbacks:

```c
struct mp7_panel_state state;
mp7_get_panel_state(ctx, &state);

/* stopped: light the lens for state.bank; running: pulse it when
 * state.flash_count advances. */
```

`struct mp7_panel_state` carries the applied bank and button mask, any pending
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

## User-defined patterns

The library ships with the 20 factory rhythms and a second set of 20 pattern
slots that the host fills itself, plus a switch that points the sequencer at
those slots instead of at the factory rhythms. The factory rhythms stay
read-only in either mode.

A user pattern is a complete pattern and not a rhythm plus the shared
percussion layer: its grid may use every instrument, the Tambourine, Guiro
and Quijada included, and the shared layer is never added to it.

```c
#include <mp7.h>

struct mp7_user_pattern pattern;

/* 64 columns of 16-bit instrument masks, bit n = instrument n plays. */
for (unsigned int column = 0; column < 64; column++) {
    pattern.notes[column] = ((column % 8) == 0)
        ? (uint16_t)((1u << MP7_INSTR_BASS) | (1u << MP7_INSTR_COWBELL))
        : 0;
}
pattern.enable = ~(uint64_t)0;  /* every column is enabled */
pattern.notes_period = 64;      /* the grid does not repeat earlier */
pattern.triplet = false;

mp7_set_user_pattern(ctx, 0, &pattern);  /* slot 0 = bank A, button 0 */
mp7_set_pattern_source(ctx, MP7_PATTERN_SOURCE_USER);
mp7_select_rhythm(ctx, MP7_BANK_A, mp7_rhythm_button_bit(MP7_RHYTHM_WALTZ));
```

`mp7_get_user_pattern()` reads a slot back, `mp7_set_user_pattern(ctx, index,
NULL)` empties one, and `mp7_reset()` empties all of them and returns to the
preset source. A pattern that is playing changes with the pattern.

With no button pressed the user source keeps the transport running and stays
silent: the shared percussion layer belongs to the preset source, and a user
pattern carries its own percussion lines.

## Synthesis tweaks

The extended build (`LIBPOPPY_EXTRA`, which is the default) exposes a few
synthesis parameters that the original unit keeps fixed. They are not panel
controls, and they start from the factory values, so a context that never
touches them sounds exactly like the preset build.

```c
mp7_set_accent_gain(ctx, 78643);    /* accented columns, 1.2x (Q16 units) */
mp7_set_snare1_decay(ctx, 65536);   /* Snare 1 decay, 1.0x (Q16 units) */
mp7_set_snare1_cutoff(ctx, 5500);   /* Snare 1 band-pass, Hz */
mp7_set_hihat_cutoff(ctx, 1000);    /* Hihat high-pass, Hz */
```

The accent gain applies to user-pattern columns that set the
`MP7_PATTERN_ACCENT` bit, and is read when the column fires, so a change
takes effect on the notes triggered after it. The Snare 1 decay scales the
factory decay time; a note that is already sounding keeps playing and decays
at the new rate. The two cut-offs move the Snare 1 band-pass centre and the
Hihat high-pass.

None of them costs anything per sample: the accent is folded into the gain
the mixer already applies to a voice, and the other three only recompute
coefficients between blocks.

The cut-offs are also limited to two fifths of the sample rate, because a
biquad is only stable below Nyquist. At 44100 Hz that limit is above
15000 Hz, so the whole documented range is available at the default rate.

`mp7_reset()` restores the factory defaults for all of them.
