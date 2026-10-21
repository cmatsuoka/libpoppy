/*
 * mp7.h - public API of the Mini Pops library.
 *
 * The Mini Pops is a 15-voice percussion unit. This library is a integer-only
 * (32-bit fixed point) reimplementation of its sound generation and its 20
 * factory rhythm patterns. It renders a single stereo mix into a caller-supplied
 * buffer; there is no allocation, no I/O and no floating point in the library.
 */

#ifndef MP7_H
#define MP7_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample rate used when the caller passes 0 to mp7_init() or
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

/* The opaque context. */
typedef void *mp7_context;

/* A view of one factory rhythm. The pointers refer to const data owned by the
 * library and remain valid for the life of the process.
 *
 * `notes` is 64 columns of 16-bit instrument masks: bit n is set when
 * instrument n (in mp7_instr order) plays in that column, so bit 0 is the
 * Bass Drum. It holds the rhythm's own twelve instruments, bits 0..11.
 *
 * `percussion` is the shared Tambourine/Guiro/Quijada layer, 64 columns of
 * the same kind of mask, with bits 12..14 (Tambourine 12, Guiro 13,
 * Quijada 14). It is the same array for every rhythm, and it is the layer
 * that plays on its own when no rhythm button is pressed. The complete
 * pattern of a rhythm is `notes[column] | percussion[column]`.
 *
 * `enable` is 64 bytes, each 0 or 1; disabled columns take no time.
 * `triplet` is true for the shuffle rhythms. */
struct mp7_pattern_view {
    const uint16_t *notes;
    const uint16_t *percussion;
    const uint8_t *enable;
    bool triplet;
};

/* Number of bytes needed for one context. */
size_t mp7_context_size(void);

/* Initialize a context and reset its voices and sequencer. A sample_rate of
 * 0 selects MP7_DEFAULT_SAMPLE_RATE. */
void mp7_init(mp7_context ctx, uint32_t sample_rate);

/* Change the sample rate.
 *
 * The biquad coefficients depend on the rate, so every voice is re-initialized
 * and the envelopes and filter state of any sounding voice are lost. */
void mp7_set_sample_rate(mp7_context ctx, uint32_t sample_rate);

/* Reset the voices and the sequencer, keeping the sample rate. */
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
 * thread itself. */
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

/* Trigger by MIDI note. The factory note map is fixed (bass drum 36,
 * snare 1 40, snare 2 38, small bongo 60, large bongo 61, conga 64,
 * cowbell 56, claves 75, hihat 42, rimshot 37, cymbal 1 49, cymbal 2 57,
 * tambourine 54, guiro 74, quijada 79). A note that is not mapped is
 * ignored. velocity = 0 sends note-off. */
void mp7_trigger_note(mp7_context ctx, uint8_t note, uint8_t velocity);

/* Select the pressed rhythm buttons immediately.
 *
 * `bank` is the Rhythm Section Control Lever position. `buttons` is a mask of
 * the ten physical rhythm buttons: bit k is button k (0..9), and bits 10..15
 * are ignored. Several buttons in the same bank are OR-ed together, so a
 * selection can never mix bank A and bank B, just as on the original panel.
 *
 * buttons = 0 means no button is pressed. The sequencer keeps running in that
 * case: the main rhythm instruments fall silent and the fixed
 * Tambourine/Guiro/Quijada layer continues, matching the original unit.
 *
 * The playhead is preserved: the new grid takes effect where the current one
 * is playing, without restarting the pattern. Calling this starts the
 * sequencer if it was stopped. */
void mp7_set_rhythm(mp7_context ctx, mp7_bank bank, uint16_t buttons);

/* Queue the same bank/button selection for the next pattern cycle.
 *
 * While the sequencer is running, the current cycle finishes with the old
 * selection; the new selection is applied when the last enabled column wraps
 * to the first, and starts from its first enabled column. If the sequencer is
 * stopped, the selection takes effect immediately and starts it. An immediate
 * mp7_set_rhythm() cancels a pending change.
 *
 * A change that only moves the lever is queued and applied like any other:
 * with no rhythm button pressed both banks play the same percussion layer,
 * so the rhythm does not change, but the bank indication still follows the
 * lever at the cycle wrap. */
void mp7_queue_rhythm(mp7_context ctx, mp7_bank bank, uint16_t buttons);

/* Stop the sequencer immediately (the panel's Stop switch): silence, reset the
 * playhead and cancel any pending change. The last bank/button selection is
 * retained for mp7_get_panel_state(). This is distinct from selecting no
 * rhythm buttons, which keeps the percussion layer running. */
void mp7_stop_rhythms(mp7_context ctx);

/* Set the internal tempo in BPM. Clamped to 1..240; default 120. */
void mp7_set_tempo(mp7_context ctx, uint16_t bpm);

/* True while the sequencer transport is running, including the no-key case
 * where only the fixed percussion layer plays. False after mp7_stop_rhythms()
 * or before the first mp7_set_rhythm()/mp7_queue_rhythm(). */
bool mp7_rhythm_running(const void *ctx);

/* A snapshot of the panel state for a host UI.
 *
 * `bank` and `buttons` are the selection last applied by mp7_set_rhythm() or
 * by a queued change at the cycle wrap. `buttons` is the ten-button mask (bits
 * 10..15 are always clear). The selection is retained across
 * mp7_stop_rhythms(), which only stops the transport.
 *
 * `pending_*` describe a selection queued with mp7_queue_rhythm() and not yet
 * applied; they are meaningful only when `pending` is true.
 *
 * `column` is the current grid column, 0..63. `flash_count` is a monotonic
 * count of the lamp flashes produced by the sequencer. It advances at the same
 * two points as the reference unit: when the playhead reaches the first
 * enabled column at or after column 31 - the last column of the first
 * 32-column half - and when the playhead wraps back to the first enabled
 * column. It never decreases, is not reset by mp7_stop_rhythms(), and is
 * reset only by mp7_init()/mp7_reset().
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
void mp7_get_panel_state(const void *ctx,
                         struct mp7_panel_state *out);

/* Global mix controls, mapping the panel's remaining faders.
 *
 * Gains are Q16 units: 1.0 is exactly 65536. The values are linear
 * multipliers; the reference's knob taper is a host-side concern.
 *
 * Volume is the panel value 0..4 (0..262144). It is not the master gain: the
 * mixer applies the reference's 0.5 factor, so panel 1.0 is a master gain of
 * 0.5 and panel 2.0 is unity.
 *
 * Balance is 0..1 (0..65536), default 0.5; it crossfades the Bass Drum
 * against Hihat/Cymbal 1/Cymbal 2 only.
 *
 * perc_gain is the Additional Volume fader for Tambourine, Guiro and
 * Quijada only: 0..1, default 0.5. Any other instrument is ignored (it has
 * no corresponding panel fader). */
void mp7_set_volume(mp7_context ctx, int32_t volume_q16);
void mp7_set_balance(mp7_context ctx, int32_t balance_q16);
void mp7_set_perc_gain(mp7_context ctx, mp7_instr instr,
                       int32_t gain_q16);

/* Copy a view of one factory rhythm into `out`. Out-of-range input yields
 * null pointers and a false triplet flag. */
void mp7_get_factory_pattern(mp7_rhythm rhythm, struct mp7_pattern_view *out);

/* Map a factory rhythm to the bank and button bit that select it on the
 * panel. These helpers let a name/list based interface build a bank/button
 * selection. An out-of-range rhythm returns MP7_BANK_A and a zero button
 * bit. */
mp7_bank mp7_rhythm_bank(mp7_rhythm rhythm);
uint16_t mp7_rhythm_button_bit(mp7_rhythm rhythm);

/* Human-readable names for display. Return "?" for an invalid index. */
const char *mp7_instr_name(mp7_instr instr);
const char *mp7_rhythm_name(mp7_rhythm rhythm);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MP7_H */
