/*
 * mp7_pattern.c - rhythm combination and the sample-clocked sequencer.
 *
 * A rhythm is a 15 x 64 note grid plus a 64-column step-enable mask and a
 * triplet flag (see the patterns specification). Selecting several rhythms
 * combines them by OR-ing the note grids, AND-ing the enable masks and OR-ing
 * the triplet flags.
 *
 * Disabled columns take no time: the sequencer only ever visits enabled
 * columns. Each visited column lasts two sub-steps (straight) or three
 * (triplet), and each sub-step lasts round(2.5 * sample_rate / tempo) samples.
 * Triggers are evaluated before the sample at a sub-step boundary is rendered.
 */

#include <string.h>

#include "mp7_internal.h"

#define MP7_SUB_STEPS_STRAIGHT 2u
#define MP7_SUB_STEPS_TRIPLET 3u
#define MP7_DEFAULT_TEMPO 120u
#define MP7_MAX_TEMPO 240u

static uint32_t sub_steps_per_column(const struct mp7_sequencer *seq)
{
    return (seq->triplet != 0u) ? MP7_SUB_STEPS_TRIPLET : MP7_SUB_STEPS_STRAIGHT;
}

static uint8_t first_enabled_column(const struct mp7_sequencer *seq)
{
    uint8_t column;

    for (column = 0u; column < (uint8_t)MP7_PATTERN_STEPS; column++) {
        if ((seq->enable & ((uint64_t)1u << column)) != 0u) {
            return column;
        }
    }
    return 0u;
}

static uint8_t next_enabled_column(const struct mp7_sequencer *seq, uint8_t from)
{
    uint8_t offset;

    for (offset = 1u; offset < (uint8_t)MP7_PATTERN_STEPS; offset++) {
        uint8_t column = (uint8_t)((from + offset) & (MP7_PATTERN_STEPS - 1));

        if ((seq->enable & ((uint64_t)1u << column)) != 0u) {
            return column;
        }
    }
    return from;
}

void mp7_pattern_reset(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    /* A reset or a rhythm change can drop a Guiro gate that is still open. */
    if (seq->guiro_gated != 0u) {
        mp7_voice_release(&ctx->voices[MP7_INSTR_GUIRO]);
    }

    seq->guiro_gated = 0u;
    seq->substep = 0u;
    seq->sub_step_pos = 0u;
    seq->column = (seq->enable != 0u) ? first_enabled_column(seq) : 0u;
}

void mp7_pattern_set_tempo(struct mp7_context_s *ctx, uint32_t bpm)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint64_t numerator;
    uint64_t denominator;
    uint32_t step;

    if (bpm < 1u) {
        bpm = 1u;
    }
    if (bpm > MP7_MAX_TEMPO) {
        bpm = MP7_MAX_TEMPO;
    }
    seq->tempo = bpm;

    /* round(2.5 * sample_rate / tempo), i.e. floor((5 * sr + tempo) / (2 * tempo)). */
    numerator = ((uint64_t)5u * (uint64_t)ctx->sample_rate) + (uint64_t)bpm;
    denominator = (uint64_t)2u * (uint64_t)bpm;
    step = (uint32_t)(numerator / denominator);
    if (step == 0u) {
        step = 1u;
    }
    seq->sub_step_samples = step;

    if (seq->sub_step_pos > seq->sub_step_samples) {
        seq->sub_step_pos = seq->sub_step_samples;
    }
}

void mp7_pattern_combine(struct mp7_context_s *ctx, uint32_t selected_mask)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint32_t selected = selected_mask & ((1u << MP7_RHYTHM_COUNT) - 1u);
    uint64_t enable = ~(uint64_t)0;
    uint8_t triplet = 0u;
    unsigned int rhythm;
    unsigned int instr;
    unsigned int row;
    unsigned int column;

    seq->selected = selected;
    memset(seq->notes, 0, sizeof(seq->notes));

    if (selected == 0u) {
        seq->enable = 0u;
        seq->triplet = 0u;
        mp7_pattern_reset(ctx);
        return;
    }

    for (rhythm = 0u; rhythm < (unsigned int)MP7_RHYTHM_COUNT; rhythm++) {
        const struct mp7_factory_rhythm *pattern;

        if ((selected & ((uint32_t)1u << rhythm)) == 0u) {
            continue;
        }
        pattern = &mp7_factory_rhythms[rhythm];

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (pattern->enable[column] == 0u) {
                enable &= ~((uint64_t)1u << column);
            }
        }

        for (instr = 0u; instr < (unsigned int)MP7_PATTERN_MELODIC_ROWS; instr++) {
            for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
                if (pattern->notes[instr][column] != 0u) {
                    seq->notes[instr][column] = 1u;
                }
            }
        }

        triplet |= pattern->triplet;
    }

    /* The percussion rows are shared by every rhythm, so they are OR-ed in
     * once rather than stored in each grid. */
    for (row = 0u; row < (unsigned int)MP7_PATTERN_PERCUSSION_ROWS; row++) {
        instr = (unsigned int)MP7_PATTERN_MELODIC_ROWS + row;
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if (mp7_shared_percussion_notes[row][column] != 0u) {
                seq->notes[instr][column] = 1u;
            }
        }
    }

    seq->enable = enable;
    seq->triplet = triplet;
    mp7_pattern_reset(ctx);
}

void mp7_pattern_init(struct mp7_context_s *ctx)
{
    memset(&ctx->sequencer, 0, sizeof(ctx->sequencer));
    ctx->sequencer.tempo = MP7_DEFAULT_TEMPO;
    mp7_pattern_set_tempo(ctx, MP7_DEFAULT_TEMPO);
    mp7_pattern_combine(ctx, 0u);
}

/* Emit the hits of the current column, and handle the Guiro gate. */
static void fire_column(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint8_t column = seq->column;
    unsigned int instr;

    for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
        uint8_t hit = seq->notes[instr][column];

        if (instr == (unsigned int)MP7_INSTR_GUIRO) {
            if (hit != 0u) {
                /* Re-entering an adjacent X keeps the gate open, so the
                 * ratchet runs without a release gap. */
                if (seq->guiro_gated == 0u) {
                    mp7_voice_trigger(&ctx->voices[instr]);
                    seq->guiro_gated = 1u;
                }
            } else if (seq->guiro_gated != 0u) {
                mp7_voice_release(&ctx->voices[instr]);
                seq->guiro_gated = 0u;
            }
        } else if (hit != 0u) {
            mp7_voice_trigger(&ctx->voices[instr]);
        }
    }
}

void mp7_pattern_tick(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq;

    if (ctx == 0) {
        return;
    }
    seq = &ctx->sequencer;

    if (seq->enable == 0u) {
        return;
    }

    if (seq->sub_step_pos != 0u) {
        seq->sub_step_pos--;
        return;
    }

    if (seq->substep == 0u) {
        fire_column(ctx);
        seq->substep = 1u;
    } else {
        seq->substep++;
        if (seq->substep >= sub_steps_per_column(seq)) {
            seq->substep = 0u;
            seq->column = next_enabled_column(seq, seq->column);
        }
    }

    /* The boundary sample itself counts as the first of the interval. */
    seq->sub_step_pos = seq->sub_step_samples - 1u;
}
