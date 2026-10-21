/*
 * fm1_wrapper.c - example integration of libpoppy into the FM-1 firmware.
 *
 * This file is not part of libpoppy. It shows the shape of the glue code:
 *
 *   1. At startup, reserve one context in static RAM, using
 *      mp7_context_size(), and call mp7_init().
 *   2. In the audio ISR, render one half-buffer of 128 stereo frames with
 *      mp7_process() and hand it to the I2S DMA.
 *   3. From the control loop, translate the panel/encoder positions to the
 *      documented integer units and call the mp7_* setters.
 *
 * The panel mappings below are placeholders for the FM-1's own control
 * surface. They are not part of the Mini Pops spec; see the panel page.
 */

#include <stddef.h>
#include <stdint.h>

#include "mp7.h"

/* The FM-1 audio ISR fills one half-buffer of 128 stereo frames. */
#define FM1_BLOCK_FRAMES 128u

/* Full-scale position of a 10-bit encoder or ADC reading. */
#define FM1_POSITION_MAX 1023u

static mp7_context *fm1_context;
static int16_t *fm1_half_buffer;

void fm1_audio_init(void *context_memory, int16_t *half_buffer, uint32_t sample_rate)
{
    fm1_context = (mp7_context *)context_memory;
    fm1_half_buffer = half_buffer;

    mp7_init(fm1_context, sample_rate);
}

/* Audio ISR: called once per half-buffer. Nothing here allocates, blocks or
 * uses floating point. `fm1_half_buffer` is the DMA buffer the codec reads;
 * mp7_process() overwrites it with interleaved stereo Q15 samples. */
void fm1_audio_isr(void)
{
    if ((fm1_context == 0) || (fm1_half_buffer == 0)) {
        return;
    }

    mp7_process(fm1_context, fm1_half_buffer, FM1_BLOCK_FRAMES);

    /* The driver now hands fm1_half_buffer to the I2S DMA. On the FM-1 the
     * Q15 samples are shifted left by 7 bits by the I2S layer. */
}

/* Master Volume: the reference's square-root taper, value = 4 * n^2. */
void fm1_panel_volume(uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    uint64_t value_q16 = ((uint64_t)p * (uint64_t)p * 4u * 65536u) /
                         ((uint64_t)FM1_POSITION_MAX * FM1_POSITION_MAX);

    mp7_set_volume(fm1_context, (int32_t)value_q16);
}

/* Balance and the three Additional Volume faders are linear over 0..1. */
void fm1_panel_balance(uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    int32_t balance_q16 = (int32_t)(((uint32_t)p * 65536u) / FM1_POSITION_MAX);

    mp7_set_balance(fm1_context, balance_q16);
}

void fm1_panel_perc_gain(mp7_instr instr, uint16_t position)
{
    uint32_t p = (position > FM1_POSITION_MAX) ? FM1_POSITION_MAX : position;
    int32_t gain_q16 = (int32_t)(((uint32_t)p * 65536u) / FM1_POSITION_MAX);

    mp7_set_perc_gain(fm1_context, instr, gain_q16);
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
 * keeps the fixed percussion layer running. */
void fm1_panel_rhythm(mp7_bank bank, uint16_t buttons)
{
    mp7_set_rhythm(fm1_context, bank, buttons);
}

/* The same bank/button selection, applied when the current pattern cycle
 * ends. */
void fm1_panel_rhythm_at_cycle(mp7_bank bank, uint16_t buttons)
{
    mp7_queue_rhythm(fm1_context, bank, buttons);
}

void fm1_panel_stop(void)
{
    mp7_stop_rhythms(fm1_context);
}

/* Optional pad/keyboard input. */
void fm1_panel_note(uint8_t note, uint8_t velocity)
{
    mp7_trigger_note(fm1_context, note, velocity);
}
