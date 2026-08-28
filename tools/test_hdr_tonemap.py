#!/usr/bin/env python3
"""Static integration checks for preview-grade HDR handling."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TONEMAP = (ROOT / "src" / "hdr_tonemap.h").read_text(encoding="utf-8")
PROVIDER = (ROOT / "src" / "video_provider_ffmpegsource.cpp").read_text(encoding="utf-8")
MESON = (ROOT / "tests" / "meson.build").read_text(encoding="utf-8")
GTEST = (ROOT / "tests" / "tests" / "hdr_tonemap.cpp").read_text(encoding="utf-8")


def test_standard_hdr_only_uses_rgb48_cpu_path():
    assert "UseCpuToneMap = HDRTonemap::IsHDRSource" in PROVIDER
    assert "!IsDolbyVisionIpt" in PROVIDER
    assert 'FFMS_GetPixFmt("rgb48le")' in PROVIDER
    assert "UseCpuToneMap ? rgb48_fmt : bgra_fmt" in PROVIDER
    assert "ToneMapRGB48toBGRA8" in PROVIDER


def test_dolby_vision_p5_is_not_pretended_to_be_supported():
    assert "AGI_CS_ICTCP = 14" in PROVIDER
    assert "AGI_CS_IPT_C2 = 15" in PROVIDER
    assert "IPT/ICtCp (profile 5 style) video is unsupported" in PROVIDER
    assert "RPU reshaping is not applied" in PROVIDER
    assert "IPTToBT2020" not in TONEMAP


def test_rpu_base_layer_limitation_is_explicit():
    assert "HasDolbyVision && UseCpuToneMap" in PROVIDER
    assert "uses only the" in PROVIDER and "standard PQ/HLG base layer" in PROVIDER
    assert "RPU reshaping is not applied" in PROVIDER


def test_mapper_has_luminance_pipeline_and_output_space_dither():
    assert "PQEOTF" in TONEMAP
    assert "HLGInverseOETF" in TONEMAP
    assert "HLGScale" in TONEMAP
    assert "CompressGamut" in TONEMAP
    assert "ToneMapLuma" in TONEMAP
    assert "srgb_codes_[index] + dither" in TONEMAP
    assert "float dither" in TONEMAP
    assert "uint8_t dither" not in TONEMAP


def test_stride_and_bgra_are_explicit():
    assert "ptrdiff_t source_stride" in TONEMAP
    assert "ptrdiff_t destination_stride" in TONEMAP
    assert "destination + y * destination_stride" in TONEMAP
    assert "dst[4 * x + 3] = 255" in TONEMAP


def test_yuv_reference_decode_path_exists():
    """The precise BT.2100 decode path must exist: swscale hands back untouched
    PQ/HLG YCbCr planes and the mapper performs inverse EOTF per channel, the
    linear-domain YCbCr->RGB matrix, and range expansion — instead of letting
    swscale apply the matrix in the nonlinear transfer domain."""
    assert "ToneMapYUV444P16toBGRA8" in TONEMAP
    assert "ToneMapYuvPixel" in TONEMAP
    # Video-range expansion (10-bit limited shifted <<6 into 16 bits).
    assert "4096.0f" in TONEMAP
    assert "32768.0f" in TONEMAP
    # Linear-domain YCbCr matrices for both container families.
    assert "bt2020_matrix_" in TONEMAP
    assert "1.474600f" in TONEMAP  # BT.2020
    assert "1.574800f" in TONEMAP  # BT.709
    # Provider negotiates YUV planes with an RGB fallback.
    assert 'FFMS_GetPixFmt("yuv444p16le")' in PROVIDER
    assert "UseYuvDecodePath" in PROVIDER
    assert "frame->Data[1], frame->Linesize[1]" in PROVIDER


def test_hdr_preview_size_is_bounded_and_never_zero():
    assert "if (IsHDR && Width > 1920)" in PROVIDER
    assert "preview_max_width = 1920" in PROVIDER
    assert "std::max(2, static_cast<int>(Height * scale) & ~1)" in PROVIDER


def test_real_gtests_are_registered():
    assert "tests/hdr_tonemap.cpp" in MESON
    for behavior in ("TransferFunctions", "NeutralPixels", "HighlightShoulder", "Dither", "Strides"):
        assert behavior in GTEST


def main():
    tests = [value for name, value in globals().items() if name.startswith("test_")]
    for test in tests:
        test()
    print(f"{len(tests)} HDR handling tests passed")


if __name__ == "__main__":
    main()
