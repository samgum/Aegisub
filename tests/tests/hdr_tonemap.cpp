#include "../../src/hdr_tonemap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

namespace {

uint16_t EncodePQ(float nits) {
	float low = 0.0f;
	float high = 1.0f;
	for (int i = 0; i < 24; ++i) {
		float middle = (low + high) * 0.5f;
		if (HDRTonemap::ToneMapper::PQEOTF(middle) < nits)
			low = middle;
		else
			high = middle;
	}
	return static_cast<uint16_t>((low + high) * 32767.5f);
}

void StoreLE16(uint8_t *destination, uint16_t value) {
	destination[0] = static_cast<uint8_t>(value);
	destination[1] = static_cast<uint8_t>(value >> 8);
}

TEST(HDRToneMap, TransferFunctionsHaveCorrectEndpointsAndAreMonotonic) {
	EXPECT_FLOAT_EQ(HDRTonemap::ToneMapper::PQEOTF(0.0f), 0.0f);
	EXPECT_NEAR(HDRTonemap::ToneMapper::PQEOTF(1.0f), 10000.0f, 1.0f);
	EXPECT_FLOAT_EQ(HDRTonemap::ToneMapper::HLGInverseOETF(0.0f), 0.0f);
	EXPECT_NEAR(HDRTonemap::ToneMapper::HLGInverseOETF(0.5f), 1.0f / 12.0f, 1e-6f);
	EXPECT_NEAR(HDRTonemap::ToneMapper::HLGInverseOETF(1.0f), 1.0f, 1e-5f);

	float previous_pq = -1.0f;
	float previous_hlg = -1.0f;
	for (int i = 0; i <= 1000; ++i) {
		float code = i / 1000.0f;
		float pq = HDRTonemap::ToneMapper::PQEOTF(code);
		float hlg = HDRTonemap::ToneMapper::HLGInverseOETF(code);
		EXPECT_GE(pq, previous_pq);
		EXPECT_GE(hlg, previous_hlg);
		previous_pq = pq;
		previous_hlg = hlg;
	}
}

TEST(HDRToneMap, NeutralPixelsRemainNeutralForPQAndHLG) {
	for (int transfer : {HDRTonemap::kTransferPQ, HDRTonemap::kTransferHLG}) {
		HDRTonemap::ToneMapper mapper(transfer, true, 1000);
		for (uint16_t value : {uint16_t{0}, uint16_t{16384}, uint16_t{32768}, uint16_t{65535}}) {
			uint8_t b, g, r;
			mapper.ToneMapPixel(value, value, value, 3, 4, b, g, r);
			EXPECT_LE(std::max({r, g, b}) - std::min({r, g, b}), 1);
		}
	}
}

TEST(HDRToneMap, HLGUsesOneConsistentNominalDisplayTransform) {
	auto nominal = std::make_unique<HDRTonemap::ToneMapper>(HDRTonemap::kTransferHLG, true, 1000);
	auto misleading_metadata = std::make_unique<HDRTonemap::ToneMapper>(HDRTonemap::kTransferHLG, true, 4000);
	for (uint16_t value : {uint16_t{8192}, uint16_t{32768}, uint16_t{57344}}) {
		uint8_t nominal_b, nominal_g, nominal_r;
		uint8_t metadata_b, metadata_g, metadata_r;
		nominal->ToneMapPixel(value, value, value, 2, 5, nominal_b, nominal_g, nominal_r);
		misleading_metadata->ToneMapPixel(value, value, value, 2, 5, metadata_b, metadata_g, metadata_r);
		EXPECT_EQ(nominal_b, metadata_b);
		EXPECT_EQ(nominal_g, metadata_g);
		EXPECT_EQ(nominal_r, metadata_r);
	}
}

TEST(HDRToneMap, HighlightShoulderRetainsSeparationPastReferenceWhite) {
	HDRTonemap::ToneMapper mapper(HDRTonemap::kTransferPQ, false, 1000);
	std::array<int, 4> results{};
	std::array<float, 4> nits = {203.0f, 400.0f, 700.0f, 1000.0f};
	for (size_t i = 0; i < nits.size(); ++i) {
		uint16_t value = EncodePQ(nits[i]);
		uint8_t b, g, r;
		mapper.ToneMapPixel(value, value, value, 0, 0, b, g, r);
		results[i] = r;
	}
	EXPECT_LT(results[0], results[1]);
	EXPECT_LT(results[1], results[2]);
	EXPECT_LT(results[2], results[3]);
}

TEST(HDRToneMap, DitherUsesOnlyAdjacentCodesAndActuallyVaries) {
	HDRTonemap::ToneMapper mapper;
	bool found_varying_value = false;
	for (int sample = 1; sample < 65535 && !found_varying_value; sample += 17) {
		float linear = sample / 65535.0f;
		std::set<int> codes;
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x)
				codes.insert(mapper.EncodeLinear(linear, x, y));
		ASSERT_LE(codes.size(), 2u);
		if (codes.size() == 2) {
			EXPECT_EQ(*codes.rbegin() - *codes.begin(), 1);
			found_varying_value = true;
		}
	}
	EXPECT_TRUE(found_varying_value);
}

TEST(HDRToneMap, RGB48ConversionHonoursBothStridesAndProducesBGRA) {
	constexpr int width = 3;
	constexpr int height = 2;
	constexpr int source_stride = width * 6 + 7;
	constexpr int destination_stride = width * 4 + 5;
	std::vector<uint8_t> source(source_stride * height, 0xcd);
	std::vector<uint8_t> destination(destination_stride * height, 0xa5);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			uint16_t value = EncodePQ(100.0f * (x + 1) + 50.0f * y);
			auto pixel = source.data() + y * source_stride + x * 6;
			StoreLE16(pixel + 0, value);
			StoreLE16(pixel + 2, value);
			StoreLE16(pixel + 4, value);
		}
	}

	HDRTonemap::ToneMapper mapper(HDRTonemap::kTransferPQ, false, 1000);
	mapper.ToneMapRGB48toBGRA8(source.data(), source_stride, destination.data(), destination_stride, width, height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto pixel = destination.data() + y * destination_stride + x * 4;
			EXPECT_NEAR(pixel[0], pixel[1], 1);
			EXPECT_NEAR(pixel[1], pixel[2], 1);
			EXPECT_EQ(pixel[3], 255);
		}
		for (int i = width * 4; i < destination_stride; ++i)
			EXPECT_EQ(destination[y * destination_stride + i], 0xa5);
	}

	std::vector<uint8_t> reversed(destination_stride * height, 0xa5);
	mapper.ToneMapRGB48toBGRA8(source.data() + source_stride * (height - 1), -source_stride,
		reversed.data(), destination_stride, width, height);
	for (int x = 0; x < width; ++x) {
		for (int channel = 0; channel < 3; ++channel) {
			EXPECT_NEAR(reversed[x * 4 + channel],
				destination[destination_stride + x * 4 + channel], 1);
			EXPECT_NEAR(reversed[destination_stride + x * 4 + channel],
				destination[x * 4 + channel], 1);
		}
	}
}

TEST(HDRToneMap, SaturatedBT2020InputIsCompressedIntoLegalOutput) {
	HDRTonemap::ToneMapper mapper(HDRTonemap::kTransferPQ, true, 1000);
	uint8_t b, g, r;
	mapper.ToneMapPixel(EncodePQ(1000.0f), 0, 0, 1, 1, b, g, r);
	EXPECT_GT(r, g);
	EXPECT_GT(r, b);
	EXPECT_FALSE(r == 255 && g == 255 && b == 255);
}

// Compare against independently reconstructed transfer-encoded RGB. Checking
// only legal byte ranges would miss an entirely black or tinted YUV path.
TEST(HDRToneMap, YuvNeutralRampMatchesRGBForBothRangesAndTransfers) {
	for (int transfer : {HDRTonemap::kTransferPQ, HDRTonemap::kTransferHLG}) {
		for (bool limited : {false, true}) {
			HDRTonemap::ToneMapper mapper(transfer, true, 1000, true, limited);
			int last = -1;
			for (int step = 0; step <= 16; ++step) {
				float code = step / 16.0f;
				uint16_t y = static_cast<uint16_t>(limited ? 4096 + code * 56064 : code * 65535);
				uint16_t rgb = static_cast<uint16_t>(code * 65535 + 0.5f);
				uint8_t b, g, r, ref_b, ref_g, ref_r;
				mapper.ToneMapYuvPixel(y, 32768, 32768, 0, 0, b, g, r);
				mapper.ToneMapPixel(rgb, rgb, rgb, 0, 0, ref_b, ref_g, ref_r);
				EXPECT_NEAR(b, ref_b, 1);
				EXPECT_NEAR(g, ref_g, 1);
				EXPECT_NEAR(r, ref_r, 1);
				EXPECT_GE(r, last);
				last = r;
			}
			EXPECT_EQ(last, 255);
		}
	}
}

TEST(HDRToneMap, YuvNclColoursReconstructRGBBeforeInverseTransfer) {
	for (int transfer : {HDRTonemap::kTransferPQ, HDRTonemap::kTransferHLG}) {
		for (bool bt2020 : {false, true}) {
			for (bool limited : {false, true}) {
				HDRTonemap::ToneMapper mapper(transfer, bt2020, 1000, bt2020, limited);
				double kr = bt2020 ? 0.2627 : 0.2126;
				double kb = bt2020 ? 0.0593 : 0.0722;
				for (auto rgb : {std::array<double, 3>{0.7, 0.4, 0.2}, {0.2, 0.6, 0.4}, {0.4, 0.2, 0.7}}) {
					double y = kr * rgb[0] + (1 - kr - kb) * rgb[1] + kb * rgb[2];
					double cb = (rgb[2] - y) / (2 * (1 - kb));
					double cr = (rgb[0] - y) / (2 * (1 - kr));
					uint8_t b, g, r, ref_b, ref_g, ref_r;
					mapper.ToneMapYuvPixel(static_cast<uint16_t>(std::lround(limited ? 4096 + y * 56064 : y * 65535)),
						static_cast<uint16_t>(std::lround(32768 + cb * (limited ? 57344 : 65535))),
						static_cast<uint16_t>(std::lround(32768 + cr * (limited ? 57344 : 65535))),
						3, 4, b, g, r);
					mapper.ToneMapPixel(static_cast<uint16_t>(std::lround(rgb[0] * 65535)),
						static_cast<uint16_t>(std::lround(rgb[1] * 65535)), static_cast<uint16_t>(std::lround(rgb[2] * 65535)),
						3, 4, ref_b, ref_g, ref_r);
					EXPECT_NEAR(b, ref_b, 1);
					EXPECT_NEAR(g, ref_g, 1);
					EXPECT_NEAR(r, ref_r, 1);
				}
			}
		}
	}
}

TEST(HDRToneMap, YuvPlanesHonourIndependentAndNegativeStrides) {
	std::array<uint8_t, 12> yp{}, up{}, vp{};
	std::array<uint8_t, 16> output;
	output.fill(0xa5);
	StoreLE16(yp.data(), 4096);
	StoreLE16(yp.data() + 6, 60160);
	StoreLE16(up.data(), 32768);
	StoreLE16(up.data() + 8, 32768);
	StoreLE16(vp.data(), 32768);
	StoreLE16(vp.data() + 10, 32768);
	HDRTonemap::ToneMapper mapper;
	mapper.ToneMapYUV444P16toBGRA8(yp.data() + 6, -6, up.data() + 8, -8,
		vp.data() + 10, -10, output.data(), 8, 1, 2);
	for (int channel = 0; channel < 3; ++channel) {
		EXPECT_EQ(output[channel], 255);
		EXPECT_EQ(output[8 + channel], 0);
	}
	EXPECT_EQ(output[3], 255);
	EXPECT_EQ(output[11], 255);
	EXPECT_EQ(output[4], 0xa5);
	EXPECT_EQ(output[12], 0xa5);
}

} // namespace
