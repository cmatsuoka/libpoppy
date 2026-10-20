/*
 * mp7_internal.h - internal structures of the Mini Pops library.
 *
 * Not part of the public API. Two kinds of things live here:
 *
 *   1. The shared layer (fixed-point helpers, the sine table and its builder,
 *      the PRNG, the biquads, the soft-clip LUT, the mixer core). These must
 *      stay free of the mp7_ prefix so a future variant can link them; they
 *      are named poppy_*.
 *   2. The per-variant state of this instrument: mp7_context and the tables
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

/* Full sine period, one entry per sample of a 4096-step cycle. */
#define POPPY_SINE_TABLE_SIZE 4096

/* Number of points in the soft-clip transfer table. */
#define POPPY_SOFTCLIP_TABLE_SIZE 256

/* A factory rhythm is a note grid plus a step-enable mask and a triplet flag.
 * Columns that are disabled in the combined mask take no time at all.
 *
 * The Tambourine, Guiro and Quijada rows are identical in every factory
 * rhythm, so they are stored once as MP7_PATTERN_PERCUSSION_ROWS shared rows
 * instead of being repeated in each rhythm's grid. The per-rhythm grids hold
 * the MP7_PATTERN_MELODIC_ROWS rows above them. */
#define MP7_PATTERN_STEPS 64
#define MP7_PATTERN_PERCUSSION_ROWS 3
#define MP7_PATTERN_MELODIC_ROWS (MP7_INSTR_COUNT - MP7_PATTERN_PERCUSSION_ROWS)

struct mp7_factory_rhythm {
    const uint8_t (*notes)[MP7_PATTERN_STEPS]; /* melodic rows, mp7_instr order */
    const uint8_t *enable;                     /* 64 bytes, 0 or 1 */
    uint8_t triplet;
};

extern const struct mp7_factory_rhythm mp7_factory_rhythms[MP7_RHYTHM_COUNT];
extern const uint8_t mp7_shared_percussion_notes[MP7_PATTERN_PERCUSSION_ROWS]
                                                [MP7_PATTERN_STEPS];
extern const char *const mp7_rhythm_names[MP7_RHYTHM_COUNT];

/* Marker written into a context by mp7_init(), so that a half-initialized
 * context can be spotted. */
#define MP7_CONTEXT_MAGIC 0x4D5037u /* "MP7" */

/* Q15 sine of 2*pi*i/4096 for i = 0..4095, i.e. the full period.
 *
 * This is a file-scope `const` array in flash, shared by every context,
 * every voice and every future variant of the library. It is the only sine
 * data in the image, and no context carries a copy of it. */
extern const int16_t poppy_sine[POPPY_SINE_TABLE_SIZE];

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
 * applies the 0.5 factor itself. The direct gains are the three
 * Additional Volume faders, in Tambourine/Guiro/Quijada order. */
#define POPPY_MIXER_DIRECT_COUNT 3

struct poppy_mixer {
    int32_t panel_volume_q16; /* the panel Volume, 0..4; the mixer applies 0.5x */
    int32_t balance_q16;
    int32_t balance_a_gain_q16; /* cached, recomputed on a Balance change */
    int32_t balance_b_gain_q16;
    int32_t direct_gain_q16[POPPY_MIXER_DIRECT_COUNT];
};

/* Balance touches exactly two groups: Base against Hihat/Cymbal 1/Cymbal 2.
 * The group tags are neutral because the mixer core is shared. */
enum poppy_mix_group {
    POPPY_MIX_GROUP_NONE = 0,
    POPPY_MIX_GROUP_A,
    POPPY_MIX_GROUP_B
};

#define MP7_DIRECT_TAMBOURINE 0u
#define MP7_DIRECT_GUIRO 1u
#define MP7_DIRECT_QUIJADA 2u
#define MP7_DIRECT_INDEX_NONE 0xFFu

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
    uint8_t direct_gain_index; /* index into mixer.direct_gain_q16, or none */
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
 * combined grid in mp7_instr row-major order.
 *
 * The sequencer triggers on sub-step boundaries. A straight column spans two
 * sub-steps and a triplet column spans three; `substep` counts the sub-steps
 * elapsed in the current column. `sub_step_pos` counts down samples to the
 * next boundary and is zero on the sample where the boundary is evaluated. */
struct mp7_sequencer {
    uint32_t selected;                     /* bit n = mp7_rhythm n */
    uint32_t tempo;                        /* BPM, clamped to 1..240 */
    uint32_t sub_step_samples;             /* round(2.5 * sample_rate / tempo) */
    uint32_t sub_step_pos;                 /* samples until the next boundary */
    uint64_t enable;                       /* combined step-enable mask */
    uint8_t notes[MP7_INSTR_COUNT][MP7_PATTERN_STEPS];
    uint8_t column;                        /* current enabled column */
    uint8_t substep;                       /* sub-steps elapsed in the column */
    uint8_t triplet;
    uint8_t guiro_gated;
};

/* Per-variant context. The caller owns the memory; mp7_context_size() gives
 * the size and mp7_init() prepares it. */
struct mp7_context_s {
    uint32_t sample_rate;                  /* Hz, never 0 after init */
    uint32_t magic;                         /* MP7_CONTEXT_MAGIC once initialized */
    struct mp7_voice voices[MP7_INSTR_COUNT];
    struct mp7_sequencer sequencer;
    struct poppy_mixer mixer;
};

/* Voice and engine internals, exercised directly by the Phase 2 test harness.
 * These are not part of the public API.
 *
 * A trigger carries no velocity: the panel has no velocity control and the
 * level that reaches the output is the voice's own Level, times the Balance
 * or direct gain, times the master Volume. */
void mp7_voice_init_all(struct mp7_context_s *ctx);
void mp7_voice_trigger(struct mp7_voice *voice);
void mp7_voice_release(struct mp7_voice *voice);
int64_t mp7_voice_process(struct mp7_voice *voice);

int64_t mp7_engine_process(struct mp7_voice *voice);

/* Rhythm player internals. These are the shared, testable core of the
 * sequencer; the public mp7_set_rhythm()/mp7_set_tempo() wrappers are added
 * by the API phase. */
void mp7_pattern_init(struct mp7_context_s *ctx);
void mp7_pattern_combine(struct mp7_context_s *ctx, uint32_t selected_mask);
void mp7_pattern_set_tempo(struct mp7_context_s *ctx, uint32_t bpm);
void mp7_pattern_reset(struct mp7_context_s *ctx);
void mp7_pattern_tick(struct mp7_context_s *ctx);

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
void poppy_mixer_set_direct_gain(struct poppy_mixer *mixer,
                                 unsigned int index,
                                 int32_t gain_q16);
int32_t poppy_mixer_balance_gain_q16(const struct poppy_mixer *mixer,
                                     unsigned int group);
int64_t poppy_mixer_pan_left(int64_t sample_q31, int32_t pan_q16);
int64_t poppy_mixer_pan_right(int64_t sample_q31, int32_t pan_q16);
int16_t poppy_mixer_to_q15(int64_t sum_q31, int32_t panel_volume_q16);

#endif /* MP7_INTERNAL_H */
