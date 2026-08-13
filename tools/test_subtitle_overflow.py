#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "subtitle_overflow.cpp"
HEADER = ROOT / "src" / "subtitle_overflow.h"
EDIT_CTRL = ROOT / "src" / "subs_edit_ctrl.cpp"
CONFIG = ROOT / "src" / "libresrc" / "default_config.json"
PREFERENCES = ROOT / "src" / "preferences.cpp"
BASE_GRID = ROOT / "src" / "base_grid.cpp"


def simulated_overflow_indices(char_widths, anchor_x, alignment, wrap_left, wrap_right, video_width):
    width = sum(char_widths)
    if alignment == "left":
        left = anchor_x
    elif alignment == "center":
        left = anchor_x - width / 2
    else:
        left = anchor_x - width

    indices = []
    cursor = left
    for index, width in enumerate(char_widths):
        if cursor < wrap_left or cursor + width > wrap_right or cursor < 0 or cursor + width > video_width:
            indices.append(index)
        cursor += width
    return indices


def test_edit_box_checks_live_text_not_cached_line_text():
    edit = EDIT_CTRL.read_text(encoding="utf-8")
    assert "subtitle_overflow::CheckText(context, diag, line_text)" in edit
    assert "subtitle_overflow::Check(context, diag)" not in edit


def test_cache_is_guarded_by_text_content():
    source = SOURCE.read_text(encoding="utf-8")
    assert "struct CachedResult" in source
    assert "std::string text;" in source
    assert "std::string signature;" in source
    assert "cache_signature(context, *line)" in source
    assert "it->second.text != text || it->second.signature != signature" in source
    assert "store_result(fast_result_cache" in source
    assert "store_result(exact_result_cache" in source


def test_fast_and_exact_caches_are_isolated():
    source = SOURCE.read_text(encoding="utf-8")
    assert "static std::unordered_map<int, CachedResult> fast_result_cache;" in source
    assert "static std::unordered_map<int, CachedResult> exact_result_cache;" in source
    assert "cache_hit(fast_result_cache" in source
    assert "cache_hit(exact_result_cache" in source
    assert "InvalidateExactLine" in source


def test_keystroke_check_never_calls_libass():
    source = SOURCE.read_text(encoding="utf-8")
    fast_body = source.split("Result CheckText(", 1)[1].split("Result CheckTextExact(", 1)[0]
    exact_body = source.split("Result CheckTextExact(", 1)[1].split("void InvalidateLine", 1)[0]
    assert "check_with_libass" not in fast_body
    assert "measure_with_dc" in fast_body
    assert "check_with_libass" in exact_body


def test_exact_check_is_debounced_and_revalidates_active_text():
    edit = EDIT_CTRL.read_text(encoding="utf-8")
    header = (ROOT / "src" / "subs_edit_ctrl.h").read_text(encoding="utf-8")
    assert "wxTimer overflow_check_timer;" in header
    assert "overflow_check_timer.StartOnce(400);" in edit
    assert "SubsTextEditCtrl::OnOverflowCheckTimer" in edit
    assert "diag->Id != overflow_pending_line_id" in edit
    assert "current_text != overflow_pending_text" in edit
    assert "subtitle_overflow::CheckTextExact(context, diag, overflow_pending_text)" in edit
    assert "context->subsGrid->Refresh(false);" in edit


def test_wrap_bounds_use_margins_and_video_edges():
    source = SOURCE.read_text(encoding="utf-8")
    assert "line_wrap_left" in source
    assert "line_wrap_right" in source
    assert "bool video_overflow = bound_left < 0. || bound_right > video_w;" in source
    assert "bool wrap_overflow = detect_wrap_overflow" in source
    assert "segment.width > wrap_width * 1.02" in source
    assert "wrap_overflow && (ch_left < wrap_left || ch_right > wrap_right)" in source


def test_final_overflow_uses_libass_pixels():
    source = SOURCE.read_text(encoding="utf-8")
    libass = (ROOT / "src" / "subtitles_provider_libass.cpp").read_text(encoding="utf-8")
    header = (ROOT / "src" / "subtitles_provider_libass.h").read_text(encoding="utf-8")
    assert "libass::GetRenderedBounds" in source
    assert "make_single_line_file" in source
    assert "file.SetScriptInfo(\"WrapStyle\", \"2\")" in source
    assert "appears_auto_wrapped" in source
    assert "outside_video(normal, video_w, video_h)" in source
    assert "RenderedBounds GetRenderedBounds" in header
    assert "row[x]" in libass or "nonzero_span(row, img->w)" in libass
    assert "bounds.bands = count_bands" in libass


def test_grid_cache_uses_render_relevant_signature():
    source = SOURCE.read_text(encoding="utf-8")
    assert "static_cast<int>(line.Start)" in source
    assert "static_cast<int>(line.End)" in source
    assert "line.Actor.get()" in source
    assert "line.Effect.get()" in source
    signature_body = source.split("std::string cache_signature", 1)[1].split("void apply_override_block", 1)[0]
    assert "cache_generation" in signature_body
    assert "context->ass->Info" not in signature_body
    assert "context->ass->Styles" not in signature_body
    assert "context->ass->Attachments" not in signature_body


def test_signature_covers_line_extradata_and_style_geometry():
    source = SOURCE.read_text(encoding="utf-8")
    signature_body = source.split("std::string cache_signature", 1)[1].split("void apply_override_block", 1)[0]
    assert "line.ExtradataIds.get().size()" in signature_body
    assert "for (auto id : line.ExtradataIds.get())" in signature_body
    assert "context->ass->Extradata" not in signature_body
    for field in (
        "style->underline",
        "style->strikeout",
        "style->angle",
        "style->borderstyle",
        "style->encoding",
        "style->primary.a",
        "style->secondary.a",
        "style->outline.a",
        "style->shadow.a",
    ):
        assert field in signature_body


def test_grid_full_commit_invalidation_requires_all_dialogue_flags():
    grid = BASE_GRID.read_text(encoding="utf-8")
    assert "(type & AssFile::COMMIT_DIAG_FULL) == AssFile::COMMIT_DIAG_FULL" in grid
    assert "AssFile::COMMIT_ATTACHMENT" in grid
    assert "AssFile::COMMIT_EXTRADATA" in grid
    assert "AssFile::COMMIT_DIAG_ADDREM" in grid


def test_active_grid_reuses_provisional_then_exact_result():
    source = SOURCE.read_text(encoding="utf-8")
    check_body = source.split("Result Check(", 1)[1].split("Result CheckText(", 1)[0]
    assert "cache_hit(exact_result_cache" in check_body
    assert "context->selectionController->GetActiveLine()" in check_body
    assert "cache_hit(fast_result_cache" in check_body


def test_grid_mode_uses_video_overflow_only():
    source = SOURCE.read_text(encoding="utf-8")
    assert "result = check_with_libass(context, line, text);" in source
    assert "measure_with_dc(context, line, text, dc, false)" in source
    assert "measure_with_dc(context, line, text, dc, true)" in source


def test_public_check_text_api_exists():
    header = HEADER.read_text(encoding="utf-8")
    assert "Result CheckText(agi::Context *context, AssDialogue const *line, std::string const& text, wxDC *dc = nullptr);" in header
    assert "Result CheckTextExact(agi::Context *context, AssDialogue const *line, std::string const& text, wxDC *dc = nullptr);" in header


def test_caches_have_hard_size_limits():
    source = SOURCE.read_text(encoding="utf-8")
    assert "max_result_cache_entries = 512" in source
    assert "max_font_cache_entries = 128" in source
    assert "max_chars_per_font = 1024" in source
    assert "cache.erase(cache.begin())" in source
    assert "char_width_cache.erase(char_width_cache.begin())" in source
    assert "widths.erase(widths.begin())" in source


def test_margin_overflow_marks_wrapped_english_tail():
    # A centered line wider than the style margins should be treated as too long
    # even if it would not cross the physical video edge.
    indices = simulated_overflow_indices(
        char_widths=[10] * 18,
        anchor_x=100,
        alignment="center",
        wrap_left=40,
        wrap_right=160,
        video_width=200,
    )
    assert indices == [0, 1, 2, 15, 16, 17]


def test_cjk_without_spaces_still_overflows_video_edge():
    # No-space CJK text can run straight off screen; this must still count even
    # when margins are not the limiting factor.
    indices = simulated_overflow_indices(
        char_widths=[12] * 10,
        anchor_x=10,
        alignment="left",
        wrap_left=0,
        wrap_right=200,
        video_width=100,
    )
    assert indices == [7, 8, 9]


def test_old_rendered_width_mode_does_not_force_cjk_dc_fallback():
    source = SOURCE.read_text(encoding="utf-8")
    assert "has_long_unbroken_cjk_run" not in source
    assert "is_cjk_or_kana" not in source
    assert "decode_utf8(std::string const& text" not in source
    assert "normal_w < nowrap_w * 0.92" in source
    assert "outside_video(nowrap, video_w, video_h) && !outside_video(normal, video_w, video_h)" in source


def test_character_limit_mode_counts_plain_text_only():
    source = SOURCE.read_text(encoding="utf-8")
    config = CONFIG.read_text(encoding="utf-8")
    preferences = PREFERENCES.read_text(encoding="utf-8")
    edit = EDIT_CTRL.read_text(encoding="utf-8")
    assert '"Mode" : 0' in config
    assert '"Character Limit" : 15' in config
    assert "Rendered subtitle width" in preferences
    assert "Plain text character count" in preferences
    assert "Overflow highlight mode" in preferences
    assert "Overflow character limit" in preferences
    assert "check_with_character_limit" in source
    assert 'OPT_GET("Subtitle/Overflow Highlight/Mode")->GetInt() == 1' in source
    assert "if (text[pos] == '{')" in source
    assert "command == 'N' || command == 'n'" in source
    assert "command == 'h'" in source
    assert "add_plain_char(pos, len)" in source
    assert 'OPT_SUB("Subtitle/Overflow Highlight/Mode"' in edit
    assert 'OPT_SUB("Subtitle/Overflow Highlight/Character Limit"' in edit


def test_positioned_lines_use_video_bounds_not_style_margins():
    source = SOURCE.read_text(encoding="utf-8")
    assert "if (layout.positioned)\n\t\treturn 0.;" in source
    assert "if (layout.positioned)\n\t\treturn video_w;" in source


def main():
    tests = [
        test_edit_box_checks_live_text_not_cached_line_text,
        test_cache_is_guarded_by_text_content,
        test_fast_and_exact_caches_are_isolated,
        test_keystroke_check_never_calls_libass,
        test_exact_check_is_debounced_and_revalidates_active_text,
        test_wrap_bounds_use_margins_and_video_edges,
        test_final_overflow_uses_libass_pixels,
        test_grid_cache_uses_render_relevant_signature,
        test_signature_covers_line_extradata_and_style_geometry,
        test_grid_full_commit_invalidation_requires_all_dialogue_flags,
        test_active_grid_reuses_provisional_then_exact_result,
        test_grid_mode_uses_video_overflow_only,
        test_public_check_text_api_exists,
        test_caches_have_hard_size_limits,
        test_margin_overflow_marks_wrapped_english_tail,
        test_cjk_without_spaces_still_overflows_video_edge,
        test_old_rendered_width_mode_does_not_force_cjk_dc_fallback,
        test_character_limit_mode_counts_plain_text_only,
        test_positioned_lines_use_video_bounds_not_style_margins,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} subtitle overflow tests passed")


if __name__ == "__main__":
    main()
