/*
 * mp7.h - public API of the Mini Pops library.
 *
 * The Mini Pops is a 15-voice percussion unit. This library is a integer-only
 * (32-bit fixed point) reimplementation of its sound generation and its 20
 * factory rhythm patterns. It renders a single stereo mix into a caller-supplied
 * buffer. It has no I/O and no floating point. A context is created with
 * mp7_create_context(), which also builds the shared full-period sine table on
 * first use.
 *
 * Building with -DLIBPOPPY_EXTRA adds a second, host-writable set of 20
 * pattern slots and the switch that points the sequencer at them. Without
 * the macro the library is the preset-only build.
 */

#ifndef MP7_H
#define MP7_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library version. major/minor/patch are packed as 0x00MMmmpp.
 * The plain integer constants keep MP7_VERSION_NUMBER usable in #if. */
#define MP7_VERSION_MAJOR 0
#define MP7_VERSION_MINOR 2
#define MP7_VERSION_PATCH 0
#define MP7_VERSION_NUMBER \
    ((MP7_VERSION_MAJOR << 16) | (MP7_VERSION_MINOR << 8) | MP7_VERSION_PATCH)

/* Sample rate used when the caller passes 0 to mp7_create_context() or
 * mp7_set_sample_rate(). The FM-1 runs at 44100. */
#define MP7_DEFAULT_SAMPLE_RATE 44100u

/* The 15 instruments, in voice-slot order. */
typedef enum {
    MP7_INSTR_BASS = 0,
    MP7_INSTR_SNARE_1,
    MP7_INSTR_SNARE_2,
    MP7_INSTR_SMALL_BONGO,
    MP7_INSTR_LARGE_BONGO,
    MP7_INSTR_CONGA,
    MP7_INSTR_COWBELL,
    MP7_INSTR_CLAVES,
    MP7_INSTR_HIHAT,
    MP7_INSTR_RIMSHOT,
    MP7_INSTR_CYMBAL_1,
    MP7_INSTR_CYMBAL_2,
    MP7_INSTR_TAMBOURINE,
    MP7_INSTR_GUIRO,
    MP7_INSTR_QUIJADA,
    MP7_INSTR_COUNT = 15
} mp7_instr;

/* The 20 factory rhythms: 10 panel buttons x 2 banks. */
typedef enum {
    MP7_RHYTHM_WALTZ = 0,
    MP7_RHYTHM_SAMBA,
    MP7_RHYTHM_MAMBO,
    MP7_RHYTHM_RUMBA,
    MP7_RHYTHM_TANGO,
    MP7_RHYTHM_BOSSANOVA,
    MP7_RHYTHM_ROCK_1,
    MP7_RHYTHM_ROCK_3,
    MP7_RHYTHM_MARCH_2_4,
    MP7_RHYTHM_SWING,
    MP7_RHYTHM_JAZZ_WALTZ,
    MP7_RHYTHM_MERINGUE,
    MP7_RHYTHM_CHA_CHA,
    MP7_RHYTHM_BEGUINE,
    MP7_RHYTHM_HABANERA,
    MP7_RHYTHM_SLOW_ROCK,
    MP7_RHYTHM_ROCK_2,
    MP7_RHYTHM_ROCK_4,
    MP7_RHYTHM_MARCH_6_8,
    MP7_RHYTHM_LATIN_SWING,
    MP7_RHYTHM_COUNT = 20
} mp7_rhythm;

/* The two positions of the Rhythm Section Control Lever. */
typedef enum {
    MP7_BANK_A = 0,
    MP7_BANK_B = 1
} mp7_bank;

/* The time signature of a pattern.
 *
 * The signature is the metric the grid is built on. It names the hole
 * pattern of the grid - which columns are skipped - and with it the grid
 * the pattern plays on. A pattern never carries a step-enable mask of its
 * own: the library derives the mask from the signature, so a pattern cannot
 * ask for a hole pattern the hardware does not have.
 *
 * A bar is as long as the signature says: the 3/4 bar is three beats, the
 * 4/4 bar four, the 5/4 bar five and the 7/4 bar seven, and the grid skips
 * the columns that are not part of a bar. The 2/4 and the 6/8 share one
 * hole pattern, the compound one, so their masks are the same and the name
 * only records the label the rhythm carries on the panel.
 *
 * The zero value is 4/4, so a pattern that is only zeroed is a straight
 * 64-column loop. */
typedef enum {
    MP7_TIMESIG_4_4 = 0,
    MP7_TIMESIG_2_4 = 1,
    MP7_TIMESIG_3_4 = 2,
    MP7_TIMESIG_5_4 = 3,
    MP7_TIMESIG_6_8 = 4,
    MP7_TIMESIG_7_4 = 5,
    MP7_TIMESIG_COUNT = 6
} mp7_timesig;

/* The step-enable mask of a time signature: a 64-bit word, one bit per
 * column, and a disabled column takes no time.
 */
uint64_t mp7_timesig_enable_mask(mp7_timesig timesig);

/* Schedule flags for mp7_select_rhythm(), OR-ed into the button mask.
 * MP7_APPLY_NOW is zero, so the immediate case needs no flag. */
#define MP7_APPLY_NOW 0x0000u
#define MP7_APPLY_NEXT_CYCLE 0x8000u

/* The opaque context. */
typedef void *mp7_context;

/* Return the version of the library the process is linked against, packed as
 * MP7_VERSION_NUMBER. This reports the compiled library, so a host can compare
 * it with the header's MP7_VERSION_NUMBER to detect a mismatched build. */
uint32_t mp7_version_number(void);

/* A view of one factory rhythm. The pointers refer to const data owned by the
 * library and remain valid for the life of the process.
 *
 * `notes` is `notes_period` columns of 16-bit instrument masks: bit n is set
 * when instrument n (in mp7_instr order) plays in that column, so bit 0 is
 * the Bass Drum. It holds the rhythm's own twelve instruments, bits 0..11.
 * `notes_period` is the shortest repeating period of the grid: 16, 32 or 64
 * columns. Column c of the 64-column cycle is
 * `notes[c & (notes_period - 1)]`.
 *
 * `percussion` is the shared Tambourine/Guiro/Quijada layer,
 * `percussion_period` columns of the same kind of mask, with bits 12..14
 * (Tambourine 12, Guiro 13, Quijada 14). It is the same array for every
 * rhythm, and it is the layer that plays on its own when no rhythm button is
 * pressed. The complete pattern column c is
 * `notes[c & (notes_period - 1)] | percussion[c & (percussion_period - 1)]`.
 *
 * `enable` is a 64-bit mask, one bit per column; disabled columns take no
 * time. It is one of the masks of the time signatures, and
 * mp7_timesig_enable_mask() returns the mask of each of them. The mask also
 * decides the grid the rhythm plays on: the compound mask plays compound,
 * every other mask plays straight.
 */
struct mp7_pattern_view {
    const uint16_t *notes;
    const uint16_t *percussion;
    uint64_t enable;
    uint8_t notes_period;
    uint8_t percussion_period;
};

/* Allocate and initialize a context. A sample_rate of 0 selects
 * MP7_DEFAULT_SAMPLE_RATE. Returns NULL when the allocation fails.
 *
 * The caller owns the returned context and releases it with
 * mp7_destroy_context(). */
mp7_context mp7_create_context(uint32_t sample_rate);

/* Release a context returned by mp7_create_context(). A null context is
 * ignored. Every context must be released exactly once: the handle cannot be
 * validated, so releasing it twice, or releasing a pointer that
 * mp7_create_context() did not return, is undefined behaviour. */
void mp7_destroy_context(mp7_context ctx);

/* Change the sample rate.
 *
 * The biquad coefficients depend on the rate, so every voice is re-initialized
 * and the envelopes and filter state of any sounding voice are lost. */
void mp7_set_sample_rate(mp7_context ctx, uint32_t sample_rate);

/* Reset the voices, the sequencer and the mix controls to their defaults,
 * keeping the sample rate.
 *
 * In an extended build (-DLIBPOPPY_EXTRA) the user-defined pattern slots
 * are preserved, but the pattern source is reset to the factory rhythms. */
void mp7_reset(mp7_context ctx);

/* Render `frames` stereo interleaved frames into `stereo_out`.
 *
 * `stereo_out` must have room for `frames` stereo frames (2 * frames
 * int16_t values). The buffer is cleared in bounded chunks, so any
 * `frames` value is safe on a 32-bit target as long as the buffer is
 * really that large.
 *
 * The library is not thread-safe: mp7_process() and the setters must never
 * run at the same time. A panel control that is changed from another thread
 * has to be applied while the audio thread is blocked, or from the audio
 * thread itself. mp7_create_context() and mp7_destroy_context() must not run
 * at the same time either. */
void mp7_process(mp7_context ctx, int16_t *stereo_out, size_t frames);

/* Trigger one instrument. `velocity` is 0..127; 0 means note-off.
 *
 * The velocity value is accepted but instruments don't respond to it.
 * Note-off is meaningful only for the gated Guiro; every other instrument
 * is a one-shot and ignores it.
 *
 * The Guiro gate has two holders: the sequencer's own pattern gate and the
 * note the caller triggers here. The voice sounds while either one holds it,
 * so a triggered note is not cut off by the pattern and the pattern is not
 * cut off by a note-off. A note that arrives while the gate is already open
 * does not restart the ratchet. */
void mp7_trigger(mp7_context ctx, mp7_instr instr, uint8_t velocity);

/* Select the pressed rhythm buttons.
 *
 * `bank` is the Rhythm Section Control Lever position. `buttons` is a mask of
 * the ten physical rhythm buttons: bit k is button k (0..9), and bits 10..14
 * are ignored. Several buttons in the same bank are OR-ed together, so a
 * selection can never mix bank A and bank B.
 *
 * OR MP7_APPLY_NEXT_CYCLE into `buttons` to apply the selection at the next
 * pattern cycle instead of immediately. MP7_APPLY_NOW is zero, so the plain
 * button mask means "apply now":
 *
 * - With MP7_APPLY_NOW the selection takes effect at the current playhead
 *   position, without restarting the pattern, and cancels any pending change.
 * - With MP7_APPLY_NEXT_CYCLE the current cycle finishes with the old
 *   selection; the new selection is applied when the last enabled column wraps
 *   to the first, and starts from its first enabled column. A later
 *   MP7_APPLY_NEXT_CYCLE call replaces an earlier pending selection.
 *
 * buttons = 0 means no button is pressed. The sequencer keeps running in that
 * case: the main rhythm instruments fall silent and the fixed
 * Tambourine/Guiro/Quijada layer continues.
 *
 * With either flag, a call starts the sequencer if it was stopped, and a
 * selection made while it is stopped takes effect immediately.
 *
 * A deferred change that only moves the lever is applied like any other:
 * with no rhythm button pressed both banks play the same percussion layer,
 * so the rhythm does not change, but the bank indication still follows the
 * lever at the cycle wrap. */
void mp7_select_rhythm(mp7_context ctx, mp7_bank bank, uint16_t buttons);

/* Stop the sequencer immediately (the panel's Stop switch): silence, reset the
 * playhead and cancel any pending change. The last bank/button selection is
 * retained for mp7_get_panel_state(). This is distinct from selecting no
 * rhythm buttons, which keeps the percussion layer running. */
void mp7_stop_rhythms(mp7_context ctx);

/* Set the internal tempo in BPM. Clamped to 1..240; default 120. */
void mp7_set_tempo(mp7_context ctx, uint16_t bpm);

/* True while the sequencer transport is running, including the no-key case
 * where only the fixed percussion layer plays. False after mp7_stop_rhythms()
 * or before the first mp7_select_rhythm(). */
bool mp7_rhythm_running(const void *ctx);

/* A snapshot of the panel state for a host UI.
 *
 * `bank` and `buttons` are the selection last applied by mp7_select_rhythm()
 * or by a pending change at the cycle wrap. `buttons` is the ten-button mask
 * (bits 10..15 are always clear). The selection is retained across
 * mp7_stop_rhythms(), which only stops the transport.
 *
 * `pending_*` describe a selection made with MP7_APPLY_NEXT_CYCLE and not yet
 * applied; they are meaningful only when `pending` is true.
 *
 * `column` is the current grid column, 0..63. `flash_count` is a monotonic
 * count of the lamp flashes produced by the sequencer. It advances on two
 * grid points: when the playhead reaches the first enabled column at or
 * after column 31 - the last column of the first 32-column half - and when
 * the playhead wraps back to the first enabled column. It never decreases, is
 * not reset by mp7_stop_rhythms(), and is reset only by mp7_reset().
 *
 * The host can drive the two pilot/tempo lamps from this snapshot: light the
 * lamp for `bank` while stopped; while running, pulse that lamp whenever
 * `flash_count` changes. The host owns the visible pulse length. */
struct mp7_panel_state {
    mp7_bank bank;
    uint16_t buttons;
    mp7_bank pending_bank;
    uint16_t pending_buttons;
    bool pending;
    bool running;
    uint8_t column;
    uint32_t flash_count;
};

/* Copy the current panel state into `out`. A null `out` is ignored and
 * nothing is written. A null or invalid context yields a zeroed snapshot
 * (bank A, not running). The library is not thread-safe: do not call this
 * while mp7_process() is running on another thread. */
void mp7_get_panel_state(const void *ctx, struct mp7_panel_state *out);

/* Global mix controls, mapping the panel's remaining faders.
 *
 * Gains are Q16 units: 1.0 is exactly 65536. The values are linear
 * multipliers; the knob taper is a host-side concern.
 *
 * Volume is the panel value 0..4 (0..262144). It is not the master gain: the
 * mixer applies a fixed 0.5 factor, so panel 1.0 is a master gain of
 * 0.5 and panel 2.0 is unity.
 *
 * Balance is 0..1 (0..65536), default 0.5; it crossfades the Bass Drum
 * against Hihat/Cymbal 1/Cymbal 2 only.
 *
 * instr_gain is a per-instrument gain: 0..1. Tambourine, Guiro and Quijada
 * default to 0.5, matching their panel Additional Volume faders; every other
 * instrument defaults to 1.0.
 *
 * instr_pan is a per-instrument pan position: 0..1 (0..65536), where 0 is
 * hard left, 65536 is hard right and 32768 - the default of every
 * instrument - is centre. The pan law is linear, so a centred voice is half
 * in each channel. The pan takes effect at once, also on a note that is
 * already sounding. */
void mp7_set_volume(mp7_context ctx, int32_t volume_q16);
void mp7_set_balance(mp7_context ctx, int32_t balance_q16);
void mp7_set_instr_gain(mp7_context ctx, mp7_instr instr, int32_t gain_q16);
void mp7_set_instr_pan(mp7_context ctx, mp7_instr instr, int32_t pan_q16);

/* Copy a view of one factory rhythm into `out`. Out-of-range input yields
 * null pointers and a zeroed mask. */
void mp7_get_factory_pattern(mp7_rhythm rhythm, struct mp7_pattern_view *out);

/* Map a factory rhythm to the bank and button bit that select it on the
 * panel. These helpers let a name/list based interface build a bank/button
 * selection. An out-of-range rhythm returns MP7_BANK_A and a zero button
 * bit. */
mp7_bank mp7_rhythm_bank(mp7_rhythm rhythm);
uint16_t mp7_rhythm_button_bit(mp7_rhythm rhythm);

#ifdef LIBPOPPY_EXTRA
/* ---------------------------------------------------------------------------
 * Extended build: user-defined patterns.
 *
 * Define LIBPOPPY_EXTRA when building the library to get a second set of 20
 * pattern slots that the host fills itself, and a switch that points the
 * sequencer at those slots instead of at the 20 factory rhythms. The factory
 * rhythms stay read-only in either build: the sequencer never writes them.
 *
 * Without the macro the library is the preset-only build it has always been
 * - the 20 factory rhythms, the shared percussion layer, and no extra RAM
 * and no extra entry points.
 * ------------------------------------------------------------------------ */

/* The number of user pattern slots. They are addressed the way the factory
 * rhythms are: slots 0..9 are the bank A buttons, slots 10..19 are the bank
 * B buttons. */
#define MP7_USER_PATTERN_COUNT 20u

/* The number of columns in a user pattern grid. */
#define MP7_USER_PATTERN_COLUMNS 64u

/* Accent flag for a user-defined pattern column. When this bit is set in a
 * column, every instrument in that column (except the gated Guiro) is played
 * louder. It occupies the 16th bit because the 15 instruments fill bits 0..14. */
#define MP7_PATTERN_ACCENT ((uint16_t)(1u << MP7_INSTR_COUNT))

/* Which set of patterns the sequencer plays. */
typedef enum {
    MP7_PATTERN_SOURCE_PRESET = 0, /* the 20 factory rhythms */
    MP7_PATTERN_SOURCE_USER = 1    /* the 20 user-defined slots */
} mp7_pattern_source;

/* A user-defined pattern.
 *
 * `notes` is MP7_USER_PATTERN_COLUMNS columns of 16-bit instrument masks:
 * bit n is set when instrument n (in mp7_instr order) plays in that column,
 * and MP7_PATTERN_ACCENT (bit 15) marks an accented column. Unlike a factory
 * rhythm, a user pattern may use every instrument, the Tambourine, Guiro and
 * Quijada included: a user pattern is a complete pattern and not a rhythm
 * plus the shared percussion layer, so the shared layer is never added to it.
 *
 * `timesig` is the time signature of the pattern. It is the only way to
 * shape the grid: the library derives the step-enable mask from it, so the
 * pattern cannot carry a mask of its own and cannot ask for a hole pattern
 * the hardware does not have. A 2/4 or 6/8 pattern plays on the compound
 * (shuffle) grid, every other signature plays straight. An unknown signature
 * is rejected.
 *
 * `notes_period` is the length of the repeating part of the grid: 16, 32
 * or 64 columns. Column c of the pattern is `notes[c & (notes_period - 1)]`
 * and the columns from notes_period on are ignored. Any other value is
 * stored as a full 64-column grid.
 *
 * Every member of the struct is an input: the grid, the metric and the
 * period are what the host writes, and the library derives the rest. */
struct mp7_user_pattern {
    uint16_t notes[MP7_USER_PATTERN_COLUMNS];
    mp7_timesig timesig;
    uint8_t notes_period;
};

/* Copy a pattern into one of the user slots.
 *
 * `index` is 0..MP7_USER_PATTERN_COUNT-1 and follows the panel: slots 0..9
 * are the bank A buttons, slots 10..19 the bank B buttons. The pattern is
 * copied, so the caller keeps ownership of `pattern` and may release it
 * right away.
 *
 * A null `pattern` clears the slot. An empty slot plays nothing and
 * disables no column, so it behaves like a silent pattern when it is
 * selected on its own.
 *
 * A pattern that is playing changes with the pattern: the grid is rebuilt
 * from the current playhead, like an immediate rhythm change.
 *
 * Returns false when `ctx` is null, `index` is out of range or the pattern
 * carries an unknown time signature; the slots are left alone in that case. */
bool mp7_set_user_pattern(mp7_context ctx,
                           unsigned int index,
                           const struct mp7_user_pattern *pattern);

/* Copy the pattern in one of the user slots into `out`.
 *
 * Returns false when `ctx` is null, `index` is out of range, `out` is null
 * or the slot is empty; an empty or invalid slot yields a zeroed pattern.
 * The copy is the pattern as it was stored, so a set/get round trip returns
 * the same grid, time signature and period. */
bool mp7_get_user_pattern(const void *ctx,
                           unsigned int index,
                           struct mp7_user_pattern *out);

/* Point the sequencer at the factory rhythms or at the user slots.
 *
 * The switch is immediate, like MP7_APPLY_NOW: the current bank/button
 * selection keeps its meaning and is re-read against the other set of
 * patterns, the playhead keeps its position and a pending change is
 * cancelled. */
void mp7_set_pattern_source(mp7_context ctx, mp7_pattern_source source);

/* The set of patterns the sequencer plays. */
mp7_pattern_source mp7_get_pattern_source(const void *ctx);

/* ---------------------------------------------------------------------------
 * Extended build: accent intensity and per-voice synthesis tweaks.
 *
 * These controls are not part of the original panel; they are provided for
 * hosts that want to shape the sound beyond the factory defaults. All of
 * them are reset to their defaults by mp7_reset().
 * ------------------------------------------------------------------------ */

/* Set the accent gain multiplier for accented user-pattern columns.
 *
 * gain_q16 is in Q16 units (1.0 = 65536). The default is 1.2 (78643).
 * The control range is 0.5..2.0, so 1.0 means "no accent boost" and
 * values above it make accented columns louder. Clamped to 0.5..2.0.
 *
 * The value is read when a column fires, so a change takes effect on
 * the notes triggered after it, not on the ones already sounding. */
void mp7_set_accent_gain(mp7_context ctx, int32_t gain_q16);

/* Return the current accent gain multiplier in Q16 units.
 *
 * Returns 0 when `ctx` is null. */
int32_t mp7_get_accent_gain(const void *ctx);

/* Set the Snare 1 decay time multiplier.
 *
 * 1.0 is the factory decay. Values below 1.0 shorten the decay; values
 * above 1.0 lengthen it. Clamped to 0.25..4.0.
 *
 * A note that is already sounding is not cut off: it keeps playing and
 * decays at the new rate from the next sample. */
void mp7_set_snare1_decay(mp7_context ctx, int32_t decay_q16);

/* Set the Snare 1 band-pass filter cut-off frequency in Hz.
 *
 * The factory default is 5500 Hz. Clamped to 1000..15000.
 *
 * The filter is stable only below the sample rate's Nyquist
 * frequency, so the cut-off is also limited to two fifths of the
 * sample rate. At 44100 Hz that is above 15000 Hz, so the whole
 * documented range is available at the default rate. */
void mp7_set_snare1_cutoff(mp7_context ctx, uint32_t freq_hz);

/* Set the Hihat high-pass filter cut-off frequency in Hz.
 *
 * The factory default is 1000 Hz. Clamped to 500..15000.
 *
 * As with the Snare 1 cut-off, the filter is stable only below
 * Nyquist, so the cut-off is also limited to two fifths of the
 * sample rate. */
void mp7_set_hihat_cutoff(mp7_context ctx, uint32_t freq_hz);
#endif /* LIBPOPPY_EXTRA */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MP7_H */
