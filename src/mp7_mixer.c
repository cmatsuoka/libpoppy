/*
 * mp7_mixer.c - the shared stereo mixer core.
 *
 * This file belongs to the SHARED layer: every symbol here is named poppy_*
 * so a future instrument variant can reuse it without a rename. The mixer
 * state holds one gain per instrument channel, so the shared core is compiled
 * against the variant's instrument count. It applies the per-voice pan, the
 * master Volume, Balance and the per-instrument gains.
 *
 * There is deliberately no master soft clip: soft clip is a per-voice
 * transfer curve in the noise engine. The only output-stage protection is the
 * single Q31 -> Q15 saturation at the end.
 */

#include "mp7_internal.h"

#define POPPY_VOLUME_MAX_Q16 (4 * POPPY_Q16_ONE)

static int32_t balance_gain(int32_t b, unsigned int group)
{
    const int right = (b >= POPPY_Q16_ONE / 2);

    if (group == POPPY_MIX_GROUP_A) {
        if (!right) {
            return POPPY_Q16_ONE;
        }
        return (int32_t)((((int64_t)18 * POPPY_Q16_ONE) -
                          ((int64_t)16 * b) + 5) / 10);
    }

    if (group == POPPY_MIX_GROUP_B) {
        if (right) {
            return POPPY_Q16_ONE;
        }
        return (int32_t)((((int64_t)2 * POPPY_Q16_ONE) +
                          ((int64_t)16 * b) + 5) / 10);
    }

    return POPPY_Q16_ONE;
}

void poppy_mixer_init(struct poppy_mixer *mixer)
{
    unsigned int i;

    mixer->panel_volume_q16 = POPPY_Q16_ONE;
    mixer->balance_q16 = POPPY_Q16_ONE / 2;
    mixer->balance_a_gain_q16 = POPPY_Q16_ONE;
    mixer->balance_b_gain_q16 = POPPY_Q16_ONE;

    /* The generic default is unity; the variant applies its panel-specific
     * defaults after this call. */
    for (i = 0u; i < (unsigned int)MP7_INSTR_COUNT; i++) {
        mixer->instr_gain_q16[i] = POPPY_Q16_ONE;
    }
}

void poppy_mixer_set_volume(struct poppy_mixer *mixer, int32_t panel_volume_q16)
{
    mixer->panel_volume_q16 =
        poppy_clamp_i32(panel_volume_q16, 0, POPPY_VOLUME_MAX_Q16);
}

void poppy_mixer_set_balance(struct poppy_mixer *mixer, int32_t balance_q16)
{
    mixer->balance_q16 = poppy_clamp_i32(balance_q16, 0, POPPY_Q16_ONE);
    mixer->balance_a_gain_q16 = balance_gain(mixer->balance_q16,
                                             POPPY_MIX_GROUP_A);
    mixer->balance_b_gain_q16 = balance_gain(mixer->balance_q16,
                                             POPPY_MIX_GROUP_B);
}

void poppy_mixer_set_instr_gain(struct poppy_mixer *mixer,
                                unsigned int instr,
                                int32_t gain_q16)
{
    if (instr >= (unsigned int)MP7_INSTR_COUNT) {
        return;
    }
    mixer->instr_gain_q16[instr] = poppy_clamp_i32(gain_q16, 0, POPPY_Q16_ONE);
}

/* Balance gain for one of the two groups.
 *
 * With b in 0..1:
 *   b >= 0.5:  Base = 1.8 - 1.6*b,  cymbals = 1.0
 *   b <  0.5:  Base = 1.0,          cymbals = 0.2 + 1.6*b
 *
 * The two formulas are evaluated as integer fractions of Q16 units so that
 * the 0.5, 0.2 and 1.8 constants are exact at centre, bottom and top. The
 * results are cached, so this lookup stays out of the per-sample divide path. */
int32_t poppy_mixer_balance_gain_q16(const struct poppy_mixer *mixer,
                                     unsigned int group)
{
    if (group == POPPY_MIX_GROUP_A) {
        return mixer->balance_a_gain_q16;
    }
    if (group == POPPY_MIX_GROUP_B) {
        return mixer->balance_b_gain_q16;
    }
    return POPPY_Q16_ONE;
}

/* Linear pan law: pan 0 is hard left, pan 65536 is hard right, 32768 is
 * centre (half the signal into each channel). */
int64_t poppy_mixer_pan_left(int64_t sample_q31, int32_t pan_q16)
{
    return (sample_q31 * (int64_t)(POPPY_Q16_ONE - pan_q16)) >> 16;
}

int64_t poppy_mixer_pan_right(int64_t sample_q31, int32_t pan_q16)
{
    return (sample_q31 * (int64_t)pan_q16) >> 16;
}

/* Apply the master gain (0.5 * Volume) and convert Q31 -> Q15 with rounding
 * and saturation. This is the single saturation point of the mix.
 *
 * The shift is 17, not 16, because the master gain is half the panel value:
 * sum * volume / 2 / 65536 == (sum * volume) >> 17.
 *
 * The argument is the panel Volume, not a gain: the 0.5 factor lives here. */
int16_t poppy_mixer_to_q15(int64_t sum_q31, int32_t panel_volume_q16)
{
    int64_t scaled = (sum_q31 * (int64_t)panel_volume_q16 + ((int64_t)1 << 16)) >> 17;

    return (int16_t)poppy_q31_to_q15(poppy_sat_i64_to_i32(scaled));
}
