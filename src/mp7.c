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

    /* The voices were re-initialized, so no gate is open any more. */
    ctx->guiro_manual_gated = 0u;
    ctx->guiro_open = 0u;

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

/* Fixed factory MIDI note map, in mp7_instr order. */
static const uint8_t mp7_instr_notes[MP7_INSTR_COUNT] = {
    36u, /* Bass Drum */
    40u, /* Snare 1 */
    38u, /* Snare 2 */
    60u, /* Small Bongo */
    61u, /* Large Bongo */
    64u, /* Conga */
    56u, /* Cowbell */
    75u, /* Claves */
    42u, /* Hihat */
    37u, /* Rimshot */
    49u, /* Cymbal 1 */
    57u, /* Cymbal 2 */
    54u, /* Tambourine */
    74u, /* Guiro */
    79u  /* Quijada */
};

static int context_valid(const mp7_context *ctx)
{
    return (ctx != 0) && (ctx->magic == MP7_CONTEXT_MAGIC);
}

void mp7_trigger(mp7_context *ctx, mp7_instr instr, uint8_t velocity)
{
    if (!context_valid(ctx) || ((unsigned int)instr >= (unsigned int)MP7_INSTR_COUNT)) {
        return;
    }

    if (instr == MP7_INSTR_GUIRO) {
        /* The Guiro is the gated voice: a note the caller triggers holds the
         * same gate the sequencer uses, so the two never fight over it. */
        mp7_guiro_gate(ctx, ctx->sequencer.guiro_gated,
                       (uint8_t)(velocity != 0u));
        return;
    }

    if (velocity == 0u) {
        mp7_voice_release(&ctx->voices[instr]);
        return;
    }

    mp7_voice_trigger(&ctx->voices[instr]);
}

void mp7_trigger_note(mp7_context *ctx, uint8_t note, uint8_t velocity)
{
    unsigned int instr;

    if (!context_valid(ctx)) {
        return;
    }

    for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
        if (mp7_instr_notes[instr] == note) {
            mp7_trigger(ctx, (mp7_instr)instr, velocity);
        }
    }
}

/* The panel has one bank lever and ten buttons. Derive the internal absolute
 * rhythm mask from them; bits 10..15 of the button mask do not exist. */
static uint32_t rhythm_selection(mp7_bank bank, uint16_t buttons)
{
    uint32_t button_mask = (uint32_t)buttons & 0x03FFu;

    return (bank == MP7_BANK_B) ? (button_mask << 10) : button_mask;
}

void mp7_set_rhythm(mp7_context *ctx, mp7_bank bank, uint16_t buttons)
{
    uint32_t selected;
    int was_running;

    if (!context_valid(ctx)) {
        return;
    }

    selected = rhythm_selection(bank, buttons);
    was_running = (ctx->sequencer.running != 0u);

    ctx->sequencer.running = 1u;
    ctx->sequencer.pending_selected = selected;
    ctx->sequencer.pending_valid = 0u;

    if ((selected != ctx->sequencer.selected) || !was_running) {
        mp7_pattern_combine(ctx, selected);
    }
}

void mp7_queue_rhythm(mp7_context *ctx, mp7_bank bank, uint16_t buttons)
{
    uint32_t selected;

    if (!context_valid(ctx)) {
        return;
    }

    selected = rhythm_selection(bank, buttons);

    /* There is no current cycle to finish when the sequencer is stopped. */
    if (ctx->sequencer.running == 0u) {
        mp7_set_rhythm(ctx, bank, buttons);
        return;
    }

    ctx->sequencer.pending_selected = selected;
    ctx->sequencer.pending_valid =
        (selected != ctx->sequencer.selected) ? 1u : 0u;
}

void mp7_stop_rhythms(mp7_context *ctx)
{
    if (!context_valid(ctx)) {
        return;
    }

    ctx->sequencer.running = 0u;
    ctx->sequencer.selected = 0u;
    ctx->sequencer.pending_selected = 0u;
    ctx->sequencer.pending_valid = 0u;

    mp7_pattern_combine(ctx, 0u);
    mp7_pattern_reset(ctx);
}

void mp7_set_tempo(mp7_context *ctx, uint16_t bpm)
{
    if (!context_valid(ctx)) {
        return;
    }

    mp7_pattern_set_tempo(ctx, (uint32_t)bpm);
}

bool mp7_rhythm_running(const mp7_context *ctx)
{
    return context_valid(ctx) && (ctx->sequencer.running != 0u);
}

void mp7_set_volume(mp7_context *ctx, int32_t volume_q16)
{
    if (!context_valid(ctx)) {
        return;
    }

    poppy_mixer_set_volume(&ctx->mixer, volume_q16);
}

void mp7_set_balance(mp7_context *ctx, int32_t balance_q16)
{
    if (!context_valid(ctx)) {
        return;
    }

    poppy_mixer_set_balance(&ctx->mixer, balance_q16);
}

void mp7_set_perc_gain(mp7_context *ctx, mp7_instr instr, int32_t gain_q16)
{
    unsigned int index;

    if (!context_valid(ctx)) {
        return;
    }

    switch (instr) {
    case MP7_INSTR_TAMBOURINE:
        index = MP7_DIRECT_TAMBOURINE;
        break;
    case MP7_INSTR_GUIRO:
        index = MP7_DIRECT_GUIRO;
        break;
    case MP7_INSTR_QUIJADA:
        index = MP7_DIRECT_QUIJADA;
        break;
    default:
        /* No Additional Volume fader for this instrument on the panel. */
        return;
    }

    poppy_mixer_set_direct_gain(&ctx->mixer, index, gain_q16);
}

void mp7_get_factory_pattern(mp7_rhythm rhythm, mp7_pattern_view *out)
{
    const struct mp7_factory_rhythm *pattern;

    if (out == 0) {
        return;
    }

    if ((unsigned int)rhythm >= (unsigned int)MP7_RHYTHM_COUNT) {
        out->notes = 0;
        out->percussion = 0;
        out->enable = 0;
        out->triplet = false;
        return;
    }

    pattern = &mp7_factory_rhythms[rhythm];
    out->notes = pattern->notes;
    out->percussion = mp7_shared_percussion_notes;
    out->enable = pattern->enable;
    out->triplet = (pattern->triplet != 0u);
}

const char *mp7_instr_name(mp7_instr instr)
{
    if ((unsigned int)instr >= (unsigned int)MP7_INSTR_COUNT) {
        return "?";
    }
    return mp7_instr_names[instr];
}

const char *mp7_rhythm_name(mp7_rhythm rhythm)
{
    if ((unsigned int)rhythm >= (unsigned int)MP7_RHYTHM_COUNT) {
        return "?";
    }
    return mp7_rhythm_names[rhythm];
}

mp7_bank mp7_rhythm_bank(mp7_rhythm rhythm)
{
    if ((unsigned int)rhythm >= (unsigned int)MP7_RHYTHM_COUNT) {
        return MP7_BANK_A;
    }
    return ((unsigned int)rhythm < 10u) ? MP7_BANK_A : MP7_BANK_B;
}

uint16_t mp7_rhythm_button_bit(mp7_rhythm rhythm)
{
    if ((unsigned int)rhythm >= (unsigned int)MP7_RHYTHM_COUNT) {
        return 0u;
    }
    return (uint16_t)(1u << ((unsigned int)rhythm % 10u));
}
