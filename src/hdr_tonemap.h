#pragma once

// Preview-grade HDR10/HLG to SDR tone mapping used by the FFmpegSource video
// provider. Dolby Vision reshaping is deliberately out of scope: an RPU is not
// a fixed colour matrix and profiles without a standard HDR base layer cannot
// be rendered correctly by this code.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace HDRTonemap {

constexpr int kTransferPQ = 16;   // SMPTE ST 2084
constexpr int kTransferHLG = 18;  // ARIB STD-B67

inline bool IsHDRSource(int transfer, int primaries) {
	(void)primaries;
	return transfer == kTransferPQ || transfer == kTransferHLG;
}

class ToneMapper {
	static constexpr size_t kLutSize = 65536;
	static constexpr size_t kHlgLutSize = 4097;

	int transfer_ = kTransferPQ;
	bool convert_bt2020_ = true;
	// True when the container's YCbCr matrix follows BT.2020 luma weights
	// (as opposed to BT.709). Only meaningful for the YUV decoding path.
	bool bt2020_matrix_ = true;
	// True for the usual video (limited/MPEG) range; false for full range.
	bool limited_range_ = true;
	float source_peak_nits_ = 1000.0f;
	std::array<float, kLutSize> eotf_{};
	std::array<float, kLutSize> srgb_codes_{};
	std::array<float, kHlgLutSize> hlg_scale_{};

	static float SRGBOETF(float linear) {
		linear = std::clamp(linear, 0.0f, 1.0f);
		return linear <= 0.0031308f ? 12.92f * linear
			: 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
	}

	float HLGScale(float scene_luma) const {
		scene_luma = std::clamp(scene_luma, 0.0f, 1.0f);
		float pos = scene_luma * (kHlgLutSize - 1);
		size_t lo = static_cast<size_t>(pos);
		size_t hi = std::min(lo + 1, kHlgLutSize - 1);
		float fraction = pos - lo;
		return hlg_scale_[lo] + (hlg_scale_[hi] - hlg_scale_[lo]) * fraction;
	}

	static void CompressGamut(float luma, float& r, float& g, float& b) {
		luma = std::clamp(luma, 0.0f, 1.0f);
		float amount = 1.0f;
		for (float channel : {r, g, b}) {
			if (channel < 0.0f && channel < luma)
				amount = std::min(amount, luma / (luma - channel));
			else if (channel > 1.0f && channel > luma)
				amount = std::min(amount, (1.0f - luma) / (channel - luma));
		}
		r = luma + amount * (r - luma);
		g = luma + amount * (g - luma);
		b = luma + amount * (b - luma);
	}

	// Linear-domain BT.2020 -> BT.709 primaries, applied before tone mapping.
	static void ApplyGamut(float& r, float& g, float& b) {
		float nr =  1.660491f * r - 0.587641f * g - 0.072850f * b;
		float ng = -0.124550f * r + 1.132900f * g - 0.008349f * b;
		float nb = -0.018151f * r - 0.100579f * g + 1.118730f * b;
		r = nr; g = ng; b = nb;
	}

	float ToneMapLuma(float nits) const {
		if (nits <= 0.0f) return 0.0f;
		// Extended Reinhard in units of SDR reference white (203 nit). It is
		// continuous and monotonic, reaches display white at the trusted source
		// peak, and retains highlight separation above reference white.
		constexpr float reference_white = 203.0f;
		float x = nits / reference_white;
		float white = source_peak_nits_ / reference_white;
		return x * (1.0f + x / (white * white)) / (1.0f + x);
	}

	static uint16_t ReadLE16(uint8_t const* data) {
		return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
	}

public:
	// transfer:      PQ or HLG (AVColorTransferCharacteristic value).
	// convert_bt2020: source uses BT.2020 primaries (needs gamut mapping).
	// max_cll:       content light level, used as the PQ highlight anchor.
	// bt2020_matrix: container YCbCr matrix follows BT.2020 luma weights.
	// limited_range: video (MPEG) range — the overwhelmingly common case.
	ToneMapper(int transfer = kTransferPQ, bool convert_bt2020 = true, int max_cll = 0,
		bool bt2020_matrix = true, bool limited_range = true)
	: transfer_(transfer)
	, convert_bt2020_(convert_bt2020)
	, bt2020_matrix_(bt2020_matrix)
	, limited_range_(limited_range)
	{
		// Content-light metadata is frequently absent or malformed. Values below
		// SDR reference white and values outside PQ's defined range are ignored.
		// PQ is absolute-light content, so a trustworthy MaxCLL can define the
		// highlight shoulder. HLG is scene-referred; this preview deliberately
		// uses the nominal BT.2100 1000-nit OOTF below, so applying MaxCLL only
		// to its shoulder would make the two halves of that transform disagree.
		if (transfer_ == kTransferPQ && max_cll >= 203 && max_cll <= 10000)
			source_peak_nits_ = static_cast<float>(max_cll);

		for (size_t i = 0; i < kLutSize; ++i) {
			float code = static_cast<float>(i) / (kLutSize - 1);
			eotf_[i] = transfer_ == kTransferHLG ? HLGInverseOETF(code) : PQEOTF(code);
			srgb_codes_[i] = 255.0f * SRGBOETF(code);
		}
		for (size_t i = 0; i < kHlgLutSize; ++i) {
			float luma = static_cast<float>(i) / (kHlgLutSize - 1);
			// BT.2100 HLG system gamma for a nominal 1000-nit display is 1.2.
			hlg_scale_[i] = 1000.0f * std::pow(luma, 0.2f);
		}
	}

	static float PQEOTF(float code) {
		constexpr float m1 = 2610.0f / 16384.0f;
		constexpr float m2 = 2523.0f / 32.0f;
		constexpr float c1 = 3424.0f / 4096.0f;
		constexpr float c2 = 2413.0f / 128.0f;
		constexpr float c3 = 2392.0f / 128.0f;
		code = std::clamp(code, 0.0f, 1.0f);
		float p = std::pow(code, 1.0f / m2);
		float numerator = std::max(p - c1, 0.0f);
		return 10000.0f * std::pow(numerator / (c2 - c3 * p), 1.0f / m1);
	}

	static float HLGInverseOETF(float code) {
		constexpr float a = 0.17883277f;
		constexpr float b = 0.28466892f;
		constexpr float c = 0.55991073f;
		code = std::clamp(code, 0.0f, 1.0f);
		return code <= 0.5f ? code * code / 3.0f
			: (std::exp((code - c) / a) + b) / 12.0f;
	}

	uint8_t EncodeLinear(float linear, int x, int y) const {
		static constexpr uint8_t bayer[8][8] = {
			{ 0, 48, 12, 60,  3, 51, 15, 63},
			{32, 16, 44, 28, 35, 19, 47, 31},
			{ 8, 56,  4, 52, 11, 59,  7, 55},
			{40, 24, 36, 20, 43, 27, 39, 23},
			{ 2, 50, 14, 62,  1, 49, 13, 61},
			{34, 18, 46, 30, 33, 17, 45, 29},
			{10, 58,  6, 54,  9, 57,  5, 53},
			{42, 26, 38, 22, 41, 25, 37, 21},
		};
		linear = std::clamp(linear, 0.0f, 1.0f);
		size_t index = static_cast<size_t>(linear * (kLutSize - 1) + 0.5f);
		float dither = (static_cast<float>(bayer[y & 7][x & 7]) + 0.5f) / 64.0f - 0.5f;
		return static_cast<uint8_t>(std::clamp(std::floor(srgb_codes_[index] + dither + 0.5f), 0.0f, 255.0f));
	}

	void ToneMapPixel(uint16_t r16, uint16_t g16, uint16_t b16, int x, int y,
		uint8_t& b8, uint8_t& g8, uint8_t& r8) const {
		float r = eotf_[r16];
		float g = eotf_[g16];
		float b = eotf_[b16];

		if (transfer_ == kTransferHLG) {
			float scene_luma = 0.2627f * r + 0.6780f * g + 0.0593f * b;
			float scale = HLGScale(scene_luma);
			r *= scale; g *= scale; b *= scale;
		}

		if (convert_bt2020_) {
			float rr =  1.660491f * r - 0.587641f * g - 0.072850f * b;
			float gg = -0.124550f * r + 1.132900f * g - 0.008349f * b;
			float bb = -0.018151f * r - 0.100579f * g + 1.118730f * b;
			r = rr; g = gg; b = bb;
		}

		float luma_nits = std::max(0.0f, 0.2126f * r + 0.7152f * g + 0.0722f * b);
		float mapped_luma = ToneMapLuma(luma_nits);
		if (luma_nits > 1e-8f) {
			float scale = mapped_luma / luma_nits;
			r *= scale; g *= scale; b *= scale;
		}
		else {
			r = g = b = 0.0f;
		}
		CompressGamut(mapped_luma, r, g, b);
		r8 = EncodeLinear(r, x, y);
		g8 = EncodeLinear(g, x, y);
		b8 = EncodeLinear(b, x, y);
	}

	void ToneMapRGB48toBGRA8(uint8_t const* source, ptrdiff_t source_stride,
		uint8_t* destination, ptrdiff_t destination_stride, int width, int height) const {
		for (int y = 0; y < height; ++y) {
			auto src = source + y * source_stride;
			auto dst = destination + y * destination_stride;
			for (int x = 0; x < width; ++x) {
				uint8_t b, g, r;
				ToneMapPixel(ReadLE16(src + 0), ReadLE16(src + 2), ReadLE16(src + 4), x, y, b, g, r);
				dst[4 * x + 0] = b;
				dst[4 * x + 1] = g;
				dst[4 * x + 2] = r;
				dst[4 * x + 3] = 255;
				src += 6;
			}
		}
	}

	// --- Reference decoding path (BT.2100 order) ---
	//
	// ToneMapRGB48toBGRA8 consumes swscale RGB that was produced by applying
	// the YCbCr -> RGB matrix in the *transfer-encoded* (PQ/HLG) domain, which
	// is not the ITU-defined decoding order and introduces hue/luma errors on
	// saturated colours. The path below receives the untouched YCbCr planes
	// and performs the reference sequence instead:
	//   1. video-range -> full-range expansion
	//   2. inverse EOTF per channel (Y, Cb, Cr) to linear-light signals
	//   3. linear-domain YCbCr -> RGB matrix for the source primaries
	//   4. (HLG) scene -> display OOTF
	//   5. tone map, gamut compress, encode — shared with the RGB path.

	// ToneMapYUV444P16toBGRA8: three contiguous 16-bit planes (Y, Cb, Cr),
	// each row-strided independently.
	void ToneMapYUV444P16toBGRA8(
		uint8_t const* y_plane, ptrdiff_t y_stride,
		uint8_t const* u_plane, ptrdiff_t u_stride,
		uint8_t const* v_plane, ptrdiff_t v_stride,
		uint8_t* destination, ptrdiff_t destination_stride, int width, int height) const {
		for (int y = 0; y < height; ++y) {
			auto src_y = reinterpret_cast<uint8_t const*>(y_plane) + y * y_stride;
			auto src_u = reinterpret_cast<uint8_t const*>(u_plane) + y * u_stride;
			auto src_v = reinterpret_cast<uint8_t const*>(v_plane) + y * v_stride;
			auto dst = destination + y * destination_stride;
			for (int x = 0; x < width; ++x) {
				uint8_t b, g, r;
				ToneMapYuvPixel(
					ReadLE16(src_y + 2 * x),
					ReadLE16(src_u + 2 * x),
					ReadLE16(src_v + 2 * x),
					x, y, b, g, r);
				dst[4 * x + 0] = b;
				dst[4 * x + 1] = g;
				dst[4 * x + 2] = r;
				dst[4 * x + 3] = 255;
			}
		}
	}

private:
	// Expand a video-range 16-bit code to full-range normalised [0,1].
	// 10-bit limited (64..940) arrives shifted <<6 into 16 bits, so the
	// boundaries here are 4096 / 60160 for luma and 32768 +/- 28672 for chroma.
	float ExpandLuma(uint16_t v) const {
		if (limited_range_)
			return std::clamp((static_cast<float>(v) - 4096.0f) * (1.0f / 56064.0f), 0.0f, 1.0f);
		return static_cast<float>(v) * (1.0f / 65535.0f);
	}

	// Chroma is centred at mid-scale (video-range 32768) and spans
	// [-0.5, 0.5] after normalisation, so the inverse EOTF input needs a
	// 0.5 offset back into [0,1].
	float ExpandChroma(uint16_t v) const {
		if (limited_range_)
			return std::clamp((static_cast<float>(v) - 32768.0f) * (1.0f / 57344.0f) + 0.5f, 0.0f, 1.0f);
		return std::clamp((static_cast<float>(v) * (1.0f / 65535.0f)) + 0.5f, 0.0f, 1.0f);
	}

	// Linear-domain YCbCr -> RGB for the source primaries, normalised chroma.
	// BT.2020: Kr=0.2627 Kb=0.0593. BT.709: Kr=0.2126 Kb=0.0722.
	void YcbcrToLinearRgb(float y_linear, float cb_linear, float cr_linear,
		float& r, float& g, float& b) const {
		float u = cb_linear - 0.5f;
		float v = cr_linear - 0.5f;
		if (bt2020_matrix_) {
			r = y_linear + 1.474600f * v;
			b = y_linear + 1.881400f * u;
			g = y_linear - 0.164541f * u - 0.571349f * v;
		}
		else {
			r = y_linear + 1.574800f * v;
			b = y_linear + 1.855600f * u;
			g = y_linear - 0.187000f * u - 0.468000f * v;
		}
	}

public:
	// YUV path entry: consumes PQ/HLG-encoded YCbCr and produces display SDR.
	void ToneMapYuvPixel(uint16_t y16, uint16_t cb16, uint16_t cr16,
		int x, int y, uint8_t& b8, uint8_t& g8, uint8_t& r8) const {
		// 1. Range expansion + inverse EOTF per channel to linear signals.
		float y_lin = eotf_[ExpandLuma(y16)];
		float cb_lin = eotf_[ExpandChroma(cb16)];
		float cr_lin = eotf_[ExpandChroma(cr16)];

		// 2. Linear-domain matrix to source-primary RGB.
		float r, g, b;
		YcbcrToLinearRgb(y_lin, cb_lin, cr_lin, r, g, b);

		// 3. HLG scene-referred signal needs the OOTF to become display light.
		//    (For HLG the EOTF output above is scene-linear; for PQ it is
		//    already absolute display light, so this step is PQ-only skipped.)
		if (transfer_ == kTransferHLG) {
			float scene_luma = 0.2627f * r + 0.6780f * g + 0.0593f * b;
			float scale = HLGScale(std::clamp(scene_luma, 0.0f, 1.0f));
			r *= scale; g *= scale; b *= scale;
		}

		// 4. Wide gamut -> BT.709 primaries (linear domain).
		if (convert_bt2020_)
			ApplyGamut(r, g, b);

		// 5. Luminance-preserving tone map — one scale factor for all three
		//    channels, so hue is exactly preserved (no chroma break-up).
		float luma_nits = std::max(0.0f, 0.2126f * r + 0.7152f * g + 0.0722f * b);
		float mapped = ToneMapLuma(luma_nits);
		if (luma_nits > 1e-8f) {
			float scale = mapped / luma_nits;
			r *= scale; g *= scale; b *= scale;
		}
		else {
			r = g = b = 0.0f;
		}

		CompressGamut(mapped, r, g, b);
		b8 = EncodeLinear(b, x, y);
		g8 = EncodeLinear(g, x, y);
		r8 = EncodeLinear(r, x, y);
	}
};

} // namespace HDRTonemap
