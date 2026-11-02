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

/* A factory rhythm is a note grid plus a step-enable mask. The enable mask
 * is a 64-bit word, one bit per column; columns that are disabled in the
 * combined mask take no time at all.
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

/* The public column count is the cycle length: a view always carries the
 * whole cycle, and the stored period is repeated out to it. */
#if MP7_PATTERN_COLUMNS != MP7_PATTERN_STEPS
#error "the public pattern column count must match the cycle length"
#endif

/* The step-enable mask of the compound (shuffle) grid: six columns enabled,
 * two disabled, every eight columns. The three built-in shuffle rhythms -
 * March 2/4, March 6/8 and Slow Rock - all use this one mask, so the mask
 * cannot tell them apart.
 *
 * It is the mask of the 2/4 and the 6/8 signature, and the library hands it
 * out through mp7_timesig_enable_mask(). */
#define MP7_PATTERN_COMPOUND_MASK UINT64_C(0x3F3F3F3F3F3F3F3F)

/* The step-enable masks of the time signatures.
 *
 * Every pattern's mask comes from its time signature, so the masks are
 * defined once, here, and a pattern doesn't carries a mask of its own. The
 * 4/4 mask enables every column; the 3/4 mask skips four columns twice in
 * every half of the cycle. */
#define MP7_ENABLE_4_4 UINT64_C(0xFFFFFFFFFFFFFFFF)
#define MP7_ENABLE_3_4 UINT64_C(0xFF0F0FFFFF0F0FFF)
#define MP7_ENABLE_5_4T UINT64_C(0x0F0F3F3F0F0F3F3F)
#define MP7_ENABLE_7_4 UINT64_C(0x00FFFFFFFFFFFFFF)
#define MP7_ENABLE_2_4 MP7_PATTERN_COMPOUND_MASK
#define MP7_ENABLE_6_8T MP7_PATTERN_COMPOUND_MASK

/* The grid a pattern plays on.
 *
 * The subdivision is not a setting of its own and it is not part of the
 * public API: it follows the pattern's step-enable mask, and the mask
 * follows the pattern's time signature. The compound mask and the 5/4 mask
 * play compound, every other mask plays straight. */
typedef enum {
    MP7_SUBDIV_STRAIGHT = 0,
    MP7_SUBDIV_COMPOUND = 1
} mp7_subdivision;

/* The compound (shuffle) grid is the one the shuffle rhythms play on, and it
 * is the only grid that can carry a 2:1 swung eighth. The subdivision of a
 * pattern follows its enable mask: the compound mask and the 5/4 mask play
 * compound, every other mask plays straight.
 *
 * The 5/4 signature has a hole pattern of its own, but it plays on the
 * compound grid so its grouped columns keep a triplet feel and the loop is
 * longer than the same number of columns on the straight grid. */
static inline uint8_t mp7_mask_is_compound(uint64_t enable)
{
    return ((enable == MP7_PATTERN_COMPOUND_MASK) ||
            (enable == MP7_ENABLE_5_4T)) ? 1u : 0u;
}

/* The fixed Tambourine/Guiro/Quijada layer. It is present in every factory
 * rhythm and keeps playing when no rhythm key is pressed. */
#define MP7_PATTERN_PERCUSSION_MASK \
    ((uint16_t)((((uint32_t)1u << MP7_INSTR_TAMBOURINE) | \
                 ((uint32_t)1u << MP7_INSTR_GUIRO) | \
                 ((uint32_t)1u << MP7_INSTR_QUIJADA))))

/* The instruments a rhythm's own grid may use: everything below the
 * Tambourine, which is the first of the shared percussion voices. */
#define MP7_PATTERN_RHYTHM_MASK 0x0FFFu

/* Both the rhythm grids and the shared percussion layer may store only a
 * shorter period (16 or 32 columns) that repeats to fill the 64-column
 * cycle. The period is always a power of two, so masking wraps the column. */
static inline unsigned int wrap_column(unsigned int column, unsigned int period)
{
    return column & (period - 1u);
}

struct mp7_factory_rhythm {
    const uint16_t *notes; /* notes_period columns, bit n = instrument n plays */
    uint64_t enable;        /* bit c = column c is enabled */
    uint8_t notes_period;   /* columns in notes: 16, 32 or 64 */
    mp7_timesig timesig;    /* the meter the rhythm is named after */
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
 * plays nothing and disables no column.
 *
 * The slot stores the time signature and not the step-enable mask: the mask
 * is derived from the signature, so the two cannot drift apart and a slot
 * cannot hold a mask the hardware does not have. */
struct mp7_user_slot {
    uint16_t notes[MP7_PATTERN_STEPS];
    uint8_t timesig;      /* mp7_timesig of the pattern */
    uint8_t used;
};

/* Accent processing is part of the extended build only. The 16th bit of a
 * user pattern column is the accent flag; the lower 15 bits are instruments. */
#define MP7_PATTERN_INSTRUMENT_MASK ((uint16_t)((1u << MP7_INSTR_COUNT) - 1u))
#define MP7_PATTERN_ACCENT_BIT      ((uint16_t)(1u << MP7_INSTR_COUNT))
#define MP7_PATTERN_ACCENT_GAIN_Q16 ((int32_t)((6 * POPPY_Q16_ONE) / 5))

#endif /* LIBPOPPY_EXTRA */

/* The factory cut-offs the extended build exposes as run-time parameters.
 *
 * The noise engine tunes its band-pass to the voice's frequency and its
 * high-pass to the voice's highpass field, so these are the Snare 1
 * band-pass center, the Snare 2 band-pass center, the Hihat band-pass
 * center and the Hihat high-pass cut-off. The voice table below uses the
 * same names, so the table and the defaults cannot drift apart. */
#define MP7_SNARE1_BANDPASS_HZ 5500u
#define MP7_SNARE2_BANDPASS_HZ 5500u
#define MP7_HIHAT_BANDPASS_HZ  10000u
#define MP7_HIHAT_HIGHPASS_HZ  1000u

#ifdef LIBPOPPY_EXTRA

/* The cut-off range the extended build exposes, in Hz.
 *
 * The cut-off is the band-pass center of the instrument, for every
 * instrument that has one: the Hihat's high-pass is fixed at its factory
 * value, because moving it above the band-pass only dulls the sound
 * instead of opening it up. The Hihat's band-pass cannot go below its
 * fixed high-pass, so its range starts higher than the snares'. */
#define MP7_CUTOFF_MIN_HZ       1000u
#define MP7_CUTOFF_MAX_HZ       15000u
#define MP7_HIHAT_CUTOFF_MIN_HZ 2000u

/* The decay multiplier range, in Q16 units (1.0 is POPPY_Q16_ONE). */
#define MP7_DECAY_MIN_Q16 (POPPY_Q16_ONE / 4)
#define MP7_DECAY_MAX_Q16 (4 * POPPY_Q16_ONE)

/* What the extended build exposes for one instrument.
 *
 * `has_decay` and `has_cutoff` say which of the two parameters the
 * instrument has; a call for an instrument that has none is ignored, so
 * the API can grow without a signature change. `cutoff_min_hz` and
 * `cutoff_max_hz` are the range of the cut-off, and `cutoff_default_hz`
 * the value the instrument starts at.
 *
 * `slot` is the index of the instrument in the two parameter arrays of
 * the context. The arrays are as long as the instruments that have the
 * parameters, not as long as the instrument list, so the state stays
 * small on a target where it matters.
 *
 * The cut-off is always the band-pass center of the instrument's filter.
 * The Hihat's high-pass stays at its factory value, because moving it
 * above the band-pass only dulls the sound instead of opening it up. */
#define MP7_PARAM_SLOT_NONE 0xFFu

struct mp7_instr_param {
    uint8_t has_decay;
    uint8_t has_cutoff;
    uint8_t slot;
    uint32_t cutoff_min_hz;
    uint32_t cutoff_max_hz;
    uint32_t cutoff_default_hz;
};

/* The number of instruments that carry the synthesis parameters, which is
 * the length of the two parameter arrays in the context. */
#define MP7_PARAM_COUNT 3u

extern const struct mp7_instr_param mp7_instr_params[MP7_INSTR_COUNT];

/* The effective cut-off is also limited by the sample rate.
 *
 * A biquad is only stable below Nyquist, and the design wraps around at
 * half the sample rate, where the poles reach the unit circle and the
 * filter rings without decaying. Two fifths of the sample rate keeps
 * the poles well inside it (0.64 for the high-pass, 0.72 for the
 * band-pass) and leaves the whole documented range available at
 * 44100 Hz. */
#define MP7_CUTOFF_RATE_NUM 2u
#define MP7_CUTOFF_RATE_DEN 5u

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
 * applies the 0.5 factor itself. The instrument gains apply to every voice and
 * run from 0 (silence) to 2 (twice the reference level); Tambourine, Guiro and
 * Quijada default to 0.5 (their panel Additional Volume faders) and the
 * remaining instruments default to 1.0. The instrument pans default to center
 * and are the source of the per-voice pan positions. */
struct poppy_mixer {
    int32_t panel_volume_q16; /* the panel Volume, 0..4; the mixer applies 0.5x */
    int32_t balance_q16;
    int32_t balance_a_gain_q16; /* cached, recomputed on a Balance change */
    int32_t balance_b_gain_q16;
    int32_t instr_gain_q16[MP7_INSTR_COUNT]; /* 0..2, 1.0 = the reference level */
    int32_t instr_pan_q16[MP7_INSTR_COUNT]; /* 0 = hard left, 65536 = hard right */
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
    int32_t pan_q16;        /* per-voice Pan, Q16 (32768 = center) */
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
 * The sequencer triggers on column boundaries. A column lasts a fraction of a
 * beat: the tempo is in quarter notes per minute, and a beat holds eight
 * straight columns (a 32nd note each) or six compound ones (an 8th-note
 * triplet each). `column_pos` counts down the samples to the next boundary
 * and is zero on the sample where the boundary is evaluated; `column_fired`
 * says that the column that is running has already fired its notes.
 *
 * `subdivision` is the grid the combined pattern plays on. It is derived from
 * the enable masks of the selected patterns, not from the combined mask: a
 * compound pattern combined with a straight one plays compound. The 5/4
 * signature uses the compound grid as well, with its own step-enable mask.
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
    uint32_t beat_samples;                 /* round(60 * sample_rate / tempo) */
    uint32_t column_pos;                   /* samples until the next boundary */
    uint32_t step_remainder;               /* samples of the beat not yet handed out */
    uint32_t flash_count;                  /* monotonic lamp flash counter */
    uint64_t enable;                       /* combined step-enable mask */
    uint16_t notes[MP7_PATTERN_STEPS];     /* bit n = instrument n plays */
    uint16_t buttons;                      /* applied ten-button mask */
    uint16_t pending_buttons;              /* queued ten-button mask */
    uint8_t bank;                          /* applied lever position */
    uint8_t pending_bank;                  /* queued lever position */
    uint8_t column;                        /* current enabled column */
    uint8_t column_fired;                  /* the running column has fired its notes */
    uint8_t subdivision;                   /* mp7_subdivision of the combined grid */
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

    /* The effective gain of every voice: its fixed Level, times the Balance
     * and the per-instrument gain, times the pattern accent of the note that
     * is sounding.
     *
     * It is kept here rather than in a local of mp7_process() because it also
     * changes when a column fires, which can happen in the middle of a
     * block. Folding the accent in here keeps the per-sample path down to a
     * single multiply, the same as the preset-only build. */
    int32_t voice_gain_q16[MP7_INSTR_COUNT];

    uint8_t guiro_manual_gated;             /* the caller's own Guiro note-on */
    uint8_t guiro_open;                     /* the Guiro voice is gated now */
#ifdef LIBPOPPY_EXTRA
    struct mp7_user_slot user_patterns[MP7_USER_PATTERN_COUNT];

    /* User-modifiable synthesis parameters, reset by mp7_reset().
     *
     * The two arrays are indexed by the slot of the instrument in
     * mp7_instr_params[], so they are as long as the instruments that
     * carry the parameters and not as long as the instrument list. */
    int32_t accent_gain_q16;                    /* accent gain multiplier, Q16 */
    int32_t instr_decay_q16[MP7_PARAM_COUNT];   /* decay multiplier, Q16 */
    uint32_t instr_cutoff_hz[MP7_PARAM_COUNT];  /* band-pass center, Hz */
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

/* The effective gain of one voice, and the refresh of the cached copy in the
 * context.
 *
 * The gain is the voice's fixed Level, times the Balance of its group, times
 * the per-instrument gain and, in an extended build, times the accent of the
 * note that is sounding. The order of the multiplies is fixed, because each
 * one truncates. */
int32_t mp7_voice_gain_q16(const struct mp7_context_s *ctx,
                           const struct mp7_voice *voice);
void mp7_voice_refresh_gain(struct mp7_context_s *ctx, mp7_instr instr);

#ifdef LIBPOPPY_EXTRA
/* Apply the decay multiplier or the cut-off of an instrument to its
 * voice. Both are no-ops for an instrument that does not have the
 * parameter. */
void mp7_voice_apply_decay(struct mp7_context_s *ctx, mp7_instr instr);
void mp7_voice_apply_cutoff(struct mp7_context_s *ctx, mp7_instr instr);
#endif /* LIBPOPPY_EXTRA */

int64_t mp7_engine_process(struct mp7_voice *voice);

/* Rhythm player internals. These are the testable core of the sequencer; the
 * public rhythm setters in mp7.c wrap them. */

/* The composition of a selection: the grid the sequencer plays and the
 * pattern view the host prints. It is a pure function of the selection and
 * of the source, so it can be asked for a selection that is not playing. */
void mp7_pattern_compose(struct mp7_context_s *ctx, uint32_t selected,
                         struct mp7_pattern_view *out);
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
void poppy_biquad_set_highpass_coeffs(struct poppy_biquad *filter,
                                      uint32_t freq_hz,
                                      uint32_t sample_rate);
void poppy_biquad_set_bandpass(struct poppy_biquad *filter,
                               uint32_t freq_hz,
                               uint32_t res_q88,
                               uint32_t sample_rate);
void poppy_biquad_set_bandpass_coeffs(struct poppy_biquad *filter,
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
void poppy_mixer_set_instr_pan(struct poppy_mixer *mixer,
                               unsigned int instr,
                               int32_t pan_q16);
int32_t poppy_mixer_balance_gain_q16(const struct poppy_mixer *mixer,
                                     unsigned int group);
int64_t poppy_mixer_pan_left(int64_t sample_q31, int32_t pan_q16);
int64_t poppy_mixer_pan_right(int64_t sample_q31, int32_t pan_q16);
int16_t poppy_mixer_to_q15(int64_t sum_q31, int32_t panel_volume_q16);

#endif /* MP7_INTERNAL_H */
