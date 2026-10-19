/*
 * mp7.c - context handling and entry points of the Mini Pops library.
 *
 * The context owns the 15 voice states and the rhythm sequencer. mp7_process()
 * advances the sequencer, renders every voice and sums them into a temporary
 * centred stereo mix. The pan/level/balance mixer replaces that sum in the
 * mixer phase; until then every voice is placed at the centre.
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

void mp7_process(mp7_context *ctx, int16_t *stereo_out, size_t frames)
{
    size_t frame;

    if (stereo_out == 0) {
        return;
    }

    if (ctx == 0) {
        clear_stereo(stereo_out, frames);
        return;
    }

    for (frame = 0u; frame < frames; frame++) {
        int64_t sum = 0;
        int16_t sample;
        unsigned int instr;

        /* Triggers are evaluated before the sample is rendered, so hits are
         * sample-accurate. */
        mp7_pattern_tick(ctx);

        for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
            sum += mp7_voice_process(&ctx->voices[instr]);
        }

        sample = (int16_t)poppy_q31_to_q15(poppy_sat_i64_to_i32(sum));
        stereo_out[2u * frame] = sample;
        stereo_out[2u * frame + 1u] = sample;
    }
}
