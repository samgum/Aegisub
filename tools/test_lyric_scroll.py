#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "src" / "command" / "tool.cpp"
DEFAULT_CONFIG = ROOT / "src" / "libresrc" / "default_config.json"


def test_dialog_preview_does_not_write_subtitles():
    source = TOOL.read_text(encoding="utf-8")
    on_preview = source.split("void OnPreview(wxCommandEvent&) {", 1)[1].split("\n\t}", 1)[0]
    assert "UpdateStylePreview();" in on_preview
    assert "apply_lyric_scroll" not in on_preview


def test_dialog_has_style_preview_widgets():
    source = TOOL.read_text(encoding="utf-8")
    preview_header = (ROOT / "src" / "subs_preview.h").read_text(encoding="utf-8")
    preview_cpp = (ROOT / "src" / "subs_preview.cpp").read_text(encoding="utf-8")
    dialog_pos = source.index("class DialogLyricScroll")
    assert '#include "../subs_preview.h"' in source
    assert source.index("std::string plain_lyric_text(AssDialogue *line, LyricScrollSettings const& settings);") < dialog_pos
    assert source.index("std::string escape_ass_text(std::string text);") < dialog_pos
    assert "SubtitlesPreview *active_preview" in source
    assert "SubtitlesPreview *inactive_preview" in source
    assert "std::string PreviewText(bool current)" in source
    assert "Current lyric line\\\\NTranslated line" in source
    assert "UpdateStylePreview()" in source
    assert "Update Preview" in source
    assert "void SetStyle(AssStyle const& style, bool force_center = true)" in preview_header
    assert "void SubtitlesPreview::SetStyle(AssStyle const& new_style, bool force_center)" in preview_cpp


def test_alignment_defaults_to_center_and_generates_anchor_tags():
    source = TOOL.read_text(encoding="utf-8")
    config = DEFAULT_CONFIG.read_text(encoding="utf-8")
    assert "int horizontal_alignment = 1" in source
    assert '"Horizontal Alignment" : 1' in config
    assert "settings.horizontal_alignment = opt_int(\"Tool/Lyric Scroll/Horizontal Alignment\", 0, 2)" in source
    assert "horizontal_alignment->Append(_(\"Left\"))" in source
    assert "horizontal_alignment->Append(_(\"Center\"))" in source
    assert "horizontal_alignment->Append(_(\"Right\"))" in source
    assert "return 4 + std::max(0, std::min(2, settings.horizontal_alignment))" in source
    assert "safe_margin = std::max(0, std::min(settings.margin_lr, res.target_width))" in source
    assert "{\\\\an%d\\\\q2\\\\pos" in source
    assert "{\\\\an%d\\\\q2\\\\move" in source
    assert '"{\\\\an" + std::to_string(lyric_ass_alignment(settings))' in source
    assert "style.alignment = lyric_ass_alignment(settings)" in source
    assert "SetStyle(PreviewAssStyle(true), false)" in source
    assert "SetStyle(PreviewAssStyle(false), false)" in source


def test_line_break_preservation_is_separate_from_tag_stripping():
    source = TOOL.read_text(encoding="utf-8")
    config = DEFAULT_CONFIG.read_text(encoding="utf-8")
    assert "bool preserve_line_breaks = true" in source
    assert '"Preserve Line Breaks" : true' in config
    assert "settings.preserve_line_breaks = OPT_GET(\"Tool/Lyric Scroll/Preserve Line Breaks\")->GetBool()" in source
    assert "preserve_line_breaks = new wxCheckBox" in source
    assert "collapse_spaces_preserving_line_breaks" in source
    assert "ends_with_ass_line_break" in source
    assert "if (settings.preserve_line_breaks)" in source
    assert "replace_all(std::move(text), \"\\\\N\", \" \")" in source


def test_transition_uses_continuous_move_tags():
    source = TOOL.read_text(encoding="utf-8")
    assert "std::tuple<int, int, double, double>" in source
    assert "start_progress = ease_out_cubic" in source
    assert "end_progress = ease_out_cubic" in source
    assert "add_moving_dialogue" in source
    assert "\\\\move(" in source
    assert "\\\\t(0,%d,1.0," in source
    assert "std::tie(span_start, span_end, start_progress, end_progress)" in source
    assert "bool was_visible" in source
    assert "bool is_visible" in source
    assert "if (!was_visible && !is_visible)" in source
    assert "target_y = is_visible" in source


def main():
    tests = [
        test_dialog_preview_does_not_write_subtitles,
        test_dialog_has_style_preview_widgets,
        test_alignment_defaults_to_center_and_generates_anchor_tags,
        test_line_break_preservation_is_separate_from_tag_stripping,
        test_transition_uses_continuous_move_tags,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} lyric scroll tests passed")


if __name__ == "__main__":
    main()
