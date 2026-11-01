/*
 * mp7_pattern.c - rhythm combination and the sample-clocked sequencer.
 *
 * A rhythm is a 64-column note grid plus a 64-column step-enable mask (see
 * the patterns specification). A column of the note grid is a 16-bit
 * instrument mask: bit n is set when instrument n plays in that column.
 * Selecting several rhythms combines them by OR-ing the note masks and
 * AND-ing the enable masks.
 *
 * The grid a pattern plays on follows its enable mask: the compound mask and
 * the 5/4 mask play compound, every other mask plays straight. The
 * subdivision of a selection is therefore the compound one as soon as one of
 * the selected patterns is on the compound grid.
 *
 * The Tambourine, Guiro and Quijada hits are the same in every rhythm and
 * live in their own definition, mp7_shared_percussion_notes. It is OR-ed into
 * the combined grid of every selection, and it is the whole grid when no
 * rhythm button is pressed.
 *
 * An extended build (-DLIBPOPPY_EXTRA) has a second set of patterns, the
 * user slots, and the selection is read against them when the sequencer
 * points at that source. A user pattern is complete - it carries its own
 * percussion lines - so the shared layer is not added to it. Its mask comes
 * from its time signature, like every other pattern's.
 *
 * Disabled columns take no time: the sequencer only ever visits enabled
 * columns. A column that an immediate rhythm change disables is skipped, not
 * waited out. A column lasts a fraction of a beat: the tempo is in quarter
 * notes per minute, and a beat holds eight straight columns (a 32nd note
 * each) or six compound ones (an 8th-note triplet each). Triggers are
 * evaluated before the sample at a column boundary is rendered.
 */

#include <string.h>

#include "mp7_internal.h"

/* A beat is a quarter note, and it holds eight straight columns or six
 * compound ones. The tempo is in quarter notes per minute, so the beat is
 * the only unit the sequencer needs. */
#define MP7_STEPS_PER_BEAT_STRAIGHT 8u
#define MP7_STEPS_PER_BEAT_COMPOUND 6u
#define MP7_DEFAULT_TEMPO 120u
#define MP7_MAX_TEMPO 240u

/* The pilot/tempo lamp flashes when the playhead crosses column 31 - the
 * last column of the first 32-column half - and when the playhead wraps.
 * These are grid positions, not a musical meter. */
#define MP7_LAMP_HALF_COLUMN 31u

/* Both the rhythm grids and the shared percussion layer may store only a
 * shorter period (16 or 32 columns) that repeats to fill the 64-column
 * cycle. The period is always a power of two, so masking wraps the column.
 * The helper is in mp7_internal.h, because the pattern views need it too. */

static uint32_t steps_per_beat(const struct mp7_sequencer *seq)
{
    return (seq->subdivision == (uint8_t)MP7_SUBDIV_COMPOUND)
               ? MP7_STEPS_PER_BEAT_COMPOUND
               : MP7_STEPS_PER_BEAT_STRAIGHT;
}

/* The length of the next column, in samples.
 *
 * A beat is beat_samples long and holds steps_per_beat() columns, and it
 * does not divide evenly into them. The leftover samples are handed out one
 * at a time, so every beat lasts exactly as long as the tempo says and no
 * column drifts away from the grid. */
static uint32_t column_length(struct mp7_sequencer *seq)
{
    uint32_t steps = steps_per_beat(seq);
    uint32_t length = seq->beat_samples / steps;

    seq->step_remainder += seq->beat_samples % steps;
    if (seq->step_remainder >= steps) {
        seq->step_remainder -= steps;
        length++;
    }
    if (length == 0u) {
        length = 1u;
    }
    return length;
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

void mp7_guiro_gate(struct mp7_context_s *ctx,
                    uint8_t sequencer_gate,
                    uint8_t manual_gate)
{
    int held;

    ctx->sequencer.guiro_gated = sequencer_gate;
    ctx->guiro_manual_gated = manual_gate;

    /* The voice sounds while either holder keeps the gate open. */
    held = (sequencer_gate != 0u) || (manual_gate != 0u);

    if (held != 0) {
        /* Re-entering an adjacent X keeps the gate open, so the ratchet runs
         * without a release gap and is not restarted. */
        if (ctx->guiro_open == 0u) {
            mp7_voice_trigger(&ctx->voices[MP7_INSTR_GUIRO]);
            ctx->guiro_open = 1u;
        }
    } else if (ctx->guiro_open != 0u) {
        mp7_voice_release(&ctx->voices[MP7_INSTR_GUIRO]);
        ctx->guiro_open = 0u;
    }
}

void mp7_pattern_reset(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    /* A reset or a rhythm change can drop a Guiro gate that is still open.
     * A note the caller triggered itself is not affected. */
    mp7_guiro_gate(ctx, 0u, ctx->guiro_manual_gated);

    seq->column_fired = 0u;
    seq->column_pos = 0u;
    seq->step_remainder = 0u;
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

    /* round(60 * sample_rate / tempo), i.e. floor((120 * sr + tempo) / (2 * tempo)). */
    numerator = ((uint64_t)120u * (uint64_t)ctx->sample_rate) + (uint64_t)bpm;
    denominator = (uint64_t)2u * (uint64_t)bpm;
    step = (uint32_t)(numerator / denominator);
    if (step == 0u) {
        step = 1u;
    }
    seq->beat_samples = step;

    /* A faster tempo shortens the column that is running, so the countdown
     * cannot outlast it. */
    if (seq->column_pos > seq->beat_samples / steps_per_beat(seq)) {
        seq->column_pos = seq->beat_samples / steps_per_beat(seq);
    }
}

/* The composition of a selection: the grid the sequencer plays and the card
 * prints.
 *
 * It is a pure function of the selection and of the source, so a host can
 * ask for the selection that is queued as well. The rules are the ones of
 * the hardware: the notes of the patterns that are combined are OR-ed
 * together, their masks are AND-ed, and the shared Tambourine/Guiro/Quijada
 * layer plays with every factory rhythm.
 *
 * A user pattern is a complete pattern: it carries its own percussion lines,
 * so the shared layer is not added to it. An empty slot plays nothing and
 * disables no column, so it is skipped instead of being folded into the
 * composition.
 *
 * The grid a selection plays on follows the masks of the patterns in it, not
 * the combined mask: the combined mask of a compound and a straight pattern
 * is neither of the two.
 *
 * The signature of the selection is the signature its patterns carry, and
 * MP7_TIMESIG_NONE when they do not all carry the same one. It is not the
 * signature of the combined mask: a 4/4 and a 3/4 pattern combined are a
 * mixed selection, although their combined mask is the one of the 3/4. */
void mp7_pattern_compose(struct mp7_context_s *ctx, uint32_t selected,
                         struct mp7_pattern_view *out)
{
    unsigned int column;
    unsigned int index;
    int have_signature = 0;
    int mixed = 0;
    mp7_timesig signature = MP7_TIMESIG_NONE;

    memset(out, 0, sizeof(*out));
    out->enable = ~(uint64_t)0;

#ifndef LIBPOPPY_EXTRA
    /* The preset-only build has no user slots, so the composition does not
     * need the context. */
    (void)ctx;
#endif

#ifdef LIBPOPPY_EXTRA
    /* The user slots are a complete set of patterns of their own, so the
     * selection is read against them instead of the factory rhythms. */
    if (ctx->sequencer.source == MP7_SOURCE_USER) {
        for (index = 0u; index < (unsigned int)MP7_RHYTHM_COUNT; index++) {
            const struct mp7_user_slot *slot;
            uint64_t mask;

            if ((selected & ((uint32_t)1u << index)) == 0u) {
                continue;
            }

            slot = &ctx->user_patterns[index];
            if (slot->used == 0u) {
                continue;
            }

            for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
                out->notes[column] |= slot->notes[column];
            }

            /* The mask comes from the time signature of the slot, so it is
             * one of the six the library knows. */
            mask = mp7_timesig_enable_mask((mp7_timesig)slot->timesig);
            out->enable &= mask;
            out->compound |= mp7_mask_is_compound(mask);

            if (!have_signature) {
                signature = (mp7_timesig)slot->timesig;
                have_signature = 1;
            } else if ((mp7_timesig)slot->timesig != signature) {
                mixed = 1;
            }
        }

        /* With no button pressed the user source keeps the transport running
         * and stays silent: the shared percussion layer belongs to the
         * factory rhythms. */
        out->timesig = mixed ? MP7_TIMESIG_NONE
                              : (have_signature ? signature : MP7_TIMESIG_4_4);
        return;
    }
#endif /* LIBPOPPY_EXTRA */

    for (index = 0u; index < (unsigned int)MP7_RHYTHM_COUNT; index++) {
        const struct mp7_factory_rhythm *pattern;
        mp7_timesig pattern_signature;

        if ((selected & ((uint32_t)1u << index)) == 0u) {
            continue;
        }
        pattern = &mp7_factory_rhythms[index];

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            out->notes[column] |= pattern->notes[
                wrap_column(column, pattern->notes_period)];
        }
        out->enable &= pattern->enable;
        out->compound |= mp7_mask_is_compound(pattern->enable);

        pattern_signature = pattern->timesig;
        if (!have_signature) {
            signature = pattern_signature;
            have_signature = 1;
        } else if (pattern_signature != signature) {
            mixed = 1;
        }
    }

    /* The shared Tambourine/Guiro/Quijada layer plays with every rhythm, and
     * it is what plays on its own when no rhythm button is pressed. */
    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        out->notes[column] |= mp7_shared_percussion_notes[
            wrap_column(column, MP7_PATTERN_HALF_STEPS)];
    }

    out->timesig = mixed ? MP7_TIMESIG_NONE
                          : (have_signature ? signature : MP7_TIMESIG_4_4);
}

void mp7_pattern_combine(struct mp7_context_s *ctx, uint32_t selected_mask)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    struct mp7_pattern_view view;
    uint32_t selected = selected_mask & ((1u << MP7_RHYTHM_COUNT) - 1u);

    seq->selected = selected;

    /* The sequencer plays the composition, so the grid it runs on is the
     * view the host prints. */
    mp7_pattern_compose(ctx, selected, &view);

    memcpy(seq->notes, view.notes, sizeof(seq->notes));
    seq->enable = view.enable;
    seq->subdivision = view.compound ? (uint8_t)MP7_SUBDIV_COMPOUND
                                      : (uint8_t)MP7_SUBDIV_STRAIGHT;

    /* A stopped transport plays nothing, so the mask is empty. */
    if ((seq->running == 0u) && (selected == 0u)) {
        seq->enable = 0u;
        seq->subdivision = (uint8_t)MP7_SUBDIV_STRAIGHT;
    }
}

void mp7_pattern_init(struct mp7_context_s *ctx)
{
    memset(&ctx->sequencer, 0, sizeof(ctx->sequencer));
    ctx->sequencer.tempo = MP7_DEFAULT_TEMPO;
    mp7_pattern_set_tempo(ctx, MP7_DEFAULT_TEMPO);
    mp7_pattern_combine(ctx, 0u);
}

/* Apply a queued selection when the current cycle wraps. The new pattern
 * starts at its first enabled column, and the caller fires it, so the first
 * hit lands one column later, exactly like an ordinary column change. */
static void apply_pending(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;

    if (seq->pending_valid == 0u) {
        return;
    }

    seq->pending_valid = 0u;

    /* The bank and the button mask are the panel indication, and they are
     * applied even when the rhythm mask itself does not change: the
     * no-button layer is the same on both banks, so a lever change is
     * invisible in the rhythm but the bank lamp still has to follow it. */
    seq->bank = seq->pending_bank;
    seq->buttons = seq->pending_buttons;

    if (seq->pending_selected == seq->selected) {
        return;
    }

    mp7_pattern_combine(ctx, seq->pending_selected);

    if (seq->enable == 0u) {
        mp7_pattern_reset(ctx);
        return;
    }

    seq->column = first_enabled_column(seq);
}

/* Move the playhead to the next enabled column and apply a queued selection
 * when the move wraps the cycle. Returns 0 when the sequencer has nothing
 * left to play. */
static int advance_column(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint8_t previous = seq->column;
    uint8_t next = next_enabled_column(seq, previous);

    /* Flash the pilot/tempo lamp when the playhead crosses column 31 and
     * when it wraps to the start of the cycle. A single-column grid
     * (next == previous) does not flash every step. */
    if (((previous < MP7_LAMP_HALF_COLUMN) &&
         (next >= MP7_LAMP_HALF_COLUMN)) ||
        (next < previous)) {
        seq->flash_count++;
    }

    seq->column = next;

    /* The column numbers increase within a cycle and wrap to a lower number
     * (or to the same column when only one is enabled) at the cycle
     * boundary. */
    if ((next <= previous) && (seq->pending_valid != 0u)) {
        apply_pending(ctx);
        if (seq->enable == 0u) {
            return 0;
        }
    }

    return 1;
}

/* Emit the hits of the current column, and handle the Guiro gate.
 *
 * An immediate rhythm change can disable the column the playhead is on. Such
 * a column takes no time, so the sequencer skips it and fires the next
 * enabled column instead of idling for a whole column. */
static void fire_column(struct mp7_context_s *ctx)
{
    struct mp7_sequencer *seq = &ctx->sequencer;
    uint8_t column;
    uint16_t hits;
    unsigned int instr;

    if ((seq->enable & ((uint64_t)1u << seq->column)) == 0u) {
        if (advance_column(ctx) == 0) {
            return;
        }
        if ((seq->enable & ((uint64_t)1u << seq->column)) == 0u) {
            return; /* nothing is enabled, so there is nothing to fire */
        }
    }

    column = seq->column;
    hits = seq->notes[column];

#ifdef LIBPOPPY_EXTRA
    {
        int accented = (int)((hits & MP7_PATTERN_ACCENT_BIT) != 0u);

        hits &= MP7_PATTERN_INSTRUMENT_MASK;

        for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
            int hit = (int)((hits >> instr) & 1u);

            if (instr == (unsigned int)MP7_INSTR_GUIRO) {
                /* The gate belongs to the sequencer, but a note the caller
                 * triggered itself keeps it open as well. */
                mp7_guiro_gate(ctx, (uint8_t)(hit != 0), ctx->guiro_manual_gated);
            } else if (hit != 0) {
                struct mp7_voice *voice = &ctx->voices[instr];

                voice->accent_gain_q16 = accented
                    ? ctx->accent_gain_q16
                    : POPPY_Q16_ONE;
                /* The accent is part of the voice's gain, so the cached
                 * gain has to follow it. */
                mp7_voice_refresh_gain(ctx, (mp7_instr)instr);
                mp7_voice_trigger(voice);
            }
        }
    }
#else
    for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
        int hit = (int)((hits >> instr) & 1u);

        if (instr == (unsigned int)MP7_INSTR_GUIRO) {
            /* The gate belongs to the sequencer, but a note the caller
             * triggered itself keeps it open as well. */
            mp7_guiro_gate(ctx, (uint8_t)(hit != 0), ctx->guiro_manual_gated);
        } else if (hit != 0) {
            mp7_voice_trigger(&ctx->voices[instr]);
        }
    }
#endif
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

    if (seq->column_pos != 0u) {
        seq->column_pos--;
        return;
    }

    /* The column that was running has used up its time. The playhead moves
     * to the next enabled column, which fires right away: a column that a
     * rhythm change disabled takes no time. */
    if (seq->column_fired != 0u) {
        seq->column_fired = 0u;
        if (advance_column(ctx) == 0) {
            return;
        }
    }

    fire_column(ctx);
    seq->column_fired = 1u;
    seq->column_pos = column_length(seq) - 1u;
}
