#!/usr/bin/env python3
"""Generate the sine table used by the Mini Pops library.

The library keeps the full period of the sine in flash as a single shared
`const` table:

    s[i] = floor(32767 * sin(2*pi*i/4096) + 0.5)      for i = 0..4095

The table is `const` and file-scope, so every context, every voice and every
future variant of the library shares one copy. Nothing is built at init time
and no context carries a copy of the table.

The rounding convention is **round-half-up**, i.e. `floor(x + 0.5)`. The C
harness (`mp7_test --sine`) uses the same convention, so the generator and the
test can never disagree about an entry that lands exactly on a half.

Usage:
    python3 tools/gen_sine_table.py            # print the C table on stdout
    python3 tools/gen_sine_table.py --check    # verify only, report on stderr
"""

import math
import sys

FULL_SIZE = 4096
Q15_FULL_SCALE = 32767


def q15_sine(index: int) -> int:
    """floor(32767 * sin(2*pi*index/4096) + 0.5), i.e. round-half-up.

    Round-half-up, not Python's banker's rounding, so the value matches the
    C harness (`floor(exact + 0.5)`) exactly for every index.
    """
    exact = Q15_FULL_SCALE * math.sin(2.0 * math.pi * index / FULL_SIZE)
    return math.floor(exact + 0.5)


def full_table() -> list[int]:
    return [q15_sine(i) for i in range(FULL_SIZE)]


def check(table: list[int]) -> int:
    """Return the number of mismatching entries; print a report to stderr."""
    bad = 0
    worst = 0
    for i in range(FULL_SIZE):
        expected = q15_sine(i)
        err = abs(table[i] - expected)
        worst = max(worst, err)
        if err:
            bad += 1
            if bad <= 8:
                print(
                    f"  mismatch i={i}: got {table[i]}, expected {expected} "
                    f"(err {err})",
                    file=sys.stderr,
                )
    print(f"table: {len(table)} entries, {len(table) * 2} bytes", file=sys.stderr)
    print(f"peak  = {table[FULL_SIZE // 4]} (must be 32767)", file=sys.stderr)
    print(
        f"mismatches = {bad}, worst error = {worst}",
        file=sys.stderr,
    )
    return bad


def emit(table: list[int]) -> None:
    print("const int16_t poppy_sine[POPPY_SINE_TABLE_SIZE] = {")
    for start in range(0, FULL_SIZE, 8):
        row = ", ".join(f"{v:6d}" for v in table[start : start + 8])
        print(f"    {row},")
    print("};")


def main() -> int:
    table = full_table()
    bad = check(table)
    if bad:
        print("FAIL: the table does not match sin(2*pi*i/4096)", file=sys.stderr)
        return 1
    if table[FULL_SIZE // 4] != Q15_FULL_SCALE:
        print("FAIL: the peak is not exactly 32767", file=sys.stderr)
        return 1
    if "--check" not in sys.argv:
        emit(table)
    return 0


if __name__ == "__main__":
    sys.exit(main())
