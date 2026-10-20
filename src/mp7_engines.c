/*
 * mp7_engines.c - the five sound generators of the Mini Pops library.
 *
 * Each engine is a small state machine that consumes its fixed parameter
 * constants from mp7_voice.c, so nothing in the audio path is configurable
 * at run time.
 *
 * All samples are Q31. Envelope values and filter coefficients are Q30,
 * because both need values slightly outside [-1, 1) (the attack target is
 * 10/9 and biquad b1/a1 reach about -2.0 at low frequencies). Products that
 * cross formats go through 64-bit intermediates.
 */

#include "mp7_internal.h"

#define Q30_ONE POPPY_Q30_ONE
#define Q31_ONE ((int64_t)1 << 31)
#define PHASE_MASK 0x0FFFFFFFu

/* Attack reaches 1.0 by overshooting to 10/9; decay overshoots to -1/9. */
#define ENV_ATTACK_TARGET ((int32_t)(((int64_t)10 * Q30_ONE) / 9))
#define ENV_DECAY_TARGET (-(Q30_ONE / 9))
#define ENV_HOLD_TARGET Q30_ONE

/* 0.4 in Q30, used by the Quijada secondary-envelope mix. */
#define Q30_TWO_FIFTHS ((int32_t)(((int64_t)2 * Q30_ONE) / 5))

/* Phase increment for a frequency in Hz. */
void poppy_osc_set_freq(struct poppy_osc *osc, uint32_t freq_hz, uint32_t sample_rate)
{
    uint64_t numerator = ((uint64_t)freq_hz << 28) + (sample_rate / 2u);

    osc->inc = (uint32_t)(numerator / sample_rate);
}

int32_t poppy_sine_sample(const struct poppy_osc *osc)
{
    int32_t q15 = (int32_t)poppy_sine[(osc->phase >> 16) & 0xFFFu];

    return poppy_q15_to_q31(q15);
}

/* Envelope coefficient for a segment time in microseconds.
 *
 * coef = 1 / ((t + 100us) * 0.4 * sample_rate), in Q30. */
int32_t poppy_env_coef_us(uint32_t time_us, uint32_t sample_rate)
{
    int64_t denominator = ((int64_t)time_us + 100) * (int64_t)sample_rate;
    int64_t numerator = (int64_t)2500000 * (int64_t)Q30_ONE;
    int64_t coef = numerator / denominator;

    if (coef > (int64_t)Q30_ONE) {
        coef = Q30_ONE;
    }
    if (coef < 0) {
        coef = 0;
    }
    return (int32_t)coef;
}

void poppy_env_trigger(struct poppy_env *env)
{
    env->value = 0;
    env->target = ENV_ATTACK_TARGET;
    env->coef = env->attack_coef;
    env->stage = POPPY_ENV_ATTACK;
    env->attack_to_hold = 0;
}

void poppy_env_trigger_hold(struct poppy_env *env)
{
    env->value = 0;
    env->target = ENV_ATTACK_TARGET;
    env->coef = env->attack_coef;
    env->stage = POPPY_ENV_ATTACK;
    env->attack_to_hold = 1;
}

void poppy_env_release(struct poppy_env *env, int32_t release_coef)
{
    env->target = ENV_DECAY_TARGET;
    env->coef = release_coef;
    env->stage = POPPY_ENV_RELEASE;
    env->attack_to_hold = 0;
}

int32_t poppy_env_process(struct poppy_env *env)
{
    int32_t diff;

    if (env->stage == POPPY_ENV_IDLE) {
        return 0;
    }

    diff = env->target - env->value;
    env->value = poppy_sat_add_q30(env->value, poppy_mul_q30(diff, env->coef));

    if (env->stage == POPPY_ENV_ATTACK) {
        if (env->value >= Q30_ONE) {
            env->value = Q30_ONE;
            if (env->attack_to_hold) {
                env->target = ENV_HOLD_TARGET;
                env->coef = 0;
                env->stage = POPPY_ENV_HOLD;
            } else {
                env->target = ENV_DECAY_TARGET;
                env->coef = env->decay_coef;
                env->stage = POPPY_ENV_DECAY;
            }
        }
    } else if ((env->stage == POPPY_ENV_DECAY) ||
               (env->stage == POPPY_ENV_RELEASE)) {
        if (env->value <= 0) {
            env->value = 0;
            env->stage = POPPY_ENV_IDLE;
            return 0;
        }
    }

    return poppy_clamp_i32(env->value, 0, Q30_ONE);
}

/* Turn a frequency into a table phase for the coefficient calculators. */
static uint32_t filter_phase(uint32_t freq_hz, uint32_t sample_rate)
{
    uint64_t numerator = ((uint64_t)freq_hz << 12) + (sample_rate / 2u);

    return (uint32_t)(numerator / sample_rate) & 0xFFFu;
}

static void biquad_clear(struct poppy_biquad *filter)
{
    filter->x1 = 0;
    filter->x2 = 0;
    filter->y1 = 0;
    filter->y2 = 0;
}

void poppy_biquad_set_highpass(struct poppy_biquad *filter,
                               uint32_t freq_hz,
                               uint32_t sample_rate)
{
    const int32_t q88 = 181; /* 1/sqrt(2), rounded to Q8.8 */
    uint32_t phase = filter_phase(freq_hz, sample_rate);
    int32_t sin30 = (int32_t)poppy_sine[phase] * 32768;
    int32_t cos30 = (int32_t)poppy_sine[(phase + 1024u) & 0xFFFu] * 32768;
    int64_t alpha = ((int64_t)sin30 * 128) / q88;
    int64_t k;
    int32_t half_sum;
    int32_t b0;
    int32_t c;

    k = ((int64_t)Q30_ONE * Q30_ONE) / ((int64_t)Q30_ONE + alpha);
    half_sum = (int32_t)(((int64_t)Q30_ONE + cos30) / 2);
    b0 = poppy_mul_q30(half_sum, (int32_t)k);
    c = poppy_mul_q30(cos30, (int32_t)k);

    filter->b0 = b0;
    filter->b1 = (int32_t)(-2LL * b0);
    filter->b2 = b0;
    filter->a1 = (int32_t)(-2LL * c);
    filter->a2 = poppy_mul_q30(Q30_ONE - (int32_t)alpha, (int32_t)k);
    biquad_clear(filter);
}

void poppy_biquad_set_bandpass(struct poppy_biquad *filter,
                               uint32_t freq_hz,
                               uint32_t res_q88,
                               uint32_t sample_rate)
{
    uint32_t phase = filter_phase(freq_hz, sample_rate);
    int32_t sin30 = (int32_t)poppy_sine[phase] * 32768;
    int32_t cos30 = (int32_t)poppy_sine[(phase + 1024u) & 0xFFFu] * 32768;
    int64_t alpha;
    int64_t k;
    int32_t qa;
    int32_t b0;
    int32_t c;

    /* A zero resonance would divide by zero below. Clamp it to the smallest
     * representable Q (1/256), which is far below the 0.2 minimum of the
     * panel range anyway. */
    if (res_q88 == 0u) {
        res_q88 = 1u;
    }

    alpha = ((int64_t)sin30 * 128) / (int64_t)res_q88;
    k = ((int64_t)Q30_ONE * Q30_ONE) / ((int64_t)Q30_ONE + alpha);
    qa = (int32_t)(((int64_t)res_q88 * alpha) / 256);
    b0 = poppy_mul_q30(qa, (int32_t)k);
    c = poppy_mul_q30(cos30, (int32_t)k);

    filter->b0 = b0;
    filter->b1 = 0;
    filter->b2 = -b0;
    filter->a1 = (int32_t)(-2LL * c);
    filter->a2 = poppy_mul_q30(Q30_ONE - (int32_t)alpha, (int32_t)k);
    biquad_clear(filter);
}

int32_t poppy_biquad_process(struct poppy_biquad *filter, int32_t sample_q31)
{
    int64_t acc = 0;

    acc += ((int64_t)filter->b0 * sample_q31) >> 30;
    acc += ((int64_t)filter->b1 * filter->x1) >> 30;
    acc += ((int64_t)filter->b2 * filter->x2) >> 30;
    acc -= ((int64_t)filter->a1 * filter->y1) >> 30;
    acc -= ((int64_t)filter->a2 * filter->y2) >> 30;

    filter->x2 = filter->x1;
    filter->x1 = sample_q31;
    filter->y2 = filter->y1;
    filter->y1 = poppy_sat_i64_to_i32(acc);

    return filter->y1;
}

/* Soft clip over [-3, 3], with the table's /10 scaling undone.
 *
 * The result is Q31 and may exceed 1.0 (up to +/-10), so it is returned as
 * int64_t. */
int64_t poppy_softclip_q31(int64_t sample_q31)
{
    const int64_t limit = 3 * Q31_ONE;
    int64_t u;
    int64_t pos_q;
    int32_t pos;
    uint32_t frac;
    int32_t v0;
    int32_t v1;
    int64_t value;

    if (sample_q31 >= limit) {
        return (int64_t)INT32_MAX * 10;
    }
    if (sample_q31 <= -limit) {
        return -(int64_t)INT32_MAX * 10;
    }

    u = (sample_q31 * 715827883LL) >> 31;
    if (u >= Q31_ONE) {
        return (int64_t)INT32_MAX * 10;
    }
    if (u <= -Q31_ONE) {
        return -(int64_t)INT32_MAX * 10;
    }

    pos_q = (u + Q31_ONE) * (POPPY_SOFTCLIP_TABLE_SIZE - 1);
    pos = (int32_t)(pos_q >> 32);
    frac = (uint32_t)(pos_q & 0xFFFFFFFFu);

    if (pos >= POPPY_SOFTCLIP_TABLE_SIZE - 1) {
        return (int64_t)poppy_softclip[POPPY_SOFTCLIP_TABLE_SIZE - 1] * 10;
    }

    v0 = poppy_softclip[pos];
    v1 = poppy_softclip[pos + 1];
    value = (int64_t)v0 + (((int64_t)(v1 - v0) * (int64_t)frac) >> 32);

    return value * 10;
}

static int32_t next_sine(struct poppy_osc *osc)
{
    int32_t sample = poppy_sine_sample(osc);

    osc->phase = (osc->phase + osc->inc) & PHASE_MASK;
    return sample;
}

static int64_t engine_sine_ad(struct mp7_voice *voice)
{
    int32_t env = poppy_env_process(&voice->amp_env);
    int32_t sine = next_sine(&voice->body_osc);

    if (voice->amp_env.stage == POPPY_ENV_IDLE) {
        voice->active = 0;
    }
    return poppy_mul_q30_q31(env, sine);
}

static int64_t engine_dual_tone(struct mp7_voice *voice)
{
    int32_t env1 = poppy_env_process(&voice->amp_env);
    int32_t env2 = poppy_env_process(&voice->aux_env);
    int32_t tone1 = next_sine(&voice->body_osc);
    int32_t tone2 = next_sine(&voice->aux_osc);
    int64_t out = poppy_mul_q30_q31(env1, tone1);

    /* Level2 scales the second tone only (algorithm page, dual-tone
     * engine). It is a Q16 gain, so unity is the identity. */
    out += poppy_mul_q31_gain(poppy_mul_q30_q31(env2, tone2), voice->level2_q16);

    if ((voice->amp_env.stage == POPPY_ENV_IDLE) &&
        (voice->aux_env.stage == POPPY_ENV_IDLE)) {
        voice->active = 0;
    }
    return out;
}

static int64_t engine_noise(struct mp7_voice *voice)
{
    int32_t amp = poppy_env_process(&voice->amp_env);
    int32_t tone_env = poppy_env_process(&voice->aux_env);
    int32_t noise = (int32_t)poppy_prng_next(&voice->prng);
    int32_t noise_scaled = poppy_mul_q31_q30(noise, Q30_ONE / 100);
    int32_t input = noise_scaled;
    int32_t filtered;
    int32_t pre;
    int64_t clipped;
    int32_t tone;

    if ((voice->tick_q30 > 0) && (tone_env > 0)) {
        /* The tone envelope and the tick depth are both Q30, so their
         * product needs a shift of 29 to land in Q31, where it is summed
         * with the filter input. */
        tone = poppy_sat_i64_to_i32(((int64_t)tone_env * (int64_t)voice->tick_q30) >> 29);
        input = poppy_sat_i64_to_i32((int64_t)input + (int64_t)tone);
    }

    filtered = poppy_biquad_process(&voice->highpass, input);
    filtered = poppy_biquad_process(&voice->bandpass, filtered);

    pre = poppy_mul_q30_q31(amp, filtered);

    /* filtered is scaled by 1/100, so the 0.1 ahead of the soft clip
     * becomes a *10 here. */
    clipped = poppy_softclip_q31((int64_t)pre * 10);

    if ((voice->amp_env.stage == POPPY_ENV_IDLE) &&
        (voice->aux_env.stage == POPPY_ENV_IDLE)) {
        voice->active = 0;
    }
    return clipped;
}

static int64_t engine_guiro(struct mp7_voice *voice)
{
    int32_t amp;
    int32_t rate;
    int32_t body_env;
    int32_t body;
    int32_t body_sine;

    if (!voice->active) {
        return 0;
    }

    amp = poppy_env_process(&voice->amp_env);
    rate = next_sine(&voice->aux_osc);

    if ((rate > 0) && (voice->last_rate <= 0)) {
        poppy_env_trigger(&voice->body_env);
    }
    voice->last_rate = rate;

    body_env = poppy_env_process(&voice->body_env);
    body_sine = next_sine(&voice->body_osc);
    body = poppy_mul_q30_q31(body_env, body_sine);

    if (voice->amp_env.stage == POPPY_ENV_IDLE) {
        voice->active = 0;
        return 0;
    }
    return poppy_mul_q30_q31(amp, body);
}

static int64_t engine_quijada(struct mp7_voice *voice)
{
    int32_t amp = poppy_env_process(&voice->amp_env);
    int32_t clock = next_sine(&voice->aux_osc);
    int32_t clock_q30 = clock >> 1;
    int32_t long_env;
    int32_t short_env;
    int32_t secondary;
    int32_t body;
    int32_t body_sine;

    if ((voice->prev_clock <= voice->threshold_lo) &&
        (clock_q30 > voice->threshold_lo)) {
        /* Crossing up through the low threshold fires the short envelope. */
        poppy_env_trigger(&voice->short_env);
    }
    if ((voice->prev_clock <= voice->threshold_hi) &&
        (clock_q30 > voice->threshold_hi)) {
        /* Crossing up through the high threshold fires the long envelope. */
        poppy_env_trigger(&voice->long_env);
    }
    voice->prev_clock = clock_q30;

    long_env = poppy_env_process(&voice->long_env);
    short_env = poppy_env_process(&voice->short_env);
    secondary = poppy_sat_add_q30(long_env, poppy_mul_q30(short_env, Q30_TWO_FIFTHS));

    body_sine = next_sine(&voice->body_osc);
    body = poppy_mul_q30_q31(amp, body_sine);

    /* The body oscillator is parked while both secondary envelopes are
     * idle. */
    if ((voice->long_env.stage == POPPY_ENV_IDLE) &&
        (voice->short_env.stage == POPPY_ENV_IDLE)) {
        voice->body_osc.phase = 0;
    }

    if (voice->amp_env.stage == POPPY_ENV_IDLE) {
        voice->active = 0;
    }
    return ((int64_t)body * (int64_t)secondary) >> 30;
}

int64_t mp7_engine_process(struct mp7_voice *voice)
{
    switch (voice->engine) {
    case MP7_ENGINE_SINE_AD:
        return engine_sine_ad(voice);
    case MP7_ENGINE_DUAL_TONE:
        return engine_dual_tone(voice);
    case MP7_ENGINE_NOISE:
        return engine_noise(voice);
    case MP7_ENGINE_GUIRO:
        return engine_guiro(voice);
    case MP7_ENGINE_QUIJADA:
        return engine_quijada(voice);
    default:
        return 0;
    }
}
