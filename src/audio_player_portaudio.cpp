// Copyright (c) 2005-2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file audio_player_portaudio.cpp
/// @brief PortAudio v18-based audio output
/// @ingroup audio_output
///

#ifdef WITH_PORTAUDIO
#include "audio_player_portaudio.h"

#include "audio_controller.h"
#include "compat.h"
#include "options.h"
#include "utils.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cstring>

#ifdef WITH_SOUNDTOUCH
#include "audio_player_soundtouch.h"
#endif

// Uncomment to enable extremely spammy debug logging
//#define PORTAUDIO_DEBUG

/// Order that the host APIs should be tried if there are multiple available
static const PaHostApiTypeId pa_host_api_priority[] = {
	// No WDMKS or ASIO as they don't support shared mode (and WDMKS is pretty broken)
	paWASAPI,
	paDirectSound,
	paMME,

	paCoreAudio,
#ifdef __APPLE__
	paAL,
#endif

	paALSA,
	paOSS
};
static const size_t pa_host_api_priority_count = sizeof(pa_host_api_priority) / sizeof(pa_host_api_priority[0]);

// Calculate optimal buffer size based on audio quality for different formats
static size_t CalculateOptimalBufferSize(agi::AudioProvider *provider) {
    if (!provider) return 0;

    const int sample_rate = provider->GetSampleRate();
    const int channels = provider->GetChannels();
    const int bytes_per_sample = provider->GetBytesPerSample();

    // Base buffer: supports 4x playback speed
    const size_t base_frames = (sample_rate * 4) / 256 + 4;

    // Quality adjustment factors for different audio formats
    double rate_factor = sample_rate / 44100.0;    // CD quality baseline
    double depth_factor = bytes_per_sample / 2.0;  // 16-bit baseline
    double channel_factor = channels / 2.0;        // Stereo baseline
    double quality_factor = rate_factor * depth_factor * channel_factor;

    // Adjust buffer size based on quality
    size_t adjusted_frames = static_cast<size_t>(base_frames * quality_factor);

    // Additional buffer for immersive audio formats (Atmos, DTS:X, etc.)
    if (channels > 8) {
        adjusted_frames = static_cast<size_t>(adjusted_frames * 1.5); // 50% more for immersive
        LOG_D("audio/player/portaudio") << "Immersive audio detected: " << channels << " channels, increasing buffer size";
    }

    // Additional optimization for Hi-Res audio (96kHz+, 24-bit+)
    const bool is_hires = (sample_rate >= 96000) || (bytes_per_sample >= 3);
    if (is_hires) {
        // Hi-Res audio needs more consistent buffering to maintain quality
        adjusted_frames = static_cast<size_t>(adjusted_frames * 1.2); // 20% more for Hi-Res
        LOG_D("audio/player/portaudio") << "Hi-Res audio detected: " << sample_rate << "Hz/"
            << (bytes_per_sample * 8) << "bit, optimizing buffer size";
    }

    // Ensure min/max limits (extended for immersive audio)
    adjusted_frames = std::max(adjusted_frames, size_t(100));    // Minimum buffer
    adjusted_frames = std::min(adjusted_frames, size_t(16384));  // Maximum buffer (increased for immersive)

    // Calculate bytes
    size_t buffer_bytes = adjusted_frames * channels * bytes_per_sample;

    return buffer_bytes;
}

// Detect if audio is high quality (FLAC, ALAC, high-bitrate, etc.)
static bool IsHighQualityAudio(agi::AudioProvider *provider) {
    if (!provider) return false;

    const int sample_rate = provider->GetSampleRate();
    const int bytes_per_sample = provider->GetBytesPerSample();

    return (sample_rate >= 48000) || (bytes_per_sample > 2);
}

// Detect immersive audio configurations (Atmos, DTS:X, Auro, etc.)
static const char* DetectImmersiveConfig(int channels) {
    switch (channels) {
        case 8:  return "5.1.2";  // 5.1 bed + 2 height
        case 10: return "5.1.4/7.1.2";  // Could be either configuration
        case 12: return "7.1.4/Auro 11.1";  // Atmos or Auro
        case 14: return "Auro 13.1";  // Auro max
        case 16: return "9.1.6";  // High-end immersive
        default: return nullptr;
    }
}

// Check if audio is immersive/object-based format
static bool IsImmersiveAudio(agi::AudioProvider *provider) {
    if (!provider) return false;
    const int channels = provider->GetChannels();
    return channels > 8 || DetectImmersiveConfig(channels) != nullptr;
}

// Detect high-resolution audio (96kHz+, 24-bit+)
static bool IsHiResAudio(agi::AudioProvider *provider) {
    if (!provider) return false;

    const int sample_rate = provider->GetSampleRate();
    const int bytes_per_sample = provider->GetBytesPerSample();

    // Hi-Res Audio: typically 96kHz+ and/or 24-bit+
    return (sample_rate >= 96000) || (bytes_per_sample >= 3);
}

// Get Hi-Res audio quality level
static const char* GetHiResQualityLevel(agi::AudioProvider *provider) {
    if (!provider) return "Standard";

    const int sample_rate = provider->GetSampleRate();
    const int bytes_per_sample = provider->GetBytesPerSample();

    if (sample_rate >= 192000) return "Ultra Hi-Res (192kHz+)";
    if (sample_rate >= 96000 && bytes_per_sample >= 3) return "Hi-Res (96kHz/24bit+)";
    if (sample_rate >= 96000) return "Hi-Res (96kHz+)";
    if (sample_rate >= 48000 && bytes_per_sample >= 3) return "High Quality (48kHz/24bit+)";
    if (bytes_per_sample >= 4) return "Professional (32-bit)";
    return "Standard";
}

PortAudioPlayer::PortAudioPlayer(agi::AudioProvider *provider) : AudioPlayer(provider) {
	PaError err = Pa_Initialize();

	if (err != paNoError)
		throw AudioPlayerOpenError(std::string("Failed opening PortAudio: ") + Pa_GetErrorText(err));

	// Build a list of host API-specific devices we can use
	// Some host APIs may not support all audio formats, so build a priority
	// list of host APIs for each device rather than just always using the best
	for (size_t i = 0; i < pa_host_api_priority_count; ++i) {
		PaHostApiIndex host_idx = Pa_HostApiTypeIdToHostApiIndex(pa_host_api_priority[i]);
		if (host_idx >= 0)
			GatherDevices(host_idx);
	}
	GatherDevices(Pa_GetDefaultHostApi());

	if (devices.empty())
		throw AudioPlayerOpenError("No PortAudio output devices found");

	if (provider)
		OpenStream();

#ifdef WITH_SOUNDTOUCH
	try {
		if (provider)
			tempo_processor = std::make_unique<SoundTouchAudioProcessor>(provider);
	}
	catch (...) {
		// SoundTouch init failed, fall back to nearest-neighbor resampling
		// Pre-allocate speed_buffer to avoid reallocation in audio callback
		if (provider) {
			// Use intelligent buffer sizing for different audio formats (WAV, FLAC, ALAC, etc.)
			size_t max_bytes = CalculateOptimalBufferSize(provider);
			speed_buffer.reserve(max_bytes);

			const bool is_hq = IsHighQualityAudio(provider);
			LOG_D("audio/player/portaudio") << "Pre-allocated speed_buffer: " << max_bytes << " bytes"
				<< " (format: " << provider->GetSampleRate() << "Hz"
				<< ", " << provider->GetChannels() << "ch"
				<< ", " << provider->GetBytesPerSample() << "bytes/sample"
				<< ", high_quality: " << (is_hq ? "yes" : "no") << ")";
		}
	}
#else
	// Pre-allocate speed_buffer to avoid reallocation in audio callback
	if (provider) {
		// Use intelligent buffer sizing for different audio formats (WAV, FLAC, ALAC, etc.)
		size_t max_bytes = CalculateOptimalBufferSize(provider);
		speed_buffer.reserve(max_bytes);

		const bool is_hq = IsHighQualityAudio(provider);
		LOG_D("audio/player/portaudio") << "Pre-allocated speed_buffer: " << max_bytes << " bytes"
			<< " (format: " << provider->GetSampleRate() << "Hz"
			<< ", " << provider->GetChannels() << "ch"
			<< ", " << provider->GetBytesPerSample() << "bytes/sample"
			<< ", high_quality: " << (is_hq ? "yes" : "no") << ")";
	}
#endif
}

void PortAudioPlayer::GatherDevices(PaHostApiIndex host_idx) {
	const PaHostApiInfo *host_info = Pa_GetHostApiInfo(host_idx);
	if (!host_info) return;

	for (int host_device_idx = 0; host_device_idx < host_info->deviceCount; ++host_device_idx) {
		PaDeviceIndex real_idx = Pa_HostApiDeviceIndexToDeviceIndex(host_idx, host_device_idx);
		if (real_idx < 0) continue;

		const PaDeviceInfo *device_info = Pa_GetDeviceInfo(real_idx);
		if (!device_info) continue;
		if (device_info->maxOutputChannels <= 0) continue;

		// MME truncates device names so check for prefix rather than exact match
		auto dev_it = devices.lower_bound(device_info->name);
		if (dev_it == devices.end() || dev_it->first.find(device_info->name) != 0) {
			devices[device_info->name];
			--dev_it;
		}

		dev_it->second.push_back(real_idx);
		if (real_idx == host_info->defaultOutputDevice)
			default_device.push_back(real_idx);
	}
}

PortAudioPlayer::~PortAudioPlayer() {
	if (stream) {
		Stop();
		Pa_CloseStream(stream);
	}
#ifdef WITH_SOUNDTOUCH
	tempo_processor.reset();
#endif
	Pa_Terminate();
}

void PortAudioPlayer::OpenStream() {
	DeviceVec *device_ids = nullptr;
	std::string device_name = OPT_GET("Player/Audio/PortAudio/Device Name")->GetString();

	if (devices.count(device_name)) {
		device_ids = &devices[device_name];
		LOG_D("audio/player/portaudio") << "using config device: " << device_name;
	}

	if (!device_ids || device_ids->empty()) {
		device_ids = &default_device;
		LOG_D("audio/player/portaudio") << "using default output device";
	}

	std::string error;

	for (size_t i = 0; i < device_ids->size(); ++i) {
		const PaDeviceInfo *device_info = Pa_GetDeviceInfo((*device_ids)[i]);
		PaStreamParameters pa_output_p;
		pa_output_p.device = (*device_ids)[i];
		pa_output_p.channelCount = provider->GetChannels();
		pa_output_p.sampleFormat = paInt16;
		pa_output_p.suggestedLatency = device_info->defaultLowOutputLatency;
#ifdef __APPLE__
		// CoreAudio is more sensitive to very small callback buffers on some
		// Apple Silicon devices; use the high-latency default as a stable floor.
		pa_output_p.suggestedLatency = std::max(pa_output_p.suggestedLatency, std::max(device_info->defaultHighOutputLatency, 0.12));
#endif
		pa_output_p.hostApiSpecificStreamInfo = nullptr;

		LOG_D("audio/player/portaudio") << "OpenStream:"
			<< " output channels: " << pa_output_p.channelCount
			<< " latency: " << pa_output_p.suggestedLatency
			<< " sample rate: " << provider->GetSampleRate()
			<< " sample format: " << pa_output_p.sampleFormat;

		PaError err = Pa_OpenStream(&stream, nullptr, &pa_output_p, provider->GetSampleRate(), 0, paPrimeOutputBuffersUsingStreamCallback | paDitherOff, paCallback, this);

		if (err == paNoError) {
			LOG_D("audo/player/portaudio") << "Using device " << pa_output_p.device << " " << device_info->name << " " << Pa_GetHostApiInfo(device_info->hostApi)->name;
			return;
		}
		else {
			const PaHostErrorInfo *pa_err = Pa_GetLastHostErrorInfo();
			LOG_D_IF(pa_err->errorCode != 0, "audio/player/portaudio") << "HostError: API: " << pa_err->hostApiType << ", " << pa_err->errorText << ", " << pa_err->errorCode;
			LOG_D("audio/player/portaudio") << "Failed initializing PortAudio stream with error: " << Pa_GetErrorText(err);
			error += Pa_GetErrorText(err);
			error += " ";
		}
	}

	throw AudioPlayerOpenError("Failed initializing PortAudio stream: " + error);
}

void PortAudioPlayer::paStreamFinishedCallback(void *) {
	LOG_D("audio/player/portaudio") << "stopping stream";
}

void PortAudioPlayer::Play(int64_t start_sample, int64_t count) {
	// Enhanced audio format detection for different codecs (WAV, FLAC, ALAC, AAC, etc.)
	if (provider) {
		const int sample_rate = provider->GetSampleRate();
		const int channels = provider->GetChannels();
		const int bytes_per_sample = provider->GetBytesPerSample();
		const bool is_hq = IsHighQualityAudio(provider);
		const bool is_immersive = IsImmersiveAudio(provider);
		const bool is_hires = IsHiResAudio(provider);
		const char* immersive_config = DetectImmersiveConfig(channels);
		const char* hires_level = GetHiResQualityLevel(provider);

		// Determine likely format based on characteristics
		const char* likely_format = "unknown";
		if (is_immersive && immersive_config)
			likely_format = immersive_config;
		else if (is_hires && sample_rate >= 192000)
			likely_format = "Ultra Hi-Res (192kHz+)";
		else if (is_hires && sample_rate >= 96000)
			likely_format = "Hi-Res (96kHz/24bit+)";
		else if (channels > 8)
			likely_format = "Object-based/Immersive";
		else if (sample_rate >= 48000 && bytes_per_sample >= 3)
			likely_format = "High Quality (48kHz/24bit+)";
		else if (bytes_per_sample == 4)
			likely_format = "Professional (32-bit)";
		else if (sample_rate == 44100 && bytes_per_sample == 2 && channels == 2)
			likely_format = "MP3/AAC standard";
		else if (sample_rate == 48000 && bytes_per_sample == 2)
			likely_format = "Video standard";

		LOG_D("audio/player/portaudio") << "Audio format detected:"
			<< " sample_rate=" << sample_rate << "Hz"
			<< " channels=" << channels
			<< " bytes_per_sample=" << bytes_per_sample
			<< " likely_format=" << likely_format
			<< " hires_level=" << hires_level
			<< " high_quality=" << (is_hq ? "yes" : "no")
			<< " hires=" << (is_hires ? "yes" : "no")
			<< " immersive=" << (is_immersive ? "yes" : "no")
			<< " buffer_size=" << CalculateOptimalBufferSize(provider) << "bytes";
	}

	current = start_sample;
	start = start_sample;
	end = start_sample + count;
	speed_position = 0.0;
	draining = false;

#ifdef WITH_SOUNDTOUCH
	if (tempo_processor)
		tempo_processor->Reset(start_sample, start_sample + count, playback_speed, volume);
#endif

	// Start playing
	if (!IsPlaying()) {
		PaError err = Pa_SetStreamFinishedCallback(stream, paStreamFinishedCallback);
		if (err != paNoError) {
			LOG_D("audio/player/portaudio") << "could not set FinishedCallback";
			return;
		}

		err = Pa_StartStream(stream);
		if (err != paNoError) {
			LOG_D("audio/player/portaudio") << "error playing stream";
			return;
		}
	}
	pa_start = Pa_GetStreamTime(stream);
}

void PortAudioPlayer::Stop() {
	Pa_StopStream(stream);
}

int PortAudioPlayer::paCallback(const void *, void *outputBuffer,
	unsigned long framesPerBuffer, [[maybe_unused]] const PaStreamCallbackTimeInfo* timeInfo,
	[[maybe_unused]] PaStreamCallbackFlags statusFlags, void *userData)
{
	PortAudioPlayer *player = (PortAudioPlayer *)userData;

#ifdef PORTAUDIO_DEBUG
	LOG_D("audio/player/portaudio") << "psCallback:"
		<< " current: " << player->current
		<< " start: " << player->start
		<< " pa_start: " << player->pa_start
		<< " currentTime: " << timeInfo->currentTime
		<< " AdcTime: " << timeInfo->inputBufferAdcTime
		<< " DacTime: " << timeInfo->outputBufferDacTime
		<< " status: " << statusFlags
		<< " framesPerBuffer: " << framesPerBuffer
		<< " CPU: " << Pa_GetStreamCpuLoad(player->stream);
#endif

	// Calculate how much left
	int64_t lenAvailable = std::min<int64_t>(player->end - player->current, framesPerBuffer);
	const int bytes_per_frame = player->provider->GetChannels() * player->provider->GetBytesPerSample();
	if (player->draining) {
		memset(outputBuffer, 0, framesPerBuffer * bytes_per_frame);
		player->draining = false;
		return paComplete;
	}

	// Play something
	if (lenAvailable > 0 && player->playback_speed == 1.0) {
		player->provider->GetAudioWithVolume(outputBuffer, player->current, lenAvailable, player->GetVolume());

		// Set play position
		player->current += lenAvailable;
		if ((unsigned long)lenAvailable < framesPerBuffer) {
			auto out = static_cast<char *>(outputBuffer);
			memset(out + lenAvailable * bytes_per_frame, 0, (framesPerBuffer - lenAvailable) * bytes_per_frame);
			player->draining = true;
			return paContinue;
		}

		// Continue as normal
		return paContinue;
	}

	if (player->playback_speed != 1.0) {
#ifdef WITH_SOUNDTOUCH
		if (player->tempo_processor) {
			auto out = static_cast<char *>(outputBuffer);
			player->tempo_processor->Fill(out, framesPerBuffer);
			player->current = player->tempo_processor->GetInputFrame();
			if (player->tempo_processor->IsFinished()) {
				player->draining = true;
				return paContinue;
			}
			return paContinue;
		}
#endif
		const auto source_frames = std::min<int64_t>(
			player->end - player->current,
			(int64_t)(framesPerBuffer * player->playback_speed) + 2);

		if (source_frames <= 0) {
			memset(outputBuffer, 0, framesPerBuffer * bytes_per_frame);
			player->draining = true;
			return paContinue;
		}

		// Use pre-allocated buffer when possible to avoid audio crackling
		// Buffer was pre-allocated in constructor to support max playback speed
		const size_t required_bytes = source_frames * bytes_per_frame;
		if (player->speed_buffer.capacity() < required_bytes) {
			// Emergency reallocation with detailed debugging
			LOG_W("audio/player/portaudio") << "Buffer reallocation: needed=" << required_bytes
				<< " available=" << player->speed_buffer.capacity()
				<< " sample_rate=" << player->provider->GetSampleRate()
				<< " speed=" << player->playback_speed;
			player->speed_buffer.resize(required_bytes);
		} else {
			// Use pre-allocated memory - this is the normal case
			player->speed_buffer.resize(required_bytes);
		}
		player->provider->GetAudioWithVolume(player->speed_buffer.data(), player->current, source_frames, player->GetVolume());

		auto out = static_cast<char *>(outputBuffer);
		double source_position = player->speed_position;
		unsigned long output_frame = 0;
		for (; output_frame < framesPerBuffer; ++output_frame) {
			int64_t source_frame = (int64_t)source_position;
			if (source_frame >= source_frames)
				break;

			memcpy(out + output_frame * bytes_per_frame, player->speed_buffer.data() + source_frame * bytes_per_frame, bytes_per_frame);
			source_position += player->playback_speed;
		}

		if (output_frame < framesPerBuffer)
			memset(out + output_frame * bytes_per_frame, 0, (framesPerBuffer - output_frame) * bytes_per_frame);

		auto consumed = std::min<int64_t>((int64_t)source_position, source_frames);
		player->current += consumed;
		player->speed_position = source_position - consumed;

		if (output_frame == framesPerBuffer)
			return paContinue;
		player->draining = true;
		return paContinue;
	}

	// Abort stream and stop the callback.
	return paComplete;
}

int64_t PortAudioPlayer::GetCurrentPosition() {
	if (!IsPlaying()) return 0;

#ifdef WITH_SOUNDTOUCH
	// When SoundTouch is active, use the tracked input position directly
	// as it's more reliable than time-based estimation
	if (playback_speed != 1.0 && tempo_processor) {
		return current;
	}
#endif

	PaTime pa_time = Pa_GetStreamTime(stream);
	int64_t real = (pa_time - pa_start) * provider->GetSampleRate() * playback_speed + start;

	// If portaudio isn't giving us time info then estimate based on buffer fill and current latency
	if (pa_time == 0 && pa_start == 0)
		real = current - Pa_GetStreamInfo(stream)->outputLatency * provider->GetSampleRate();

#ifdef PORTAUDIO_DEBUG
	LOG_D("audio/player/portaudio") << "GetCurrentPosition:"
		<< " pa_time: " << pa_time
		<< " start: " << start
		<< " current: " << current
		<< " pa_start: " << pa_start
		<< " real: " << real
		<< " diff: " << pa_time - pa_start;
#endif

	return real;
}

wxArrayString PortAudioPlayer::GetOutputDevices() {
	wxArrayString list;
	list.push_back("Default");

	try {
		PortAudioPlayer player(nullptr);

		for (auto it = player.devices.begin(); it != player.devices.end(); ++it)
			list.push_back(to_wx(it->first));
	}
	catch (AudioPlayerOpenError const&) {
		// No output devices, just return the list with only Default
	}

	return list;
}

bool PortAudioPlayer::IsPlaying() {
	return !!Pa_IsStreamActive(stream);
}

void PortAudioPlayer::SetPlaybackSpeed(double speed) {
	if (IsPlaying()) {
		start = GetCurrentPosition();
		current = start;
		speed_position = 0.0;
		pa_start = Pa_GetStreamTime(stream);
	}

	playback_speed = std::max(0.25, std::min(speed, 4.0));

#ifdef WITH_SOUNDTOUCH
	if (tempo_processor)
		tempo_processor->SetPlaybackSpeed(playback_speed);
#endif
}

std::unique_ptr<AudioPlayer> CreatePortAudioPlayer(agi::AudioProvider *provider, wxWindow *) {
	return std::make_unique<PortAudioPlayer>(provider);
}

#endif // WITH_PORTAUDIO
