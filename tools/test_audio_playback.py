#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OPENAL = ROOT / "src" / "audio_player_openal.cpp"
PORTAUDIO = ROOT / "src" / "audio_player_portaudio.cpp"
PORTAUDIO_H = ROOT / "src" / "audio_player_portaudio.h"
SOUNDTOUCH = ROOT / "src" / "audio_player_soundtouch.cpp"
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
    assert "AudioSampleSafety::kSoundTouchInputHeadroom" in source
    assert "provider->GetAudio(dst, input_frame, available)" in source
    assert "AudioSampleSafety::ApplyGainLimiter(out, available * channels(), volume)" in source
    assert "AudioSampleSafety::ConvertFloatToInt16Limited" in source
    assert "std::clamp(output_buffer[i], -1.0f, 1.0f)" not in source


def test_volume_changes_reach_soundtouch_processors():
    openal = OPENAL.read_text(encoding="utf-8")
    portaudio_h = PORTAUDIO_H.read_text(encoding="utf-8")
    assert "void OpenALPlayer::SetVolume(double vol)" in openal
    assert "tempo_processor->SetVolume(vol);" in openal
    assert "tempo_processor->SetVolume(vol);" in portaudio_h


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
        test_portaudio_uses_safer_macos_latency_and_no_dither,
        test_soundtouch_avoids_preclipping_and_output_clipping,
        test_volume_changes_reach_soundtouch_processors,
        test_preview_output_uses_shared_peak_limiter,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} audio playback tests passed")


if __name__ == "__main__":
    main()
