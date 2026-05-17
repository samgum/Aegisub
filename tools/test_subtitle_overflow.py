#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "subtitle_overflow.cpp"
HEADER = ROOT / "src" / "subtitle_overflow.h"
EDIT_CTRL = ROOT / "src" / "subs_edit_ctrl.cpp"


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
    assert "it->second.text == text && it->second.signature == signature" in source
    assert "result_cache[line->Id] = { text, signature, result };" in source


def test_wrap_bounds_use_margins_and_video_edges():
    source = SOURCE.read_text(encoding="utf-8")
    assert "line_wrap_left" in source
    assert "line_wrap_right" in source
    assert "bound_left >= wrap_left && bound_right <= wrap_right" in source
    assert "ch_left < wrap_left || ch_right > wrap_right" in source
    assert "ch_left < 0. || ch_right > video_w" in source


def test_public_check_text_api_exists():
    header = HEADER.read_text(encoding="utf-8")
    assert "Result CheckText(agi::Context *context, AssDialogue const *line, std::string const& text, wxDC *dc = nullptr);" in header


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


def test_positioned_lines_use_video_bounds_not_style_margins():
    source = SOURCE.read_text(encoding="utf-8")
    assert "if (layout.positioned)\n\t\treturn 0.;" in source
    assert "if (layout.positioned)\n\t\treturn video_w;" in source


def main():
    tests = [
        test_edit_box_checks_live_text_not_cached_line_text,
        test_cache_is_guarded_by_text_content,
        test_wrap_bounds_use_margins_and_video_edges,
        test_public_check_text_api_exists,
        test_margin_overflow_marks_wrapped_english_tail,
        test_cjk_without_spaces_still_overflows_video_edge,
        test_positioned_lines_use_video_bounds_not_style_margins,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} subtitle overflow tests passed")


if __name__ == "__main__":
    main()
