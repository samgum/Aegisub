#!/usr/bin/env python3
"""Static tests for HDR video handling.

After the CPU tone-map experiment was removed (it produced banding/blocking
artifacts on real Dolby Vision content), HDR sources now go through the same
bgra path as SDR, letting libswscale handle the colorspace conversion. These
tests verify that:
- HDR sources are still detected and opened (not rejected).
- HDR sources are downscaled at decode time for performance.
- The CPU tone-map code path is NOT used (no rgb48le, no ToneMapRGB48toBGRA8
  call, no BuildToneMapper).
- hdr_tonemap.h is retained only for IsHDRSource detection.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TONEMAP = ROOT / "src" / "hdr_tonemap.h"
FFMPEGSOURCE = ROOT / "src" / "video_provider_ffmpegsource.cpp"


def test_hdr_detection_still_present():
    """The provider must still detect HDR sources (for downscale + logging)."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "TransferCharateristics" in source
    assert "ColorPrimaries" in source
    assert "ContentLightLevelMax" in source
    assert "HDRTonemap::IsHDRSource(Transfer, Primaries)" in source
    assert '#include "hdr_tonemap.h"' in source


def test_hdr_uses_bgra_not_rgb48le():
    """HDR must NOT request 16-bit rgb48le — that path caused banding artifacts.
    All sources use plain bgra and let swscale convert."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    # The format must be bgra for everything.
    assert 'FFMS_GetPixFmt("bgra")' in source
    # rgb48le must NOT be requested.
    assert 'FFMS_GetPixFmt("rgb48le")' not in source


def test_no_cpu_tonemap_in_provider():
    """The CPU tone-map entry point must not be called from the provider."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "ToneMapRGB48toBGRA8" not in source
    assert "BuildToneMapper" not in source
    assert "ToneMap" not in source  # no ToneMapper member variable


def test_hdr_downscaled_at_decode_time():
    """4K HDR sources must still be subsampled for performance."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "if (IsHDR && Width > 1920)" in source
    assert "preview_max_width = 1920" in source
    assert "FFMS_RESIZER_BILINEAR" in source


def test_hdr_getframe_uses_simple_copy():
    """GetFrame must use the same simple bgra copy for all sources — no
    HDR-specific branch that could introduce artifacts."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "out.data.assign(frame->Data[0]" in source
    # No HDR-specific data processing branch.
    assert "reinterpret_cast<const uint16_t" not in source


def test_hdr_detection_logic_correct():
    """IsHDRSource must key off the transfer function (PQ/HLG), not primaries."""
    source = TONEMAP.read_text(encoding="utf-8")
    assert "namespace HDRTonemap" in source
    assert "inline bool IsHDRSource" in source
    assert "transfer == kTransferPQ || transfer == kTransferHLG" in source


def main():
    tests = [
        test_hdr_detection_still_present,
        test_hdr_uses_bgra_not_rgb48le,
        test_no_cpu_tonemap_in_provider,
        test_hdr_downscaled_at_decode_time,
        test_hdr_getframe_uses_simple_copy,
        test_hdr_detection_logic_correct,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} HDR handling tests passed")


if __name__ == "__main__":
    main()
