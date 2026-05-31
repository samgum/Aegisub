#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace AudioSampleSafety {
using Sample = std::int16_t;

constexpr double kInt16Min = -32768.0;
constexpr double kInt16Max = 32767.0;
constexpr double kPreviewCeiling = kInt16Max * 0.92;
constexpr float kSoundTouchInputHeadroom = 0.98f;
constexpr double kMaxPreviewVolume = 8.0;

inline double CleanVolume(double volume) {
	if (!std::isfinite(volume))
		return 1.0;
	return std::clamp(volume, 0.0, kMaxPreviewVolume);
}

inline Sample ToInt16(double sample) {
	return static_cast<Sample>(std::lround(std::clamp(sample, kInt16Min, kInt16Max)));
}

inline void ApplyGainLimiter(Sample *samples, size_t sample_count, double volume) {
	if (!samples || sample_count == 0)
		return;

	double gain = CleanVolume(volume);
	double peak = 0.0;
	for (size_t i = 0; i < sample_count; ++i)
		peak = std::max(peak, std::abs(static_cast<double>(samples[i]) * gain));

	if (peak > kPreviewCeiling)
		gain *= kPreviewCeiling / peak;

	for (size_t i = 0; i < sample_count; ++i)
		samples[i] = ToInt16(static_cast<double>(samples[i]) * gain);
}

inline void ConvertFloatToInt16Limited(Sample *dst, float const *src, size_t sample_count, double volume) {
	if (!dst || !src || sample_count == 0)
		return;

	double gain = CleanVolume(volume) * kInt16Max;
	double peak = 0.0;
	for (size_t i = 0; i < sample_count; ++i)
		peak = std::max(peak, std::abs(static_cast<double>(src[i]) * gain));

	if (peak > kPreviewCeiling)
		gain *= kPreviewCeiling / peak;

	for (size_t i = 0; i < sample_count; ++i)
		dst[i] = ToInt16(static_cast<double>(src[i]) * gain);
}
}
