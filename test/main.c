/*
 * main.c - test harness for the Mini Pops library.
 *
 * Commands:
 *   --init              report the context size, the default sample rate and
 *                       the sine table footprint
 *   --sine              verify the shared sine table against
 *                       sin(2*pi*i/4096) at all 4096 indices
 *   --gain              verify the Q15 gain multiply, including gains above
 *                       unity
 *   --silence [path]    render silence (no rhythm selected), check it, and
 *                       write a WAV file
 *   --instruments [dir] render all 15 instruments to WAV files
 *   --noise             verify the 6 noise voices are uncorrelated
 *   --guiro             verify Guiro gate hold and note-off release
 *   --quijada           verify Quijada frequencies, thresholds and comparator
 *   --mixer             verify pan, Volume, Balance and direct gains
 *   --pattern           verify the 20 patterns, combination and timing
 *   --rhythms [dir]     render all 20 rhythms and two combinations to WAV
 *   --help              this text
 *
 * Exit status is 0 when every check passed, 1 otherwise.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp7.h"
#include "mp7_fixed.h"
#include "mp7_internal.h"
#include "wav_writer.h"

#ifndef TEST_PI
#define TEST_PI 3.14159265358979323846
#endif

#define TEST_SILENCE_SECONDS 0.25

static int failures = 0;

static void fail(const char *what)
{
    printf("FAIL: %s\n", what);
    failures++;
}

static void ok(const char *what)
{
    printf("ok  : %s\n", what);
}

static mp7_context *context_new(void)
{
    mp7_context *ctx = (mp7_context *)malloc(mp7_context_size());

    if (ctx == 0) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    return ctx;
}

static int command_init(void)
{
    mp7_context *ctx = context_new();

    printf("context size        : %lu bytes\n", (unsigned long)mp7_context_size());
    printf("default sample rate : %u Hz\n", (unsigned)MP7_DEFAULT_SAMPLE_RATE);

    mp7_init(ctx, 0);
    if (ctx->sample_rate != MP7_DEFAULT_SAMPLE_RATE) {
        fail("mp7_init(ctx, 0) did not select the default sample rate");
    } else {
        ok("mp7_init(ctx, 0) selects the default sample rate");
    }

    if (ctx->magic != MP7_CONTEXT_MAGIC) {
        fail("context is not marked as initialized");
    } else {
        ok("context is marked as initialized");
    }

    mp7_set_sample_rate(ctx, 22050);
    if (ctx->sample_rate != 22050u) {
        fail("mp7_set_sample_rate() did not take effect");
    } else {
        ok("mp7_set_sample_rate() takes effect");
    }

    printf("sine table (shared) : %u entries, %lu bytes\n",
           (unsigned)POPPY_SINE_TABLE_SIZE,
           (unsigned long)(POPPY_SINE_TABLE_SIZE * sizeof(int16_t)));

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_sine(void)
{
    mp7_context *ctx = context_new();
    unsigned int i;
    int mismatches = 0;
    int worst = 0;

    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);

    for (i = 0; i < POPPY_SINE_TABLE_SIZE; i++) {
        double exact = 32767.0 * sin(2.0 * TEST_PI * (double)i / (double)POPPY_SINE_TABLE_SIZE);
        int expected = (int)floor(exact + 0.5);
        int got = (int)poppy_sine[i];
        int error = got - expected;

        if (error < 0) {
            error = -error;
        }
        if (error > worst) {
            worst = error;
        }
        if (error != 0) {
            if (mismatches < 8) {
                printf("      i=%u: got %d, expected %d\n", i, got, expected);
            }
            mismatches++;
        }
    }

    if (mismatches != 0) {
        printf("FAIL: %d of %u entries differ (worst %d LSB)\n",
               mismatches, (unsigned)POPPY_SINE_TABLE_SIZE, worst);
        failures++;
    } else {
        ok("the shared table matches sin(2*pi*i/4096) at all 4096 indices");
    }

    /* The table is the only sine data in the image, and it is shared:
     * it must not live inside the context. */
    if (sizeof(poppy_sine) != 8192u) {
        fail("the sine table is not 8 KB");
    } else {
        ok("the sine table is 8 KB (4096 x int16)");
    }

    {
        const char *table = (const char *)poppy_sine;
        const char *ctx_start = (const char *)ctx;
        const char *ctx_end = ctx_start + mp7_context_size();

        if ((table >= ctx_start) && (table < ctx_end)) {
            fail("the sine table lives inside the context");
        } else {
            ok("the sine table is shared, not stored in the context");
        }
    }

    /* The table is const data: it must not depend on the context. */
    if (ctx->sample_rate != MP7_DEFAULT_SAMPLE_RATE) {
        fail("the sample rate was not set");
    }

    mp7_set_sample_rate(ctx, 22050);
    if (ctx->sample_rate != 22050u) {
        fail("mp7_set_sample_rate() did not take effect");
    } else {
        ok("mp7_set_sample_rate() takes effect");
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_gain(void)
{
    /* Gain parameters are Q16 units: 1.0 is 65536. The interesting case is
     * a gain above unity, which a Q31 gain could not represent at all. */
    const int32_t unity = 65536;
    const int32_t max_level = 262144;   /* 4.0 */
    int64_t product;

    /* unity gain is the identity */
    product = poppy_mul_q31_gain(2147483647, unity);
    if (product != 2147483647) {
        printf("FAIL: unity gain changed the sample (%lld)\n", (long long)product);
        failures++;
    } else {
        ok("unity gain leaves a Q31 sample unchanged");
    }

    /* half gain halves it */
    product = poppy_mul_q31_gain(2147483647, unity / 2);
    if (product != 2147483647LL / 2) {
        printf("FAIL: half gain gave %lld\n", (long long)product);
        failures++;
    } else {
        ok("half gain halves a Q31 sample");
    }

    /* zero gain silences it */
    if (poppy_mul_q31_gain(2147483647, 0) != 0) {
        fail("zero gain did not silence the sample");
    } else {
        ok("zero gain silences a Q31 sample");
    }

    /* a negative sample stays negative */
    if (poppy_mul_q31_gain(-2147483647, unity) > 0) {
        fail("a negative sample changed sign");
    } else {
        ok("a negative sample keeps its sign");
    }

    /* 4.0 gain overflows a Q31 gain but must still be exact in 64 bits */
    product = poppy_mul_q31_gain(2147483647, max_level);
    if (product != 2147483647LL * 4LL) {
        printf("FAIL: 4.0 gain gave %lld, expected %lld\n",
               (long long)product, 2147483647LL * 4LL);
        failures++;
    } else {
        ok("4.0 gain is exact in 64 bits (a Q31 gain could not hold it)");
    }

    return failures == 0 ? 0 : 1;
}

static int command_silence(const char *path)
{
    mp7_context *ctx = context_new();
    const uint32_t sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    size_t frames = (size_t)((TEST_SILENCE_SECONDS * (double)sample_rate) + 0.5);
    int16_t *buffer = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    size_t i;
    int nonzero = 0;

    if (buffer == 0) {
        printf("FAIL: out of memory\n");
        free(ctx);
        return 1;
    }

    memset(buffer, 0xA5, frames * 2u * sizeof(int16_t));

    mp7_init(ctx, sample_rate);
    mp7_process(ctx, buffer, frames);

    for (i = 0; i < frames * 2u; i++) {
        if (buffer[i] != 0) {
            nonzero++;
        }
    }

    if (nonzero != 0) {
        printf("FAIL: %d samples are not silent\n", nonzero);
        failures++;
    } else {
        ok("the renderer writes silence when no rhythm is selected");
    }

    if (wav_write_stereo16(path, buffer, frames, sample_rate) != 0) {
        printf("FAIL: could not write %s\n", path);
        failures++;
    } else {
        printf("ok  : wrote %s (%lu frames)\n", path, (unsigned long)frames);
    }

    free(buffer);
    free(ctx);
    return failures == 0 ? 0 : 1;
}


static void render_voice_i32(mp7_context *ctx,
                             mp7_instr instr,
                             int32_t *out,
                             size_t frames,
                             size_t release_at)
{
    size_t i;

    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[instr]);

    for (i = 0; i < frames; i++) {
        int64_t sample;

        if (i == release_at) {
            mp7_voice_release(&ctx->voices[instr]);
        }
        sample = mp7_voice_process(&ctx->voices[instr]);
        out[i] = poppy_sat_i64_to_i32(sample);
    }
}

static int peak_i32(const int32_t *samples, size_t count)
{
    int peak = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        int value = poppy_abs_i32(samples[i]);

        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

static double rms_i32(const int32_t *samples, size_t count)
{
    double sum = 0.0;
    size_t i;

    for (i = 0; i < count; i++) {
        sum += (double)samples[i] * (double)samples[i];
    }
    return (count > 0u) ? sqrt(sum / (double)count) : 0.0;
}

static int command_instruments(const char *dir)
{
    static const mp7_instr instruments[MP7_INSTR_COUNT] = {
        MP7_INSTR_BASS, MP7_INSTR_SNARE_1, MP7_INSTR_SNARE_2,
        MP7_INSTR_HI_BONGO, MP7_INSTR_LO_BONGO, MP7_INSTR_CONGA,
        MP7_INSTR_COWBELL, MP7_INSTR_CLAVES, MP7_INSTR_HIHAT,
        MP7_INSTR_RIMSHOT, MP7_INSTR_CYMBAL_1, MP7_INSTR_CYMBAL_2,
        MP7_INSTR_TAMBOURINE, MP7_INSTR_GUIRO, MP7_INSTR_QUIJADA
    };
    mp7_context *ctx = context_new();
    const uint32_t sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    const size_t frames = (size_t)sample_rate;
    const size_t tail_frames = (size_t)(sample_rate / 20u);
    int32_t *mono = (int32_t *)malloc(frames * sizeof(int32_t));
    int16_t *stereo = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    unsigned int i;

    if ((mono == 0) || (stereo == 0)) {
        printf("FAIL: out of memory\n");
        free(mono);
        free(stereo);
        free(ctx);
        return 1;
    }

    for (i = 0; i < (unsigned int)MP7_INSTR_COUNT; i++) {
        char path[512];
        size_t release_at = frames;
        int peak;
        int tail;
        size_t j;

        if (instruments[i] == MP7_INSTR_GUIRO) {
            release_at = (size_t)(sample_rate / 5u);
        }

        render_voice_i32(ctx, instruments[i], mono, frames, release_at);
        peak = peak_i32(mono, frames);
        tail = peak_i32(mono + (frames - tail_frames), tail_frames);

        if (peak < 100) {
            printf("FAIL: instrument %u is silent (peak %d)\n", i, peak);
            failures++;
        }
        if (tail > 100) {
            printf("FAIL: instrument %u does not decay (tail %d)\n", i, tail);
            failures++;
        }

        for (j = 0; j < frames; j++) {
            int16_t value = (int16_t)poppy_q31_to_q15(mono[j]);

            stereo[2u * j] = value;
            stereo[2u * j + 1u] = value;
        }

        if (snprintf(path, sizeof(path), "%s/instrument_%02u.wav", dir, i) >= (int)sizeof(path)) {
            printf("FAIL: path too long\n");
            failures++;
            continue;
        }
        if (wav_write_stereo16(path, stereo, frames, sample_rate) != 0) {
            printf("FAIL: could not write %s\n", path);
            failures++;
        }
    }

    if (failures == 0) {
        ok("all 15 instruments render a non-silent, decaying hit");
    }

    free(mono);
    free(stereo);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static double correlation(const int32_t *a, const int32_t *b, size_t count)
{
    double mean_a = 0.0;
    double mean_b = 0.0;
    double cov = 0.0;
    double var_a = 0.0;
    double var_b = 0.0;
    size_t i;

    for (i = 0; i < count; i++) {
        mean_a += (double)a[i];
        mean_b += (double)b[i];
    }
    mean_a /= (double)count;
    mean_b /= (double)count;

    for (i = 0; i < count; i++) {
        double da = (double)a[i] - mean_a;
        double db = (double)b[i] - mean_b;

        cov += da * db;
        var_a += da * da;
        var_b += db * db;
    }

    if ((var_a <= 0.0) || (var_b <= 0.0)) {
        return 1.0;
    }
    return cov / sqrt(var_a * var_b);
}

static int command_noise(void)
{
    static const mp7_instr noise[6] = {
        MP7_INSTR_SNARE_1, MP7_INSTR_SNARE_2, MP7_INSTR_HIHAT,
        MP7_INSTR_CYMBAL_1, MP7_INSTR_CYMBAL_2, MP7_INSTR_TAMBOURINE
    };
    mp7_context *ctx = context_new();
    const size_t frames = 8192;
    int32_t *samples = (int32_t *)malloc(6u * frames * sizeof(int32_t));
    unsigned int i;
    unsigned int j;
    int worst = 1000;

    if (samples == 0) {
        printf("FAIL: out of memory\n");
        free(ctx);
        return 1;
    }

    for (i = 0; i < 6u; i++) {
        int peak;

        render_voice_i32(ctx, noise[i], samples + i * frames, frames, frames);
        peak = peak_i32(samples + i * frames, frames);
        if (peak < 100) {
            printf("FAIL: noise voice %u is silent\n", i);
            failures++;
        }
    }

    for (i = 0; i < 6u; i++) {
        for (j = i + 1u; j < 6u; j++) {
            double c = correlation(samples + i * frames, samples + j * frames, frames);
            double absolute = (c < 0.0) ? -c : c;

            if (absolute > 0.2) {
                printf("FAIL: noise voices %u and %u correlate at %.3f\n",
                       i, j, c);
                failures++;
            }
            if ((int)(absolute * 1000.0) < worst) {
                worst = (int)(absolute * 1000.0);
            }
        }
    }

    /* The Hihat has no Highpass control; its high-pass is fixed at 1000 Hz. */
    {
        struct poppy_biquad expected;
        const struct poppy_biquad *actual = &ctx->voices[MP7_INSTR_HIHAT].highpass;

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_biquad_set_highpass(&expected, 1000u, MP7_DEFAULT_SAMPLE_RATE);
        if ((actual->b0 != expected.b0) || (actual->b1 != expected.b1) ||
            (actual->b2 != expected.b2) || (actual->a1 != expected.a1) ||
            (actual->a2 != expected.a2)) {
            fail("Hihat high-pass is not fixed at 1000 Hz");
        } else {
            ok("Hihat high-pass is fixed at 1000 Hz");
        }
    }

    if (failures == 0) {
        printf("ok  : the 6 noise voices are uncorrelated (worst |r| = %.3f)\n",
               (double)worst / 1000.0);
    }

    free(samples);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_guiro(void)
{
    mp7_context *ctx = context_new();
    const uint32_t sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    const size_t frames = (size_t)(sample_rate / 2u);
    const size_t late = (size_t)(sample_rate / 10u);
    int32_t *held = (int32_t *)malloc(frames * sizeof(int32_t));
    int32_t *released = (int32_t *)malloc(frames * sizeof(int32_t));
    double held_rms;
    double released_rms;

    if ((held == 0) || (released == 0)) {
        printf("FAIL: out of memory\n");
        free(held);
        free(released);
        free(ctx);
        return 1;
    }

    render_voice_i32(ctx, MP7_INSTR_GUIRO, held, frames, frames);
    render_voice_i32(ctx, MP7_INSTR_GUIRO, released, frames, (size_t)(sample_rate / 10u));

    held_rms = rms_i32(held + (frames - late), late);
    released_rms = rms_i32(released + (frames - late), late);

    if (held_rms < 100.0) {
        printf("FAIL: held Guiro stopped early (RMS %.1f)\n", held_rms);
        failures++;
    }
    if (released_rms > 20.0) {
        printf("FAIL: Guiro note-off did not release (RMS %.1f)\n", released_rms);
        failures++;
    }
    if (failures == 0) {
        ok("Guiro holds while gated and releases on note-off");
    }

    free(held);
    free(released);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int pattern_enabled_count(unsigned int rhythm)
{
    const struct mp7_factory_rhythm *pattern = &mp7_factory_rhythms[rhythm];
    int count = 0;
    unsigned int column;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        if (pattern->enable[column] != 0u) {
            count++;
        }
    }
    return count;
}

static uint64_t pattern_enable_mask(unsigned int rhythm)
{
    const struct mp7_factory_rhythm *pattern = &mp7_factory_rhythms[rhythm];
    uint64_t mask = 0;
    unsigned int column;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        if (pattern->enable[column] != 0u) {
            mask |= (uint64_t)1u << column;
        }
    }
    return mask;
}

static int command_pattern(void)
{
    static const unsigned int short_loop[5] = { 0u, 8u, 10u, 15u, 18u };
    static const unsigned int triplet_rhythms[3] = { 8u, 15u, 18u };
    mp7_context *ctx = context_new();
    const uint32_t sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    unsigned int rhythm;
    unsigned int i;

    mp7_init(ctx, sample_rate);

    /* The factory data itself: a column mask only uses the rhythm
     * instrument bits, the five short loops enable 48 columns, everything
     * else enables all 64, and the triplet flags match. */
    for (rhythm = 0u; rhythm < (unsigned int)MP7_RHYTHM_COUNT; rhythm++) {
        const struct mp7_factory_rhythm *pattern = &mp7_factory_rhythms[rhythm];
        int expect_short = 0;
        int expect_triplet = 0;
        unsigned int column;

        for (i = 0u; i < 5u; i++) {
            if (short_loop[i] == rhythm) {
                expect_short = 1;
            }
        }
        for (i = 0u; i < 3u; i++) {
            if (triplet_rhythms[i] == rhythm) {
                expect_triplet = 1;
            }
        }

        if (pattern_enabled_count(rhythm) != (expect_short ? 48 : 64)) {
            printf("FAIL: %s enables %d columns\n", mp7_rhythm_names[rhythm],
                   pattern_enabled_count(rhythm));
            failures++;
        }
        if ((int)pattern->triplet != expect_triplet) {
            printf("FAIL: %s triplet flag is %d\n", mp7_rhythm_names[rhythm],
                   (int)pattern->triplet);
            failures++;
        }

        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if ((pattern->notes[column] & ~MP7_PATTERN_RHYTHM_MASK) != 0u) {
                printf("FAIL: %s column %u uses the percussion bits\n",
                       mp7_rhythm_names[rhythm], column);
                failures++;
                break;
            }
        }
    }

    if (failures == 0) {
        ok("the 20 factory patterns are well formed");
    }

    /* The shared percussion is stored once; combining any rhythm must place
     * it in the three percussion bits of every column. */
    for (rhythm = 0u; rhythm < (unsigned int)MP7_RHYTHM_COUNT; rhythm++) {
        unsigned int column;

        mp7_pattern_combine(ctx, (uint32_t)1u << rhythm);
        for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
            if ((ctx->sequencer.notes[column] & MP7_PATTERN_PERCUSSION_MASK) !=
                mp7_shared_percussion_notes[column]) {
                printf("FAIL: %s did not include the shared percussion\n",
                       mp7_rhythm_names[rhythm]);
                failures++;
                break;
            }
        }
    }

    /* Combination: OR the notes, AND the enable mask, OR the triplet flag. */
    mp7_pattern_combine(ctx, (uint32_t)1u << MP7_RHYTHM_ROCK_1);
    if (ctx->sequencer.enable != ~(uint64_t)0) {
        fail("Rock 1 on its own does not enable all 64 columns");
    }
    if (ctx->sequencer.triplet != 0u) {
        fail("Rock 1 is marked as triplet");
    }
    if ((ctx->sequencer.notes[12] & (1u << MP7_INSTR_BASS)) == 0u) {
        fail("Rock 1 lost its Base hit at column 12");
    }

    mp7_pattern_combine(ctx,
                        ((uint32_t)1u << MP7_RHYTHM_ROCK_1) |
                        ((uint32_t)1u << MP7_RHYTHM_WALTZ));
    if (ctx->sequencer.enable != pattern_enable_mask(MP7_RHYTHM_WALTZ)) {
        fail("Rock 1 + Waltz does not use the Waltz enable mask");
    }
    if (ctx->sequencer.triplet != 0u) {
        fail("Rock 1 + Waltz is marked as triplet");
    }
    if ((ctx->sequencer.notes[24] & (1u << MP7_INSTR_SNARE_1)) == 0u) {
        fail("Rock 1 + Waltz did not OR the Snare 1 hit at column 24");
    }

    mp7_pattern_combine(ctx,
                        ((uint32_t)1u << MP7_RHYTHM_ROCK_1) |
                        ((uint32_t)1u << MP7_RHYTHM_MARCH_6_8));
    if (ctx->sequencer.enable != pattern_enable_mask(MP7_RHYTHM_MARCH_6_8)) {
        fail("Rock 1 + March 6/8 does not use the March enable mask");
    }
    if (ctx->sequencer.triplet != 1u) {
        fail("Rock 1 + March 6/8 did not OR the triplet flag");
    }

    mp7_pattern_combine(ctx, 0u);
    if ((ctx->sequencer.enable != 0u) || (ctx->sequencer.triplet != 0u)) {
        fail("an empty selection is not stopped");
    }

    /* Timing: Rock 1 at 120 BPM has a 919-sample sub-step. The Base row is
     * X...........X...X... so the first hit gaps are 12 and 4 columns. */
    mp7_pattern_combine(ctx, (uint32_t)1u << MP7_RHYTHM_ROCK_1);
    mp7_pattern_set_tempo(ctx, 120u);
    if (ctx->sequencer.sub_step_samples != 919u) {
        printf("FAIL: sub-step is %u samples, expected 919\n",
               ctx->sequencer.sub_step_samples);
        failures++;
    } else {
        ok("the sub-step is round(2.5 * 44100 / 120) = 919 samples");
    }

    {
        const unsigned int step = ctx->sequencer.sub_step_samples;
        size_t trigger_times[16];
        unsigned int trigger_count = 0;
        size_t total = (size_t)MP7_PATTERN_STEPS * 2u * step + 1u;
        size_t t;

        for (t = 0u; t < total; t++) {
            ctx->voices[MP7_INSTR_BASS].active = 0;
            mp7_pattern_tick(ctx);
            if (ctx->voices[MP7_INSTR_BASS].active != 0u) {
                if (trigger_count < 16u) {
                    trigger_times[trigger_count] = t;
                }
                trigger_count++;
            }
        }

        if (trigger_count < 9u) {
            printf("FAIL: Rock 1 Base fired %u times in one loop\n", trigger_count);
            failures++;
        } else {
            static const unsigned int gap_columns[9] = { 12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u };
            unsigned int hit;

            if (trigger_times[0] != 0u) {
                fail("the first Base hit is not at the first sub-step boundary");
            }
            for (hit = 1u; hit < 9u; hit++) {
                size_t expected = (size_t)gap_columns[hit - 1u] * 2u * step;
                size_t got = trigger_times[hit] - trigger_times[hit - 1u];

                if (got != expected) {
                    printf("FAIL: Base gap %u is %lu samples, expected %lu\n",
                           hit, (unsigned long)got, (unsigned long)expected);
                    failures++;
                }
            }
            if (failures == 0) {
                ok("Rock 1 Base hits land on the expected sub-step boundaries");
            }
        }
    }

    /* Guiro gate: the shared row starts with XXXX and then rests, so the
     * gate must stay open across columns 0..3 and close on column 4. */
    mp7_init(ctx, sample_rate);
    mp7_pattern_combine(ctx, (uint32_t)1u << MP7_RHYTHM_ROCK_1);
    mp7_pattern_set_tempo(ctx, 120u);
    {
        const size_t column_samples = 2u * (size_t)ctx->sequencer.sub_step_samples;
        size_t t;

        mp7_pattern_tick(ctx);
        if (ctx->sequencer.guiro_gated != 1u) {
            fail("the sequencer did not gate the Guiro on an X column");
        }
        for (t = 1u; t <= 4u * column_samples; t++) {
            mp7_pattern_tick(ctx);
            if ((t == 3u * column_samples) && (ctx->sequencer.guiro_gated != 1u)) {
                fail("the Guiro gate closed between adjacent X columns");
            }
            if ((t == 4u * column_samples) && (ctx->sequencer.guiro_gated != 0u)) {
                fail("the Guiro gate stayed open on a rest column");
            }
        }
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int render_rhythm(mp7_context *ctx,
                         uint32_t selection,
                         int16_t *stereo,
                         size_t *frames_out)
{
    unsigned int column;
    size_t enabled = 0u;
    size_t steps;
    size_t frames;

    mp7_pattern_combine(ctx, selection);
    mp7_pattern_set_tempo(ctx, 120u);
    steps = (ctx->sequencer.triplet != 0u) ? 3u : 2u;

    for (column = 0u; column < MP7_PATTERN_STEPS; column++) {
        if ((ctx->sequencer.enable & ((uint64_t)1u << column)) != 0u) {
            enabled++;
        }
    }
    frames = enabled * steps * (size_t)ctx->sequencer.sub_step_samples;

    mp7_process(ctx, stereo, frames);
    *frames_out = frames;
    return (int)enabled;
}

static int command_rhythms(const char *dir)
{
    mp7_context *ctx = context_new();
    const uint32_t sample_rate = MP7_DEFAULT_SAMPLE_RATE;
    size_t max_frames = (size_t)(sample_rate * 4u);
    int16_t *stereo = (int16_t *)malloc(max_frames * 2u * sizeof(int16_t));
    unsigned int rhythm;
    unsigned int failures_before = (unsigned int)failures;

    if (stereo == 0) {
        printf("FAIL: out of memory\n");
        free(ctx);
        return 1;
    }

    for (rhythm = 0u; rhythm < (unsigned int)MP7_RHYTHM_COUNT; rhythm++) {
        char path[512];
        size_t frames = 0u;
        size_t i;
        int peak = 0;

        mp7_init(ctx, sample_rate);
        render_rhythm(ctx, (uint32_t)1u << rhythm, stereo, &frames);

        for (i = 0u; i < frames * 2u; i++) {
            int value = (stereo[i] < 0) ? -stereo[i] : stereo[i];

            if (value > peak) {
                peak = value;
            }
        }
        if (peak < 100) {
            printf("FAIL: %s rendered silence\n", mp7_rhythm_names[rhythm]);
            failures++;
        }
        if (snprintf(path, sizeof(path), "%s/rhythm_%02u.wav", dir, rhythm) >=
            (int)sizeof(path)) {
            printf("FAIL: path too long\n");
            failures++;
            continue;
        }
        if (wav_write_stereo16(path, stereo, frames, sample_rate) != 0) {
            printf("FAIL: could not write %s\n", path);
            failures++;
        }
    }

    /* Two documented combinations, for A/B listening. */
    {
        static const uint32_t selections[2] = {
            ((uint32_t)1u << MP7_RHYTHM_ROCK_1) | ((uint32_t)1u << MP7_RHYTHM_WALTZ),
            ((uint32_t)1u << MP7_RHYTHM_ROCK_1) | ((uint32_t)1u << MP7_RHYTHM_MARCH_6_8)
        };
        static const char *const names[2] = {
            "combo_rock1_waltz.wav",
            "combo_rock1_march68.wav"
        };
        unsigned int i;

        for (i = 0u; i < 2u; i++) {
            char path[512];
            size_t frames = 0u;

            mp7_init(ctx, sample_rate);
            render_rhythm(ctx, selections[i], stereo, &frames);
            if (snprintf(path, sizeof(path), "%s/%s", dir, names[i]) >=
                (int)sizeof(path)) {
                printf("FAIL: path too long\n");
                failures++;
                continue;
            }
            if (wav_write_stereo16(path, stereo, frames, sample_rate) != 0) {
                printf("FAIL: could not write %s\n", path);
                failures++;
            }
        }
    }

    if ((unsigned int)failures == failures_before) {
        ok("all 20 rhythms and both combinations render audio");
    }

    free(stereo);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int command_quijada(void)
{
    mp7_context *ctx = context_new();
    struct poppy_osc body_expected;
    struct poppy_osc clock_expected;
    const int32_t low = POPPY_Q30_ONE / 10;
    const int32_t high = (int32_t)(((int64_t)POPPY_Q30_ONE * 6) / 10);
    struct mp7_voice *voice;
    int i;

    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    voice = &ctx->voices[MP7_INSTR_QUIJADA];

    poppy_osc_set_freq(&body_expected, 2700u, MP7_DEFAULT_SAMPLE_RATE);
    poppy_osc_set_freq(&clock_expected, 25u, MP7_DEFAULT_SAMPLE_RATE);
    if (voice->body_osc.inc != body_expected.inc) {
        fail("Quijada body oscillator is not 2700 Hz");
    } else {
        ok("Quijada body oscillator is 2700 Hz");
    }
    if (voice->aux_osc.inc != clock_expected.inc) {
        fail("Quijada comparator clock is not 25 Hz");
    } else {
        ok("Quijada comparator clock is 25 Hz");
    }

    for (i = 0; i < 8; i++) {
        mp7_voice_trigger(voice);
        if ((voice->threshold_lo < low) || (voice->threshold_lo > high) ||
            (voice->threshold_hi < low) || (voice->threshold_hi > high)) {
            fail("Quijada threshold is outside [0.1, 0.6]");
            break;
        }
    }
    if (i == 8) {
        ok("both Quijada thresholds are re-randomized in [0.1, 0.6]");
    }

    /* Comparator mapping: a crossing of the low threshold must fire the
     * short envelope, and a crossing of the high threshold the long one. */
    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    voice = &ctx->voices[MP7_INSTR_QUIJADA];
    voice->prev_clock = -POPPY_Q30_ONE;
    voice->aux_osc.phase = (uint32_t)1024u << 16; /* sine peak */
    voice->threshold_lo = POPPY_Q30_ONE / 5;      /* 0.2 */
    voice->threshold_hi = INT32_MAX;              /* above the clock peak */
    (void)mp7_engine_process(voice);
    if ((voice->short_env.stage != POPPY_ENV_ATTACK) ||
        (voice->long_env.stage != POPPY_ENV_IDLE)) {
        fail("low Quijada threshold does not fire the short envelope");
    } else {
        ok("low Quijada threshold fires the short envelope");
    }

    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    voice = &ctx->voices[MP7_INSTR_QUIJADA];
    voice->prev_clock = -POPPY_Q30_ONE;
    voice->aux_osc.phase = (uint32_t)1024u << 16; /* sine peak */
    voice->threshold_lo = INT32_MAX;              /* above the clock peak */
    voice->threshold_hi = (int32_t)(((int64_t)POPPY_Q30_ONE * 2) / 5);  /* 0.4 */
    (void)mp7_engine_process(voice);
    if ((voice->long_env.stage != POPPY_ENV_ATTACK) ||
        (voice->short_env.stage != POPPY_ENV_IDLE)) {
        fail("high Quijada threshold does not fire the long envelope");
    } else {
        ok("high Quijada threshold fires the long envelope");
    }

    free(ctx);
    return failures == 0 ? 0 : 1;
}

static int channel_peak(const int16_t *stereo, size_t frames, unsigned int channel)
{
    int peak = 0;
    size_t i;

    for (i = 0u; i < frames; i++) {
        int value = (int)stereo[2u * i + channel];

        if (value < 0) {
            value = -value;
        }
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

static int stereo_is_silent(const int16_t *stereo, size_t frames)
{
    size_t i;

    for (i = 0u; i < frames * 2u; i++) {
        if (stereo[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void render_trigger(mp7_context *ctx, mp7_instr instr,
                           int16_t *stereo, size_t frames)
{
    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[instr]);
    mp7_process(ctx, stereo, frames);
}

static int command_mixer(void)
{
    mp7_context *ctx = context_new();
    const size_t frames = 4096u;
    int16_t *stereo = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    int16_t *other = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    unsigned int i;

    if ((stereo == 0) || (other == 0)) {
        printf("FAIL: out of memory\n");
        free(stereo);
        free(other);
        free(ctx);
        return 1;
    }

    /* Defaults: Volume 1.0 (master gain 0.5), Balance centre, each direct
     * gain at its 0.5 default. */
    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    if ((ctx->mixer.panel_volume_q16 != POPPY_Q16_ONE) ||
        (ctx->mixer.balance_q16 != POPPY_Q16_ONE / 2)) {
        fail("the mixer defaults are not Volume 1.0 / Balance 0.5");
    }
    for (i = 0u; i < POPPY_MIXER_DIRECT_COUNT; i++) {
        if (ctx->mixer.direct_gain_q16[i] != POPPY_Q16_ONE / 2) {
            fail("a direct gain does not default to 0.5");
            break;
        }
    }
    for (i = 0u; i < (unsigned int)MP7_INSTR_COUNT; i++) {
        if ((ctx->voices[i].level_q16 != POPPY_Q16_ONE) ||
            (ctx->voices[i].pan_q16 != POPPY_Q16_ONE / 2)) {
            fail("a voice does not default to Level 1.0 / Pan 0.5");
            break;
        }
    }
    if (failures == 0) {
        ok("the mixer defaults to Volume 1.0, Balance 0.5 and direct gain 0.5");
    }

    /* A centred voice appears equally in both channels, and a hard pan sends
     * it entirely to one side. */
    render_trigger(ctx, MP7_INSTR_BASS, stereo, frames);
    if ((channel_peak(stereo, frames, 0u) == 0) ||
        (channel_peak(stereo, frames, 0u) != channel_peak(stereo, frames, 1u))) {
        fail("a centred voice is not equal in both channels");
    } else {
        ok("a centred voice is equal in both channels");
    }

    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
    ctx->voices[MP7_INSTR_BASS].pan_q16 = 0;
    mp7_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 0u) == 0) ||
        (channel_peak(stereo, frames, 1u) != 0)) {
        fail("pan 0 is not hard left");
    } else {
        ok("pan 0 sends a voice hard left");
    }

    /* The centre pan splits the voice 0.5 / 0.5, so a hard-left voice is
     * exactly twice as loud in the left channel as a centred one. Together
     * with the default master gain of 0.5 that puts a default hit at 0.25
     * per channel (-12.04 dBFS) at the anchor of +/-1.0 for a Level 1.0
     * voice. */
    {
        int centred;
        int hard_left;

        render_trigger(ctx, MP7_INSTR_BASS, stereo, frames);
        centred = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
        ctx->voices[MP7_INSTR_BASS].pan_q16 = 0;
        mp7_process(ctx, stereo, frames);
        hard_left = channel_peak(stereo, frames, 0u);

        if ((centred == 0) ||
            (hard_left < (2 * centred) - 1) ||
            (hard_left > (2 * centred) + 1)) {
            printf("FAIL: the centre pan does not split the voice in half (peaks %d -> %d)\n",
                   centred, hard_left);
            failures++;
        } else {
            ok("the centre pan splits the voice 0.5 / 0.5");
        }
    }

    mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
    mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
    ctx->voices[MP7_INSTR_BASS].pan_q16 = POPPY_Q16_ONE;
    mp7_process(ctx, stereo, frames);
    if ((channel_peak(stereo, frames, 1u) == 0) ||
        (channel_peak(stereo, frames, 0u) != 0)) {
        fail("pan 1.0 is not hard right");
    } else {
        ok("pan 1.0 sends a voice hard right");
    }

    /* The master Volume is applied as 0.5 * value: 1.0 is half scale, 2.0 is
     * unity, and 0 is silence. */
    if (poppy_mixer_to_q15(INT32_MAX, POPPY_Q16_ONE) != 16384) {
        fail("Volume 1.0 is not half the panel value as a master gain");
    } else {
        ok("Volume 1.0 applies the 0.5x master gain");
    }
    if ((poppy_mixer_to_q15(INT32_MAX, 2 * POPPY_Q16_ONE) != 32767) ||
        (poppy_mixer_to_q15(INT32_MIN, 2 * POPPY_Q16_ONE) != -32768)) {
        fail("the master gain does not saturate at full scale");
    } else {
        ok("the master gain saturates once, at the final Q15 conversion");
    }

    render_trigger(ctx, MP7_INSTR_BASS, stereo, frames);
    {
        int base_peak = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_volume(&ctx->mixer, 2 * POPPY_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
        mp7_process(ctx, stereo, frames);
        {
            int doubled = channel_peak(stereo, frames, 0u);
            int expected = 2 * base_peak;

            if ((doubled < expected - 2) || (doubled > expected + 2)) {
                printf("FAIL: Volume 2.0 gave %d, expected about %d\n",
                       doubled, expected);
                failures++;
            } else {
                ok("Volume 2.0 gives unity master gain (double the default)");
            }
        }

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_volume(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
        mp7_process(ctx, stereo, frames);
        if (!stereo_is_silent(stereo, frames)) {
            fail("Volume 0 is not silent");
        } else {
            ok("Volume 0 mutes the mix");
        }
    }

    /* Balance is a Base-versus-cymbals crossfade. Snare 1 belongs to neither
     * group and must not move. */
    render_trigger(ctx, MP7_INSTR_BASS, stereo, frames);
    {
        int bass_bottom;
        int bass_top;

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_balance(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
        mp7_process(ctx, stereo, frames);
        bass_bottom = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_balance(&ctx->mixer, POPPY_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
        mp7_process(ctx, stereo, frames);
        bass_top = channel_peak(stereo, frames, 0u);

        /* Balance takes the Base from gain 1.0 down to 0.2, a factor of 5,
         * and never mutes it completely. */
        {
            int expected = 5 * bass_top;
            int tolerance = expected / 10 + 2;

            if ((bass_top == 0) ||
                (bass_bottom < expected - tolerance) ||
                (bass_bottom > expected + tolerance)) {
                printf("FAIL: Balance does not attenuate Base to 0.2 (peaks %d -> %d)\n",
                       bass_bottom, bass_top);
                failures++;
            } else {
                ok("Balance attenuates Base to about 0.2 at the right");
            }
        }
    }

    render_trigger(ctx, MP7_INSTR_HIHAT, stereo, frames);
    {
        int hihat_left;
        int hihat_right;

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_balance(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_HIHAT]);
        mp7_process(ctx, stereo, frames);
        hihat_left = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_balance(&ctx->mixer, POPPY_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_HIHAT]);
        mp7_process(ctx, stereo, frames);
        hihat_right = channel_peak(stereo, frames, 0u);

        /* The cymbal group goes from 0.2 at the left to 1.0 at the right, the
         * same factor of 5, and is never muted completely. */
        {
            int expected = 5 * hihat_left;
            int tolerance = expected / 10 + 2;

            if ((hihat_left == 0) ||
                (hihat_right < expected - tolerance) ||
                (hihat_right > expected + tolerance)) {
                printf("FAIL: Balance does not attenuate the cymbal group to 0.2 (peaks %d -> %d)\n",
                       hihat_left, hihat_right);
                failures++;
            } else {
                ok("Balance attenuates Hihat/Cymbal 1/Cymbal 2 to about 0.2 at the left");
            }
        }
    }

    render_trigger(ctx, MP7_INSTR_SNARE_1, stereo, frames);
    {
        int snare_bottom;
        int snare_top;

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_balance(&ctx->mixer, 0);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_SNARE_1]);
        mp7_process(ctx, stereo, frames);
        snare_bottom = channel_peak(stereo, frames, 0u);

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_balance(&ctx->mixer, POPPY_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_SNARE_1]);
        mp7_process(ctx, stereo, frames);
        snare_top = channel_peak(stereo, frames, 0u);

        if (snare_bottom != snare_top) {
            printf("FAIL: Balance moved Snare 1 (%d -> %d)\n",
                   snare_bottom, snare_top);
            failures++;
        } else {
            ok("Balance leaves Snare 1 untouched");
        }
    }

    /* The three direct-percussion gains multiply their own voice only. */
    render_trigger(ctx, MP7_INSTR_TAMBOURINE, stereo, frames);
    {
        int tamb_default = channel_peak(stereo, frames, 0u);
        int tamb_full;

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_direct_gain(&ctx->mixer, MP7_DIRECT_TAMBOURINE,
                                    POPPY_Q16_ONE);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_TAMBOURINE]);
        mp7_process(ctx, stereo, frames);
        tamb_full = channel_peak(stereo, frames, 0u);

        if ((tamb_default == 0) || (tamb_full < tamb_default + tamb_default / 2)) {
            printf("FAIL: the Tambourine direct gain does not scale its voice\n");
            failures++;
        } else {
            ok("the Tambourine direct gain scales its voice");
        }

        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        poppy_mixer_set_direct_gain(&ctx->mixer, MP7_DIRECT_TAMBOURINE, 0);
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_TAMBOURINE]);
        mp7_process(ctx, stereo, frames);
        if (!stereo_is_silent(stereo, frames)) {
            fail("a zero direct gain does not silence its voice");
        } else {
            ok("a zero direct gain silences its voice");
        }
    }

    {
        static const mp7_instr direct_voices[3] = {
            MP7_INSTR_TAMBOURINE, MP7_INSTR_GUIRO, MP7_INSTR_QUIJADA
        };
        static const unsigned int direct_index[3] = {
            MP7_DIRECT_TAMBOURINE, MP7_DIRECT_GUIRO, MP7_DIRECT_QUIJADA
        };
        unsigned int v;

        for (v = 0u; v < 3u; v++) {
            mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
            poppy_mixer_set_direct_gain(&ctx->mixer, direct_index[v], 0);
            mp7_voice_trigger(&ctx->voices[direct_voices[v]]);
            mp7_process(ctx, stereo, frames);
            if (!stereo_is_silent(stereo, frames)) {
                printf("FAIL: direct gain %u did not silence its voice\n", v);
                failures++;
            }
        }

        /* A non-direct voice must not be affected by the direct gains. */
        render_trigger(ctx, MP7_INSTR_BASS, stereo, frames);
        mp7_init(ctx, MP7_DEFAULT_SAMPLE_RATE);
        for (v = 0u; v < 3u; v++) {
            poppy_mixer_set_direct_gain(&ctx->mixer, direct_index[v], 0);
        }
        mp7_voice_trigger(&ctx->voices[MP7_INSTR_BASS]);
        mp7_process(ctx, other, frames);
        if (memcmp(stereo, other, frames * 2u * sizeof(int16_t)) != 0) {
            fail("the direct gains changed a non-direct voice");
        } else {
            ok("the direct gains affect only Tambourine, Guiro and Quijada");
        }
    }

    free(stereo);
    free(other);
    free(ctx);
    return failures == 0 ? 0 : 1;
}

static void usage(const char *program)
{
    printf("usage: %s <command>\n\n", program);
    printf("  --init             report the context size, the default sample rate\n"
           "                     and the sine table footprint\n"
           "  --sine             verify the shared sine table, index by index\n"
           "  --gain             verify the Q15 gain multiply, including >unity\n"
           "  --silence [path]   render silence, check it and write a WAV file\n"
           "                     (default path: silence.wav)\n"
           "  --instruments [dir] render all 15 instruments to WAV files\n"
           "                     (default dir: build)\n"
           "  --noise            verify the 6 noise voices are uncorrelated\n"
           "  --guiro            verify Guiro gate hold and note-off release\n"
           "  --mixer            verify pan, Volume, Balance and direct gains\n"
           "  --pattern          verify the factory patterns, combination and timing\n"
           "  --rhythms [dir]    render all 20 rhythms and two combinations to WAV\n"
           "                     files (default dir: build)\n"
           "  --help             this text\n");
}

int main(int argc, char **argv)
{
    const char *command;
    const char *path = "silence.wav";

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    command = argv[1];

    if (strcmp(command, "--init") == 0) {
        return command_init();
    }
    if (strcmp(command, "--sine") == 0) {
        return command_sine();
    }
    if (strcmp(command, "--gain") == 0) {
        return command_gain();
    }
    if (strcmp(command, "--silence") == 0) {
        if (argc > 2) {
            path = argv[2];
        }
        return command_silence(path);
    }
    if (strcmp(command, "--instruments") == 0) {
        const char *dir = (argc > 2) ? argv[2] : "build";

        return command_instruments(dir);
    }
    if (strcmp(command, "--noise") == 0) {
        return command_noise();
    }
    if (strcmp(command, "--guiro") == 0) {
        return command_guiro();
    }
    if (strcmp(command, "--quijada") == 0) {
        return command_quijada();
    }
    if (strcmp(command, "--mixer") == 0) {
        return command_mixer();
    }
    if (strcmp(command, "--pattern") == 0) {
        return command_pattern();
    }
    if (strcmp(command, "--rhythms") == 0) {
        const char *dir = (argc > 2) ? argv[2] : "build";

        return command_rhythms(dir);
    }
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    printf("unknown command: %s\n\n", command);
    usage(argv[0]);
    return 1;
}
