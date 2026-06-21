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

#include "audio_sample_safety.h"
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

PortAudioPlayer::PortAudioPlayer(agi::AudioProvider *provider) : AudioPlayer(provider) {
	PaError err = Pa_Initialize();

	if (err != paNoError)
		throw AudioPlayerOpenError(std::string("Failed opening PortAudio: ") + Pa_GetErrorText(err));

	RebuildDeviceList();

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

		if (std::find(dev_it->second.begin(), dev_it->second.end(), real_idx) == dev_it->second.end())
			dev_it->second.push_back(real_idx);
		if (real_idx == host_info->defaultOutputDevice && std::find(default_device.begin(), default_device.end(), real_idx) == default_device.end())
			default_device.push_back(real_idx);
	}
}

PortAudioPlayer::~PortAudioPlayer() {
	CloseStream();
#ifdef WITH_SOUNDTOUCH
	tempo_processor.reset();
#endif
	Pa_Terminate();
}

void PortAudioPlayer::RebuildDeviceList() {
	devices.clear();
	default_device.clear();

	// Build a list of host API-specific devices we can use
	// Some host APIs may not support all audio formats, so build a priority
	// list of host APIs for each device rather than just always using the best
	for (size_t i = 0; i < pa_host_api_priority_count; ++i) {
		PaHostApiIndex host_idx = Pa_HostApiTypeIdToHostApiIndex(pa_host_api_priority[i]);
		if (host_idx >= 0)
			GatherDevices(host_idx);
	}
	GatherDevices(Pa_GetDefaultHostApi());

	// On macOS the route can move between speakers, headphone jack and external
	// devices without preserving our cached host-api priority order. Prefer the
	// system's live default output device whenever the user selected Default.
	PaDeviceIndex current_default = Pa_GetDefaultOutputDevice();
	const PaDeviceInfo *current_default_info = current_default == paNoDevice ? nullptr : Pa_GetDeviceInfo(current_default);
	if (current_default_info && current_default_info->maxOutputChannels > 0) {
		auto it = std::find(default_device.begin(), default_device.end(), current_default);
		if (it == default_device.end())
			default_device.insert(default_device.begin(), current_default);
		else if (it != default_device.begin())
			std::rotate(default_device.begin(), it, it + 1);
	}
}

void PortAudioPlayer::CloseStream() {
	if (!stream)
		return;

	PaError active = Pa_IsStreamActive(stream);
	if (active == 1)
		Pa_StopStream(stream);

	Pa_CloseStream(stream);
	stream = nullptr;
	active_device = paNoDevice;
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
			active_device = pa_output_p.device;
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

void PortAudioPlayer::RefreshDefaultDevice(bool force) {
	if (!provider || OPT_GET("Player/Audio/PortAudio/Device Name")->GetString() != "Default")
		return;

	PaDeviceIndex current_default = Pa_GetDefaultOutputDevice();
	if (!force && (current_default == paNoDevice || current_default == active_device))
		return;

	bool was_playing = IsPlaying();
	if (was_playing)
		Stop();

	CloseStream();
	RebuildDeviceList();
	OpenStream();
}

bool PortAudioPlayer::EnsureStreamForDefaultDevice() {
	// Only applies when the user selected "Default" output. Named devices are
	// opened once in the constructor and left alone.
	if (!provider || OPT_GET("Player/Audio/PortAudio/Device Name")->GetString() != "Default")
		return stream != nullptr;

	PaDeviceIndex current_default = Pa_GetDefaultOutputDevice();

	// Fast path: the stream we already hold is still on the live default
	// device. CoreAudio keeps the same PortAudio device index while the route
	// sits still, so we can skip the (expensive) full device enumeration.
	if (stream && current_default != paNoDevice && current_default == active_device)
		return true;

	// The output route moved (headphones plugged/unplugged, Bluetooth headset
	// connected, etc.). Tear down and reopen against the fresh default device.
	bool was_playing = IsPlaying();
	if (was_playing)
		Stop();

	CloseStream();

	try {
		RebuildDeviceList();
		OpenStream();
	}
	catch (AudioPlayerOpenError const& err) {
		// Never let a route-change hiccup propagate out of Play(): the
		// previous stream is already closed, so report failure and let the
		// caller fall back gracefully instead of crashing the app.
		LOG_E("audio/player/portaudio") << "Failed to reopen stream after route change: " << err.GetMessage();
		return false;
	}
	catch (...) {
		LOG_E("audio/player/portaudio") << "Unknown error reopening stream after route change";
		return false;
	}

	return stream != nullptr;
}

void PortAudioPlayer::paStreamFinishedCallback(void *) {
	LOG_D("audio/player/portaudio") << "stopping stream";
}

void PortAudioPlayer::Play(int64_t start_sample, int64_t count) {
#ifdef __APPLE__
	// CoreAudio can keep the same PortAudio device index while the route sits
	// still, but the live default device changes when the user plugs/unplugs
	// headphones or connects a Bluetooth output. Only pay the cost of
	// reopening the stream when the default device has actually moved, and do
	// it through the exception-safe helper so a transient open failure can
	// never crash Play().
	if (!EnsureStreamForDefaultDevice()) {
		LOG_D("audio/player/portaudio") << "no usable output stream; aborting Play";
		return;
	}
#else
	RefreshDefaultDevice();
#endif

	// Defensive guard: if for any reason there is no live stream, bail out
	// instead of handing a null pointer to PortAudio below.
	if (!stream) {
		LOG_D("audio/player/portaudio") << "Play called without an open stream";
		return;
	}

	current = start_sample;
	start = start_sample;
	end = start_sample + count;
	speed_position = 0.0;
	draining = false;
	last_position = start_sample;

#ifdef WITH_SOUNDTOUCH
	if (tempo_processor)
		tempo_processor->Reset(start_sample, start_sample + count, playback_speed, volume);
#endif

	// Restart the callback if it had previously completed (paComplete stops
	// invoking the callback but leaves the stream open). IsPlaying() is false
	// both for a never-started stream and for one that stopped after the
	// callback returned paComplete, and also covers the error recovery case.
	if (!IsPlaying()) {
		PaError err = Pa_SetStreamFinishedCallback(stream, paStreamFinishedCallback);
		if (err != paNoError) {
			LOG_D("audio/player/portaudio") << "could not set FinishedCallback";
			return;
		}

		err = Pa_StartStream(stream);
		if (err != paNoError) {
			LOG_D("audio/player/portaudio") << "error playing stream: " << Pa_GetErrorText(err);
			return;
		}
	}
	pa_start = Pa_GetStreamTime(stream);
}

void PortAudioPlayer::Stop() {
	if (stream)
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
		player->provider->GetAudio(outputBuffer, player->current, lenAvailable);
		AudioSampleSafety::ApplyGainLimiter(
			static_cast<int16_t *>(outputBuffer),
			(size_t)lenAvailable * player->provider->GetChannels(),
			player->GetVolume());

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

		player->speed_buffer.resize(source_frames * bytes_per_frame);
		player->provider->GetAudio(player->speed_buffer.data(), player->current, source_frames);
		AudioSampleSafety::ApplyGainLimiter(
			reinterpret_cast<int16_t *>(player->speed_buffer.data()),
			(size_t)source_frames * player->provider->GetChannels(),
			player->GetVolume());

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
		int64_t real = current;
		if (real < last_position) real = last_position;
		else last_position = real;
		return real;
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

	if (real < last_position)
		real = last_position;
	else
		last_position = real;

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
	return stream && Pa_IsStreamActive(stream) == 1;
}

void PortAudioPlayer::SetPlaybackSpeed(double speed) {
	if (IsPlaying()) {
		start = GetCurrentPosition();
		current = start;
		speed_position = 0.0;
		pa_start = Pa_GetStreamTime(stream);
		last_position = start;
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
