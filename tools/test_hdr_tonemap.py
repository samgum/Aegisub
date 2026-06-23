#!/usr/bin/env python3
"""Static tests for the HDR tone-mapping preview layer.

These guard the feature that lets HDR (PQ/HLG/BT.2020) footage open and show
recognizable colour in Aegisub's 8-bit BGRA display pipeline, instead of a
near-black frame. The tone-map runs on the CPU inside the FFmpegSource provider
because the OpenGL renderer is hard-locked to 8-bit.

Static (string-based) on purpose: they don't link FFMS2, matching the other
tools/test_*.py tests.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TONEMAP = ROOT / "src" / "hdr_tonemap.h"
FFMPEGSOURCE = ROOT / "src" / "video_provider_ffmpegsource.cpp"


def test_tonemap_header_exists_with_core_api():
    """The tone-mapping module must expose the building blocks the provider
    depends on: HDR detection, the two EOTFs, the operator, and the entry
    point that converts a full rgb48le frame to 8-bit BGRA."""
    source = TONEMAP.read_text(encoding="utf-8")
    assert "namespace HDRTonemap" in source
    assert "inline bool IsHDRSource(int transfer, int primaries)" in source
    assert "inline double PQEOTF(double e)" in source
    assert "inline double HLGOOTF(double e)" in source
    assert "inline double ToneMapReinhardt(double l, double peak)" in source
    assert "inline ToneMapper BuildToneMapper(int transfer, int primaries, int max_cll)" in source
    assert "inline void ToneMapRGB48toBGRA8(const ToneMapper &tm," in source
    # The transfer-function constants must match libavutil's values.
    assert "kTransferPQ        = 16" in source
    assert "kTransferHLG       = 18" in source
    assert "kPrimariesBT2020   = 9" in source


def test_tonemap_uses_luts_not_per_pixel_pow():
    """The per-frame hot path must use precomputed lookup tables so 4K HDR
    preview is not stalled by ~9 std::pow calls per pixel. The EOTF LUT folds
    in PQ/HLG decode + tone-map; the gamma LUT folds in display encoding."""
    source = TONEMAP.read_text(encoding="utf-8")
    assert "std::vector<float> eotf_lut" in source
    assert "std::vector<uint8_t> gamma_lut" in source
    assert "struct ToneMapper" in source
    # The hot loop reads channels from the EOTF LUT, never calling PQEOTF /
    # HLGOOTF directly (those are only used to build the LUT).
    assert "tm.EOTF(src" in source


def test_tonemap_is_hdr_keyed_off_transfer_not_primaries():
    """SDR UHD content uses BT.2020 primaries too, so HDR detection must be
    based on the transfer function (PQ/HLG), never on primaries alone."""
    source = TONEMAP.read_text(encoding="utf-8")
    assert "transfer == kTransferPQ || transfer == kTransferHLG" in source


def test_tonemap_has_bt2020_to_709_gamut_matrix():
    """Wide-gamut BT.2020 must be brought into the BT.709/sRGB cube the 8-bit
    preview targets, otherwise colours stay oversaturated."""
    source = TONEMAP.read_text(encoding="utf-8")
    assert "kBT2020To709" in source
    assert "ApplyGamut" in source


def test_provider_reads_hdr_metadata_from_first_frame():
    """The provider must capture transfer/primaries/MaxCLL and set IsHDR so the
    rest of the pipeline knows whether to tone-map."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert '#include "hdr_tonemap.h"' in source
    # FFMS2's field is deliberately misspelled in the upstream header.
    assert "TransferCharateristics" in source
    assert "ColorPrimaries" in source
    assert "ContentLightLevelMax" in source
    assert "HDRTonemap::IsHDRSource(Transfer, Primaries)" in source


def test_provider_requests_16_bit_for_hdr_and_bgra_for_sdr():
    """HDR sources must decode at 16-bit rgb48le (so highlight detail survives
    the tone-map); SDR sources must keep the original bgra path unchanged."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert 'FFMS_GetPixFmt("rgb48le")' in source
    assert 'IsHDR ? rgb48_fmt : bgra_fmt' in source


def test_getframe_tonemaps_hdr_and_passes_sdr_through():
    """GetFrame must branch on IsHDR: tone-map + quantize for HDR, raw copy for
    SDR. Both branches must produce a tightly-packed 8-bit BGRA buffer so the
    existing flip/rotation code keeps working."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "if (IsHDR)" in source
    assert "HDRTonemap::ToneMapRGB48toBGRA8(" in source
    # SDR fallback must still be the straight 8-bit copy.
    assert "out.data.assign(frame->Data[0]" in source


def test_flip_and_rotation_use_pitch_not_linesize():
    """flip/rotation must use out.pitch (which is correct for both the
    tone-mapped buffer and the raw SDR buffer), not frame->Linesize[0] which
    is wrong for the 16-bit-derived HDR path."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    # The flip/rotation loops must reference out.pitch.
    assert "out.data[out.pitch * x" in source
    assert "data[out.pitch *" in source


def test_provider_builds_tonemapper_once_not_per_frame():
    """The EOTF/gamma LUTs are expensive to build (~5ms) and must be
    constructed once per source (in LoadVideo) and reused per frame, otherwise
    4K playback rebuilds 65536-entry tables on every GetFrame."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "std::unique_ptr<HDRTonemap::ToneMapper> ToneMap" in source
    assert "HDRTonemap::BuildToneMapper(Transfer, Primaries, MaxCLL)" in source
    # Per-frame call must take the prebuilt mapper, not rebuild it.
    assert "HDRTonemap::ToneMapRGB48toBGRA8(\n\t\t\t*ToneMap," in source or \
           "*ToneMap," in source


def main():
    tests = [
        test_tonemap_header_exists_with_core_api,
        test_tonemap_uses_luts_not_per_pixel_pow,
        test_tonemap_is_hdr_keyed_off_transfer_not_primaries,
        test_tonemap_has_bt2020_to_709_gamut_matrix,
        test_provider_reads_hdr_metadata_from_first_frame,
        test_provider_requests_16_bit_for_hdr_and_bgra_for_sdr,
        test_getframe_tonemaps_hdr_and_passes_sdr_through,
        test_flip_and_rotation_use_pitch_not_linesize,
        test_provider_builds_tonemapper_once_not_per_frame,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} HDR tone-map tests passed")


if __name__ == "__main__":
    main()
