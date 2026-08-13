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
    assert "unsupported colour-space id" in source
    assert "preview conversion and the .709 label may be approximate" in source


def test_standard_colorspaces_still_mapped():
    """The pre-existing mappings must stay intact (no regression)."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    assert "case AGI_CS_RGB:" in source
    assert "case AGI_CS_BT709:" in source
    assert "case AGI_CS_FCC:" in source
    assert "case AGI_CS_BT470BG:" in source
    assert "case AGI_CS_SMPTE170M:" in source
    assert "case AGI_CS_SMPTE240M:" in source


def test_hdr_tonemap_requires_a_swscale_supported_matrix():
    """RGB48 tone mapping must never reinterpret ICTCP or an unknown matrix."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    helper = source[source.index("bool swscale_has_input_matrix("):
                    source.index("void copy_bgra(")]
    for color_space in ["RGB", "BT709", "FCC", "BT470BG", "SMPTE170M",
                        "SMPTE240M", "BT2020_NCL", "BT2020_CL"]:
        assert f"case AGI_CS_{color_space}:" in helper
    for color_space in ["ICTCP", "IPT_C2", "YCOCG", "SMPTE2085"]:
        assert f"case AGI_CS_{color_space}:" not in helper
    assert "default:" in helper and "return false;" in helper

    load = source[source.index("void FFmpegSourceVideoProvider::LoadVideo("):]
    fallback = load.index("if (CS == AGI_CS_UNSPECIFIED)")
    decision = load.index("UseCpuToneMap = HDRTonemap::IsHDRSource", fallback)
    assert fallback < decision
    assert "HasSupportedInputMatrix = swscale_has_input_matrix(CS);" in load
    assert "&& HasSupportedInputMatrix && !IsDolbyVisionIpt;" in load
    assert "disabling CPU tone mapping" in load


def test_hdr_tonemap_rejects_tv601_overrides():
    """The tone mapper and swscale input matrix must stay paired."""
    source = FFMPEGSOURCE.read_text(encoding="utf-8")
    setter = source[source.index("void SetColorSpace("):source.index("int GetFrameCount()")]
    assert "if (UseCpuToneMap)" in setter
    assert "Ignoring manual colour-matrix override" in setter

    load = source[source.index("void FFmpegSourceVideoProvider::LoadVideo("):]
    override = load[load.index("RealColorSpace = ColorSpace"):load.index("if (CS != VideoCS)")]
    assert "if (!UseCpuToneMap" in override
    assert "Ignoring saved TV.601 override" in override


def main():
    tests = [
        test_no_unknown_color_space_throw,
        test_bt2020_colorspaces_are_recognized,
        test_unknown_colorspace_falls_back_gracefully,
        test_standard_colorspaces_still_mapped,
        test_hdr_tonemap_requires_a_swscale_supported_matrix,
        test_hdr_tonemap_rejects_tv601_overrides,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} video colorspace tests passed")


if __name__ == "__main__":
    main()
