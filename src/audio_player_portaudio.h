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

/// @file audio_player_portaudio.h
/// @see audio_player_portaudio.cpp
/// @ingroup audio_output
///

#ifdef WITH_PORTAUDIO

#include "include/aegisub/audio_player.h"

#ifdef WITH_SOUNDTOUCH
#include "audio_player_soundtouch.h"
#endif

extern "C" {
#include <portaudio.h>
}

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

class wxArrayString;

/// @class PortAudioPlayer
/// @brief PortAudio Player
///
class PortAudioPlayer final : public AudioPlayer {
	typedef std::vector<PaDeviceIndex> DeviceVec;
	/// Map of supported output devices from name -> device index
	std::map<std::string, DeviceVec> devices;

	/// The index of the default output devices sorted by host API priority
	DeviceVec default_device;

	/// Volume is changed by the UI while the callback is active. The callback
	/// only tries the atomic_flag once and retains callback_volume on contention.
	mutable std::atomic_flag volume_guard = ATOMIC_FLAG_INIT;
	float requested_volume = 1.f;
	float callback_volume = 1.f;
	double playback_speed = 1.0; ///< Current playback speed
	double speed_position = 0.0; ///< Fractional source position for speed-adjusted playback
	std::atomic<bool> callback_finished{true}; ///< Set after queued output has finished playing
	int64_t current = 0; ///< Callback-owned position while the stream is active
	int64_t start = 0;   ///< Start position
	mutable std::atomic_flag end_guard = ATOMIC_FLAG_INIT;
	int64_t requested_end = 0; ///< UI-visible playback end
	int64_t callback_end = 0; ///< Callback's last non-blocking snapshot of requested_end
	PaTime pa_start = 0.0;     ///< PortAudio internal start position
	PaDeviceIndex active_device = paNoDevice; ///< Device used by the currently open stream
	std::vector<char> speed_buffer; ///< Temporary buffer for speed-adjusted playback
	int64_t last_position = 0; ///< UI-thread-only position for monotonic stability

	PaStream *stream = nullptr; ///< PortAudio stream

#ifdef WITH_SOUNDTOUCH
	std::unique_ptr<SoundTouchAudioProcessor> tempo_processor; ///< SoundTouch tempo processor for pitch-preserving speed changes
	struct SoundTouchPosition {
		int64_t frame = 0;
		PaTime dac_time = 0.0;
		double speed = 1.0;
		bool valid = false;
	};

	/// atomic_flag is the only atomic type which the C++ standard guarantees
	/// to be lock-free. The audio callback only tries the guard once and skips
	/// a publication on contention, so it can never wait for the UI thread.
	mutable std::atomic_flag soundtouch_position_guard = ATOMIC_FLAG_INIT;
	SoundTouchPosition soundtouch_position;

	void ResetSoundTouchPosition();
	void PublishSoundTouchPosition(int64_t frame, PaTime dac_time, double speed);
	bool ReadSoundTouchPosition(int64_t& frame, PaTime& dac_time, double& speed) const;
#endif

	/// @brief PortAudio callback, used to fill buffer for playback, and prime the playback buffer.
	/// @param inputBuffer     Input buffer.
	/// @param outputBuffer    Output buffer.
	/// @param framesPerBuffer Frames per buffer.
	/// @param timeInfo        PortAudio time information.
	/// @param statusFlags     Status flags
	/// @param userData        Local data to hand callback
	/// @return Whether to stop playback.
	static int paCallback(
		const void *inputBuffer,
		void *outputBuffer,
		unsigned long framesPerBuffer,
		const PaStreamCallbackTimeInfo*
		timeInfo,
		PaStreamCallbackFlags
		statusFlags,
		void *userData);

	/// @brief Called when the callback has finished.
	/// @param userData Local data to be handed to the callback.
	static void paStreamFinishedCallback(void *userData);

	/// Gather the list of output devices supported by a host API
	/// @param host_idx Host API ID
	void GatherDevices(PaHostApiIndex host_idx);

	void RebuildDeviceList();
	void CloseStream();
	void OpenStream();
	void RefreshDefaultDevice(bool force = false);

	/// Reopen the stream against the current system default device without
	/// throwing. Returns true if a usable stream is open afterwards. This is
	/// the safe entry point used from the playback hot path on macOS, where an
	/// output route change must never propagate an exception into Play().
	bool EnsureStreamForDefaultDevice();
	double VolumeForCallback();
	void ResetEndPosition(int64_t position);
	int64_t EndForCallback();

public:
	/// @brief Constructor
	PortAudioPlayer(agi::AudioProvider *provider);

	/// @brief Destructor
	~PortAudioPlayer();

	/// @brief Play audio.
	/// @param start Start position.
	/// @param count Frame count
	void Play(int64_t start,int64_t count);
	/// @brief Stop Playback
	/// @param timerToo Stop display timer?
	void Stop();

	/// @brief Whether audio is currently being played.
	/// @return Status
	bool IsPlaying();

	/// @brief End position playback will stop at.
	/// @return End position.
	int64_t GetEndPosition();
	/// @brief Get current stream position.
	/// @return Stream position
	int64_t GetCurrentPosition();

	/// @brief Set end position of playback
	/// @param pos End position
	void SetEndPosition(int64_t position);


	/// @brief Set volume level
	/// @param vol Volume
	void SetVolume(double vol);

	/// @brief Set playback speed
	/// @param speed Playback speed multiplier, 1.0 is normal speed
	void SetPlaybackSpeed(double speed) override;

	/// @brief Get current volume level
	/// @return Volume level
	double GetVolume();

	/// Get list of available output devices
	static wxArrayString GetOutputDevices();
};
#endif //ifdef WITH_PORTAUDIO
