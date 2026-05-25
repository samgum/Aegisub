#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OPENAL = ROOT / "src" / "audio_player_openal.cpp"
PORTAUDIO = ROOT / "src" / "audio_player_portaudio.cpp"
PORTAUDIO_H = ROOT / "src" / "audio_player_portaudio.h"
SOUNDTOUCH = ROOT / "src" / "audio_player_soundtouch.cpp"
COREAUDIO = ROOT / "src" / "audio_player_coreaudio.cpp"
AUDIO_PLAYER = ROOT / "src" / "audio_player.cpp"
MESON = ROOT / "meson.build"
SRC_MESON = ROOT / "src" / "meson.build"


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
    assert "player->draining = true;" in source
    assert "return paComplete;" in source


def test_portaudio_uses_safer_macos_latency_and_no_dither():
    source = PORTAUDIO.read_text(encoding="utf-8")
    header = PORTAUDIO_H.read_text(encoding="utf-8")
    assert "bool draining = false" in header
    assert "defaultHighOutputLatency" in source
    assert "0.12" in source
    assert "paPrimeOutputBuffersUsingStreamCallback | paDitherOff" in source


def test_soundtouch_avoids_preclipping_and_output_clipping():
    source = SOUNDTOUCH.read_text(encoding="utf-8")
    assert "provider->GetAudio(source_buffer.data(), input_frame, frames)" in source
    assert "GetAudioWithVolume(source_buffer.data(), input_frame, frames, volume)" not in source
    assert "* 0.98" in source
    assert "std::clamp(output_buffer[i], -1.0f, 1.0f)" in source
    assert "32767.0f" in source


def test_volume_changes_reach_soundtouch_processors():
    openal = OPENAL.read_text(encoding="utf-8")
    portaudio_h = PORTAUDIO_H.read_text(encoding="utf-8")
    assert "void OpenALPlayer::SetVolume(double vol)" in openal
    assert "tempo_processor->SetVolume(vol);" in openal
    assert "tempo_processor->SetVolume(vol);" in portaudio_h


def test_macos_coreaudio_backend_is_native_float_stereo_default():
    coreaudio = COREAUDIO.read_text(encoding="utf-8")
    audio_player = AUDIO_PLAYER.read_text(encoding="utf-8")
    meson = MESON.read_text(encoding="utf-8")
    src_meson = SRC_MESON.read_text(encoding="utf-8")
    assert "AudioToolbox" in meson
    assert "WITH_COREAUDIO" in meson
    assert "dep_avail = ['CoreAudio'] + dep_avail" in meson
    assert "'audio_player_coreaudio.cpp'" in src_meson
    assert "CreateCoreAudioPlayer" in audio_player
    assert '{"CoreAudio", CreateCoreAudioPlayer, false}' in audio_player
    assert "AudioQueueNewOutput" in coreaudio
    assert "kAudioFormatFlagsNativeFloatPacked" in coreaudio
    assert "static constexpr UInt32 output_channels = 2" in coreaudio
    assert "preview_headroom = 0.72f" in coreaudio
    assert "peak_ceiling = 0.90f" in coreaudio
    assert "ClampFloat" in coreaudio
    assert "ApplyPeakLimiter" in coreaudio
    assert "peak_ceiling / peak" in coreaudio
    assert "std::clamp(volume.load(), 0.0, 1.0) * preview_headroom" in coreaudio
    assert "provider->GetAudio(source_buffer.data()" in coreaudio
    assert "AudioQueuePrime" in coreaudio
    assert "failed to enqueue buffer" in coreaudio
    assert "failed to start queue" in coreaudio


def main():
    tests = [
        test_openal_uses_provider_sample_format,
        test_portaudio_normal_speed_zero_fills_last_buffer,
        test_portaudio_uses_safer_macos_latency_and_no_dither,
        test_soundtouch_avoids_preclipping_and_output_clipping,
        test_volume_changes_reach_soundtouch_processors,
        test_macos_coreaudio_backend_is_native_float_stereo_default,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} audio playback tests passed")


if __name__ == "__main__":
    main()
