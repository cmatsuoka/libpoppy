/*
 * mp7_voice.c - per-instrument state and fixed parameter constants.
 *
 * The original unit does not expose synthesis parameters to the user, so the
 * values below are compiled in. Frequencies are in Hz, times in microseconds,
 * and resonance in Q8.8. Level and Pan are fixed here as well, but they are
 * mixer inputs rather than synthesis parameters: the mixer applies them
 * together with the panel's Volume, Balance and direct gains.
 */

#include <string.h>

#include "mp7_internal.h"

struct mp7_voice_config {
    uint8_t engine;
    uint16_t freq_hz;
    uint16_t freq2_hz;
    uint16_t rate_hz;
    uint16_t highpass_hz;
    uint32_t attack_us;
    uint32_t decay_us;
    uint32_t decay2_us;
    uint32_t density_us;
    uint16_t res_q88;
    int32_t level2_q16;
    int32_t tick_q16;
};

static const struct mp7_voice_config voice_config[MP7_INSTR_COUNT] = {
    /* Bass */
    { MP7_ENGINE_SINE_AD,     65u,    0u,    0u,    0u,  1000u, 200000u,     0u,    0u,    0u,     0,     0 },
    /* Snare 1 */
    { MP7_ENGINE_NOISE,     5500u,    0u,    0u,  800u,  1000u, 400000u,     0u,    0u,  102u,     0, 32768 },
    /* Snare 2 */
    { MP7_ENGINE_NOISE,     5500u,    0u,    0u, 1400u,  1000u,  80000u,     0u,    0u,   77u,     0, 13107 },
    /* Hi Bongo */
    { MP7_ENGINE_SINE_AD,    600u,    0u,    0u,    0u,  1000u,  50000u,     0u,    0u,    0u,     0,     0 },
    /* Lo Bongo */
    { MP7_ENGINE_SINE_AD,    400u,    0u,    0u,    0u,  1000u,  50000u,     0u,    0u,    0u,     0,     0 },
    /* Conga */
    { MP7_ENGINE_DUAL_TONE,  195u, 4600u,    0u,    0u,  1000u, 250000u,  4000u,    0u,    0u, 65536,     0 },
    /* Cowbell */
    { MP7_ENGINE_DUAL_TONE,  860u, 4600u,    0u,    0u,  1000u,  50000u,  4000u,    0u,    0u, 65536,     0 },
    /* Claves */
    { MP7_ENGINE_SINE_AD,   2120u,    0u,    0u,    0u,  1000u,   4000u,     0u,    0u,    0u,     0,     0 },
    /* Hihat */
    { MP7_ENGINE_NOISE,    10000u,    0u,    0u, 1000u, 10000u,  20000u,     0u,    0u,  512u,     0,     0 },
    /* Rimshot */
    { MP7_ENGINE_SINE_AD,   1650u,    0u,    0u,    0u,  1000u,   1000u,     0u,    0u,    0u,     0,     0 },
    /* Cymbal 1 */
    { MP7_ENGINE_NOISE,     5600u,    0u,    0u, 5500u,  1000u, 200000u,     0u,    0u,  154u,     0,     0 },
    /* Cymbal 2 */
    { MP7_ENGINE_NOISE,     5600u,    0u,    0u, 5500u,  1000u, 200000u,     0u,    0u,   77u,     0,     0 },
    /* Tambourine */
    { MP7_ENGINE_NOISE,     4600u,    0u,    0u, 6600u, 20000u,  80000u,     0u,    0u, 2560u,     0,     0 },
    /* Guiro */
    { MP7_ENGINE_GUIRO,     4100u,    0u,   65u,    0u,  1000u,   4000u,     0u,  600u,    0u,     0,     0 },
    /* Quijada */
    { MP7_ENGINE_QUIJADA,   2700u,    0u,   25u,    0u, 50000u, 300000u,     0u, 3600u,    0u,     0,     0 }
};

static void env_set(struct poppy_env *env,
                    uint32_t attack_us,
                    uint32_t decay_us,
                    uint32_t sample_rate)
{
    memset(env, 0, sizeof(*env));
    env->attack_coef = poppy_env_coef_us(attack_us, sample_rate);
    env->decay_coef = poppy_env_coef_us(decay_us, sample_rate);
    env->stage = POPPY_ENV_IDLE;
}

static void voice_setup(struct mp7_voice *voice,
                        mp7_instr instr,
                        const struct mp7_voice_config *config,
                        uint32_t sample_rate)
{
    uint32_t seed;

    memset(voice, 0, sizeof(*voice));
    voice->instr = instr;
    voice->engine = config->engine;

    voice->level_q16 = POPPY_Q16_ONE;
    voice->pan_q16 = POPPY_Q16_ONE / 2;
    voice->balance_group = POPPY_MIX_GROUP_NONE;
    voice->direct_gain_index = MP7_DIRECT_INDEX_NONE;

    switch (instr) {
    case MP7_INSTR_BASS:
        voice->balance_group = POPPY_MIX_GROUP_A;
        break;
    case MP7_INSTR_HIHAT:
    case MP7_INSTR_CYMBAL_1:
    case MP7_INSTR_CYMBAL_2:
        voice->balance_group = POPPY_MIX_GROUP_B;
        break;
    case MP7_INSTR_TAMBOURINE:
        voice->direct_gain_index = MP7_DIRECT_TAMBOURINE;
        break;
    case MP7_INSTR_GUIRO:
        voice->direct_gain_index = MP7_DIRECT_GUIRO;
        break;
    case MP7_INSTR_QUIJADA:
        voice->direct_gain_index = MP7_DIRECT_QUIJADA;
        break;
    default:
        break;
    }

    seed = 0x9E3779B9u + (uint32_t)instr * 0x85EBCA6Bu;
    voice->prng = (seed != 0u) ? seed : 1u;

    poppy_osc_set_freq(&voice->body_osc, config->freq_hz, sample_rate);

    switch (config->engine) {
    case MP7_ENGINE_SINE_AD:
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        break;

    case MP7_ENGINE_DUAL_TONE:
        poppy_osc_set_freq(&voice->aux_osc, config->freq2_hz, sample_rate);
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        env_set(&voice->aux_env, config->attack_us, config->decay2_us, sample_rate);
        voice->level2_q16 = config->level2_q16;
        break;

    case MP7_ENGINE_NOISE:
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        env_set(&voice->aux_env, config->attack_us, config->decay_us, sample_rate);
        poppy_biquad_set_highpass(&voice->highpass, config->highpass_hz, sample_rate);
        poppy_biquad_set_bandpass(&voice->bandpass,
                                  config->freq_hz,
                                  config->res_q88,
                                  sample_rate);
        voice->tick_q30 = (int32_t)((int64_t)config->tick_q16 * 16384);
        break;

    case MP7_ENGINE_GUIRO:
        poppy_osc_set_freq(&voice->aux_osc, config->rate_hz, sample_rate);
        env_set(&voice->amp_env, config->attack_us, config->density_us, sample_rate);
        env_set(&voice->body_env, 1000u, config->decay_us, sample_rate);
        break;

    case MP7_ENGINE_QUIJADA:
        /* Rate drives the comparator clock; Frequency drives the audible
         * body oscillator (algorithm page, Quijada engine). */
        poppy_osc_set_freq(&voice->body_osc, config->freq_hz, sample_rate);
        poppy_osc_set_freq(&voice->aux_osc, config->rate_hz, sample_rate);
        env_set(&voice->amp_env, config->attack_us, config->decay_us, sample_rate);
        env_set(&voice->short_env, 100u, config->density_us, sample_rate);
        env_set(&voice->long_env, 100u, config->density_us, sample_rate);
        break;

    default:
        break;
    }
}

void mp7_voice_init_all(struct mp7_context_s *ctx)
{
    unsigned int i;

    /* voice_setup() resets the whole voice, so it also resets the fixed
     * Level and Pan. They are constants today; if they ever become
     * per-voice settings, a sample-rate change must not clear them. */
    for (i = 0; i < (unsigned int)MP7_INSTR_COUNT; i++) {
        voice_setup(&ctx->voices[i], (mp7_instr)i, &voice_config[i], ctx->sample_rate);
    }
}

void mp7_voice_trigger(struct mp7_voice *voice)
{
    uint32_t r1;
    uint32_t r2;
    int32_t t;

    voice->active = 1;

    switch (voice->engine) {
    case MP7_ENGINE_SINE_AD:
        poppy_env_trigger(&voice->amp_env);
        break;

    case MP7_ENGINE_DUAL_TONE:
        poppy_env_trigger(&voice->amp_env);
        poppy_env_trigger(&voice->aux_env);
        break;

    case MP7_ENGINE_NOISE:
        poppy_env_trigger(&voice->amp_env);
        poppy_env_trigger(&voice->aux_env);
        voice->highpass.x1 = 0;
        voice->highpass.x2 = 0;
        voice->highpass.y1 = 0;
        voice->highpass.y2 = 0;
        voice->bandpass.x1 = 0;
        voice->bandpass.x2 = 0;
        voice->bandpass.y1 = 0;
        voice->bandpass.y2 = 0;
        break;

    case MP7_ENGINE_GUIRO:
        poppy_env_trigger_hold(&voice->amp_env);
        voice->body_env.stage = POPPY_ENV_IDLE;
        voice->body_env.value = 0;
        voice->last_rate = 0;
        break;

    case MP7_ENGINE_QUIJADA:
        poppy_env_trigger(&voice->amp_env);
        /* The secondary envelopes are deliberately not reset: the trigger
         * leaves them running and only restarts the body envelope. */

        /* Both thresholds are re-randomized on every trigger:
         * t = 0.1 + fabs(u) * 0.5, with u uniform in [-1, 1). In Q30 that is
         * 0.1 * 2^30 + abs(u_q31) / 4. */
        r1 = poppy_prng_next(&voice->prng);
        r2 = poppy_prng_next(&voice->prng);
        t = POPPY_Q30_ONE / 10;
        voice->threshold_lo = t + (poppy_abs_i32((int32_t)r1) / 4);
        voice->threshold_hi = t + (poppy_abs_i32((int32_t)r2) / 4);
        break;

    default:
        break;
    }
}

void mp7_voice_release(struct mp7_voice *voice)
{
    if ((voice->engine == MP7_ENGINE_GUIRO) && voice->active) {
        poppy_env_release(&voice->amp_env, voice->amp_env.decay_coef);
    }
}

int64_t mp7_voice_process(struct mp7_voice *voice)
{
    if (!voice->active) {
        return 0;
    }

    /* The voice's Level, the pan split and the master gain are all applied
     * by the mixer, so the engine returns the raw voice signal. */
    return mp7_engine_process(voice);
}
