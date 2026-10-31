/*
 * mp7.c - context handling and entry points of the Mini Pops library.
 *
 * The context owns the 15 voice states, the rhythm sequencer and the global
 * mix controls. mp7_process() advances the sequencer, renders every voice and
 * sums it into a stereo mix through the shared mixer core.
 */

#include <stdlib.h>
#include <string.h>

#include "mp7_internal.h"

/* The output buffer is cleared in bounded chunks so that the byte count
 * (frames * 2 channels * 2 bytes) cannot overflow size_t on a 32-bit
 * target. On pi32v2 a single memset of more than SIZE_MAX bytes would
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

mp7_context mp7_create_context(uint32_t sample_rate)
{
    struct mp7_context_s *c =
        (struct mp7_context_s *)malloc(sizeof(struct mp7_context_s));

    if (c == 0) {
        return 0;
    }

    mp7_init((mp7_context)c, sample_rate);
    return (mp7_context)c;
}

void mp7_destroy_context(mp7_context ctx)
{
    free(ctx);
    ctx = NULL;
}

/* Prepare a context. Internal: mp7_create_context() and mp7_reset() drive
 * it, and the public API has no other entry point. */
void mp7_init(mp7_context ctx, uint32_t sample_rate)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    poppy_sine_init();

    memset(c, 0, sizeof(*c));

    if (sample_rate == 0u) {
        sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    }
    c->sample_rate = sample_rate;

#ifdef LIBPOPPY_EXTRA
    /* Extended-build synthesis defaults. These must match the factory voice
     * configuration in src/mp7_voice.c. Set them before mp7_voice_init_all()
     * so the voice setup can apply them. */
    c->accent_gain_q16 = MP7_PATTERN_ACCENT_GAIN_Q16;
    c->snare1_decay_q16 = POPPY_Q16_ONE;
    c->snare1_bandpass_hz = MP7_SNARE1_BANDPASS_HZ;
    c->hihat_highpass_hz = MP7_HIHAT_HIGHPASS_HZ;
#endif

    /* The mixer is initialized first: the voices take their pan from the
     * per-instrument pan positions. */
    poppy_mixer_init(&c->mixer);

    mp7_voice_init_all(c);
    mp7_pattern_init(c);

    /* The panel's three Additional Volume faders start at 0.5; every other
     * instrument starts at unity. */
    poppy_mixer_set_instr_gain(&c->mixer, MP7_INSTR_TAMBOURINE, POPPY_Q16_ONE / 2);
    poppy_mixer_set_instr_gain(&c->mixer, MP7_INSTR_GUIRO, POPPY_Q16_ONE / 2);
    poppy_mixer_set_instr_gain(&c->mixer, MP7_INSTR_QUIJADA, POPPY_Q16_ONE / 2);
}

void mp7_set_sample_rate(mp7_context ctx, uint32_t sample_rate)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    if (sample_rate == 0u) {
        sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    }
    c->sample_rate = sample_rate;
    mp7_voice_init_all(c);

    /* The voices were re-initialized, so no gate is open any more. */
    c->guiro_manual_gated = 0u;
    c->guiro_open = 0u;

    /* Keep the selected rhythms and tempo, but rebuild the timing and start
     * the current pattern from its first enabled column at the new rate. */
    mp7_pattern_set_tempo(c, c->sequencer.tempo);
    mp7_pattern_reset(c);
}

void mp7_reset(mp7_context ctx)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    uint32_t sample_rate;

    if (c == 0) {
        return;
    }

    /* Keep the sample rate, drop everything else back to the defaults. */
    sample_rate = c->sample_rate;
    mp7_init(ctx, sample_rate);
}

/* Effective per-voice gain: its fixed Level, scaled by Balance and by the
 * per-instrument gain, and by the accent of the note that is sounding.
 *
 * The gain lives in the context rather than in a local, because it also
 * changes when a column fires, which can happen in the middle of a block.
 * It is refreshed there, and whenever a note is triggered, so the per-sample
 * path only has to apply it. */
void mp7_process(mp7_context ctx, int16_t *stereo_out, size_t frames)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    size_t frame;
    unsigned int instr;

    if (stereo_out == 0) {
        return;
    }

    if (c == 0) {
        clear_stereo(stereo_out, frames);
        return;
    }

    /* The effective per-voice gain only changes when a mix control changes,
     * and a control cannot change while a block is being rendered, so the
     * gain is computed once per block instead of once per sample. */
    for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
        mp7_voice_refresh_gain(c, (mp7_instr)instr);
    }

    for (frame = 0u; frame < frames; frame++) {
        int64_t sum_left = 0;
        int64_t sum_right = 0;

        /* Triggers are evaluated before the sample is rendered, so hits are
         * sample-accurate. */
        mp7_pattern_tick(c);

        for (instr = 0u; instr < (unsigned int)MP7_INSTR_COUNT; instr++) {
            struct mp7_voice *voice = &c->voices[instr];
            int64_t sample;

            /* An idle voice has no envelope or filter state left to
             * advance, so skip the engine call entirely. */
            if (!voice->active) {
                continue;
            }

            sample = mp7_voice_process(voice);
            if (sample == 0) {
                continue;
            }

            sample = poppy_mul_q31_gain(sample, c->voice_gain_q16[instr]);
            sum_left += poppy_mixer_pan_left(sample, voice->pan_q16);
            sum_right += poppy_mixer_pan_right(sample, voice->pan_q16);
        }

        stereo_out[2u * frame] = poppy_mixer_to_q15(sum_left, c->mixer.panel_volume_q16);
        stereo_out[2u * frame + 1u] = poppy_mixer_to_q15(sum_right,
                                                         c->mixer.panel_volume_q16);
    }
}

void mp7_trigger(mp7_context ctx, mp7_instr instr, uint8_t velocity)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if ((c == 0) || ((unsigned int)instr >= (unsigned int)MP7_INSTR_COUNT)) {
        return;
    }

    if (instr == MP7_INSTR_GUIRO) {
        /* The Guiro is the gated voice: a note the caller triggers holds the
         * same gate the sequencer uses, so the two never fight over it. */
        mp7_guiro_gate(c, c->sequencer.guiro_gated,
                       (uint8_t)(velocity != 0u));
        return;
    }

    if (velocity == 0u) {
        mp7_voice_release(&c->voices[instr]);
        return;
    }

#ifdef LIBPOPPY_EXTRA
    c->voices[instr].accent_gain_q16 = POPPY_Q16_ONE;
    mp7_voice_refresh_gain(c, instr);
#endif
    mp7_voice_trigger(&c->voices[instr]);
}

/* The panel has one bank lever and ten buttons. Derive the internal absolute
 * rhythm mask from them; bits 10..14 of the button mask do not exist, and bit
 * 15 is the apply flag rather than a button. */
static uint32_t rhythm_selection(mp7_bank bank, uint16_t buttons)
{
    uint32_t button_mask = (uint32_t)buttons & 0x03FFu;

    return (bank == MP7_BANK_B) ? (button_mask << 10) : button_mask;
}

void mp7_select_rhythm(mp7_context ctx, mp7_bank bank, uint16_t buttons)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    uint16_t button_mask = (uint16_t)(buttons & 0x03FFu);
    int apply_next = (buttons & MP7_APPLY_NEXT_CYCLE) != 0u;
    uint32_t selected;
    int was_running;

    if (c == 0) {
        return;
    }

    bank = (bank == MP7_BANK_B) ? MP7_BANK_B : MP7_BANK_A;
    selected = rhythm_selection(bank, button_mask);
    was_running = (c->sequencer.running != 0u);

    c->sequencer.running = 1u;

    /* There is no current cycle to finish when the sequencer is stopped, so a
     * deferred selection takes effect immediately and starts the transport. */
    if (apply_next && was_running) {
        c->sequencer.pending_selected = selected;
        c->sequencer.pending_bank = (uint8_t)bank;
        c->sequencer.pending_buttons = button_mask;

        /* A change is pending when the rhythm mask or the panel indication
         * changes. The two are not the same thing: the no-button layer is the
         * same on both banks, so a lever change alone would not show up in the
         * mask, yet the bank lamp still has to follow it. */
        c->sequencer.pending_valid =
            ((selected != c->sequencer.selected) ||
             ((uint8_t)bank != c->sequencer.bank) ||
             (button_mask != c->sequencer.buttons)) ? 1u : 0u;
        return;
    }

    c->sequencer.bank = (uint8_t)bank;
    c->sequencer.buttons = button_mask;
    c->sequencer.pending_bank = (uint8_t)bank;
    c->sequencer.pending_buttons = button_mask;
    c->sequencer.pending_selected = selected;
    c->sequencer.pending_valid = 0u;

    if ((selected != c->sequencer.selected) || !was_running) {
        mp7_pattern_combine(c, selected);
    }
}

void mp7_stop_rhythms(mp7_context ctx)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    c->sequencer.running = 0u;
    c->sequencer.selected = 0u;
    c->sequencer.pending_selected = 0u;
    c->sequencer.pending_bank = c->sequencer.bank;
    c->sequencer.pending_buttons = c->sequencer.buttons;
    c->sequencer.pending_valid = 0u;

    mp7_pattern_combine(c, 0u);
    mp7_pattern_reset(c);
}

void mp7_set_tempo(mp7_context ctx, uint16_t bpm)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    mp7_pattern_set_tempo(c, (uint32_t)bpm);
}

bool mp7_rhythm_running(const void *ctx)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;
    return (c != 0) && (c->sequencer.running != 0u);
}

void mp7_get_panel_state(const void *ctx,
                         struct mp7_panel_state *out)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;
    if (out == 0) {
        return;
    }

    if (c == 0) {
        memset(out, 0, sizeof(*out));
        out->bank = MP7_BANK_A;
        out->pending_bank = MP7_BANK_A;
        return;
    }

    out->bank = (mp7_bank)c->sequencer.bank;
    out->buttons = c->sequencer.buttons;
    out->pending_bank = (mp7_bank)c->sequencer.pending_bank;
    out->pending_buttons = c->sequencer.pending_buttons;
    out->pending = (c->sequencer.pending_valid != 0u);
    out->running = (c->sequencer.running != 0u);
    out->column = c->sequencer.column;
    out->flash_count = c->sequencer.flash_count;
}

void mp7_set_volume(mp7_context ctx, int32_t volume_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    poppy_mixer_set_volume(&c->mixer, volume_q16);
}

void mp7_set_balance(mp7_context ctx, int32_t balance_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    if (c == 0) {
        return;
    }

    poppy_mixer_set_balance(&c->mixer, balance_q16);
}

void mp7_set_instr_gain(mp7_context ctx, mp7_instr instr, int32_t gain_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)MP7_INSTR_COUNT) {
        return;
    }

    poppy_mixer_set_instr_gain(&c->mixer, (unsigned int)instr, gain_q16);
}

void mp7_set_instr_pan(mp7_context ctx, mp7_instr instr, int32_t pan_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if ((unsigned int)instr >= (unsigned int)MP7_INSTR_COUNT) {
        return;
    }

    poppy_mixer_set_instr_pan(&c->mixer, (unsigned int)instr, pan_q16);

    /* The voice pans immediately, so a note that is already sounding moves
     * to the new position. */
    c->voices[instr].pan_q16 = c->mixer.instr_pan_q16[instr];
}

void mp7_get_factory_pattern(mp7_rhythm rhythm, struct mp7_pattern_view *out)
{
    const struct mp7_factory_rhythm *pattern;

    if (out == 0) {
        return;
    }

    if ((unsigned int)rhythm >= (unsigned int)MP7_RHYTHM_COUNT) {
        out->notes = 0;
        out->percussion = 0;
        out->enable = 0;
        out->notes_period = 0;
        out->percussion_period = 0;
        return;
    }

    pattern = &mp7_factory_rhythms[rhythm];
    out->notes = pattern->notes;
    out->percussion = mp7_shared_percussion_notes;
    out->enable = pattern->enable;
    out->notes_period = pattern->notes_period;
    out->percussion_period = MP7_PATTERN_HALF_STEPS;
}

uint64_t mp7_timesig_enable_mask(mp7_timesig timesig)
{
    switch (timesig) {
    case MP7_TIMESIG_2_4:
        return MP7_ENABLE_2_4;
    case MP7_TIMESIG_3_4:
        return MP7_ENABLE_3_4;
    case MP7_TIMESIG_5_4:
        return MP7_ENABLE_5_4;
    case MP7_TIMESIG_6_8:
        return MP7_ENABLE_6_8;
    case MP7_TIMESIG_7_4:
        return MP7_ENABLE_7_4;
    case MP7_TIMESIG_4_4:
        return MP7_ENABLE_4_4;
    default:
        return 0u;
    }
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

#ifdef LIBPOPPY_EXTRA
bool mp7_set_user_pattern(mp7_context ctx,
                           unsigned int index,
                           const struct mp7_user_pattern *pattern)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;
    struct mp7_user_slot *slot;
    uint8_t period;
    unsigned int column;

    if (c == 0) {
        return false;
    }
    if (index >= (unsigned int)MP7_USER_PATTERN_COUNT) {
        return false;
    }

    slot = &c->user_patterns[index];

    if (pattern == 0) {
        /* An empty slot plays nothing and disables no column. */
        memset(slot, 0, sizeof(*slot));
        slot->notes_period = (uint8_t)MP7_PATTERN_STEPS;
        return true;
    }

    /* The time signature names the hole pattern of the grid, and the
     * library derives the step-enable mask from it. A pattern therefore
     * cannot carry a mask of its own, and an unknown signature is refused
     * instead of being turned into one. */
    if ((unsigned int)pattern->timesig >= (unsigned int)MP7_TIMESIG_COUNT) {
        return false;
    }

    /* The stored period is the shortest repeating length of the grid, and it
     * is always a power of two, so the column wraps with a mask. */
    period = pattern->notes_period;
    if ((period != (uint8_t)MP7_PATTERN_QUARTER_STEPS) &&
        (period != (uint8_t)MP7_PATTERN_HALF_STEPS) &&
        (period != (uint8_t)MP7_PATTERN_STEPS)) {
        period = (uint8_t)MP7_PATTERN_STEPS;
    }

    memset(slot->notes, 0, sizeof(slot->notes));
    for (column = 0u; column < (unsigned int)period; column++) {
        slot->notes[column] = pattern->notes[column];
    }

    slot->timesig = (uint8_t)pattern->timesig;
    slot->notes_period = period;
    slot->used = 1u;

    /* A pattern that is playing changes with the pattern: the grid is
     * rebuilt from the current playhead, like an immediate rhythm change.
     * The preset source does not read the slots, so it is left alone. */
    if ((c->sequencer.source == MP7_SOURCE_USER) &&
        (c->sequencer.running != 0u)) {
        mp7_pattern_combine(c, c->sequencer.selected);
    }

    return true;
}

bool mp7_get_user_pattern(const void *ctx,
                           unsigned int index,
                           struct mp7_user_pattern *out)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;
    const struct mp7_user_slot *slot;

    if (out == 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if ((c == 0) || (index >= (unsigned int)MP7_USER_PATTERN_COUNT)) {
        return false;
    }

    slot = &c->user_patterns[index];
    if (slot->used == 0u) {
        return false;
    }

    memcpy(out->notes, slot->notes, sizeof(out->notes));
    out->timesig = (mp7_timesig)slot->timesig;
    out->notes_period = slot->notes_period;

    return true;
}

void mp7_set_pattern_source(mp7_context ctx, mp7_pattern_source source)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if (source != MP7_PATTERN_SOURCE_USER) {
        source = MP7_PATTERN_SOURCE_PRESET;
    }

    c->sequencer.source = (uint8_t)((source == MP7_PATTERN_SOURCE_USER)
                                     ? MP7_SOURCE_USER
                                     : MP7_SOURCE_PRESET);

    /* The switch is immediate, like MP7_APPLY_NOW: the selection keeps its
     * meaning and is re-read against the other set of patterns, the playhead
     * keeps its position, and a pending change is cancelled. */
    c->sequencer.pending_valid = 0u;
    c->sequencer.pending_selected = c->sequencer.selected;
    c->sequencer.pending_bank = c->sequencer.bank;
    c->sequencer.pending_buttons = c->sequencer.buttons;

    mp7_pattern_combine(c, c->sequencer.selected);
}

mp7_pattern_source mp7_get_pattern_source(const void *ctx)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if ((c == 0) || (c->sequencer.source != MP7_SOURCE_USER)) {
        return MP7_PATTERN_SOURCE_PRESET;
    }
    return MP7_PATTERN_SOURCE_USER;
}

void mp7_set_accent_gain(mp7_context ctx, int32_t gain_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    c->accent_gain_q16 = poppy_clamp_i32(gain_q16, POPPY_Q16_ONE / 2,
                                          2 * POPPY_Q16_ONE);
}

int32_t mp7_get_accent_gain(const void *ctx)
{
    const struct mp7_context_s *c = (const struct mp7_context_s *)ctx;

    if (c == 0) {
        return 0;
    }

    return c->accent_gain_q16;
}

void mp7_set_snare1_decay(mp7_context ctx, int32_t decay_q16)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    c->snare1_decay_q16 = poppy_clamp_i32(decay_q16, POPPY_Q16_ONE / 4,
                                          4 * POPPY_Q16_ONE);
    mp7_voice_apply_snare1_decay(c);
}

void mp7_set_snare1_cutoff(mp7_context ctx, uint32_t freq_hz)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if (freq_hz < MP7_SNARE1_CUTOFF_MIN_HZ) {
        freq_hz = MP7_SNARE1_CUTOFF_MIN_HZ;
    }
    if (freq_hz > MP7_SNARE1_CUTOFF_MAX_HZ) {
        freq_hz = MP7_SNARE1_CUTOFF_MAX_HZ;
    }

    c->snare1_bandpass_hz = freq_hz;
    mp7_voice_apply_snare1_cutoff(c);
}

void mp7_set_hihat_cutoff(mp7_context ctx, uint32_t freq_hz)
{
    struct mp7_context_s *c = (struct mp7_context_s *)ctx;

    if (c == 0) {
        return;
    }

    if (freq_hz < MP7_HIHAT_CUTOFF_MIN_HZ) {
        freq_hz = MP7_HIHAT_CUTOFF_MIN_HZ;
    }
    if (freq_hz > MP7_HIHAT_CUTOFF_MAX_HZ) {
        freq_hz = MP7_HIHAT_CUTOFF_MAX_HZ;
    }

    c->hihat_highpass_hz = freq_hz;
    mp7_voice_apply_hihat_cutoff(c);
}
#endif /* LIBPOPPY_EXTRA */

uint32_t mp7_version_number(void)
{
    return MP7_VERSION_NUMBER;
}
