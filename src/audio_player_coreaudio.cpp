// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

/// @file audio_player_coreaudio.cpp
/// @brief Native CoreAudio output for macOS
/// @ingroup audio_output

#ifdef WITH_COREAUDIO

#include "include/aegisub/audio_player.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#ifdef WITH_SOUNDTOUCH
#include "audio_player_soundtouch.h"
#endif

namespace {
class CoreAudioPlayer final : public AudioPlayer {
	static constexpr UInt32 output_channels = 2;
	static constexpr UInt32 buffer_count = 4;
	static constexpr double buffer_seconds = 0.08;

	AudioQueueRef queue = nullptr;
	AudioQueueBufferRef buffers[buffer_count]{};
	AudioStreamBasicDescription format{};

	std::vector<int16_t> source_buffer;
	std::vector<int16_t> tempo_buffer;
	UInt32 buffer_frames = 0;
	UInt32 bytes_per_buffer = 0;
	int source_channels = 1;

	std::atomic<double> volume{1.0};
	std::atomic<bool> playing{false};
	bool draining = false;
	double playback_speed = 1.0;
	double speed_position = 0.0;
	int64_t start_frame = 0;
	int64_t current_frame = 0;
	int64_t end_frame = 0;
	std::mutex state_mutex;

#ifdef WITH_SOUNDTOUCH
	std::unique_ptr<SoundTouchAudioProcessor> tempo_processor;
#endif

	static void Callback(void *user_data, AudioQueueRef, AudioQueueBufferRef buffer) {
		static_cast<CoreAudioPlayer *>(user_data)->OnBuffer(buffer);
	}

	static float ClampFloat(float value) {
		return std::max(-1.0f, std::min(1.0f, value));
	}

	float MixFrameToFloat(std::vector<int16_t> const& samples, size_t frame, int channels, float gain) const {
		int sum = 0;
		size_t base = frame * channels;
		for (int ch = 0; ch < channels; ++ch)
			sum += samples[base + ch];
		return ClampFloat((float)sum * gain / (32768.0f * (float)channels));
	}

	void WriteStereo(float *out, UInt32 frame, float sample) {
		out[frame * output_channels] = sample;
		out[frame * output_channels + 1] = sample;
	}

	void FillSilence(float *out, UInt32 start, UInt32 frames) {
		std::memset(out + start * output_channels, 0, frames * output_channels * sizeof(float));
	}

	bool FillNormalSpeed(float *out, UInt32 frames) {
		int64_t available = std::min<int64_t>(frames, end_frame - current_frame);
		float gain = static_cast<float>(std::clamp(volume.load(), 0.0, 2.0) * 0.98);

		if (available > 0) {
			size_t needed = (size_t)available * source_channels;
			if (source_buffer.size() < needed)
				source_buffer.resize(needed);
			provider->GetAudio(source_buffer.data(), current_frame, available);
			for (int64_t i = 0; i < available; ++i)
				WriteStereo(out, (UInt32)i, MixFrameToFloat(source_buffer, (size_t)i, source_channels, gain));
			current_frame += available;
		}

		if ((UInt32)available < frames) {
			FillSilence(out, (UInt32)available, frames - (UInt32)available);
			draining = true;
		}
		return available > 0 || draining;
	}

	bool FillNearestSpeed(float *out, UInt32 frames) {
		int64_t source_frames = std::min<int64_t>(end_frame - current_frame, (int64_t)(frames * playback_speed) + 2);
		float gain = static_cast<float>(std::clamp(volume.load(), 0.0, 2.0) * 0.98);

		if (source_frames <= 0) {
			FillSilence(out, 0, frames);
			draining = true;
			return true;
		}

		size_t needed = (size_t)source_frames * source_channels;
		if (source_buffer.size() < needed)
			source_buffer.resize(needed);
		provider->GetAudio(source_buffer.data(), current_frame, source_frames);

		double source_position = speed_position;
		UInt32 output_frame = 0;
		for (; output_frame < frames; ++output_frame) {
			int64_t source_frame = (int64_t)source_position;
			if (source_frame >= source_frames)
				break;
			WriteStereo(out, output_frame, MixFrameToFloat(source_buffer, (size_t)source_frame, source_channels, gain));
			source_position += playback_speed;
		}

		if (output_frame < frames) {
			FillSilence(out, output_frame, frames - output_frame);
			draining = true;
		}

		int64_t consumed = std::min<int64_t>((int64_t)source_position, source_frames);
		current_frame += consumed;
		speed_position = source_position - consumed;
		return true;
	}

#ifdef WITH_SOUNDTOUCH
	bool FillSoundTouch(float *out, UInt32 frames) {
		if (!tempo_processor)
			return FillNearestSpeed(out, frames);

		size_t needed = (size_t)frames * source_channels;
		if (tempo_buffer.size() < needed)
			tempo_buffer.resize(needed);
		tempo_processor->Fill(tempo_buffer.data(), frames);
		float gain = 0.98f;
		for (UInt32 i = 0; i < frames; ++i)
			WriteStereo(out, i, MixFrameToFloat(tempo_buffer, i, source_channels, gain));

		current_frame = tempo_processor->GetInputFrame();
		if (tempo_processor->IsFinished())
			draining = true;
		return true;
	}
#endif

	bool FillPlaybackBuffer(float *out, UInt32 frames) {
		if (std::abs(playback_speed - 1.0) < 0.001)
			return FillNormalSpeed(out, frames);
#ifdef WITH_SOUNDTOUCH
		if (tempo_processor)
			return FillSoundTouch(out, frames);
#endif
		return FillNearestSpeed(out, frames);
	}

	void OnBuffer(AudioQueueBufferRef buffer) {
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!playing.load())
			return;

		auto out = static_cast<float *>(buffer->mAudioData);
		bool filled = false;
		try {
			if (draining) {
				playing.store(false);
				return;
			}

			filled = FillPlaybackBuffer(out, buffer_frames);
		}
		catch (...) {
			LOG_E("audio/player/coreaudio") << "Audio callback failed; stopping playback";
			playing.store(false);
			return;
		}

		if (!filled) {
			playing.store(false);
			return;
		}

		buffer->mAudioDataByteSize = bytes_per_buffer;
		AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
	}

public:
	explicit CoreAudioPlayer(agi::AudioProvider *provider)
	: AudioPlayer(provider)
	, source_channels(std::max(1, provider->GetChannels()))
	{
		format.mSampleRate = provider->GetSampleRate();
		format.mFormatID = kAudioFormatLinearPCM;
		format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
		format.mBytesPerPacket = output_channels * sizeof(float);
		format.mFramesPerPacket = 1;
		format.mBytesPerFrame = output_channels * sizeof(float);
		format.mChannelsPerFrame = output_channels;
		format.mBitsPerChannel = sizeof(float) * 8;

		buffer_frames = std::max<UInt32>(1024, (UInt32)std::lround(format.mSampleRate * buffer_seconds));
		bytes_per_buffer = buffer_frames * format.mBytesPerFrame;
		source_buffer.resize((size_t)buffer_frames * 4 * source_channels + source_channels * 8);
		tempo_buffer.resize((size_t)buffer_frames * source_channels);

		auto err = AudioQueueNewOutput(&format, Callback, this, nullptr, nullptr, 0, &queue);
		if (err != noErr)
			throw AudioPlayerOpenError("CoreAudio: failed to create output queue");

		AudioQueueSetParameter(queue, kAudioQueueParam_Volume, 1.0f);
		for (auto& buffer : buffers) {
			err = AudioQueueAllocateBuffer(queue, bytes_per_buffer, &buffer);
			if (err != noErr)
				throw AudioPlayerOpenError("CoreAudio: failed to allocate output buffer");
		}

#ifdef WITH_SOUNDTOUCH
		try {
			tempo_processor = std::make_unique<SoundTouchAudioProcessor>(provider);
		}
		catch (...) {
			LOG_D("audio/player/coreaudio") << "SoundTouch init failed; using fallback speed adjustment";
		}
#endif
	}

	~CoreAudioPlayer() override {
		Stop();
		if (queue)
			AudioQueueDispose(queue, true);
	}

	void Play(int64_t start, int64_t count) override {
		if (queue)
			AudioQueueStop(queue, true);

		std::lock_guard<std::mutex> lock(state_mutex);
		start_frame = start;
		current_frame = start;
		end_frame = start + count;
		speed_position = 0.0;
		draining = false;
		playing.store(true);

#ifdef WITH_SOUNDTOUCH
		if (tempo_processor)
			tempo_processor->Reset(start_frame, end_frame, playback_speed, volume.load());
#endif

		for (auto buffer : buffers) {
			auto out = static_cast<float *>(buffer->mAudioData);
			bool filled = FillPlaybackBuffer(out, buffer_frames);
			if (!filled)
				break;
			buffer->mAudioDataByteSize = bytes_per_buffer;
			AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
			if (draining)
				break;
		}

		auto err = AudioQueueStart(queue, nullptr);
		if (err != noErr)
			playing.store(false);
	}

	void Stop() override {
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			playing.store(false);
			draining = false;
		}
		if (queue)
			AudioQueueStop(queue, true);
	}

	bool IsPlaying() override {
		if (!queue || !playing.load())
			return false;
		UInt32 running = 0;
		UInt32 size = sizeof(running);
		if (AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &running, &size) != noErr)
			return playing.load();
		return running != 0;
	}

	int64_t GetEndPosition() override {
		return end_frame;
	}

	int64_t GetCurrentPosition() override {
		if (!IsPlaying())
			return 0;

		AudioTimeStamp time{};
		Boolean discontinuity = false;
		if (AudioQueueGetCurrentTime(queue, nullptr, &time, &discontinuity) == noErr &&
			(time.mFlags & kAudioTimeStampSampleTimeValid)) {
			double played = std::max(0.0, time.mSampleTime);
			return start_frame + (int64_t)std::lround(played * playback_speed);
		}
		return current_frame;
	}

	void SetEndPosition(int64_t pos) override {
		std::lock_guard<std::mutex> lock(state_mutex);
		end_frame = pos;
#ifdef WITH_SOUNDTOUCH
		if (tempo_processor)
			tempo_processor->SetEndFrame(pos);
#endif
	}

	void SetVolume(double vol) override {
		volume.store(vol);
#ifdef WITH_SOUNDTOUCH
		if (tempo_processor)
			tempo_processor->SetVolume(vol);
#endif
	}

	void SetPlaybackSpeed(double speed) override {
		std::lock_guard<std::mutex> lock(state_mutex);
		playback_speed = std::max(0.25, std::min(speed, 4.0));
		speed_position = 0.0;
#ifdef WITH_SOUNDTOUCH
		if (tempo_processor)
			tempo_processor->SetPlaybackSpeed(playback_speed);
#endif
	}
};
}

std::unique_ptr<AudioPlayer> CreateCoreAudioPlayer(agi::AudioProvider *provider, wxWindow *) {
	return std::make_unique<CoreAudioPlayer>(provider);
}

#endif // WITH_COREAUDIO
