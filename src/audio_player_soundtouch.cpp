#ifdef WITH_SOUNDTOUCH
#include "audio_player_soundtouch.h"

#include "audio_sample_safety.h"

#include <libaegisub/audio/provider.h>

#include <SoundTouch.h>
#include <algorithm>
#include <cmath>
#include <cstring>

SoundTouchAudioProcessor::SoundTouchAudioProcessor(agi::AudioProvider *prov)
: provider(prov)
, processor(std::make_unique<soundtouch::SoundTouch>())
{
	processor->setSampleRate(provider->GetSampleRate());
	processor->setChannels(provider->GetChannels());

	// Pre-allocate buffers to avoid memory allocation in the audio hot path
	source_buffer.resize(4096 * provider->GetChannels());
	process_buffer.resize(4096 * provider->GetChannels());
	output_buffer.resize(8192 * provider->GetChannels());
}

SoundTouchAudioProcessor::~SoundTouchAudioProcessor() = default;

int SoundTouchAudioProcessor::channels() const {
	return provider->GetChannels();
}

void SoundTouchAudioProcessor::feed_more() {
	if (input_frame >= end_frame) {
		input_finished = true;
		return;
	}

	auto frames = (size_t)std::min<int64_t>(4096, end_frame - input_frame);
	auto sample_count = frames * channels();

	// Ensure buffers are large enough (pre-allocated in constructor, but grow if needed)
	if (source_buffer.size() < sample_count)
		source_buffer.resize(sample_count);
	if (process_buffer.size() < sample_count)
		process_buffer.resize(sample_count);

	provider->GetAudio(source_buffer.data(), input_frame, frames);
	input_frame += frames;

	for (size_t i = 0; i < sample_count; ++i)
		process_buffer[i] = (float)source_buffer[i] * AudioSampleSafety::kSoundTouchInputHeadroom / 32768.0f;

	processor->putSamples(process_buffer.data(), (unsigned int)frames);
}

void SoundTouchAudioProcessor::Reset(int64_t start, int64_t end, double speed, double vol) {
	input_frame = start;
	end_frame = end;
	playback_speed = std::max(0.25, std::min(speed, 4.0));
	volume = vol;
	input_finished = input_frame >= end_frame;
	flushed = input_finished;
	output_finished = input_finished;
	processor->clear();
	processor->setSampleRate(provider->GetSampleRate());
	processor->setChannels(provider->GetChannels());
	processor->setTempo(playback_speed);
	processor->setPitch(1.0);
	processor->setRate(1.0);
}

void SoundTouchAudioProcessor::SetEndFrame(int64_t end) {
	end_frame = end;
	if (input_frame < end_frame) {
		input_finished = false;
		flushed = false;
		output_finished = false;
	}
}

void SoundTouchAudioProcessor::SetPlaybackSpeed(double speed) {
	playback_speed = std::max(0.25, std::min(speed, 4.0));
	processor->setTempo(playback_speed);
}

size_t SoundTouchAudioProcessor::Fill(void *dst, size_t frames_requested) {
	auto bytes_per_frame = (size_t)channels() * provider->GetBytesPerSample();
	auto out = static_cast<int16_t *>(dst);

	if (frames_requested == 0)
		return 0;

	if (std::abs(playback_speed - 1.0) < 0.001) {
		auto available = (size_t)std::max<int64_t>(0, std::min<int64_t>((int64_t)frames_requested, end_frame - input_frame));
		if (available) {
			provider->GetAudio(dst, input_frame, available);
			AudioSampleSafety::ApplyGainLimiter(out, available * channels(), volume);
		}
		if (available < frames_requested)
			memset(out + available * channels(), 0, (frames_requested - available) * bytes_per_frame);

		input_frame += available;
		input_finished = input_frame >= end_frame;
		output_finished = input_finished;
		return frames_requested;
	}

	size_t filled = 0;
	if (output_buffer.size() < frames_requested * channels())
		output_buffer.resize(frames_requested * channels());

	while (filled < frames_requested && !output_finished) {
		auto got = processor->receiveSamples(output_buffer.data(), (unsigned int)(frames_requested - filled));
		if (got) {
			AudioSampleSafety::ConvertFloatToInt16Limited(
				out + filled * channels(),
				output_buffer.data(),
				(size_t)got * channels(),
				volume);
			filled += got;
			continue;
		}

		if (!input_finished) {
			feed_more();
			continue;
		}

		if (!flushed) {
			processor->flush();
			flushed = true;
			continue;
		}

		output_finished = true;
	}

	if (filled < frames_requested)
		memset(out + filled * channels(), 0, (frames_requested - filled) * bytes_per_frame);

	return frames_requested;
}

#endif // WITH_SOUNDTOUCH
