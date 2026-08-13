#!/usr/bin/env python3
"""Exactness and cost-model regressions for libass mask bounds scanning."""

from pathlib import Path
import random


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "subtitles_provider_libass.cpp"
WORD = 8


def reference(row):
    covered = [i for i, value in enumerate(row) if value]
    return (-1, -1) if not covered else (covered[0], covered[-1] + 1)


def chunked(row):
    """Portable model of the C++ word scan, returning span and probes."""
    width = len(row)
    left = 0
    probes = 0
    while left + WORD <= width:
        probes += 1
        if any(row[left:left + WORD]):
            break
        left += WORD
    while left < width and not row[left]:
        probes += 1
        left += 1
    if left == width:
        return (-1, -1), probes

    right = width
    while right - WORD >= left:
        probes += 1
        if any(row[right - WORD:right]):
            break
        right -= WORD
    while right > left and not row[right - 1]:
        probes += 1
        right -= 1
    return (left, right), probes


def test_cpp_uses_unaligned_safe_word_scanning():
    source = SOURCE.read_text(encoding="utf-8")
    helper = source[source.index("std::pair<int, int> nonzero_span"):
                    source.index("// Stuff used on the cache thread")]
    render = source[source.index("RenderedBounds GetRenderedBounds"):]
    assert "sizeof(uint64_t)" in helper
    assert helper.count("std::memcpy") == 2
    assert "auto [row_left, row_right] = nonzero_span(row, img->w);" in render
    assert "for (int x = 0; x < img->w; ++x)" not in render


def test_chunked_scan_matches_exact_bounds():
    rng = random.Random(0xA3615)
    cases = [bytearray(n) for n in range(0, 34)]
    for width in list(range(1, 65)) + [127, 128, 129, 1919, 1920, 3840]:
        for _ in range(20):
            row = bytearray(width)
            for _ in range(rng.randrange(0, min(width, 12) + 1)):
                row[rng.randrange(width)] = rng.randrange(1, 256)
            cases.append(row)
    for row in cases:
        assert chunked(row)[0] == reference(row)


def test_sparse_wide_row_probe_budget():
    """Benchmark-style deterministic cost check, avoiding flaky wall time."""
    width = 3840
    row = bytearray(width)
    row[1919] = 1
    span, probes = chunked(row)
    assert span == (1919, 1920)
    # Two word scans plus at most one word of byte refinement per edge.
    assert probes <= 2 * ((width + WORD - 1) // WORD) + 2 * WORD
    assert probes < width // 2


def main():
    tests = [
        test_cpp_uses_unaligned_safe_word_scanning,
        test_chunked_scan_matches_exact_bounds,
        test_sparse_wide_row_probe_budget,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} render-bounds performance tests passed")


if __name__ == "__main__":
    main()
