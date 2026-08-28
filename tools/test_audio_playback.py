#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OPENAL = ROOT / "src" / "audio_player_openal.cpp"
PORTAUDIO = ROOT / "src" / "audio_player_portaudio.cpp"
PORTAUDIO_H = ROOT / "src" / "audio_player_portaudio.h"
SOUNDTOUCH = ROOT / "src" / "audio_player_soundtouch.cpp"
SOUNDTOUCH_H = ROOT / "src" / "audio_player_soundtouch.h"
SAMPLE_SAFETY = ROOT / "src" / "audio_sample_safety.h"


def test_openal_uses_provider_sample_format():
    source = OPENAL.read_text(encoding="utf-8")
    assert "ALenum audio_format" in source
    assert "provider->GetChannels() == 2" in source
    assert "AL_FORMAT_STEREO16" in source
    assert "alBufferData(buffers[buf_first_free], audio_format" in source
    assert "alBufferData(buffers[buf_first_free], AL_FORMAT_MONO16" not in source


def test_portaudio_normal_speed_zero_fills_last_buffer():
    source = PORTAUDIO.read_text(encoding="utf-8")
    assert "const int bytes_per_frame" in source
    assert "framesPerBuffer - lenAvailable" in source
    assert "memset(out + lenAvailable * bytes_per_frame, 0" in source
    assert "return paComplete;" in source
    assert "draining" not in source


def test_portaudio_zero_fills_callbacks_with_no_valid_frames():
    source = PORTAUDIO.read_text(encoding="utf-8")
    no_samples = source.split("// No samples remain.", 1)[1].split("}", 1)[0]
    assert "memset(outputBuffer, 0, framesPerBuffer * bytes_per_frame);" in no_samples
    assert "return paComplete;" in no_samples


def test_portaudio_uses_safer_macos_latency_and_no_dither():
    source = PORTAUDIO.read_text(encoding="utf-8")
    header = PORTAUDIO_H.read_text(encoding="utf-8")
    assert "std::atomic<bool> callback_finished{true}" in header
    assert "bool draining" not in header
    # macOS must use LOW output latency (not the old 0.12s high-latency floor
    # that made timing work impossible due to 120ms audio lag).
    assert "defaultLowOutputLatency" in source
    assert "0.01" in source  # minimum floor to avoid underruns
    # The old high-latency floor must be gone from actual code (comments
    # describing the old bug are fine).
    code_lines = [l for l in source.splitlines() if not l.strip().startswith("//")]
    assert not any("0.12" in l for l in code_lines)
    assert "paPrimeOutputBuffersUsingStreamCallback | paDitherOff" in source


def test_soundtouch_avoids_preclipping_and_output_clipping():
    source = SOUNDTOUCH.read_text(encoding="utf-8")
    assert "provider->GetAudio(source_buffer.data(), input_frame, frames)" in source
    assert "GetAudioWithVolume(source_buffer.data(), input_frame, frames, volume)" not in source
    assert "AudioSampleSafety::kSoundTouchInputHeadroom" in source
    assert "provider->GetAudio(dst, input_frame, available)" in source
    assert "AudioSampleSafety::ApplyGainLimiter(out, available * channels(), volume)" in source
    assert "AudioSampleSafety::ConvertFloatToInt16Limited" in source
    assert "std::clamp(output_buffer[i], -1.0f, 1.0f)" not in source
    assert "std::atomic_flag volume_guard" in SOUNDTOUCH_H.read_text(encoding="utf-8")
    assert "volume_for_fill()" in source


def test_volume_changes_reach_soundtouch_processors():
    openal = OPENAL.read_text(encoding="utf-8")
    portaudio = PORTAUDIO.read_text(encoding="utf-8")
    portaudio_h = PORTAUDIO_H.read_text(encoding="utf-8")
    assert "void OpenALPlayer::SetVolume(double vol)" in openal
    assert "tempo_processor->SetVolume(vol);" in openal
    assert "void PortAudioPlayer::SetVolume(double vol)" in portaudio
    assert "tempo_processor->SetVolume(vol);" in portaudio
    assert "std::atomic_flag volume_guard" in portaudio_h
    assert "VolumeForCallback()" in portaudio


def test_portaudio_soundtouch_position_tracks_emitted_audio():
    portaudio = PORTAUDIO.read_text(encoding="utf-8")
    portaudio_h = PORTAUDIO_H.read_text(encoding="utf-8")
    soundtouch = SOUNDTOUCH.read_text(encoding="utf-8")
    soundtouch_h = SOUNDTOUCH_H.read_text(encoding="utf-8")

    assert "double output_frame = 0.0" in soundtouch_h
    assert "int64_t GetOutputFrame() const" in soundtouch_h
    assert "output_frame + got * playback_speed" in soundtouch
    assert "output_start_frame = player->tempo_processor->GetOutputFrame()" in portaudio
    assert "player->current = player->tempo_processor->GetOutputFrame()" in portaudio
    assert "player->tempo_processor->GetInputFrame()" not in portaudio
    assert "timeInfo->outputBufferDacTime" in portaudio
    assert "outputBufferDacTime > 0.0" not in portaudio
    assert "ReadSoundTouchPosition(anchor_frame, anchor_time, anchor_speed)" in portaudio
    assert "elapsed -= info->outputLatency" in portaudio
    assert "std::atomic_flag soundtouch_position_guard" in portaudio_h
    assert "soundtouch_position_sequence" not in portaudio_h
    assert "if (soundtouch_position_guard.test_and_set" in portaudio
    assert "soundtouch_position.valid = false" in portaudio
    assert "ResetSoundTouchPosition();" in portaudio
    assert "std::atomic_flag end_guard" in portaudio_h
    assert "auto playback_end = player->EndForCallback()" in portaudio
    assert "player->tempo_processor->SetEndFrame(playback_end)" in portaudio
    assert "remaining_source / playback_speed" in soundtouch
    assert "tempo_processor->SetPlaybackSpeed(playback_speed)" not in portaudio
    assert "Pa_AbortStream(stream)" in portaudio


def test_openal_reports_source_playback_offset_not_prefill_position():
    source = OPENAL.read_text(encoding="utf-8")
    position_method = source.split("int64_t OpenALPlayer::GetCurrentPosition()", 1)[1]
    position_method = position_method.split("std::unique_ptr<AudioPlayer> CreateOpenALPlayer", 1)[0]
    assert "alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed)" in source
    assert "alGetSourcei(source, AL_SAMPLE_OFFSET, &sample_offset)" in source
    assert "cur_frame + extra * samplerate" not in source
    assert "tempo_processor && tempo_processor->IsFinished())" not in source
    # AL_SAMPLE_OFFSET and processed buffers are already measured in source
    # samples. Dividing them by AL_PITCH double-corrects the position.
    assert "output_frames / playback_speed" not in position_method


def test_openal_soundtouch_uses_output_duration_for_completion():
    source = OPENAL.read_text(encoding="utf-8")
    soundtouch = SOUNDTOUCH.read_text(encoding="utf-8")
    assert "auto fill_len = tempo_processor->Fill" in source
    assert "if (fill_len == 0)" in source
    assert "selected_audio_finished = tempo_processor->IsFinished()" in source
    assert "selected_audio_finished && buffers_free == num_buffers" in source
    assert "if (buffers_free == num_buffers)" in source
    assert "return available;" in soundtouch
    assert "return filled;" in soundtouch
    assert "expected_output_frames" not in source
    assert "(end_frame - start_frame) * bpf" not in source
    assert "buffers_played - num_buffers" not in source


def test_openal_soundtouch_restarts_from_audible_position_on_speed_change():
    source = OPENAL.read_text(encoding="utf-8")
    method = source.split("void OpenALPlayer::SetPlaybackSpeed(double speed)", 1)[1]
    method = method.split("int64_t OpenALPlayer::GetCurrentPosition()", 1)[0]
    assert "if (!playing)" in method
    assert "if (tempo_processor)" in method
    restart = method.split("if (tempo_processor)", 1)[1]
    assert restart.index("GetCurrentPosition()") < restart.index("playback_speed = new_speed")
    assert "Play(restart_position, restart_end - restart_position);" in method
    assert "tempo_processor->SetPlaybackSpeed" not in method
    assert "start_frame = GetCurrentPosition() - buffers_played" not in method


def test_openal_rebuilds_queue_when_end_changes():
    source = OPENAL.read_text(encoding="utf-8")
    method = source.split("void OpenALPlayer::SetEndPosition(int64_t pos)", 1)[1]
    method = method.split("void OpenALPlayer::SetVolume(double vol)", 1)[0]
    assert "if (playing)" in method
    assert "tempo_processor" not in method
    assert "GetCurrentPosition()" in method
    assert "std::min(end_frame, pos)" in method
    assert "Play(restart_position, pos - restart_position);" in method


def test_portaudio_reopens_default_device_after_output_route_change():
    source = PORTAUDIO.read_text(encoding="utf-8")
    header = PORTAUDIO_H.read_text(encoding="utf-8")
    assert "PaDeviceIndex active_device = paNoDevice" in header
    assert "void RebuildDeviceList()" in header
    assert "void CloseStream()" in header
    assert "void RefreshDefaultDevice(bool force = false)" in header
    assert "void PortAudioPlayer::RebuildDeviceList()" in source
    assert "void PortAudioPlayer::CloseStream()" in source
    assert "void PortAudioPlayer::RefreshDefaultDevice(bool force)" in source
    assert "Pa_GetDefaultOutputDevice()" in source
    assert "Pa_CloseStream(stream)" in source
    assert "current_default_info && current_default_info->maxOutputChannels > 0" in source
    assert "std::find(default_device.begin(), default_device.end(), real_idx) == default_device.end()" in source
    assert "std::rotate(default_device.begin(), it, it + 1)" in source
    assert "RefreshDefaultDevice();" in source
    # The PortAudio finished callback runs only after all paComplete output
    # has drained, and publishes completion safely to the UI thread.
    assert "std::atomic<bool> callback_finished{true}" in header
    assert "Pa_SetStreamFinishedCallback(stream, paStreamFinishedCallback)" in source
    assert "callback_finished.store(true, std::memory_order_release)" in source
    assert "Pa_IsStreamActive(stream) == 1" in source
    # IsPlaying must NOT consult callback_finished: paComplete only means "no
    # more callbacks"; the buffered output is still playing. Reporting not-
    # playing at that point makes AudioController Stop() immediately, which
    # drops the buffered audio (audible "last 0.x seconds cut off").
    assert "do NOT consult callback_finished" in source


def test_portaudio_macos_route_change_is_exception_safe():
    """macOS Play() must not crash when the output route changes: reopening the
    stream happens through an exception-safe helper and only when the live
    default device has actually moved, instead of unconditionally on every
    Play() call."""
    source = PORTAUDIO.read_text(encoding="utf-8")
    header = PORTAUDIO_H.read_text(encoding="utf-8")
    # Exception-safe helper declared and defined
    assert "bool EnsureStreamForDefaultDevice();" in header
    assert "bool PortAudioPlayer::EnsureStreamForDefaultDevice()" in source
    # Play() uses the helper instead of the throwing RefreshDefaultDevice(true)
    assert "EnsureStreamForDefaultDevice()" in source
    assert "RefreshDefaultDevice(true);" not in source
    # Fast path: skip the rebuild while the default device is unchanged
    assert "current_default == active_device" in source
    # Route-change reopen must be wrapped so failures never escape Play()
    assert "catch (AudioPlayerOpenError const& err)" in source
    assert "return false;" in source
    # pa_start must be initialized so the first position query is deterministic
    assert "PaTime pa_start = 0.0;" in header


def test_portaudio_play_guards_null_stream():
    """Play() must never hand a null stream to PortAudio: it bails out early
    when no usable stream is open, instead of dereferencing it below."""
    source = PORTAUDIO.read_text(encoding="utf-8")
    assert "if (!stream)" in source
    assert "Play called without an open stream" in source
    # Restart logic must distinguish active playback from an inactive
    # paComplete stream, which still needs to be stopped before restart.
    assert "Pa_IsStreamActive(stream)" in source
    assert "Pa_IsStreamStopped(stream)" in source
    assert "Pa_StartStream(stream)" in source


def test_preview_output_uses_shared_peak_limiter():
    safety = SAMPLE_SAFETY.read_text(encoding="utf-8")
    openal = OPENAL.read_text(encoding="utf-8")
    portaudio = PORTAUDIO.read_text(encoding="utf-8")
    soundtouch = SOUNDTOUCH.read_text(encoding="utf-8")

    assert "kPreviewCeiling = kInt16Max * 0.92" in safety
    assert "ApplyGainLimiter" in safety
    assert "ConvertFloatToInt16Limited" in safety

    assert '#include "audio_sample_safety.h"' in openal
    assert '#include "audio_sample_safety.h"' in portaudio
    assert '#include "audio_sample_safety.h"' in soundtouch

    assert "provider->GetAudio(outputBuffer, player->current, lenAvailable)" in portaudio
    assert "GetAudioWithVolume(outputBuffer, player->current, lenAvailable" not in portaudio
    assert "AudioSampleSafety::ApplyGainLimiter(" in portaudio

    assert "provider->GetAudio(&decode_buffer[0], cur_frame, fill_len)" in openal
    assert "GetAudioWithVolume(&decode_buffer[0], cur_frame, fill_len" not in openal
    assert "AudioSampleSafety::ApplyGainLimiter(" in openal


def main():
    tests = [
        test_openal_uses_provider_sample_format,
        test_portaudio_normal_speed_zero_fills_last_buffer,
        test_portaudio_zero_fills_callbacks_with_no_valid_frames,
        test_portaudio_uses_safer_macos_latency_and_no_dither,
        test_soundtouch_avoids_preclipping_and_output_clipping,
        test_volume_changes_reach_soundtouch_processors,
        test_portaudio_soundtouch_position_tracks_emitted_audio,
        test_openal_reports_source_playback_offset_not_prefill_position,
        test_openal_soundtouch_uses_output_duration_for_completion,
        test_openal_soundtouch_restarts_from_audible_position_on_speed_change,
        test_openal_rebuilds_queue_when_end_changes,
        test_portaudio_reopens_default_device_after_output_route_change,
        test_portaudio_macos_route_change_is_exception_safe,
        test_portaudio_play_guards_null_stream,
        test_preview_output_uses_shared_peak_limiter,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} audio playback tests passed")


if __name__ == "__main__":
    main()
