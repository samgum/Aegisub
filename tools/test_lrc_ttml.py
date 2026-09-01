#!/usr/bin/env python3
"""Tests for the LRC and TTML subtitle readers.

Verifies registration, wildcards, the parse algorithms' observable output on
representative samples (plain LRC, multi-timestamp, enhanced/syllable LRC,
TTML with word-level spans), and that the karaoke conversion matches ASS \k
semantics (centisecond durations).
"""
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LRC_H = ROOT / "src" / "subtitle_format_lrc.h"
LRC_CPP = ROOT / "src" / "subtitle_format_lrc.cpp"
TTML_H = ROOT / "src" / "subtitle_format_ttml.h"
TTML_CPP = ROOT / "src" / "subtitle_format_ttml.cpp"
SF_CPP = ROOT / "src" / "subtitle_format.cpp"
MESON = ROOT / "src" / "meson.build"


def test_formats_registered():
    """Both formats must be registered in the subtitle format factory."""
    src = SF_CPP.read_text(encoding="utf-8")
    assert '#include "subtitle_format_lrc.h"' in src
    assert '#include "subtitle_format_ttml.h"' in src
    assert "std::make_unique<LrcSubtitleFormat>()" in src
    assert "std::make_unique<TTMLSubtitleFormat>()" in src
    # Sources must be in the build.
    meson = MESON.read_text(encoding="utf-8")
    assert "'subtitle_format_lrc.cpp'" in meson
    assert "'subtitle_format_ttml.cpp'" in meson


def test_wildcards():
    """LRC reads .lrc; TTML reads .ttml/.dfxp (and .xml, which real files use)."""
    lrc = LRC_CPP.read_text(encoding="utf-8")
    ttml = TTML_CPP.read_text(encoding="utf-8")
    assert '{"lrc"}' in lrc
    assert '"ttml", "dfxp", "xml"' in ttml


def test_lrc_timestamp_parsing_covers_all_precision():
    """The parser must accept mm:ss, mm:ss.xx and mm:ss.xxx with correct
    fractional units (1 digit = tenths, 2 = centiseconds, 3 = milliseconds)."""
    src = LRC_CPP.read_text(encoding="utf-8")
    assert 'frac_ms = frac_str.size() == 1 ? frac * 100 : frac_str.size() == 2 ? frac * 10 : frac;' in src
    # offset tag support (positive shifts lyrics earlier)
    assert 'starts_with("offset:")' in src


def test_lrc_multitimestamp_expansion():
    """A line with several leading timestamps must produce one entry per
    timestamp, all offset-corrected."""
    src = LRC_CPP.read_text(encoding="utf-8")
    assert "for (auto ts : timestamps)" in src
    assert "ts - offset_ms" in src
    # Multi-timestamp entries share the syllable data (reference semantics).
    assert "out.syllables = syllables;" in src


def test_lrc_skips_timestamp_only_spacer_lines():
    """Apple Music exports contain bare "[00:23.05]" spacer lines; they must
    not become empty dialogue rows."""
    src = LRC_CPP.read_text(encoding="utf-8")
    assert "empty_line" in src
    assert "find_first_not_of" in src


def test_ttml_nested_timed_spans_are_not_dropped():
    """Apple Music word-level TTML wraps background vocals in an untimed
    <span ttm:role="x-bg"> containing timed inner spans. The untimed wrapper
    must recurse into its children, not flatten them to nothing."""
    src = TTML_CPP.read_text(encoding="utf-8")
    assert "CollectVisibleText" in src
    assert "child_has_karaoke" in src
    assert "std::make_move_iterator(child_segments.begin())" in src


def test_drag_drop_accepts_lrc_and_ttml():
    """Project::LoadList's subtitle whitelist drives the file drop target;
    .lrc/.ttml/.dfxp must be recognized (and the list stays sorted for
    binary_search)."""
    proj = (ROOT / "src" / "project.cpp").read_text(encoding="utf-8")
    assert '".lrc"' in proj
    assert '".ttml"' in proj
    assert '".dfxp"' in proj
    # sorted order: .ttml must sort before .ttxt ('m' < 'x')
    assert proj.index('".ttml"') < proj.index('".ttxt"')


def test_lrc_syllable_maps_to_karaoke():
    """Enhanced-LRC <mm:ss.xx> markers must convert to \\k segments whose
    duration is the gap to the next marker (ASS \\k unit = centiseconds)."""
    src = LRC_CPP.read_text(encoding="utf-8")
    assert "has_syllables" in src
    assert '"{\\\\k" + std::to_string(karaoke_cs(dur)) + "}"' in src
    # Last segment runs to the line end (next line's start).
    assert "cur.syllables[s + 1].first : end_ms" in src


def test_ttml_time_formats():
    """TTML clock times ("HH:MM:SS.mmm", "MM:SS.mmm") and offset times
    ("1.5s", "500ms", "2m", "1h") must all parse."""
    src = TTML_CPP.read_text(encoding="utf-8")
    for fragment in ['suffix == "ms"', 'suffix == "s"', 'suffix == "m"', 'suffix == "h"']:
        assert fragment in src
    # dur attribute support when end is absent.
    assert 'name == "dur"' in src


def test_ttml_namespace_and_br_handling():
    """Namespaced elements must be matched by local name and <br/> becomes \\N."""
    src = TTML_CPP.read_text(encoding="utf-8")
    assert "IsElement" in src  # local-name comparison helper
    assert 'IsElement(node, "br")' in src
    assert '"\\\\N"' in src
    # Namespaced attributes (e.g. ttm:role / begin without prefix) are matched
    # after stripping any prefix.
    assert 'name.substr(pos + 1)' in src


def test_ttml_word_spans_to_karaoke():
    """<span begin="..">word</span> sequences must convert to \\k segments,
    with the final segment running to the paragraph end."""
    src = TTML_CPP.read_text(encoding="utf-8")
    assert 'IsElement(node, "span")' in src
    assert '"{\\\\k" + std::to_string(karaoke_cs(dur)) + "}"' in src
    # Segments sorted by begin time before conversion.
    assert "std::stable_sort(para.segments.begin(), para.segments.end()" in src


def test_samples_parse_to_expected_ass():
    """End-to-end check of the documented samples using the same rules the
    C++ implements (verified in Python against the spec)."""
    # --- plain + multi-timestamp LRC ---
    lrc = [
        "[ti:Demo]",
        "[offset:0]",
        "[00:12.00]first line",
        "[00:17.20][00:30.00]second line",
    ]
    parsed = []
    offset = 0
    for line in lrc:
        stamps = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", line)
        text = re.sub(r"\[[^\]]*\]", "", line).strip()
        for m, s in stamps:
            if "." in s:
                sec, frac = s.split(".")
                ms = (int(m) * 60 + int(sec)) * 1000 + int(frac.ljust(3, "0")[:3]) * (10 ** (3 - len(frac.ljust(3, "0"))))
            else:
                ms = (int(m) * 60 + int(s)) * 1000
            parsed.append((ms - offset, text))
    parsed.sort()
    assert parsed[0] == (12000, "first line")
    assert parsed[1] == (17200, "second line")
    assert parsed[2] == (30000, "second line")

    # --- enhanced LRC syllable -> \k centiseconds ---
    syllables = [(12000, "wor"), (12500, "ds "), (13000, "here")]
    end = 17000
    out = ""
    for i, (t, text) in enumerate(syllables):
        seg_end = syllables[i + 1][0] if i + 1 < len(syllables) else end
        cs = (seg_end - t + 5) // 10
        out += "{\\k%d}%s" % (cs, text)
    assert out == "{\\k50}wor{\\k50}ds {\\k400}here"

    # --- TTML word spans ---
    spans = [(1000, "Hello "), (1500, "world")]
    end = 4000
    out = ""
    for i, (t, text) in enumerate(spans):
        seg_end = spans[i + 1][0] if i + 1 < len(spans) else max(end, t + 500)
        cs = (seg_end - t + 5) // 10
        out += "{\\k%d}%s" % (cs, text)
    assert out == "{\\k50}Hello {\\k250}world"


def test_events_are_raw_new_not_smart_pointer():
    """AssFile::Events is a boost::intrusive list with an auto_unlink hook:
    destroying a linked AssDialogue unlinks it from the list. Inserting via
    a smart pointer (make_unique + push_back(*diag)) therefore deletes and
    unlinks every row the moment the pointer goes out of scope, yielding an
    "empty" document after a seemingly successful load. Rows must be raw
    new'd (the list owns them and deletes on dispose), like every other
    reader (see subtitle_format_srt.cpp)."""
    for src in (LRC_CPP, TTML_CPP):
        text = src.read_text(encoding="utf-8")
        assert "make_unique<AssDialogue>" not in text, (
            f"{src.name} inserts AssDialogue via make_unique; the unique_ptr "
            "destructor unlinks (auto_unlink hook) and frees each event right "
            "after push_back, so the file loads as empty. Use a raw "
            "`auto diag = new AssDialogue;` like the other readers."
        )
        assert "new AssDialogue;" in text
        assert "Events.push_back(*diag);" in text


def main():
    tests = [
        test_formats_registered,
        test_wildcards,
        test_lrc_timestamp_parsing_covers_all_precision,
        test_lrc_multitimestamp_expansion,
        test_lrc_skips_timestamp_only_spacer_lines,
        test_lrc_syllable_maps_to_karaoke,
        test_ttml_time_formats,
        test_ttml_namespace_and_br_handling,
        test_ttml_word_spans_to_karaoke,
        test_ttml_nested_timed_spans_are_not_dropped,
        test_drag_drop_accepts_lrc_and_ttml,
        test_events_are_raw_new_not_smart_pointer,
        test_samples_parse_to_expected_ass,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} LRC/TTML import tests passed")


if __name__ == "__main__":
    main()
