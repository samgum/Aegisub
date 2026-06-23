#pragma once

// CPU HDR tone-mapping for video preview.
//
// Aegisub's display pipeline is 8-bit BGRA end to end (the OpenGL renderer in
// video_out_gl.cpp hard-codes GL_BGRA_EXT + GL_UNSIGNED_BYTE), so HDR content
// (10-bit PQ/HLG BT.2020) would otherwise be crushed to SDR by libswscale's
// default behaviour, producing a dark, washed-out preview.
//
// This module maps HDR frames down to a viewable SDR/BT.709 image on the CPU,
// working from 16-bit rgb48le data so highlight detail is preserved through
// the tone-map before being quantized to 8-bit. The math is self-contained:
// no FFMS2 or ffmpeg types are referenced here, so it compiles and is unit
// testable independently of the media backend.
//
// Algorithm:
//   1. Decode each 16-bit RGB channel through the source EOTF (PQ or HLG) to
//      linear light, normalized so 1.0 == reference white.
//   2. Apply scene-referred tone-mapping (extended Reinhardt with a soft
//      highlight knee) so super-bright highlights roll off instead of clipping.
//   3. Convert linear BT.2020 RGB to linear BT.709 RGB via a 3x3 matrix.
//   4. Apply a 2.4 display gamma (sRGB-ish) and quantize to 8-bit.
//
// This is a *preview* tone-map, not a reference HDR renderer: it exists so HDR
// footage can be opened and timed in Aegisub with recognizable colour, rather
// than a near-black frame.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace HDRTonemap {

// AVColorTransferCharacteristic / AVColorPrimaries values (libavutil). PQ and
// HLG are the two HDR transfer functions in the wild; BT.2020 is the wide
// colour gamut primaries used by essentially all UHD/HDR content.
constexpr int kTransferPQ        = 16;  // SMPTE ST 2084 (PQ / Dolby Vision HDR10)
constexpr int kTransferHLG       = 18;  // ARIB STD-B67 (HLG / BBC HDR)
constexpr int kPrimariesBT2020   = 9;
constexpr int kPrimariesBT709    = 1;

// Reference white levels (cd/m^2). 203 nits is the HDR reference white from
// BT.2408; we map it to SDR white so the preview's midtones land where a
// viewer expects them.
constexpr double kSdrWhiteNits   = 203.0;
constexpr double kPqPeakNits     = 10000.0;   // PQ is defined up to 10 000 nits

// --- SMPTE ST 2084 (PQ) EOTF constants (BT.1886 / SMPTE 2084-1) -----------
constexpr double kPqM1 = 0.1593017578125;
constexpr double kPqM2 = 78.84375;
constexpr double kPqC1 = 0.8359375;
constexpr double kPqC2 = 18.8515625;
constexpr double kPqC3 = 18.6875;

// --- ARIB STD-B67 (HLG) constants ----------------------------------------
// HLG OETF⁻¹ combined with the system gamma for a nominal 1000-nit display,
// matching the common reference used by players for SDR preview fallback.
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;

// Display gamma applied after tone-mapping (BT.1886-like / sRGB approximation).
constexpr double kDisplayGamma = 2.4;

// True when a source's transfer function is an HDR curve that needs tone-mapping.
// We key off the transfer function rather than the primaries: BT.2020 is also
// used by SDR UHD content, which must NOT be tone-mapped.
inline bool IsHDRSource(int transfer, int primaries) {
	(void)primaries;  // reserved for future per-gamut handling
	return transfer == kTransferPQ || transfer == kTransferHLG;
}

// SMPTE ST 2084 (PQ) inverse EOTF: normalized signal e ([0,1]) -> linear cd/m^2.
inline double PQEOTF(double e) {
	if (e <= 0.0)
		return 0.0;
	if (e >= 1.0)
		return kPqPeakNits;
	double ep = std::pow(e, 1.0 / kPqM2);
	double num = std::max(ep - kPqC1, 0.0);
	double den = kPqC2 - kPqC3 * ep;
	if (den <= 0.0)
		return kPqPeakNits;
	return kPqPeakNits * std::pow(num / den, 1.0 / kPqM1);
}

// HLG (ARIB STD-B67) scene-light to display-light, normalized so the OETF
// input range [0,1] maps to display light with 1.0 == reference white after
// applying the nominal 1000-nit system gamma. Input e is the OETF signal.
inline double HLGOOTF(double e) {
	if (e <= 0.0)
		return 0.0;
	double scene;  // scene-referred linear, relative to reference white
	if (e <= 0.5)
		scene = (e * e) / (3.0 * kHlgC);
	else
		scene = (std::exp((e - kHlgC) / kHlgA) + kHlgB) / 12.0;
	// System gamma for a ~1000 nit reference display: 1.2
	constexpr double kHlgSystemGamma = 1.2;
	return scene > 0.0 ? std::pow(scene, kHlgSystemGamma - 1.0) * scene : 0.0;
}

// Map a normalized linear-light value (relative to SDR white == 1.0) to a
// displayable [0,1] value using an extended Reinhardt operator with a soft
// highlight knee. Peak is the scene's max linear value (from MaxCLL when
// known, otherwise a sane default). This preserves midtone contrast while
// rolling off specular highlights instead of hard-clipping them.
inline double ToneMapReinhardt(double l, double peak) {
	if (l <= 0.0)
		return 0.0;
	// White point: anything at or above peak compresses toward 1.0.
	double wp = (peak > 0.0) ? peak : 1.0;
	// Extended Reinhardt: x' = x * (1 + x / wp^2) / (1 + x). Keeps midtones
	// nearly linear and gently compresses highlights.
	double num = l * (1.0 + l / (wp * wp));
	double den = 1.0 + l;
	double mapped = num / den;
	return std::min(mapped, 1.0);
}

// BT.2020 (linear) -> BT.709 (linear) primaries, 3x3 matrix.
// Source: ITU-R BT.2087 / standard chromatic adaptation. Applied per-channel
// after EOTF so the wide-gamut colour is brought into the sRGB/Rec.709 cube
// the 8-bit preview targets.
constexpr std::array<double, 9> kBT2020To709 = {
	1.6605, -0.5876, -0.0728,
	-0.1246, 1.1329, -0.0083,
	-0.0182, -0.1006, 1.1187
};

inline void ApplyGamut(double &r, double &g, double &b) {
	double nr = kBT2020To709[0] * r + kBT2020To709[1] * g + kBT2020To709[2] * b;
	double ng = kBT2020To709[3] * r + kBT2020To709[4] * g + kBT2020To709[5] * b;
	double nb = kBT2020To709[6] * r + kBT2020To709[7] * g + kBT2020To709[8] * b;
	r = nr; g = ng; b = nb;
}

// Convert a single 16-bit RGB pixel to tone-mapped 8-bit, writing BGRA.
// Inputs are 16-bit little-endian channel values (0..65535). Output writes
// B, G, R, A into dst[0..3] (A = 255) to match Aegisub's bgra8 layout.
inline void ToneMapPixel(uint16_t r16, uint16_t g16, uint16_t b16,
                         int transfer, int primaries, int max_cll,
                         uint8_t *dst) {
	auto to_signal = [](uint16_t v) -> double {
		return std::min(static_cast<double>(v) / 65535.0, 1.0);
	};

	double rs = to_signal(r16);
	double gs = to_signal(g16);
	double bs = to_signal(b16);

	// 1. Source EOTF -> linear cd/m^2.
	double rl, gl, bl;
	if (transfer == kTransferPQ) {
		rl = PQEOTF(rs);
		gl = PQEOTF(gs);
		bl = PQEOTF(bs);
	} else {  // HLG (or anything else we treat as HLG for preview)
		rl = HLGOOTF(rs) * kSdrWhiteNits * 3.0;
		gl = HLGOOTF(gs) * kSdrWhiteNits * 3.0;
		bl = HLGOOTF(bs) * kSdrWhiteNits * 3.0;
	}

	// Normalize so SDR reference white == 1.0.
	rl /= kSdrWhiteNits;
	gl /= kSdrWhiteNits;
	bl /= kSdrWhiteNits;

	// 2. Scene-referred tone-map, keyed off MaxCLL when available.
	double peak = (max_cll > 0 ? max_cll : 1000.0) / kSdrWhiteNits;
	rl = ToneMapReinhardt(rl, peak);
	gl = ToneMapReinhardt(gl, peak);
	bl = ToneMapReinhardt(bl, peak);

	// 3. Gamut compression BT.2020 -> BT.709 (only meaningful for wide gamut).
	if (primaries == kPrimariesBT2020)
		ApplyGamut(rl, gl, bl);

	// Clamp out-of-gamut values produced by the matrix.
	rl = std::clamp(rl, 0.0, 1.0);
	gl = std::clamp(gl, 0.0, 1.0);
	bl = std::clamp(bl, 0.0, 1.0);

	// 4. Display gamma + quantize to 8-bit.
	auto encode = [](double l) -> uint8_t {
		double v = std::pow(l, 1.0 / kDisplayGamma);
		return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
	};

	dst[0] = encode(bl);   // B
	dst[1] = encode(gl);   // G
	dst[2] = encode(rl);   // R
	dst[3] = 255;          // A
}

// Tone-map a full rgb48le frame (6 bytes/pixel: R16, G16, B16, little-endian)
// into an 8-bit BGRA buffer (4 bytes/pixel). This is the entry point used by
// the FFmpegSource provider when it has opened an HDR source at 16-bit output.
//
// `pixels` is width * height. dst must hold at least pixels * 4 bytes.
inline void ToneMapRGB48toBGRA8(const uint16_t *src, uint8_t *dst, size_t pixels,
                                int transfer, int primaries, int max_cll) {
	if (!src || !dst || pixels == 0)
		return;

	for (size_t i = 0; i < pixels; ++i) {
		// rgb48le layout per pixel: R_lo, R_hi, G_lo, G_hi, B_lo, B_hi.
		uint16_t r16 = src[i * 3 + 0];
		uint16_t g16 = src[i * 3 + 1];
		uint16_t b16 = src[i * 3 + 2];
		ToneMapPixel(r16, g16, b16, transfer, primaries, max_cll, dst + i * 4);
	}
}

}  // namespace HDRTonemap
