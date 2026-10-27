/*
 * mp7_internal.h - internal structures of the Mini Pops library.
 *
 * Not part of the public API. Two kinds of things live here:
 *
 *   1. The shared layer (fixed-point helpers, the sine table and its builder,
 *      the PRNG, the biquads, the soft-clip LUT, the mixer core). These must
 *      stay free of the mp7_ prefix so a future variant can link them; they
 *      are named poppy_*.
 *   2. The per-variant state of this instrument: the context and the tables
 *      that describe its voices and rhythms.
 *
 * The file name keeps the mp7_ prefix because the tree holds one variant
 * today; the rule is about symbols, not file names.
 */

#ifndef MP7_INTERNAL_H
#define MP7_INTERNAL_H

#include <stdint.h>

#include "mp7.h"
#include "mp7_fixed.h"

/* Full sine period, one entry per sample of a 1024-step cycle. The
 * oscillator and the filter coefficient setup interpolate between entries,
 * so this is the stored resolution only, not the output resolution. */
#define POPPY_SINE_TABLE_SIZE 1024

/* First quadrant including both endpoints, i.e. the zero crossing at index 0
 * and the peak at index 256. The other quadrants are mirrored and negated
 * from this table. */
#define POPPY_SINE_QUAD_SIZE 257

/* Number of points in the soft-clip transfer table. */
#define POPPY_SOFTCLIP_TABLE_SIZE 256

/* A factory rhythm is a note grid plus a step-enable mask and a triplet flag.
 * The enable mask is a 64-bit word, one bit per column; columns that are
 * disabled in the combined mask take no time at all.
 *
 * A column of the note grid is a 16-bit instrument mask: bit n is set when
 * instrument n (mp7_instr order) plays in that column, so bit 0 is the Bass
 * Drum and bit 14 the Quijada. A rhythm's own grid holds the twelve rhythm
 * instruments (bits 0..11). The Tambourine, Guiro and Quijada hits are the
 * same in every rhythm and are a separate definition of their own,
 * mp7_shared_percussion_notes; it is OR-ed into the combined grid and it is
 * what plays when no rhythm button is pressed.
 *
 * A rhythm stores only the shortest period that repeats to fill the
 * 64-column cycle and sets notes_period to that length. Every period is a
 * power of two, so column c is notes[c & (period - 1)]. */
#define MP7_PATTERN_QUARTER_STEPS 16
#define MP7_PATTERN_HALF_STEPS 32
#define MP7_PATTERN_STEPS 64

/* The fixed Tambourine/Guiro/Quijada layer. It is present in every factory
 * rhythm and keeps playing when no rhythm key is pressed. */
#define MP7_PATTERN_PERCUSSION_MASK \
    ((uint16_t)((((uint32_t)1u << MP7_INSTR_TAMBOURINE) | \
                 ((uint32_t)1u << MP7_INSTR_GUIRO) | \
                 ((uint32_t)1u << MP7_INSTR_QUIJADA))))

/* The instruments a rhythm's own grid may use: everything below the
 * Tambourine, which is the first of the shared percussion voices. */
#define MP7_PATTERN_RHYTHM_MASK 0x0FFFu

struct mp7_factory_rhythm {
    const uint16_t *notes; /* notes_period columns, bit n = instrument n plays */
    uint64_t enable;        /* bit c = column c is enabled */
    uint8_t notes_period;   /* columns in notes: 16, 32 or 64 */
    uint8_t triplet;
};

extern const struct mp7_factory_rhythm mp7_factory_rhythms[MP7_RHYTHM_COUNT];
extern const uint16_t mp7_shared_percussion_notes[MP7_PATTERN_HALF_STEPS];

/* Which set of patterns the sequencer plays. The field exists in every
 * build, so the factory path needs no conditional code; only the extended
 * build can move it away from the preset source. */
#define MP7_SOURCE_PRESET 0u
#define MP7_SOURCE_USER 1u

#ifdef LIBPOPPY_EXTRA

/* The user slots are addressed the way the factory rhythms are, so the two
 * sets have the same size. */
#if MP7_USER_PATTERN_COUNT != 20
#error "the user pattern slots must match the 20 factory rhythms"
#endif

/* One user-defined pattern slot.
 *
 * The grid holds every instrument, the Tambourine, Guiro and Quijada
 * included, so a user pattern is a complete pattern and not a rhythm plus
 * the shared percussion layer. `used` is clear for an empty slot, which
 * plays nothing and disables no column. */
struct mp7_user_slot {
    uint16_t notes[MP7_PATTERN_STEPS];
    uint64_t enable;
    uint8_t notes_period;
    uint8_t triplet;
    uint8_t used;
};

/* Accent processing is part of the extended build only. The 16th bit of a
 * user pattern column is the accent flag; the lower 15 bits are instruments. */
#define MP7_PATTERN_INSTRUMENT_MASK ((uint16_t)((1u << MP7_INSTR_COUNT) - 1u))
#define MP7_PATTERN_ACCENT_BIT      ((uint16_t)(1u << MP7_INSTR_COUNT))
#define MP7_PATTERN_ACCENT_GAIN_Q16 ((int32_t)((6 * POPPY_Q16_ONE) / 5))

#endif /* LIBPOPPY_EXTRA */

/* First quadrant of the sine, Q15, for i = 0..256. This is the only sine
 * data in the image, and it is shared by every context, every voice and every
 * future variant of the library. */
extern const int16_t poppy_sine_quad1[POPPY_SINE_QUAD_SIZE];

/* The full 1024-entry period, allocated and rebuilt from poppy_sine_quad1
 * the first time a context is created. NULL until then (and if that
 * allocation fails, in which case lookups fall back to the quadrant, which
 * is bit-exact with the full table). The pointer is published only after the
 * table is complete, so a lookup never sees a half-written table. */
extern int16_t *poppy_sine;

/* Allocate and rebuild the full sine period. Idempotent and meant to be
 * called once, before any voice runs; the table lives for the process. */
void poppy_sine_init(void);

/* Q15 sine of 2*pi*i/POPPY_SINE_TABLE_SIZE at a whole table index, using the
 * rebuilt table when it is available and the flash-resident quadrant
 * otherwise. */
static inline int16_t poppy_sine_at(uint32_t index)
{
    index &= 0x3FFu;

    if (poppy_sine != 0) {
        return poppy_sine[index];
    }
    if (index <= 256u) {
        return poppy_sine_quad1[index];
    }
    if (index <= 511u) {
        return poppy_sine_quad1[512u - index];
    }
    if (index <= 767u) {
        return (int16_t)-poppy_sine_quad1[index - 512u];
    }
    return (int16_t)-poppy_sine_quad1[1024u - index];
}

/* Q15 sine of a 28-bit phase (one full cycle is 2^28), linearly interpolated
 * between adjacent table entries. The interpolation error is below one Q15
 * LSB for the stored 1024-step table, and the full 28-bit phase keeps the
 * oscillator's phase resolution far finer than the old whole-index lookup. */
int16_t poppy_sine_interp(uint32_t phase);

/* Soft-clip curve over x in [-3, 3], indexed through u = x/3 in [-1, 1].
 *
 * The stored value is softclip(3*u) / 10 in Q31. The /10 is undone by the
 * engine, which multiplies this table's result by 10 to recover the +/-10
 * saturation range.
 * Q31 would overflow at 10, so the caller accumulates in int64_t. */
extern const int32_t poppy_softclip[POPPY_SOFTCLIP_TABLE_SIZE];

/* Envelope stages. HOLD is used by the gated Guiro amplitude envelope. */
enum poppy_env_stage {
    POPPY_ENV_IDLE = 0,
    POPPY_ENV_ATTACK,
    POPPY_ENV_DECAY,
    POPPY_ENV_HOLD,
    POPPY_ENV_RELEASE
};

struct poppy_env {
    int32_t value;          /* Q30, clamped to [0, 1] on output */
    int32_t target;         /* Q30 */
    int32_t coef;           /* Q30 */
    int32_t attack_coef;    /* Q30 */
    int32_t decay_coef;     /* Q30 */
    uint8_t stage;
    uint8_t attack_to_hold;
};

struct poppy_osc {
    uint32_t phase;         /* 28-bit accumulator */
    uint32_t inc;
};

/* Direct-form I biquad. Coefficients are Q30; state and output are Q31. */
struct poppy_biquad {
    int32_t b0;
    int32_t b1;
    int32_t b2;
    int32_t a1;
    int32_t a2;
    int32_t x1;
    int32_t x2;
    int32_t y1;
    int32_t y2;
};

enum mp7_engine {
    MP7_ENGINE_SINE_AD = 0,
    MP7_ENGINE_DUAL_TONE,
    MP7_ENGINE_NOISE,
    MP7_ENGINE_GUIRO,
    MP7_ENGINE_QUIJADA
};

/* Global mix controls. The master volume is the panel value (0..4); the mixer
 * applies the 0.5 factor itself. The instrument gains apply to every voice;
 * Tambourine, Guiro and Quijada default to 0.5 (their panel Additional Volume
 * faders) and the remaining instruments default to 1.0. */
struct poppy_mixer {
    int32_t panel_volume_q16; /* the panel Volume, 0..4; the mixer applies 0.5x */
    int32_t balance_q16;
    int32_t balance_a_gain_q16; /* cached, recomputed on a Balance change */
    int32_t balance_b_gain_q16;
    int32_t instr_gain_q16[MP7_INSTR_COUNT];
};

/* Balance touches exactly two groups: Bass Drum against Hihat/Cymbal 1/Cymbal 2.
 * The group tags are neutral because the mixer core is shared. */
enum poppy_mix_group {
    POPPY_MIX_GROUP_NONE = 0,
    POPPY_MIX_GROUP_A,
    POPPY_MIX_GROUP_B
};

/* One voice. The union is not used; the engines share the named fields so the
 * dispatch stays readable, at the cost of a few unused bytes per voice. */
struct mp7_voice {
    mp7_instr instr;
    uint8_t engine;
    uint8_t active;

    struct poppy_osc body_osc;
    struct poppy_osc aux_osc;

    struct poppy_env amp_env;
    struct poppy_env body_env;
    struct poppy_env aux_env;
    struct poppy_env short_env;
    struct poppy_env long_env;

    struct poppy_biquad highpass;
    struct poppy_biquad bandpass;

    int32_t tick_q30;       /* noise tone depth, from the fixed Tick value */
    int32_t level2_q16;     /* dual-tone second-oscillator level, Q16 (65536 = 1.0) */
    int32_t level_q16;      /* fixed per-voice Level, Q16 */
    int32_t pan_q16;        /* fixed per-voice Pan, Q16 (32768 = centre) */
    uint8_t balance_group;  /* enum poppy_mix_group */
#ifdef LIBPOPPY_EXTRA
    int32_t accent_gain_q16; /* pattern accent gain, Q16 (default 1.0) */
#endif
    uint32_t prng;          /* xorshift32 state, never zero */
    int32_t last_rate;      /* Guiro rate-oscillator sign detector */
    int32_t prev_clock;     /* Quijada comparator history, Q30 */
    int32_t threshold_lo;   /* Quijada thresholds, Q30 */
    int32_t threshold_hi;
};

/* Rhythm player state.
 *
 * `selected` has one bit per mp7_rhythm. `enable` is the combined 64-column
 * step-enable mask (bit c is set when column c is enabled); it is zero when
 * no rhythm is selected, which means the sequencer is stopped. `notes` is the
 * combined grid: one 16-bit instrument mask per column, bit n set when
 * instrument n plays in that column.
 *
 * The sequencer triggers on sub-step boundaries. A straight column spans three
 * sub-steps and a triplet column spans four; `substep` counts the sub-steps
 * elapsed in the current column. `sub_step_pos` counts down samples to the
 * next boundary and is zero on the sample where the boundary is evaluated.
 *
 * `pending_selected` holds a rhythm selection that is waiting for the current
 * pattern cycle to finish; `pending_valid` is set while it differs from
 * `selected`. The change is applied when the last enabled column wraps to the
 * first enabled column, and the new selection starts at its first column.
 *
 * `running` is the transport, which is separate from the key selection. When
 * it is set and `selected` is zero, the combined grid is the fixed
 * Tambourine/Guiro/Quijada percussion layer.
 *
 * `source` is the set of patterns the selection refers to: the factory
 * rhythms or, in an extended build, the user slots. */
struct mp7_sequencer {
    uint32_t selected;                     /* bit n = mp7_rhythm n */
    uint32_t pending_selected;             /* selection for the next cycle */
    uint32_t tempo;                        /* BPM, clamped to 1..240 */
    uint32_t sub_step_samples;             /* round(2.5 * sample_rate / tempo) */
    uint32_t sub_step_pos;                 /* samples until the next boundary */
    uint32_t flash_count;                  /* monotonic lamp flash counter */
    uint64_t enable;                       /* combined step-enable mask */
    uint16_t notes[MP7_PATTERN_STEPS];     /* bit n = instrument n plays */
    uint16_t buttons;                      /* applied ten-button mask */
    uint16_t pending_buttons;              /* queued ten-button mask */
    uint8_t bank;                          /* applied lever position */
    uint8_t pending_bank;                  /* queued lever position */
    uint8_t column;                        /* current enabled column */
    uint8_t substep;                       /* sub-steps elapsed in the column */
    uint8_t triplet;
    uint8_t guiro_gated;                   /* the sequencer's own Guiro gate */
    uint8_t pending_valid;
    uint8_t running;                       /* transport: false after a stop */
    uint8_t source;                        /* MP7_SOURCE_PRESET or _USER */
};

/* Per-variant context. mp7_create_context() allocates and prepares it; the
 * fixed-memory entry points below are internal so the tests and mp7_reset()
 * can drive an existing context.
 *
 * The Guiro is the one gated voice, and the gate has two holders: the
 * sequencer's own column gate (`sequencer.guiro_gated`) and a note the caller
 * triggered itself (`guiro_manual_gated`). The voice sounds while either one
 * holds the gate; `guiro_open` says whether it is gated right now. */
struct mp7_context_s {
    uint32_t sample_rate;                  /* Hz, never 0 after init */
    struct mp7_voice voices[MP7_INSTR_COUNT];
    struct mp7_sequencer sequencer;
    struct poppy_mixer mixer;
    uint8_t guiro_manual_gated;             /* the caller's own Guiro note-on */
    uint8_t guiro_open;                     /* the Guiro voice is gated now */
#ifdef LIBPOPPY_EXTRA
    struct mp7_user_slot user_patterns[MP7_USER_PATTERN_COUNT];
#endif
};

/* Fixed-memory context setup, kept out of the public header. */
void mp7_init(mp7_context ctx, uint32_t sample_rate);

/* Voice and engine internals, exercised directly by the Phase 2 test harness.
 * These are not part of the public API.
 *
 * A trigger carries no velocity: the panel has no velocity control and the
 * level that reaches the output is the voice's own Level, times the Balance
 * and per-instrument gain, times the master Volume. */
void mp7_voice_init_all(struct mp7_context_s *ctx);
void mp7_voice_trigger(struct mp7_voice *voice);
void mp7_voice_release(struct mp7_voice *voice);
int64_t mp7_voice_process(struct mp7_voice *voice);

int64_t mp7_engine_process(struct mp7_voice *voice);

/* Rhythm player internals. These are the testable core of the sequencer; the
 * public rhythm setters in mp7.c wrap them. */
void mp7_pattern_init(struct mp7_context_s *ctx);
void mp7_pattern_combine(struct mp7_context_s *ctx, uint32_t selected_mask);
void mp7_pattern_set_tempo(struct mp7_context_s *ctx, uint32_t bpm);
void mp7_pattern_reset(struct mp7_context_s *ctx);
void mp7_pattern_tick(struct mp7_context_s *ctx);

/* Apply the two Guiro gate holders and gate or release the voice so that it
 * matches them. The ratchet restarts only when the gate opens. */
void mp7_guiro_gate(struct mp7_context_s *ctx,
                    uint8_t sequencer_gate,
                    uint8_t manual_gate);

void poppy_osc_set_freq(struct poppy_osc *osc, uint32_t freq_hz, uint32_t sample_rate);
int32_t poppy_sine_sample(const struct poppy_osc *osc);
void poppy_env_trigger(struct poppy_env *env);
void poppy_env_trigger_hold(struct poppy_env *env);
void poppy_env_release(struct poppy_env *env, int32_t release_coef);
int32_t poppy_env_process(struct poppy_env *env);
int32_t poppy_env_coef_us(uint32_t time_us, uint32_t sample_rate);

void poppy_biquad_set_highpass(struct poppy_biquad *filter,
                               uint32_t freq_hz,
                               uint32_t sample_rate);
void poppy_biquad_set_bandpass(struct poppy_biquad *filter,
                               uint32_t freq_hz,
                               uint32_t res_q88,
                               uint32_t sample_rate);
int32_t poppy_biquad_process(struct poppy_biquad *filter, int32_t sample_q31);
int64_t poppy_softclip_q31(int64_t sample_q31);

void poppy_mixer_init(struct poppy_mixer *mixer);
void poppy_mixer_set_volume(struct poppy_mixer *mixer, int32_t panel_volume_q16);
void poppy_mixer_set_balance(struct poppy_mixer *mixer, int32_t balance_q16);
void poppy_mixer_set_instr_gain(struct poppy_mixer *mixer,
                                unsigned int instr,
                                int32_t gain_q16);
int32_t poppy_mixer_balance_gain_q16(const struct poppy_mixer *mixer,
                                     unsigned int group);
int64_t poppy_mixer_pan_left(int64_t sample_q31, int32_t pan_q16);
int64_t poppy_mixer_pan_right(int64_t sample_q31, int32_t pan_q16);
int16_t poppy_mixer_to_q15(int64_t sum_q31, int32_t panel_volume_q16);

#endif /* MP7_INTERNAL_H */
