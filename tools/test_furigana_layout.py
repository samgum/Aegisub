#!/usr/bin/env python3
import json
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


def display_units(ch):
    cp = ord(ch)
    if ch in (" ", "\t"):
        return 0.5
    if cp < 0x80:
        return 0.55
    if 0x3040 <= cp <= 0x30FF or 0x3400 <= cp <= 0x9FFF or 0xFF01 <= cp <= 0xFF60:
        return 1.0
    return 0.8


def auto_wrap_plain_text(text, max_units):
    lines = []
    current = ""
    width = 0.0
    for ch in text:
        char_width = display_units(ch)
        if current and width + char_width > max_units:
            lines.append(current)
            current = ""
            width = 0.0
        current += ch
        width += char_width
    lines.append(current)
    return lines


def compose_multiline_order(base_lines, lines_with_readings, above=False):
    output = []
    for index, line in enumerate(base_lines):
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


def test_source_uses_per_occurrence_reading_keys():
    source = SOURCE.read_text(encoding="utf-8")
    required = [
        "make_furigana_occurrence_key",
        "parse_furigana_occurrence_label",
        "lookup_furigana_reading",
        "row#occurrence kanji=reading",
        "line->Row + 1, occurrence, term",
    ]
    for token in required:
        assert token in source


def test_sudachi_autofill_can_return_occurrence_readings():
    source = SOURCE.read_text(encoding="utf-8")
    required = [
        "make_sudachi_text_array",
        "cjk_runs(text)",
        "readings[f\"@{row}#{occurrence}#{term}\"] = reading",
        "auto_readings.find(key)",
    ]
    for token in required:
        assert token in source


def make_occurrence_key(row, occurrence, term):
    return f"@{row}#{occurrence}#{term}"


def lookup_reading(readings, row, occurrence, term):
    return readings.get(make_occurrence_key(row, occurrence, term), readings.get(term, ""))


def test_occurrence_reading_overrides_global_default():
    readings = {
        "今日": "きょう",
        make_occurrence_key(8, 2, "今日"): "こんにち",
    }
    assert lookup_reading(readings, 8, 1, "今日") == "きょう"
    assert lookup_reading(readings, 8, 2, "今日") == "こんにち"


def test_source_supports_style_scoped_apply_button():
    source = SOURCE.read_text(encoding="utf-8")
    required = [
        "Apply by Style...",
        "ProcessByStyle",
        "wxGetSingleChoiceIndex",
        "context->ass->GetStyles()",
        "Apply(styles[choice])",
        "line->Style.get() != style_filter",
        "Style filter",
        "Skipped by style",
    ]
    for token in required:
        assert token in source


def test_source_has_auto_wrap_support():
    source = SOURCE.read_text(encoding="utf-8")
    required = [
        "auto_wrap_ass_visual_line_for_furigana",
        "split_and_auto_wrap_for_furigana",
        "furigana_auto_wrap_units",
        "GetScriptInfoAsInt(\"WrapStyle\")",
    ]
    for token in required:
        assert token in source


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


def test_explicit_multiline_below_keeps_each_ruby_line_adjacent():
    base_lines = ["mirai", "doryoku"]
    result = compose_multiline_order(base_lines, {0: "mirai-ruby", 1: "doryoku-ruby"}, above=False)
    assert split_ass_visual_lines(result) == [
        "mirai",
        MARKER + "{\\r\\fs35\\alpha&HFF&}mirai-ruby{\\r}",
        "doryoku",
        MARKER + "{\\r\\fs35\\alpha&HFF&}doryoku-ruby{\\r}",
    ]


def test_explicit_multiline_above_keeps_each_ruby_line_adjacent():
    base_lines = ["mirai", "doryoku"]
    result = compose_multiline_order(base_lines, {0: "mirai-ruby", 1: "doryoku-ruby"}, above=True)
    assert split_ass_visual_lines(result) == [
        MARKER + "{\\r\\fs35\\alpha&HFF&}mirai-ruby{\\r}",
        "mirai",
        MARKER + "{\\r\\fs35\\alpha&HFF&}doryoku-ruby{\\r}",
        "doryoku",
    ]


def test_auto_wrapped_line_gets_per_visual_line_ruby():
    base = "\u300c\u6b62\u307e\u306a\u3044\u96e8\u306f\u306a\u3044\u300d\u3088\u308a\u5148\u306b\u305d\u306e\u5098\u3092\u304f\u308c\u3088"
    wrapped = auto_wrap_plain_text(base, 14.0)
    result = compose_multiline_order(wrapped, {0: "line-1-ruby", 1: "line-2-ruby"}, above=False)
    assert wrapped == [
        "\u300c\u6b62\u307e\u306a\u3044\u96e8\u306f\u306a\u3044\u300d\u3088\u308a\u5148\u306b",
        "\u305d\u306e\u5098\u3092\u304f\u308c\u3088",
    ]
    assert split_ass_visual_lines(result) == [
        wrapped[0],
        MARKER + "{\\r\\fs35\\alpha&HFF&}line-1-ruby{\\r}",
        wrapped[1],
        MARKER + "{\\r\\fs35\\alpha&HFF&}line-2-ruby{\\r}",
    ]


def main():
    tests = [
        test_source_does_not_scale_ruby_glyphs,
        test_source_uses_small_ruby_spacing_path,
        test_source_uses_per_occurrence_reading_keys,
        test_sudachi_autofill_can_return_occurrence_readings,
        test_occurrence_reading_overrides_global_default,
        test_source_supports_style_scoped_apply_button,
        test_source_has_auto_wrap_support,
        test_default_config_has_migrated_small_ruby_default,
        test_old_marker_lines_are_removed_for_regeneration,
        test_explicit_multiline_below_keeps_each_ruby_line_adjacent,
        test_explicit_multiline_above_keeps_each_ruby_line_adjacent,
        test_auto_wrapped_line_gets_per_visual_line_ruby,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} furigana layout tests passed")


if __name__ == "__main__":
    main()
