/*
 * fm1_wrapper.c - example integration of libpoppy into the FM-1 firmware.
 *
 * This file is not part of libpoppy. It shows the shape of the glue code:
 *
 *   1. At startup, create one context with mp7_create_context(), and release
 *      it again with mp7_destroy_context() when the instrument is shut down.
 *   2. In the audio ISR, render one half-buffer of 128 stereo frames with
 *      mp7_process() and hand it to the I2S DMA.
 *   3. From the control loop, translate the panel/encoder positions to the
 *      documented integer units and call the mp7_* setters.
 *
 * The panel mappings below are placeholders for the FM-1's own control
 * surface. They are not part of the Mini Pops spec; see the panel page.
 *
 * The library is not thread-safe. The audio ISR must not run while the
 * context is created or released, and the panel setters must not run while
 * the ISR renders: the firmware has to stop the I2S DMA, or block the ISR,
 * around fm1_audio_init() and fm1_audio_deinit(), and around every panel
 * change.
 */

#include <stddef.h>
#include <stdint.h>

#include "mp7.h"

/* The FM-1 audio ISR fills one half-buffer of 128 stereo frames. */
#define FM1_BLOCK_FRAMES 128u

/* Full-scale position of a 10-bit encoder or ADC reading. */
#define FM1_POSITION_MAX 1023u

static mp7_context fm1_context;
static int16_t *fm1_half_buffer;

void fm1_audio_deinit(void);

/* Returns 0 on success, -1 when the context could not be allocated. A failed
 * init leaves the wrapper in the state the ISR handles by rendering silence.
 *
 * The ISR must not be running: the library is not thread-safe, and the
 * context is created and released here. */
int fm1_audio_init(int16_t *half_buffer, uint32_t sample_rate)
{
    fm1_half_buffer = half_buffer;

    /* A second init must not leak the first context. */
    fm1_audio_deinit();

    fm1_context = mp7_create_context(sample_rate);
    if (fm1_context == 0) {
        return -1;
    }

    return 0;
}

/* Release the context. The wrapper can be re-initialized afterwards.
 *
 * The handle is cleared right after the release, so a second call is a no-op
 * and the ISR never sees a dangling pointer. Any other copy of the handle
 * the caller kept is stale, though: the library cannot validate a handle, so
 * such a copy must not be released again.
 *
 * The ISR must not be running while the context is released. */
void fm1_audio_deinit(void)
{
    if (fm1_context != 0) {
        mp7_destroy_context(fm1_context);
        fm1_context = 0;
    }
}

/* Audio ISR: called once per half-buffer. Nothing here allocates, blocks or
 * uses floating point. `fm1_half_buffer` is the DMA buffer the codec reads;
 * mp7_process() overwrites it with interleaved stereo Q15 samples. */
void fm1_audio_isr(void)
{
    if (fm1_half_buffer == 0) {
        return;
    }

    /* A null context is not an error here: mp7_process() clears the buffer,
     * so the DMA reads silence instead of whatever was in the buffer. */
    mp7_process(fm1_context, fm1_half_buffer, FM1_BLOCK_FRAMES);

    /* The driver now hands fm1_half_buffer to the I2S DMA. On the FM-1 the
     * Q15 samples are shifted left by 7 bits by the I2S layer. */
}

/* Master Volume: square-root taper, value = 4 * n^2. */
void fm1_panel_volume(uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    uint64_t value_q16 = ((uint64_t)p * (uint64_t)p * 4u * 65536u) /
                         ((uint64_t)FM1_POSITION_MAX * FM1_POSITION_MAX);

    mp7_set_volume(fm1_context, (int32_t)value_q16);
}

/* Balance and the instrument gains are linear over 0..1. The panel provides
 * three Additional Volume faders for Tambourine, Guiro and Quijada. */
void fm1_panel_balance(uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    int32_t balance_q16 = (int32_t)(((uint32_t)p * 65536u) / FM1_POSITION_MAX);

    mp7_set_balance(fm1_context, balance_q16);
}

void fm1_panel_instr_gain(mp7_instr instr, uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    int32_t gain_q16 = (int32_t)(((uint32_t)p * 65536u) / FM1_POSITION_MAX);

    mp7_set_instr_gain(fm1_context, instr, gain_q16);
}

/* Tempo knob: map the raw position onto the library's 1..240 BPM range. */
void fm1_panel_tempo(uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    uint32_t bpm = 1u + ((p * 239u) / FM1_POSITION_MAX);

    mp7_set_tempo(fm1_context, (uint16_t)bpm);
}

/* Rhythm selection: bank lever plus the ten latched buttons. Bit k of
 * `buttons` is panel button k; buttons = 0 means no button is pressed and
 * keeps the fixed percussion layer running. OR MP7_APPLY_NEXT_CYCLE into
 * `buttons` to apply the change at the next pattern cycle instead of now. */
void fm1_panel_rhythm(mp7_bank bank, uint16_t buttons)
{
    mp7_select_rhythm(fm1_context, bank, buttons);
}

void fm1_panel_stop(void)
{
    mp7_stop_rhythms(fm1_context);
}
