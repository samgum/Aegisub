#!/usr/bin/env python3
"""Regression tests for the FFmpegSource video provider color space handling.

These guard against the bug that made 4K HDR (BT.2020) content fail to open
with "Unknown video color space": colormatrix_description() used to throw on
any colorspace outside its hardcoded switch, including the BT.2020 family
used by virtually all modern UHD/HDR releases.

The tests are static (string-based) on purpose — they don't link FFMS2 — so
they can run anywhere the source tree is checked out, exactly like the other
tools/test_*.py tests.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FFMPEGSOURCE = ROOT / "src" / "video_provider_ffmpegsource.cpp"


def test_no_unknown_color_space_throw():
    """colormatrix_description() must never throw VideoOpenError.

    A 4K HDR release using BT.2020 must be openable; refusing to open the
    video over a colorspace we merely don't have a dedicated resampler matrix
    for is the bug this test pins down.
    """
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert 'throw VideoOpenError("Unknown video color space")' not in source


def test_bt2020_colorspaces_are_recognized():
    """Both BT.2020 constant- and non-constant-luminance must map to a name."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "case AGI_CS_BT2020_NCL:" in source
    assert "case AGI_CS_BT2020_CL:" in source
    assert '"2020"' in source or '+ ".2020"' in source


def test_unknown_colorspace_falls_back_gracefully():
    """Anything else (YCOCG, ICTCp, future spaces) must fall back to a usable
    matrix rather than aborting open."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    # The default branch must return a string, not throw.
    assert "default:" in source
    assert "throw VideoOpenError" not in source.split("default:")[1].split("}")[0]


def test_standard_colorspaces_still_mapped():
    """The pre-existing mappings must stay intact (no regression)."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "case AGI_CS_RGB:" in source
    assert "case AGI_CS_BT709:" in source
    assert "case AGI_CS_FCC:" in source
    assert "case AGI_CS_BT470BG:" in source
    assert "case AGI_CS_SMPTE170M:" in source
    assert "case AGI_CS_SMPTE240M:" in source


def main():
    tests = [
        test_no_unknown_color_space_throw,
        test_bt2020_colorspaces_are_recognized,
        test_unknown_colorspace_falls_back_gracefully,
        test_standard_colorspaces_still_mapped,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} video colorspace tests passed")


if __name__ == "__main__":
    main()
