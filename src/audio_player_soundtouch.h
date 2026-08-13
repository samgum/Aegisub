#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace agi { class AudioProvider; }

#ifdef WITH_SOUNDTOUCH

namespace soundtouch { class SoundTouch; }

class SoundTouchAudioProcessor {
	agi::AudioProvider *provider;
	std::unique_ptr<soundtouch::SoundTouch> processor;
	std::vector<int16_t> source_buffer;
	std::vector<float> process_buffer;
	std::vector<float> output_buffer;

	int64_t input_frame = 0;
	double output_frame = 0.0;
	int64_t end_frame = 0;
	double playback_speed = 1.0;
	std::atomic_flag volume_guard = ATOMIC_FLAG_INIT;
	double requested_volume = 1.0;
	double active_volume = 1.0;
	bool input_finished = true;
	bool flushed = true;
	bool output_finished = true;

	int channels() const;
	double volume_for_fill();
	void feed_more();

public:
	explicit SoundTouchAudioProcessor(agi::AudioProvider *provider);
	~SoundTouchAudioProcessor();

	void Reset(int64_t start, int64_t end, double speed, double volume);
	void SetEndFrame(int64_t end);
	void SetVolume(double volume);
	void SetPlaybackSpeed(double speed);

	int64_t GetInputFrame() const { return input_frame; }
	int64_t GetOutputFrame() const;
	bool IsFinished() const { return output_finished; }

	size_t Fill(void *dst, size_t frames);
};

#endif // WITH_SOUNDTOUCH
