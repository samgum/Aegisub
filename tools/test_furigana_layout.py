#!/usr/bin/env python3
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "dialog_timing_tools.cpp"
CONFIG = ROOT / "src" / "libresrc" / "default_config.json"
MARKER = "{sgmy-furigana}"


def split_ass_visual_lines(text):
    lines = []
    start = 0
    pos = 0
    while pos < len(text):
        if text[pos] == "\\" and pos + 1 < len(text) and text[pos + 1] in ("N", "n"):
            lines.append(text[start:pos])
            pos += 2
            start = pos
        else:
            pos += 1
    lines.append(text[start:])
    return lines


def join_ass_visual_lines(lines):
    return "\\N".join(lines)


def strip_existing_furigana(text):
    kept = []
    had = False
    for line in split_ass_visual_lines(text):
        if line.startswith(MARKER):
            had = True
            continue
        kept.append(line)
    return join_ass_visual_lines(kept) if had else text, had


def compose_multiline_order(base, lines_with_readings, above=False):
    output = []
    for index, line in enumerate(split_ass_visual_lines(base)):
        if index in lines_with_readings:
            ruby = MARKER + "{\\r\\fs35\\alpha&HFF&}" + lines_with_readings[index] + "{\\r}"
            if above:
                output.extend([ruby, line])
            else:
                output.extend([line, ruby])
        else:
            output.append(line)
    return join_ass_visual_lines(output)


def test_source_does_not_scale_ruby_glyphs():
    source = SOURCE.read_text(encoding="utf-8")
    assert '"\\\\fscx" + format_ass_number' not in source
    assert "\\\\fscx100\\\\fscy100\\\\fsp0\\\\p0" in source


def test_source_uses_small_ruby_spacing_path():
    source = SOURCE.read_text(encoding="utf-8")
    required = [
        "struct FuriganaFormat",
        "append_hidden_space",
        "make_visible_furigana_segment",
        "make_furigana_format",
        "Size Default Migrated",
    ]
    for token in required:
        assert token in source
    assert "make_furigana_tags" not in source
    assert "ruby_tags" not in source


def test_default_config_has_migrated_small_ruby_default():
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    furigana = config["Tool"]["Furigana Annotation"]
    assert furigana["Size Percent"] == 35
    assert furigana["Size Default Migrated"] is False
    assert furigana["Kana Mode"] == 0


def test_old_marker_lines_are_removed_for_regeneration():
    text = "base1\\N" + MARKER + "{\\r\\fs35}ruby1{\\r}\\Nbase2\\N" + MARKER + "{\\r\\fs35}ruby2{\\r}"
    stripped, had = strip_existing_furigana(text)
    assert had is True
    assert stripped == "base1\\Nbase2"


def test_multiline_below_keeps_each_ruby_line_adjacent():
    base = "未来へ\\N努力して"
    result = compose_multiline_order(base, {0: "みらい", 1: "どりょく"}, above=False)
    assert split_ass_visual_lines(result) == [
        "未来へ",
        MARKER + "{\\r\\fs35\\alpha&HFF&}みらい{\\r}",
        "努力して",
        MARKER + "{\\r\\fs35\\alpha&HFF&}どりょく{\\r}",
    ]


def test_multiline_above_keeps_each_ruby_line_adjacent():
    base = "未来へ\\N努力して"
    result = compose_multiline_order(base, {0: "みらい", 1: "どりょく"}, above=True)
    assert split_ass_visual_lines(result) == [
        MARKER + "{\\r\\fs35\\alpha&HFF&}みらい{\\r}",
        "未来へ",
        MARKER + "{\\r\\fs35\\alpha&HFF&}どりょく{\\r}",
        "努力して",
    ]


def main():
    tests = [
        test_source_does_not_scale_ruby_glyphs,
        test_source_uses_small_ruby_spacing_path,
        test_default_config_has_migrated_small_ruby_default,
        test_old_marker_lines_are_removed_for_regeneration,
        test_multiline_below_keeps_each_ruby_line_adjacent,
        test_multiline_above_keeps_each_ruby_line_adjacent,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} furigana layout tests passed")


if __name__ == "__main__":
    main()
