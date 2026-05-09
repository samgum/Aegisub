#pragma once

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
	int64_t end_frame = 0;
	double playback_speed = 1.0;
	double volume = 1.0;
	bool input_finished = true;
	bool flushed = true;
	bool output_finished = true;

	int channels() const;
	void feed_more();

public:
	explicit SoundTouchAudioProcessor(agi::AudioProvider *provider);
	~SoundTouchAudioProcessor();

	void Reset(int64_t start, int64_t end, double speed, double volume);
	void SetEndFrame(int64_t end);
	void SetVolume(double vol) { volume = vol; }
	void SetPlaybackSpeed(double speed);

	int64_t GetInputFrame() const { return input_frame; }
	bool IsFinished() const { return output_finished; }

	size_t Fill(void *dst, size_t frames);
};

#endif // WITH_SOUNDTOUCH
