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
// PERFORMANCE
// The naive per-pixel formulation calls std::pow ~9 times per pixel (PQ EOTF
// + display gamma), and a 4K frame has ~8.3M pixels — that is tens of millions
// of transcendental ops per frame and makes preview playback unusably slow.
// We eliminate essentially all of that with two precomputed lookup tables
// built once per source in BuildToneMapper():
//   * eotf_lut_[65536] maps every possible 16-bit channel value straight to a
//     tone-mapped linear-light value, folding in the PQ/HLG EOTF, reference
//     white normalization and the Reinhardt tone-map. 65536 floats = 256KB,
//     built once, reused for every frame.
//   * gamma_lut_[1024] maps a quantized linear value back to an 8-bit display
//     code, folding in the 1/gamma encoding.
// The BT.2020->BT.709 gamut step cannot be folded into a 1D LUT (it mixes
// channels), so it stays as 9 multiplies per pixel — cheap relative to the
// transcendentals it replaces.
// After LUT-ification each pixel is: 3 table reads + 9 multiplies + 3 clamps
// + 3 table reads, i.e. no transcendentals at all in the hot loop.
//
// This is a *preview* tone-map, not a reference HDR renderer: it exists so HDR
// footage can be opened and timed in Aegisub with recognizable colour, rather
// than a near-black frame.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>

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
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;

// Display gamma applied after tone-mapping (BT.1886-like / sRGB approximation).
constexpr double kDisplayGamma = 2.4;

// Linear-light resolution used for the inverse (gamma) LUT. 1024 steps is far
// finer than 8-bit output and keeps the LUT tiny (1KB).
constexpr int kGammaLutSize = 1024;

// BT.2020 (linear) -> BT.709 (linear) primaries, 3x3 matrix.
// Source: ITU-R BT.2087 / standard chromatic adaptation. Applied per-channel
// after EOTF so the wide-gamut colour is brought into the sRGB/Rec.709 cube
// the 8-bit preview targets.
constexpr std::array<double, 9> kBT2020To709 = {
	1.6605, -0.5876, -0.0728,
	-0.1246, 1.1329, -0.0083,
	-0.0182, -0.1006, 1.1187
};

// True when a source's transfer function is an HDR curve that needs tone-mapping.
// We key off the transfer function rather than the primaries: BT.2020 is also
// used by SDR UHD content, which must NOT be tone-mapped.
inline bool IsHDRSource(int transfer, int primaries) {
	(void)primaries;  // reserved for future per-gamut handling
	return transfer == kTransferPQ || transfer == kTransferHLG;
}

// --- Scalar EOTF / tone-map primitives (used to build the LUTs) -----------

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

// Apply the BT.2020 -> BT.709 gamut matrix in place. Cheap (9 mul), kept as
// scalar math because it mixes channels and cannot be folded into a 1D LUT.
inline void ApplyGamut(double &r, double &g, double &b) {
	double nr = kBT2020To709[0] * r + kBT2020To709[1] * g + kBT2020To709[2] * b;
	double ng = kBT2020To709[3] * r + kBT2020To709[4] * g + kBT2020To709[5] * b;
	double nb = kBT2020To709[6] * r + kBT2020To709[7] * g + kBT2020To709[8] * b;
	r = nr; g = ng; b = nb;
}

/// Precomputed tables for one HDR source. Build once (BuildToneMapper) and
/// reuse for every frame of that source; building is ~65536 pow calls which
/// is ~5ms, paid once at open time instead of per frame.
struct ToneMapper {
	/// eotf_lut[v] = tone-mapped linear-light value for a 16-bit channel
	/// input v (0..65535). Folds in EOTF + reference-white normalization +
	/// Reinhardt tone-map so the per-frame loop needs no transcendentals.
	std::vector<float> eotf_lut;  // size 65536

	/// gamma_lut[i] = 8-bit display code for a quantized linear value
	/// i / (kGammaLutSize - 1). Folds in the 1/gamma encoding.
	std::vector<uint8_t> gamma_lut;  // size kGammaLutSize

	int transfer = -1;
	int primaries = -1;
	bool needs_gamut = false;

	/// Look up the tone-mapped linear value for a 16-bit channel.
	inline float EOTF(uint16_t v) const { return eotf_lut[v]; }

	/// Encode a clamped [0,1] linear value to 8-bit via the gamma LUT.
	inline uint8_t Encode(double l) const {
		int idx = static_cast<int>(std::clamp(l, 0.0, 1.0) * (kGammaLutSize - 1) + 0.5);
		return gamma_lut[idx];
	}
};

/// Build the LUTs for a given source. Expensive (~5ms) but called once per
/// opened video, not per frame. Returns a ready ToneMapper; for non-HDR
/// sources the result is unused (the provider keeps the bgra fast path).
inline ToneMapper BuildToneMapper(int transfer, int primaries, int max_cll) {
	ToneMapper tm;
	tm.transfer = transfer;
	tm.primaries = primaries;
	tm.needs_gamut = (primaries == kPrimariesBT2020);

	// Tone-map peak (scene-relative), from MaxCLL when available.
	const double peak = (max_cll > 0 ? static_cast<double>(max_cll) : 1000.0) / kSdrWhiteNits;

	tm.eotf_lut.resize(65536);
	for (int v = 0; v < 65536; ++v) {
		double e = static_cast<double>(v) / 65535.0;
		double lin;
		if (transfer == kTransferPQ)
			lin = PQEOTF(e) / kSdrWhiteNits;
		else  // HLG (or anything else we treat as HLG for preview)
			lin = HLGOOTF(e) * kSdrWhiteNits * 3.0 / kSdrWhiteNits;
		tm.eotf_lut[v] = static_cast<float>(ToneMapReinhardt(lin, peak));
	}

	// Gamma LUT over the post-tone-map [0,1] range.
	tm.gamma_lut.resize(kGammaLutSize);
	for (int i = 0; i < kGammaLutSize; ++i) {
		double l = static_cast<double>(i) / (kGammaLutSize - 1);
		double v = std::pow(l, 1.0 / kDisplayGamma);
		tm.gamma_lut[i] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
	}

	return tm;
}

/// Tone-map a full rgb48le frame (6 bytes/pixel) into an 8-bit BGRA buffer
/// (4 bytes/pixel) using a prebuilt ToneMapper. This is the per-frame hot
/// path: no std::pow calls, just table reads + a gamut matrix multiply.
///
/// `pixels` is width * height. dst must hold at least pixels * 4 bytes.
inline void ToneMapRGB48toBGRA8(const ToneMapper &tm,
                                const uint16_t *src, uint8_t *dst, size_t pixels) {
	if (!src || !dst || pixels == 0)
		return;

	const bool gamut = tm.needs_gamut;
	for (size_t i = 0; i < pixels; ++i) {
		// rgb48le layout per pixel: R16, G16, B16 (little-endian).
		float rf = tm.EOTF(src[i * 3 + 0]);
		float gf = tm.EOTF(src[i * 3 + 1]);
		float bf = tm.EOTF(src[i * 3 + 2]);

		double r = rf, g = gf, b = bf;
		if (gamut)
			ApplyGamut(r, g, b);

		r = std::clamp(r, 0.0, 1.0);
		g = std::clamp(g, 0.0, 1.0);
		b = std::clamp(b, 0.0, 1.0);

		dst[i * 4 + 0] = tm.Encode(b);   // B
		dst[i * 4 + 1] = tm.Encode(g);   // G
		dst[i * 4 + 2] = tm.Encode(r);   // R
		dst[i * 4 + 3] = 255;            // A
	}
}

}  // namespace HDRTonemap
