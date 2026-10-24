/*
 * mp7_tables.c - flash-resident tables for the Mini Pops library.
 *
 * The sine table stores only the first quadrant, as a file-scope `const`
 * array in flash. The full period is rebuilt once, on the first
 * mp7_create_context(), into a table shared by every context, every voice and
 * every future variant of the library; no context carries a copy.
 *
 * The stored values are exact: every entry equals
 *
 *     floor(32767 * sin(2*pi*i/1024) + 0.5)      for i = 0..256
 *
 * (round-half-up, the same convention the test harness uses). The peak at
 * index 256 is exactly 32767. Mirroring and negation reproduce the remaining
 * quadrants. The oscillator and the filter coefficient setup interpolate
 * between entries, so the coarser table does not quantise the output.
 *
 * Regenerate the table with: python3 tools/gen_sine_table.py
 *
 * This file belongs to the shared layer: the symbols are named poppy_*.
 */

#include <stdlib.h>

#include "mp7_internal.h"

const int16_t poppy_sine_quad1[POPPY_SINE_QUAD_SIZE] = {
         0,    201,    402,    603,    804,   1005,   1206,   1407,
      1608,   1809,   2009,   2210,   2410,   2611,   2811,   3012,
      3212,   3412,   3612,   3811,   4011,   4210,   4410,   4609,
      4808,   5007,   5205,   5404,   5602,   5800,   5998,   6195,
      6393,   6590,   6786,   6983,   7179,   7375,   7571,   7767,
      7962,   8157,   8351,   8545,   8739,   8933,   9126,   9319,
      9512,   9704,   9896,  10087,  10278,  10469,  10659,  10849,
     11039,  11228,  11417,  11605,  11793,  11980,  12167,  12353,
     12539,  12725,  12910,  13094,  13279,  13462,  13645,  13828,
     14010,  14191,  14372,  14553,  14732,  14912,  15090,  15269,
     15446,  15623,  15800,  15976,  16151,  16325,  16499,  16673,
     16846,  17018,  17189,  17360,  17530,  17700,  17869,  18037,
     18204,  18371,  18537,  18703,  18868,  19032,  19195,  19357,
     19519,  19680,  19841,  20000,  20159,  20317,  20475,  20631,
     20787,  20942,  21096,  21250,  21403,  21554,  21705,  21856,
     22005,  22154,  22301,  22448,  22594,  22739,  22884,  23027,
     23170,  23311,  23452,  23592,  23731,  23870,  24007,  24143,
     24279,  24413,  24547,  24680,  24811,  24942,  25072,  25201,
     25329,  25456,  25582,  25708,  25832,  25955,  26077,  26198,
     26319,  26438,  26556,  26674,  26790,  26905,  27019,  27133,
     27245,  27356,  27466,  27575,  27683,  27790,  27896,  28001,
     28105,  28208,  28310,  28411,  28510,  28609,  28706,  28803,
     28898,  28992,  29085,  29177,  29268,  29358,  29447,  29534,
     29621,  29706,  29791,  29874,  29956,  30037,  30117,  30195,
     30273,  30349,  30424,  30498,  30571,  30643,  30714,  30783,
     30852,  30919,  30985,  31050,  31113,  31176,  31237,  31297,
     31356,  31414,  31470,  31526,  31580,  31633,  31685,  31736,
     31785,  31833,  31880,  31926,  31971,  32014,  32057,  32098,
     32137,  32176,  32213,  32250,  32285,  32318,  32351,  32382,
     32412,  32441,  32469,  32495,  32521,  32545,  32567,  32589,
     32609,  32628,  32646,  32663,  32678,  32692,  32705,  32717,
     32728,  32737,  32745,  32752,  32757,  32761,  32765,  32766,
     32767,
};

int16_t *poppy_sine = 0;

static void rebuild_sine(int16_t *table)
{
    unsigned int i;

    for (i = 0u; i <= 256u; i++) {
        table[i] = poppy_sine_quad1[i];
    }
    for (i = 257u; i <= 511u; i++) {
        table[i] = poppy_sine_quad1[512u - i];
    }
    for (i = 512u; i <= 767u; i++) {
        table[i] = (int16_t)-poppy_sine_quad1[i - 512u];
    }
    for (i = 768u; i < POPPY_SINE_TABLE_SIZE; i++) {
        table[i] = (int16_t)-poppy_sine_quad1[1024u - i];
    }
}

/* Linear interpolation over the stored table. `phase` is 28-bit, so the top
 * ten bits select the entry and the low 18 bits are the fraction. The table
 * holds Q15 values, so the interpolation runs in 64 bits and rounds back to
 * Q15; the result always fits int16_t. */
int16_t poppy_sine_interp(uint32_t phase)
{
    uint32_t index = (phase >> 18) & 0x3FFu;
    uint32_t frac = phase & 0x3FFFFu;
    int32_t a = poppy_sine_at(index);
    int32_t b = poppy_sine_at((index + 1u) & 0x3FFu);
    int64_t v = (int64_t)a * 262144 + (int64_t)(b - a) * (int64_t)frac;

    return (int16_t)((v + (1 << 17)) >> 18);
}

void poppy_sine_init(void)
{
    static int initialized = 0;
    int16_t *table;

    if (initialized) {
        return;
    }
    initialized = 1;

    table = (int16_t *)malloc((size_t)POPPY_SINE_TABLE_SIZE * sizeof(int16_t));
    if (table == 0) {
        /* The quadrant in flash is bit-exact with the full table, so the
         * lookups fall back to it and the library still works. */
        return;
    }

    /* The table is filled before it is published: a second thread that
     * creates a context while this one is building must not find a
     * half-written table. */
    rebuild_sine(table);
    poppy_sine = table;
}

/* Soft-clip transfer curve. Entry i corresponds to u = -1 + 2*i/255 and
 * stores softclip(3*u) / 10 in Q31. The engine multiplies by 10 after the
 * lookup to restore the +/-10 saturation range. */
const int32_t poppy_softclip[POPPY_SOFTCLIP_TABLE_SIZE] = {
    -2147483647, -2147483385, -2147481525, -2147476401,
    -2147466264, -2147449288, -2147423556, -2147387067,
    -2147337724, -2147273337, -2147191614, -2147090162,
    -2146966478, -2146817950, -2146641849, -2146435327,
    -2146195410, -2145918996, -2145602849, -2145243595,
    -2144837715, -2144381543, -2143871256, -2143302875,
    -2142672254, -2141975075, -2141206847, -2140362893,
    -2139438350, -2138428158, -2137327059, -2136129585,
    -2134830055, -2133422569, -2131900999, -2130258984,
    -2128489921, -2126586963, -2124543006, -2122350690,
    -2120002383, -2117490183, -2114805907, -2111941085,
    -2108886954, -2105634453, -2102174217, -2098496569,
    -2094591520, -2090448758, -2086057648, -2081407227,
    -2076486201, -2071282940, -2065785481, -2059981520,
    -2053858418, -2047403198, -2040602547, -2033442822,
    -2025910048, -2017989929, -2009667852, -2000928896,
    -1991757842, -1982139184, -1972057146, -1961495693,
    -1950438553, -1938869235, -1926771055, -1914127158,
    -1900920545, -1887134109, -1872750665, -1857752988,
    -1842123852, -1825846075, -1808902565, -1791276367,
    -1772950722, -1753909117, -1734135350, -1713613593,
    -1692328456, -1670265059, -1647409104, -1623746954,
    -1599265706, -1573953279, -1547798492, -1520791150,
    -1492922136, -1464183491, -1434568507, -1404071815,
    -1372689470, -1340419041, -1307259694, -1273212273,
    -1238279383, -1202465458, -1165776837, -1128221827,
    -1089810757, -1050556036, -1010472186,  -969575885,
     -927885984,  -885423524,  -842211742,  -798276054,
     -753644044,  -708345425,  -662411998,  -615877590,
     -568777986,  -521150846,  -473035607,  -424473376,
     -375506810,  -326179987,  -276538263,  -226628121,
     -176497016,  -126193205,   -75765578,   -25263477,
       25263477,    75765578,   126193205,   176497016,
      226628121,   276538263,   326179987,   375506810,
      424473376,   473035607,   521150846,   568777986,
      615877590,   662411998,   708345425,   753644044,
      798276054,   842211742,   885423524,   927885984,
      969575885,  1010472186,  1050556036,  1089810757,
     1128221827,  1165776837,  1202465458,  1238279383,
     1273212273,  1307259694,  1340419041,  1372689470,
     1404071815,  1434568507,  1464183491,  1492922136,
     1520791150,  1547798492,  1573953279,  1599265706,
     1623746954,  1647409104,  1670265059,  1692328456,
     1713613593,  1734135350,  1753909117,  1772950722,
     1791276367,  1808902565,  1825846075,  1842123852,
     1857752988,  1872750665,  1887134109,  1900920545,
     1914127158,  1926771055,  1938869235,  1950438553,
     1961495693,  1972057146,  1982139184,  1991757842,
     2000928896,  2009667852,  2017989929,  2025910048,
     2033442822,  2040602547,  2047403198,  2053858418,
     2059981520,  2065785481,  2071282940,  2076486201,
     2081407227,  2086057648,  2090448758,  2094591520,
     2098496569,  2102174217,  2105634453,  2108886954,
     2111941085,  2114805907,  2117490183,  2120002383,
     2122350690,  2124543006,  2126586963,  2128489921,
     2130258984,  2131900999,  2133422569,  2134830055,
     2136129585,  2137327059,  2138428158,  2139438350,
     2140362893,  2141206847,  2141975075,  2142672254,
     2143302875,  2143871256,  2144381543,  2144837715,
     2145243595,  2145602849,  2145918996,  2146195410,
     2146435327,  2146641849,  2146817950,  2146966478,
     2147090162,  2147191614,  2147273337,  2147337724,
     2147387067,  2147423556,  2147449288,  2147466264,
     2147476401,  2147481525,  2147483385,  2147483647,
};

/*
 * Factory rhythm data.
 *
 * A column is a 16-bit instrument mask: bit n is set when instrument
 * n (mp7_instr order) plays in that column, so bit 0 is the Base and
 * bit 14 the Quijada. Every array is const flash data.
 *
 * The Tambourine, Guiro and Quijada hits are identical in every rhythm, so
 * they are a separate definition of their own: mp7_shared_percussion_notes
 * holds that layer, and it is what plays when no rhythm button is pressed.
 * A rhythm's own grid holds only the twelve rhythm instruments, and the
 * shared layer is OR-ed into the combined grid when the rhythms are put
 * together.
 *
 * The three distinct step-enable masks are defined once and shared by
 * every rhythm that uses them. Bit c is column c; the note grids are named
 * after the rhythm they belong to.
 *
 * A grid stores only its shortest repeating period (16, 32 or 64
 * columns) and sets notes_period accordingly; the sequencer wraps the
 * column with a mask. Samba, Rumba, Meringue and Cha-Cha differ between
 * the halves, so they keep all 64 columns. The shared percussion layer
 * also repeats every 32 columns.
 */

#define MP7_TIMESIG_4_4 UINT64_C(0xFFFFFFFFFFFFFFFF)
#define MP7_TIMESIG_3_4 UINT64_C(0xFF0F0FFFFF0F0FFF)
#define MP7_TIMESIG_2_4 UINT64_C(0x3F3F3F3F3F3F3F3F)

/* The Tambourine, Guiro and Quijada layer. It is the same in every
 * factory rhythm and it is what plays when no rhythm button is pressed.
 *
 * Tambourine O---O---O---O---O---O---O---O---
 * Guiro      OOOO----O---O---OOOO----O---O---
 * Quijada    ------------------------O-------
 */
const uint16_t mp7_shared_percussion_notes[MP7_PATTERN_HALF_STEPS] = {
    0x3000, 0x2000, 0x2000, 0x2000, 0x1000, 0x0000, 0x0000, 0x0000,
    0x3000, 0x0000, 0x0000, 0x0000, 0x3000, 0x0000, 0x0000, 0x0000,
    0x3000, 0x2000, 0x2000, 0x2000, 0x1000, 0x0000, 0x0000, 0x0000,
    0x7000, 0x0000, 0x0000, 0x0000, 0x3000, 0x0000, 0x0000, 0x0000,
};

/* Waltz - 3/4, straight, 32-column stored period
 * Enable     OOOOOOOOOOOO....OOOO....OOOOOOOO
 * Bass       O-----------....----....--------
 * Snare 2    --------O---....----....O-------
 * Cymbal 1   O-----------....----....--------
 */
static const uint16_t mp7_pattern_waltz_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0401, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0004, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0004, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

/* Samba - 4/4, straight, 64-column stored period
 * Bass       O-----------O---O---------------O-----------O---O---------------
 * Conga      ------------------------O-------------------------------O-------
 * Cowbell    O-------O-------O---O-------O-------O-------O-------O---O-------
 * Hihat      O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_samba_notes[MP7_PATTERN_STEPS] = {
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0140, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0120, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0100, 0x0000, 0x0000, 0x0000, 0x0141, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0160, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
};

/* Mambo - 4/4, straight, 32-column stored period
 * Bass       O-----------O---O---------------
 * Conga      ------------------------O---O---
 * Cowbell    O-------O-------O-------O-------
 * Hihat      O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_mambo_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0140, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0160, 0x0000, 0x0000, 0x0000, 0x0120, 0x0000, 0x0000, 0x0000,
};

/* Rumba - 4/4, straight, 64-column stored period
 * Bass       O---------------O-------O-------O---------------O-------O-------
 * Sm Bongo   --------O-----------O-------------------O-----------O-----------
 * Lg Bongo   O-----------O---O---------------O-----------O---O---------------
 * Conga      ------------------------O---O---------------------------O---O---
 * Claves     O-----------O-----------O---------------O-------O---------------
 * Hihat      O---O-O-O---O---O---O---O---O---O---O-O-O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_rumba_notes[MP7_PATTERN_STEPS] = {
    0x0191, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0100, 0x0000,
    0x0108, 0x0000, 0x0000, 0x0000, 0x0190, 0x0000, 0x0000, 0x0000,
    0x0111, 0x0000, 0x0000, 0x0000, 0x0108, 0x0000, 0x0000, 0x0000,
    0x01A1, 0x0000, 0x0000, 0x0000, 0x0120, 0x0000, 0x0000, 0x0000,
    0x0111, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0100, 0x0000,
    0x0188, 0x0000, 0x0000, 0x0000, 0x0110, 0x0000, 0x0000, 0x0000,
    0x0191, 0x0000, 0x0000, 0x0000, 0x0108, 0x0000, 0x0000, 0x0000,
    0x0121, 0x0000, 0x0000, 0x0000, 0x0120, 0x0000, 0x0000, 0x0000,
};

/* Tango - 4/4, straight, 32-column stored period
 * Bass       O-------O-------O-------O-------
 * Snare 2    ----------------------------O---
 * Hihat      O-------O-------O-------O-------
 * Cymbal 2   ----------------------------O---
 */
static const uint16_t mp7_pattern_tango_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0804, 0x0000, 0x0000, 0x0000,
};

/* Bossanova - 4/4, straight, 64-column stored period
 * Bass       O-----------O---O-----------O---O-----------O---O-----------O---
 * Hihat      O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---
 * Rimshot    O-----------O-----------O---------------O-----------O-----------
 */
static const uint16_t mp7_pattern_bossanova_notes[MP7_PATTERN_STEPS] = {
    0x0301, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0100, 0x0000, 0x0000, 0x0000, 0x0301, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0300, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0300, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0300, 0x0000, 0x0000, 0x0000,
    0x0100, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
};

/* Rock 1 - 4/4, straight, 32-column stored period
 * Bass       O-----------O---O-----------O---
 * Snare 1    --------O---------------O-------
 * Hihat      O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_rock1_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0102, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0102, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
};

/* Rock 3 - 4/4, straight, 32-column stored period
 * Bass       O-----------O---O---O-------O---
 * Snare 1    --------O---------------O-------
 * Snare 2    --------------O---O-----O-------
 * Hihat      O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_rock3_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0102, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0004, 0x0000,
    0x0101, 0x0000, 0x0004, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0106, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
};

/* March 2/4 - 2/4, triplet, 16-column stored period
 * Enable     OOOOOO..OOOOOO..
 * Bass       O-----..------..
 * Snare 2    ------..O-----..
 */
static const uint16_t mp7_pattern_march24_notes[MP7_PATTERN_QUARTER_STEPS] = {
    0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0004, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

/* Swing - 4/4, straight, 32-column stored period
 * Bass       O-------O-------O-------O-------
 * Snare 2    --------O---------------O-------
 * Cymbal 2   O-------O-----O-O-------O-----O-
 */
static const uint16_t mp7_pattern_swing_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0801, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0805, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x0000,
    0x0801, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0805, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x0000,
};

/* Jazz Waltz - 3/4, straight, 32-column stored period
 * Enable     OOOOOOOOOOOO....OOOO....OOOOOOOO
 * Bass       O-----------....----....--------
 * Snare 2    ----O---O---....----....O-------
 * Cymbal 1   O-----------....----....--------
 */
static const uint16_t mp7_pattern_jazzwaltz_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0401, 0x0000, 0x0000, 0x0000, 0x0004, 0x0000, 0x0000, 0x0000,
    0x0004, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0004, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

/* Meringue - 4/4, straight, 64-column stored period
 * Bass       O---------------O---------------O---------------O---------------
 * Conga      ------------------------O-------------------------------O-------
 * Cowbell    O-----------O---O---------------O---O-------O---O---------------
 * Hihat      O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_meringue_notes[MP7_PATTERN_STEPS] = {
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0100, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0120, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0100, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0120, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
};

/* Cha-Cha - 4/4, straight, 64-column stored period
 * Bass       O-----------O---O---------------O-----------O---O---------------
 * Conga      ------------------------O---O---------------------------O---O---
 * Cowbell    O-------O-------O---O---O-------O---O---O-------O---O---O-------
 * Hihat      O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_chacha_notes[MP7_PATTERN_STEPS] = {
    0x0141, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0140, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0160, 0x0000, 0x0000, 0x0000, 0x0120, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0140, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0141, 0x0000, 0x0000, 0x0000, 0x0140, 0x0000, 0x0000, 0x0000,
    0x0160, 0x0000, 0x0000, 0x0000, 0x0120, 0x0000, 0x0000, 0x0000,
};

/* Beguine - 4/4, straight, 32-column stored period
 * Bass       O---------------O-------O-------
 * Sm Bongo   ----O-------O-------O-------O---
 * Lg Bongo   O---------------O-------O-------
 * Conga      ------------------------O---O---
 * Hihat      O---O-------O---O---O---O---O---
 * Cymbal 1   ----O---------------------------
 */
static const uint16_t mp7_pattern_beguine_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0111, 0x0000, 0x0000, 0x0000, 0x0508, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0108, 0x0000, 0x0000, 0x0000,
    0x0111, 0x0000, 0x0000, 0x0000, 0x0108, 0x0000, 0x0000, 0x0000,
    0x0131, 0x0000, 0x0000, 0x0000, 0x0128, 0x0000, 0x0000, 0x0000,
};

/* Habanera - 4/4, straight, 32-column stored period
 * Bass       O-----------O---O-------O-------
 * Hihat      O-----------O---O-------O-------
 * Cymbal 2   O-------------------------------
 */
static const uint16_t mp7_pattern_habanera_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0901, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

/* Slow Rock - 2/4, triplet, 16-column stored period
 * Enable     OOOOOO..OOOOOO..
 * Bass       O-----..----O-..
 * Hihat      O-O-O-O.O-O-O-O.
 * Rimshot    ------..O-----..
 */
static const uint16_t mp7_pattern_slowrock_notes[MP7_PATTERN_QUARTER_STEPS] = {
    0x0101, 0x0000, 0x0100, 0x0000, 0x0100, 0x0000, 0x0100, 0x0000,
    0x0300, 0x0000, 0x0100, 0x0000, 0x0101, 0x0000, 0x0100, 0x0000,
};

/* Rock 2 - 4/4, straight, 32-column stored period
 * Bass       O-----------O---O-----------O---
 * Snare 1    --------O-----------O-----------
 * Hihat      O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_rock2_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0102, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0102, 0x0000, 0x0000, 0x0000,
    0x0100, 0x0000, 0x0000, 0x0000, 0x0101, 0x0000, 0x0000, 0x0000,
};

/* Rock 4 - 4/4, straight, 32-column stored period
 * Bass       O---------------O-----------O---
 * Snare 1    --------O---------------O-------
 * Snare 2    --------------O---------O---O---
 * Hihat      O---O---O---O---O---O---O---O---
 */
static const uint16_t mp7_pattern_rock4_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0102, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0004, 0x0000,
    0x0101, 0x0000, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000, 0x0000,
    0x0106, 0x0000, 0x0000, 0x0000, 0x0105, 0x0000, 0x0000, 0x0000,
};

/* March 6/8 - 2/4, triplet, 16-column stored period
 * Enable     OOOOOO..OOOOOO..
 * Bass       O-----..O-----..
 * Cymbal 2   O---O-..O---O-..
 */
static const uint16_t mp7_pattern_march68_notes[MP7_PATTERN_QUARTER_STEPS] = {
    0x0801, 0x0000, 0x0000, 0x0000, 0x0800, 0x0000, 0x0000, 0x0000,
    0x0801, 0x0000, 0x0000, 0x0000, 0x0800, 0x0000, 0x0000, 0x0000,
};

/* Latin Swing - 4/4, straight, 32-column stored period
 * Bass       O---------------O-------O-------
 * Sm Bongo   --------O-----------------------
 * Conga      ------------------------O-----O-
 * Cymbal 2   O-------O-----O-O-------O-----O-
 */
static const uint16_t mp7_pattern_latinswing_notes[MP7_PATTERN_HALF_STEPS] = {
    0x0801, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0808, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x0000,
    0x0801, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0821, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0820, 0x0000,
};

const struct mp7_factory_rhythm mp7_factory_rhythms[MP7_RHYTHM_COUNT] = {
    { mp7_pattern_waltz_notes, MP7_TIMESIG_3_4, 32, false }, /* Waltz */
    { mp7_pattern_samba_notes, MP7_TIMESIG_4_4, 64, false }, /* Samba */
    { mp7_pattern_mambo_notes, MP7_TIMESIG_4_4, 32, false }, /* Mambo */
    { mp7_pattern_rumba_notes, MP7_TIMESIG_4_4, 64, false }, /* Rumba */
    { mp7_pattern_tango_notes, MP7_TIMESIG_4_4, 32, false }, /* Tango */
    { mp7_pattern_bossanova_notes, MP7_TIMESIG_4_4, 64, false }, /* Bossanova */
    { mp7_pattern_rock1_notes, MP7_TIMESIG_4_4, 32, false }, /* Rock 1 */
    { mp7_pattern_rock3_notes, MP7_TIMESIG_4_4, 32, false }, /* Rock 3 */
    { mp7_pattern_march24_notes, MP7_TIMESIG_2_4, MP7_PATTERN_QUARTER_STEPS, true }, /* March 2/4 */
    { mp7_pattern_swing_notes, MP7_TIMESIG_4_4, 32, false }, /* Swing */
    { mp7_pattern_jazzwaltz_notes, MP7_TIMESIG_3_4, 32, false }, /* Jazz Waltz */
    { mp7_pattern_meringue_notes, MP7_TIMESIG_4_4, 64, false }, /* Meringue */
    { mp7_pattern_chacha_notes, MP7_TIMESIG_4_4, 64, false }, /* Cha-Cha */
    { mp7_pattern_beguine_notes, MP7_TIMESIG_4_4, 32, false }, /* Beguine */
    { mp7_pattern_habanera_notes, MP7_TIMESIG_4_4, 32, false }, /* Habanera */
    { mp7_pattern_slowrock_notes, MP7_TIMESIG_2_4, MP7_PATTERN_QUARTER_STEPS, true }, /* Slow Rock */
    { mp7_pattern_rock2_notes, MP7_TIMESIG_4_4, 32, false }, /* Rock 2 */
    { mp7_pattern_rock4_notes, MP7_TIMESIG_4_4, 32, false }, /* Rock 4 */
    { mp7_pattern_march68_notes, MP7_TIMESIG_2_4, MP7_PATTERN_QUARTER_STEPS, true }, /* March 6/8 */
    { mp7_pattern_latinswing_notes, MP7_TIMESIG_4_4, 32, false }, /* Latin Swing */
};

