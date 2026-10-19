/*
 * mp7.h - public API of the Mini Pops library.
 *
 * The Mini Pops is a 15-voice percussion unit. This library is a integer-only
 * (32-bit fixed point) reimplementation of its sound generation and its 20
 * factory rhythm patterns. It renders a single stereo mix into a caller-supplied
 * buffer; there is no allocation, no I/O and no floating point in the library.
 */

#ifndef MP7_H
#define MP7_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample rate used when the caller passes 0 to mp7_init() or
 * mp7_set_sample_rate(). The FM-1 runs at 44100. */
#define MP7_DEFAULT_SAMPLE_RATE 44100u

/* The 15 instruments, in the order the reference numbers its voice slots. */
typedef enum {
    MP7_INSTR_BASS = 0,
    MP7_INSTR_SNARE_1,
    MP7_INSTR_SNARE_2,
    MP7_INSTR_HI_BONGO,
    MP7_INSTR_LO_BONGO,
    MP7_INSTR_CONGA,
    MP7_INSTR_COWBELL,
    MP7_INSTR_CLAVES,
    MP7_INSTR_HIHAT,
    MP7_INSTR_RIMSHOT,
    MP7_INSTR_CYMBAL_1,
    MP7_INSTR_CYMBAL_2,
    MP7_INSTR_TAMBOURINE,
    MP7_INSTR_GUIRO,
    MP7_INSTR_QUIJADA,
    MP7_INSTR_COUNT = 15
} mp7_instr;

/* The 20 factory rhythms: 10 panel keys x 2 banks. */
typedef enum {
    MP7_RHYTHM_WALTZ = 0,
    MP7_RHYTHM_SAMBA,
    MP7_RHYTHM_MAMBO,
    MP7_RHYTHM_RUMBA,
    MP7_RHYTHM_TANGO,
    MP7_RHYTHM_BOSSANOVA,
    MP7_RHYTHM_ROCK_1,
    MP7_RHYTHM_ROCK_3,
    MP7_RHYTHM_MARCH_2_4,
    MP7_RHYTHM_SWING,
    MP7_RHYTHM_JAZZ_WALTZ,
    MP7_RHYTHM_MERINGUE,
    MP7_RHYTHM_CHA_CHA,
    MP7_RHYTHM_BEGUINE,
    MP7_RHYTHM_HABANERA,
    MP7_RHYTHM_SLOW_ROCK,
    MP7_RHYTHM_ROCK_2,
    MP7_RHYTHM_ROCK_4,
    MP7_RHYTHM_MARCH_6_8,
    MP7_RHYTHM_LATIN_SWING,
    MP7_RHYTHM_COUNT = 20
} mp7_rhythm;

typedef struct mp7_context_s mp7_context;

/* Number of bytes needed for one context. */
size_t mp7_context_size(void);

/* Initialize a context and reset its voices and sequencer. A sample_rate of
 * 0 selects MP7_DEFAULT_SAMPLE_RATE. */
void mp7_init(mp7_context *ctx, uint32_t sample_rate);

/* Change the sample rate.
 *
 * The biquad coefficients depend on the rate, so every voice is re-initialized
 * and the envelopes and filter state of any sounding voice are lost. */
void mp7_set_sample_rate(mp7_context *ctx, uint32_t sample_rate);

/* Reset the voices and the sequencer, keeping the sample rate. */
void mp7_reset(mp7_context *ctx);

/* Render `frames` stereo interleaved frames into `stereo_out`.
 *
 * `stereo_out` must have room for `frames` stereo frames (2 * frames
 * int16_t values). The buffer is cleared in bounded chunks, so any
 * `frames` value is safe on a 32-bit target as long as the buffer is
 * really that large. */
void mp7_process(mp7_context *ctx, int16_t *stereo_out, size_t frames);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MP7_H */
