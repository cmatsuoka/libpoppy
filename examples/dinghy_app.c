/*
 * dinghy_app.c - libpoppy on the M-VAVE FM-1, as a Dinghy firmware.
 *
 * This is a drop-in replacement for Dinghy's firmware/src/dinghy.c:
 *
 *   https://github.com/zvenson/dinghy
 *
 * It is not part of libpoppy and the library's Makefile does not build it.
 * Copy it into a Dinghy checkout, together with libpoppy.a and mp7.h:
 *
 *   cp examples/dinghy_app.c        dinghy/firmware/src/dinghy.c
 *   mkdir -p                        dinghy/firmware/lib
 *   cp libpoppy.a mp7.h             dinghy/firmware/lib/
 *
 * Then tell Dinghy's tools/build.py about the archive. In build_app():
 *
 *   flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen",
 *            "-Ifirmware/lib", f'-DFELUCCA_ID="{PRODUCT}"', ...]
 *   ...
 *   tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", FW / "app.ld",
 *      OUT / "crt0.o", OUT / "fm1_vec.o", OUT / "fm1_isr.o", OUT / "dinghy.o",
 *      FW / "lib" / "libpoppy.a", "-o", elf)
 *
 * For host mode, build libpoppy.a on the host and add firmware/lib/libpoppy.a
 * to the cc line in tests/run_host.sh. For the FM-1, build it with the same
 * JieLi toolchain (a host archive does not link into the firmware). With the
 * toolchain in $JIELI:
 *
 *   make CC="$JIELI/pi32v2/bin/clang -target pi32v2 \
 *             -isystem $JIELI/pi32v2/include" \
 *        AR=$JIELI/common/bin/llvm-ar RANLIB=:
 *
 * The -isystem path is the toolchain's newlib headers; libpoppy includes
 * <stdlib.h> and <string.h>, which clang does not search for by itself.
 *
 * A real firmware must also pick its own FELUCCA_ID in tools/build.py; the
 * example leaves Dinghy's default so it is not mistaken for a release.
 *
 * What this file adds around libpoppy:
 *
 *   1. app_audio() / app_main(), Dinghy's two entry points. The audio ISR is
 *      given 256 stereo frames at 44.1 kHz, not the 128-frame half buffer of
 *      the older FM-1 example. The library renders int16_t Q15 samples; the
 *      Dinghy buffer is int32_t, so the block is widened by adding it.
 *
 *   2. A .pool bump allocator. Dinghy is freestanding (libc.c has no malloc)
 *      while mp7_create_context() and the shared sine table allocate. The
 *      heap is zeroed at boot, the context is created once, and the firmware
 *      never returns from app_main(), so free() is a no-op. 16 KiB holds the
 *      8 KiB sine table and the ~4 KiB context.
 *
 *   3. Compiler helpers for 64-bit division. The JieLi toolchain links no
 *      libgcc, and libpoppy's setup path divides 64-bit values. __divdi3(),
 *      __udivdi3(), __moddi3() and __umoddi3() are only reached while the
 *      context is created or a control changes, never in the audio ISR.
 *
 *   4. IRQ masking. libpoppy is not thread-safe: mp7_process() must not run
 *      while the context is created or a setter runs. Dinghy starts the audio
 *      interrupt before app_main(), so the context is created and every
 *      panel change is applied with interrupts off. The setters are short;
 *      do not call them every pass of the main loop.
 *
 * The panel mapping is a convenience layer, not the Mini Pops panel:
 *
 *   OCT- / OCT+     bank A / bank B (the lever)
 *   FX ENV LFO EDIT GLO HOME SAVE ARP SEQ REC
 *                   the ten rhythm buttons; several may be latched at once
 *   SEL             clear the rhythm buttons (percussion layer only)
 *   PLAY            Start/Stop
 *   keys 1..15      trigger the fifteen instruments (GUIRO is gated)
 *   KNOB1           tempo, 2 BPM a detent (1..240)
 *   KNOB2           Balance (0..1)
 *   KNOB3           Tambourine gain
 *   KNOB4           Guiro gain
 *   SELECT          Quijada gain
 *   MASTER          volume (Dinghy's platform applies it after app_audio())
 *
 * USB-MIDI in uses the library's fixed note map; the app does not send MIDI.
 * Settings are saved when the transport is stopped, no key is held, nothing
 * has changed for 2 s and no trigger has sounded for 4 s; a flash erase stalls
 * the audio, so there is nothing to save while playing.
 */

#define APP_NAME "MINIPOPS"

#include "platform.c"
#include "mp7.h"

/* ---- a heap for libpoppy (Dinghy has none) */

#ifndef DINGHY_HOST
static uint8_t app_heap[16u * 1024u]
    __attribute__((section(".pool"), aligned(8)));
static size_t app_heap_used;

void *malloc(size_t size)
{
    size_t at = (app_heap_used + 7u) & ~(size_t)7u;

    if ((at > sizeof app_heap) || (size > sizeof app_heap - at)) {
        return 0;
    }
    app_heap_used = at + size;
    return &app_heap[at];
}

void free(void *p)
{
    (void)p;
}
#endif

/* ---- the compiler's 64-bit division helpers (no libgcc on the FM-1) */

#ifndef DINGHY_HOST
static uint64_t app_divmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    int32_t i;

    if (d == 0u) {
        if (rem != 0) {
            *rem = 0;
        }
        return 0;
    }

    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1u << i;
        }
    }

    if (rem != 0) {
        *rem = r;
    }
    return q;
}

static uint64_t app_abs64(int64_t v)
{
    return (v < 0) ? (~(uint64_t)v + 1u) : (uint64_t)v;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    return app_divmod64(a, b, 0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    uint64_t r;

    (void)app_divmod64(a, b, &r);
    return r;
}

int64_t __divdi3(int64_t a, int64_t b)
{
    uint64_t q = app_divmod64(app_abs64(a), app_abs64(b), 0);

    return ((a < 0) != (b < 0)) ? (int64_t)(~q + 1u) : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t r;

    (void)app_divmod64(app_abs64(a), app_abs64(b), &r);
    return (a < 0) ? (int64_t)(~r + 1u) : (int64_t)r;
}
#endif

/* ---- state shared with the audio ISR */

static mp7_context app_ctx;
static int16_t app_mix[HALF_FRAMES * 2u];

static uint32_t irq_lock(void)
{
#ifdef DINGHY_HOST
    return 0;
#else
    return irq_save();
#endif
}

static void irq_unlock(uint32_t flags)
{
#ifdef DINGHY_HOST
    (void)flags;
#else
    irq_restore(flags);
#endif
}

/* Dinghy gives app_audio() int32_t frames (Q15 in an int32_t); libpoppy writes
 * int16_t. `out` is already zero, so adding is the same as assigning, but
 * leaves room for another layer if the firmware grows one. */
static void app_audio(int32_t *out, uint32_t frames)
{
    mp7_context ctx = app_ctx;

    if (ctx == 0) {
        return;                         /* the buffer is already silence */
    }

    while (frames != 0u) {
        uint32_t n = (frames < HALF_FRAMES) ? frames : HALF_FRAMES;
        uint32_t i;

        mp7_process(ctx, app_mix, n);
        for (i = 0u; i < 2u * n; i++) {
            out[i] += app_mix[i];
        }
        out += 2u * n;
        frames -= n;
    }
}

/* ---- the panel state (main loop only) */

#define APP_ACCENT RGB(180, 140, 255)
#define APP_OFF RGB(90, 90, 90)

#define APP_SETTINGS_MAGIC 0x4D503731u   /* "MP71" */
#define APP_Q16_ONE 65536
#define APP_GAIN_STEP (APP_Q16_ONE / 32)
#define APP_BALANCE_STEP (APP_Q16_ONE / 32)

/* No public "is anything still sounding" query, so be conservative: a flash
 * erase stalls the DAC, and the longest one-shot tails run for seconds. */
#define APP_SILENCE_MS 4000u

struct app_settings {
    uint32_t magic;
    uint16_t tempo;
    uint16_t buttons;
    int32_t balance;
    int32_t gain_tambourine;
    int32_t gain_guiro;
    int32_t gain_quijada;
    uint8_t bank;
};

static uint16_t app_tempo = 120u;
static int32_t app_balance = APP_Q16_ONE / 2;
static int32_t app_gain_tambourine = APP_Q16_ONE / 2;
static int32_t app_gain_guiro = APP_Q16_ONE / 2;
static int32_t app_gain_quijada = APP_Q16_ONE / 2;
static mp7_bank app_bank = MP7_BANK_A;
static uint16_t app_buttons = 0u;
static int app_running = 0;
static int app_key_guiro;
static int app_midi_guiro;

static int app_dirty;
static uint32_t app_touched_at;
static uint32_t app_last_trigger_at;

static void app_touch(void)
{
    app_dirty = 1;
    app_touched_at = fm1_ms;
}

/* The ten rhythm buttons, in button-bit order. */
static const uint8_t APP_RHYTHM_BTN[10] = {
    BTN_FX, BTN_ENV, BTN_LFO, BTN_EDIT, BTN_GLO,
    BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_REC
};

static const char *const APP_RHYTHM_NAME[20] = {
    "WALTZ", "SAMBA", "MAMBO", "RUMBA", "TANGO",
    "BOSSA NOVA", "ROCK 1", "ROCK 3", "MARCH 2/4", "SWING",
    "JAZZ WALTZ", "MERINGUE", "CHA-CHA", "BEGUINE", "HABANERA",
    "SLOW ROCK", "ROCK 2", "ROCK 4", "MARCH 6/8", "LATIN SWING"
};

static const char *app_selection_name(void)
{
    uint32_t i, count = 0, first = 0;

    for (i = 0u; i < 10u; i++) {
        if ((app_buttons & (1u << i)) != 0u) {
            if (count == 0u) {
                first = i;
            }
            count++;
        }
    }

    if (count == 0u) {
        return "PERCUSSION";
    }
    if (count > 1u) {
        return "COMBINATION";
    }
    return APP_RHYTHM_NAME[(app_bank == MP7_BANK_B ? 10u : 0u) + first];
}

static void app_utoa(char *out, uint32_t value)
{
    char tmp[10];
    uint32_t n = 0;

    do {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);

    while (n != 0u) {
        *out++ = tmp[--n];
    }
    *out = '\0';
}

static void app_draw(void)
{
    char line[16];

    draw_text_box(0, 58, 100, &FONT_S,
                  app_bank == MP7_BANK_B ? "BANK B" : "BANK A",
                  APP_OFF, 0);
    draw_text_box(100, 58, 140, &FONT_S, app_selection_name(), C_WHITE, 2);

    line[0] = 'T'; line[1] = 'E'; line[2] = 'M'; line[3] = 'P';
    line[4] = 'O'; line[5] = ' ';
    app_utoa(&line[6], app_tempo);
    draw_text_box(0, 86, 240, &FONT_S, line, C_WHITE, 1);

    draw_text_box(0, 110, 240, &FONT_S,
                  (app_ctx == 0) ? "NO MEMORY" : (app_running ? "PLAY" : "STOP"),
                  (app_ctx == 0) ? APP_OFF : (app_running ? APP_ACCENT : APP_OFF), 1);
}

/* ---- applying controls (never while mp7_process() runs) */

static void app_set_tempo(uint16_t bpm)
{
    uint32_t flags;

    if (app_ctx == 0) {
        return;
    }
    flags = irq_lock();
    mp7_set_tempo(app_ctx, bpm);
    irq_unlock(flags);
}

static void app_set_balance(int32_t balance_q16)
{
    uint32_t flags;

    if (app_ctx == 0) {
        return;
    }
    flags = irq_lock();
    mp7_set_balance(app_ctx, balance_q16);
    irq_unlock(flags);
}

static void app_set_gain(mp7_instr instr, int32_t gain_q16)
{
    uint32_t flags;

    if (app_ctx == 0) {
        return;
    }
    flags = irq_lock();
    mp7_set_instr_gain(app_ctx, instr, gain_q16);
    irq_unlock(flags);
}

static void app_apply_rhythm(void)
{
    uint32_t flags;

    if (app_ctx == 0) {
        return;
    }
    flags = irq_lock();
    mp7_select_rhythm(app_ctx, app_bank, app_buttons);
    if (!app_running) {
        mp7_stop_rhythms(app_ctx);      /* selecting must not start it */
    }
    irq_unlock(flags);
}

static void app_set_running(int running)
{
    uint32_t flags;

    app_running = running ? 1 : 0;
    if (app_ctx == 0) {
        return;
    }
    flags = irq_lock();
    if (app_running) {
        mp7_select_rhythm(app_ctx, app_bank, app_buttons);
    } else {
        mp7_stop_rhythms(app_ctx);
        /* The last pattern hit can still be ringing; keep the flash quiet
         * for a while after the transport stops. */
        app_last_trigger_at = fm1_ms;
    }
    irq_unlock(flags);
}

static void app_trigger(mp7_instr instr, uint8_t velocity)
{
    uint32_t flags;

    if (app_ctx == 0) {
        return;
    }
    flags = irq_lock();
    mp7_trigger(app_ctx, instr, velocity);
    irq_unlock(flags);
    app_last_trigger_at = fm1_ms;
}

/* ---- settings (one sector, saved when the transport is stopped) */

static int32_t app_clamp(int32_t value, int32_t lo, int32_t hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static void app_load_settings(void)
{
    struct app_settings s;

    if ((settings_load(&s, sizeof s) != 0) ||
        (s.magic != APP_SETTINGS_MAGIC)) {
        return;
    }

    if ((s.tempo >= 1u) && (s.tempo <= 240u)) {
        app_tempo = s.tempo;
    }
    app_balance = app_clamp(s.balance, 0, APP_Q16_ONE);
    app_gain_tambourine = app_clamp(s.gain_tambourine, 0, APP_Q16_ONE);
    app_gain_guiro = app_clamp(s.gain_guiro, 0, APP_Q16_ONE);
    app_gain_quijada = app_clamp(s.gain_quijada, 0, APP_Q16_ONE);
    app_buttons = (uint16_t)(s.buttons & 0x03FFu);
    app_bank = (s.bank == (uint8_t)MP7_BANK_B) ? MP7_BANK_B : MP7_BANK_A;
}

static void app_save_settings(void)
{
    struct app_settings s;

    s.magic = APP_SETTINGS_MAGIC;
    s.tempo = app_tempo;
    s.buttons = app_buttons;
    s.balance = app_balance;
    s.gain_tambourine = app_gain_tambourine;
    s.gain_guiro = app_gain_guiro;
    s.gain_quijada = app_gain_quijada;
    s.bank = (uint8_t)app_bank;

    /* Retrying a failed erase on every main-loop pass would stall the audio
     * and wear the sector; a failure here is not going to fix itself. */
    (void)settings_save(&s, sizeof s);
    app_dirty = 0;
}

/* ---- MIDI note map (the library's fixed factory map) */

static int app_midi_instr(uint8_t note)
{
    static const uint8_t NOTES[15] = {
        36, 40, 38, 60, 61, 64, 56, 75, 42, 37, 49, 57, 54, 74, 79
    };
    uint32_t i;

    for (i = 0u; i < 15u; i++) {
        if (NOTES[i] == note) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- main loop */

static void app_main(void)
{
    uint32_t prev_keys = 0u;
    uint32_t last_flash = 0u, flash_until = 0u;
    uint32_t drawn_bank = ~0u, drawn_buttons = ~0u, drawn_tempo = ~0u;
    int drawn_running = -1;
    uint32_t flags;

    app_load_settings();

    /* Dinghy enables the audio interrupt before app_main(). Mask it while the
     * context and the shared sine table are allocated; a boot-time underrun
     * is harmless, a concurrent mp7_process() is not. */
    flags = irq_lock();
    app_ctx = mp7_create_context(FS);
    if (app_ctx != 0) {
        mp7_set_tempo(app_ctx, app_tempo);
        mp7_set_balance(app_ctx, app_balance);
        mp7_set_instr_gain(app_ctx, MP7_INSTR_TAMBOURINE, app_gain_tambourine);
        mp7_set_instr_gain(app_ctx, MP7_INSTR_GUIRO, app_gain_guiro);
        mp7_set_instr_gain(app_ctx, MP7_INSTR_QUIJADA, app_gain_quijada);
        mp7_select_rhythm(app_ctx, app_bank, app_buttons);
        if (!app_running) {
            mp7_stop_rhythms(app_ctx);
        }
    }
    irq_unlock(flags);

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, APP_NAME, APP_ACCENT, 1);
    draw_text_box(0, 126, 240, &FONT_S, "KEYS 1-15 TRIGGER", APP_OFF, 1);

    for (;;) {
        uint32_t keys, down, up, pressed, released, i;
        struct mp7_panel_state panel;
        int32_t steps;

        platform_poll();

        pressed = fm1_input_edges(&released);
        (void)released;

        if ((pressed & (1u << BTN_OCT_DOWN)) != 0u) {
            app_bank = MP7_BANK_A;
            app_touch();
            app_apply_rhythm();
        }
        if ((pressed & (1u << BTN_OCT_UP)) != 0u) {
            app_bank = MP7_BANK_B;
            app_touch();
            app_apply_rhythm();
        }
        for (i = 0u; i < 10u; i++) {
            if ((pressed & (1u << APP_RHYTHM_BTN[i])) != 0u) {
                app_buttons ^= (uint16_t)(1u << i);
                app_touch();
                app_apply_rhythm();
            }
        }
        if ((pressed & (1u << BTN_SEL)) != 0u) {
            app_buttons = 0u;
            app_touch();
            app_apply_rhythm();
        }
        if ((pressed & (1u << BTN_PLAY)) != 0u) {
            app_set_running(!app_running);
        }

        steps = fm1_enc_take(ENC_KNOB1);
        if (steps != 0) {
            int32_t tempo = (int32_t)app_tempo + (steps * 2);

            tempo = app_clamp(tempo, 1, 240);
            if ((uint16_t)tempo != app_tempo) {
                app_tempo = (uint16_t)tempo;
                app_set_tempo(app_tempo);
                app_touch();
            }
        }

        steps = fm1_enc_take(ENC_KNOB2);
        if (steps != 0) {
            int32_t balance = app_clamp(app_balance + (steps * APP_BALANCE_STEP),
                                        0, APP_Q16_ONE);

            if (balance != app_balance) {
                app_balance = balance;
                app_set_balance(app_balance);
                app_touch();
            }
        }

        steps = fm1_enc_take(ENC_KNOB3);
        if (steps != 0) {
            int32_t gain = app_clamp(app_gain_tambourine + (steps * APP_GAIN_STEP),
                                     0, APP_Q16_ONE);

            if (gain != app_gain_tambourine) {
                app_gain_tambourine = gain;
                app_set_gain(MP7_INSTR_TAMBOURINE, app_gain_tambourine);
                app_touch();
            }
        }

        steps = fm1_enc_take(ENC_KNOB4);
        if (steps != 0) {
            int32_t gain = app_clamp(app_gain_guiro + (steps * APP_GAIN_STEP),
                                     0, APP_Q16_ONE);

            if (gain != app_gain_guiro) {
                app_gain_guiro = gain;
                app_set_gain(MP7_INSTR_GUIRO, app_gain_guiro);
                app_touch();
            }
        }

        steps = fm1_enc_take(ENC_SELECT);
        if (steps != 0) {
            int32_t gain = app_clamp(app_gain_quijada + (steps * APP_GAIN_STEP),
                                     0, APP_Q16_ONE);

            if (gain != app_gain_quijada) {
                app_gain_quijada = gain;
                app_set_gain(MP7_INSTR_QUIJADA, app_gain_quijada);
                app_touch();
            }
        }

        keys = fm1_in.notes;
        down = keys & ~prev_keys;
        up = prev_keys & ~keys;
        prev_keys = keys;

        for (i = 0u; i < 15u; i++) {
            if ((down & (1u << i)) != 0u) {
                if (i == (uint32_t)MP7_INSTR_GUIRO) {
                    app_key_guiro = 1;
                }
                app_trigger((mp7_instr)i, 100u);
            }
            if (((up & (1u << i)) != 0u) && (i == (uint32_t)MP7_INSTR_GUIRO)) {
                app_key_guiro = 0;
                if (!app_midi_guiro) {
                    app_trigger(MP7_INSTR_GUIRO, 0u);
                }
            }
        }

        {
            uint8_t status, d1, d2;

            while (midi_get(&status, &d1, &d2)) {
                int instr = app_midi_instr(d1);

                if (instr < 0) {
                    continue;
                }
                if (((status & 0xF0u) == 0x90u) && (d2 != 0u)) {
                    if (instr == (int)MP7_INSTR_GUIRO) {
                        app_midi_guiro = 1;
                    }
                    app_trigger((mp7_instr)instr, d2);
                } else if (((status & 0xF0u) == 0x80u) ||
                           (((status & 0xF0u) == 0x90u) && (d2 == 0u))) {
                    if (instr == (int)MP7_INSTR_GUIRO) {
                        app_midi_guiro = 0;
                        if (!app_key_guiro) {
                            app_trigger(MP7_INSTR_GUIRO, 0u);
                        }
                    }
                }
            }
        }

        flags = irq_lock();
        mp7_get_panel_state(app_ctx, &panel);
        irq_unlock(flags);

        if (panel.flash_count != last_flash) {
            last_flash = panel.flash_count;
            flash_until = fm1_ms + 80u;
        }

        for (i = 0u; i < 10u; i++) {
            fm1_led_key(APP_RHYTHM_BTN[i], (app_buttons >> i) & 1u);
        }
        fm1_led_key(BTN_OCT_DOWN, app_bank == MP7_BANK_A);
        fm1_led_key(BTN_OCT_UP, app_bank == MP7_BANK_B);
        fm1_led_key(BTN_PLAY, app_running && ((int32_t)(flash_until - fm1_ms) > 0));

        if ((app_bank != drawn_bank) || (app_buttons != drawn_buttons) ||
            (app_tempo != drawn_tempo) || (app_running != drawn_running)) {
            drawn_bank = app_bank;
            drawn_buttons = app_buttons;
            drawn_tempo = app_tempo;
            drawn_running = app_running;
            app_draw();
        }

        /* Do not erase flash while the sequencer or a voice can still sound:
         * an erase stalls the DAC for tens of milliseconds. */
        if ((app_dirty != 0) && !app_running && (fm1_in.notes == 0u) &&
            ((uint32_t)(fm1_ms - app_touched_at) > 2000u) &&
            ((uint32_t)(fm1_ms - app_last_trigger_at) > APP_SILENCE_MS)) {
            app_save_settings();
        }
    }
}
