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
./mp7_test --pattern          # verify the 20 patterns, combination and timing
./mp7_test --rhythms dir      # render all 20 rhythms and two combinations
