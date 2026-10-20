/*
 * mp7.c - context handling and entry points of the Mini Pops library.
 *
 * The context owns the 15 voice states, the rhythm sequencer and the global
 * mix controls. mp7_process() advances the sequencer, renders every voice and
 * sums it into a stereo mix through the shared mixer core.
 */

#include <string.h>

#include "mp7_internal.h"

/* The output buffer is cleared in bounded chunks so that the byte count
 * (frames * 2 channels * 2 bytes) cannot overflow size_t on a 32-bit
 * target: on pi32v2 a single memset of more than SIZE_MAX bytes would
 * wrap to a short length and leave stale samples in the buffer. */
#define MP7_CLEAR_CHUNK_FRAMES 1024u

static void clear_stereo(int16_t *stereo_out, size_t frames)
{
    while (frames > 0u) {
        size_t chunk = (frames > MP7_CLEAR_CHUNK_FRAMES) ? MP7_CLEAR_CHUNK_FRAMES : frames;

        memset(stereo_out, 0, chunk * 2u * sizeof(int16_t));
        stereo_out += chunk * 2u;
        frames -= chunk;
    }
}

size_t mp7_context_size(void)
{
    return sizeof(struct mp7_context_s);
}

void mp7_init(mp7_context *ctx, uint32_t sample_rate)
{
    if (ctx == 0) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (sample_rate == 0u) {
        sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    }
    ctx->sample_rate = sample_rate;

    ctx->magic = MP7_CONTEXT_MAGIC;
    mp7_voice_init_all(ctx);
    mp7_pattern_init(ctx);
    poppy_mixer_init(&ctx->mixer);
}

void mp7_set_sample_rate(mp7_context *ctx, uint32_t sample_rate)
{
    if (ctx == 0) {
        return;
    }

    if (sample_rate == 0u) {
        sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    }
    ctx->sample_rate = sample_rate;
    mp7_voice_init_all(ctx);

    /* Keep the selected rhythms and tempo, but rebuild the timing and start
     * the current pattern from its first enabled column at the new rate. */
    mp7_pattern_set_tempo(ctx, ctx->sequencer.tempo);
    mp7_pattern_reset(ctx);
}

void mp7_reset(mp7_context *ctx)
{
    uint32_t sample_rate;

    if (ctx == 0) {
        return;
    }

    /* Keep the sample rate, drop everything else back to the defaults. */
    sample_rate = ctx->sample_rate;
    mp7_init(ctx, sample_rate);
}

/* Effective per-voice gain: its fixed Level, scaled by Balance or by the
 * direct-percussion gain when the voice belongs to one of those controls. */
static int32_t voice_gain_q16(const struct mp7_context_s *ctx,
                              const struct mp7_voice *voice)
{
    int32_t gain = voice->level_q16;

    gain = poppy_mul_q16(gain,
                         poppy_mixer_balance_gain_q16(&ctx->mixer,
                                                      voice->balance_group));

    if (voice->direct_gain_index != MP7_DIRECT_INDEX_NONE) {
        gain = poppy_mul_q16(gain,
                             ctx->mixer.direct_gain_q16[voice->direct_gain_index]);
    }

    return gain;
}

void mp7_process(mp7_context *ctx, int16_t *stereo_out, size_t frames)
{
    int32_t voice_gain[MP7_INSTR_COUNT];
    size_t frame;
    unsigned int instr;

    if (stereo_out == 0) {
        return;
    }

    if (ctx == 0) {
        clear_stereo(stereo_out, frames);
        return;
    }

    /* The effective per-voice gain only changes when a mix control changes,
     * and a control cannot change while a block is being rendered, so the
     * gain is computed once per block instead of once per sample. */
    for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
        voice_gain[instr] = voice_gain_q16(ctx, &ctx->voices[instr]);
    }

    for (frame = 0u; frame < frames; frame++) {
        int64_t sum_left = 0;
        int64_t sum_right = 0;

        /* Triggers are evaluated before the sample is rendered, so hits are
         * sample-accurate. */
        mp7_pattern_tick(ctx);

        for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
            const struct mp7_voice *voice = &ctx->voices[instr];
            int64_t sample = mp7_voice_process(&ctx->voices[instr]);

            if (sample == 0) {
                continue;
            }

            sample = poppy_mul_q31_gain(sample, voice_gain[instr]);
            sum_left += poppy_mixer_pan_left(sample, voice->pan_q16);
            sum_right += poppy_mixer_pan_right(sample, voice->pan_q16);
        }

        stereo_out[2u * frame] = poppy_mixer_to_q15(sum_left, ctx->mixer.panel_volume_q16);
        stereo_out[2u * frame + 1u] = poppy_mixer_to_q15(sum_right,
                                                         ctx->mixer.panel_volume_q16);
    }
}
