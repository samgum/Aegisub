#!/usr/bin/env python3
"""Targeted source-level regression checks for the Aug 6-7 fixes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def method(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def test_timer_cadence():
    source = read("src/video_controller.cpp")
    cadence = "std::max(10, std::min(33, 1000 / fps / 2))"
    assert source.count(cadence) == 2
    assert source.count("playback.Start(interval)") == 2
    assert "playback.Start(10)" not in source


def test_cjk_ime_composition_is_observer_atomic():
    source = read("src/osx/scintilla_ime.mm")
    marked = method(source, "- (void)setMarkedText:", "- (void)unmarkText")
    unmark = method(source, "- (void)unmarkText", "- (NSArray *)validAttributesForMarkedText")

    assert "SetModEventMask(0)" in marked
    assert marked.index("SetModEventMask(0)") < marked.index("DeleteRange")
    assert "SetModEventMask(saved_mask)" in marked

    assert "SetModEventMask(0)" in unmark
    assert unmark.index("SetModEventMask(0)") < unmark.index("DeleteRange")
    assert unmark.index("DeleteRange") < unmark.index("SetModEventMask(saved_mask)")


def test_libass_overflow_is_authoritative():
    source = read("src/subtitle_overflow.cpp")
    check = method(source, "Result Check(", "Result CheckText(")
    check_text = method(source, "Result CheckText(", "void InvalidateLine(")

    assert check.index("check_with_libass(") < check.index("check_with_dc(")
    assert "auto rendered = check_with_libass(context, line, text);" in check_text
    assert "if (!result.valid) {\n\t\tauto rendered" not in check_text


def test_libass_renderer_creation_uses_library_lock():
    source = read("src/subtitles_provider_libass.cpp")
    helper = method(source, "ASS_Renderer *init_renderer()", "void msg_callback")
    assert "std::lock_guard<std::mutex> lock(library_mutex)" in helper
    assert source.count("ass_renderer_init(library)") == 1
    assert source.count("init_renderer()") == 5


def test_audio_completion_and_position_units():
    openal = read("src/audio_player_openal.cpp")
    portaudio = read("src/audio_player_portaudio.cpp")
    portaudio_h = read("src/audio_player_portaudio.h")

    assert "output_frames / playback_speed" not in openal
    assert "std::atomic<bool> callback_finished{true}" in portaudio_h
    assert "Pa_SetStreamFinishedCallback(stream, paStreamFinishedCallback)" in portaudio
    assert "callback_finished.load(std::memory_order_acquire)" in portaudio
    assert "draining" not in portaudio
    assert "draining" not in portaudio_h


def test_zero_video_cache_is_really_disabled():
    source = read("src/video_provider_cache.cpp")
    disabled = source.index("if (max_cache_size == 0)")
    first_insert = source.index("if (cache.empty())", disabled)
    assert disabled < first_insert
    assert "master->GetFrame(n, out);" in source[:disabled]


def test_search_offsets_refer_to_stored_bytes():
    source = read("src/search_replace_engine.cpp")
    assert "get_dialogue_field_value" in source
    assert "get_normalized" not in source
    assert "const_cast<AssDialogue*>" not in source
    assert "boost::locale::normalize((diag->*field)" not in source


def main():
    tests = [
        test_timer_cadence,
        test_cjk_ime_composition_is_observer_atomic,
        test_libass_overflow_is_authoritative,
        test_libass_renderer_creation_uses_library_lock,
        test_audio_completion_and_position_units,
        test_zero_video_cache_is_really_disabled,
        test_search_offsets_refer_to_stored_bytes,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} Aug 6-7 regression checks passed")


if __name__ == "__main__":
    main()
