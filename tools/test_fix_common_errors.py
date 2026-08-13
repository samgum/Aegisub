#!/usr/bin/env python3
"""Static tests for the Fix Common Errors tool.

This mirrors Subtitle Edit's Tools -> Fix common errors: a single dialog that
batch-fixes overlapping times, short gaps, short/long durations, empty lines
and trailing whitespace. Guards against the command/dialog/toolbar/menu wiring
coming apart and against the time-commit type being wrong.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL_CPP = ROOT / "src" / "command" / "tool.cpp"
DIALOG_CPP = ROOT / "src" / "dialog_timing_tools.cpp"
DIALOGS_H = ROOT / "src" / "dialogs.h"
TOOLBAR = ROOT / "src" / "libresrc" / "default_toolbar.json"
MENU = ROOT / "src" / "libresrc" / "default_menu.json"
CONFIG = ROOT / "src" / "libresrc" / "default_config.json"
ZH_CN = ROOT / "po" / "zh_CN.po"
ZH_TW = ROOT / "po" / "zh_TW.po"

CMD = "tool/time/fix_common_errors"
DISP = "Fix Common Errors"


def test_command_registered():
    """The command must be defined, registered, and call the dialog."""
    src = TOOL_CPP.read_text(encoding="utf-8")
    assert ('CMD_NAME("%s")' % CMD) in src
    assert ('STR_DISP("%s")' % DISP) in src
    assert "ShowFixCommonErrorsDialog(c)" in src
    assert "reg(std::make_unique<tool_time_fix_common_errors>())" in src
    # Only one definition.
    assert src.count('CMD_NAME("%s")' % CMD) == 1


def test_dialog_implemented():
    """DialogFixCommonErrors must exist and implement all six fixes."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    assert "class DialogFixCommonErrors final : public wxDialog" in src
    assert "void ShowFixCommonErrorsDialog(agi::Context *c)" in src
    # All six fix controls must be present.
    for ctrl in ["fix_overlaps", "fix_short_gaps", "fix_short_durations",
	             "fix_long_durations", "remove_empty", "strip_trailing_space"]:
        assert ctrl in src, "missing control %s" % ctrl
    # Threshold spin controls.
    for spin in ["min_gap_ms", "min_duration_ms", "max_duration_ms"]:
        assert spin in src, "missing spin %s" % spin


def test_dialog_declared():
    """The Show function must be declared in dialogs.h."""
    src = DIALOGS_H.read_text(encoding="utf-8")
    assert "void ShowFixCommonErrorsDialog(agi::Context *c);" in src


def test_wired_into_toolbar_and_menu():
    """The command must appear in both the main toolbar and the Tools menu."""
    toolbar = TOOLBAR.read_text(encoding="utf-8")
    menu = MENU.read_text(encoding="utf-8")
    assert ('"%s"' % CMD) in toolbar, "not in main toolbar"
    assert ('"%s"' % CMD) in menu, "not in tools menu"


def test_config_defaults_present():
    """The threshold options must have defaults in default_config.json."""
    src = CONFIG.read_text(encoding="utf-8")
    assert '"Fix Errors"' in src
    assert '"Min Gap"' in src
    assert '"Min Duration"' in src
    assert '"Max Duration"' in src


def test_uses_correct_commit_types():
    """Timing fixes must commit COMMIT_DIAG_TIME; text fixes COMMIT_DIAG_TEXT;
    row removal COMMIT_DIAG_ADDREM, so undo and the audio/video/selection
    refresh hooks each fire correctly."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    assert "COMMIT_DIAG_TIME" in proc, "timing commit missing"
    assert "COMMIT_DIAG_TEXT" in proc, "text commit missing"
    assert "COMMIT_DIAG_ADDREM" in proc, "row-removal commit missing"


def test_overlap_fix_logic_correct():
    """The overlap fix must set End = next.Start (not Start = prev.End), and
    must walk the full ordered event list so the neighbour is real."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    # Must compare cur->End > nxt->Start and set cur->End = nxt->Start.
    assert "cur_end > nxt_start" in proc
    assert "cur->End = nxt_start" in proc
    assert "nxt_start > cur->Start" in proc
    # Must build the full 'all' list to find neighbours.
    assert "for (auto& line : context->ass->Events)" in proc
    # File order is not necessarily chronological; neighbour comparisons must be.
    assert "std::stable_sort(all.begin(), all.end()" in proc
    assert "return lhs->Start < rhs->Start;" in proc


def test_selected_lookup_is_constant_time():
    """Selected-row mode must not scan the selection for every event pair."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    assert "std::unordered_set<AssDialogue *> targets" in proc
    assert "targets.count(d) != 0" in proc
    assert "std::find(lines.begin(), lines.end(), d)" not in proc


def test_comments_are_not_modified():
    """Comment rows are metadata and must be excluded from every fix."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    duration = proc[proc.index("// --- Per-line duration fixes."):proc.index("// --- Neighbour timing fixes.")]
    text = proc[proc.index("// --- Text fixes."):proc.index("// --- Remove empty lines.")]
    empty = proc[proc.index("// --- Remove empty lines."):proc.index("if (!to_delete.empty())")]
    assert "if (line->Comment)" in duration
    assert "if (line->Comment)" in text
    assert "if (line->Comment)" in empty
    assert "if (!line.Comment)" in proc


def test_commits_require_real_changes():
    """Selecting an option without finding a problem must not create undo entries."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    assert "bool changed_time = n_overlap || n_gap || n_short || n_long;" in proc
    assert "bool changed_text = n_trim != 0;" in proc
    assert "bool changed_time = do_overlaps" not in proc
    assert "bool changed_text = do_trim" not in proc


def test_fix_categories_create_real_separate_undo_snapshots():
    """Timing must be committed before text mutation, and text before deletion."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    proc = src[src.index("DialogFixCommonErrors::Process"):]
    timing_commit = proc.index('Commit(_("fix common errors (timing)")')
    text_fixes = proc.index("// --- Text fixes.")
    text_commit = proc.index('Commit(_("fix common errors (text)")')
    removal = proc.index("// --- Remove empty lines.")
    assert timing_commit < text_fixes < text_commit < removal


def test_empty_removal_is_safe():
    """Empty-line removal must erase from the list before deleting the object,
    and must rebuild the selection from survivors (SetSelectionAndActive) so
    the selection controller never holds a dangling pointer to a freed line."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    assert "Events.erase" in proc
    # Must rebuild selection from survivors, not just clear the active line.
    assert "SetSelectionAndActive" in proc


def test_ass_aware_empty_detection_preserves_non_dialogue_content():
    """Whitespace detection must understand ASS escapes/tags without deleting
    drawing commands, templates, or comments."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    helper = src[src.index("bool is_ass_whitespace_only("):src.index("wxString get_history_string")]
    proc = src[src.index("DialogFixCommonErrors::Process"):]
    assert "line.ParseTags()" in helper
    assert "AssBlockType::DRAWING" in helper
    assert "AssBlockType::PLAIN" in helper
    assert "0xE3" in helper and "0x80" in helper  # UTF-8 U+3000
    assert "text[i + 1] == 'h'" in helper
    assert 'boost::istarts_with(effect, "template")' in helper
    assert 'effect.compare(0, 8, "template")' not in helper
    assert "is_ass_whitespace_only(*line)" in proc
    empty = proc[proc.index("// --- Remove empty lines."):proc.index("if (!to_delete.empty())")]
    assert "if (line->Comment)" in empty


def test_delete_all_keeps_one_editable_line_and_precise_commit():
    """Deleting every empty event must mirror edit/delete: add a replacement,
    commit ADDREM only, then select the committed replacement."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    proc = src[src.index("DialogFixCommonErrors::Process"):]
    removal = proc[proc.index("bool changed_rows = !to_delete.empty();"):]
    assert "if (!new_active)" in removal
    assert "new_active = new AssDialogue;" in removal
    assert "context->ass->Events.push_back(*new_active);" in removal
    commit = removal.index('Commit(_("fix common errors (remove empty)"), AssFile::COMMIT_DIAG_ADDREM);')
    selection = removal.index("SetSelectionAndActive", commit)
    assert commit < selection
    assert "COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL" not in removal


def test_chinese_translations():
    """Both simplified and traditional Chinese must translate the key strings."""
    for po in (ZH_CN, ZH_TW):
        src = po.read_text(encoding="utf-8")
        for msgid in [DISP, "Fix Common Subtitle Errors",
                      "Fix overlapping display times (trim end to next start)",
                      "Remove empty / whitespace-only lines"]:
            assert ('msgid "%s"' % msgid) in src, "%s missing in %s" % (msgid, po.name)


def main():
    tests = [
        test_command_registered,
        test_dialog_implemented,
        test_dialog_declared,
        test_wired_into_toolbar_and_menu,
        test_config_defaults_present,
        test_uses_correct_commit_types,
        test_overlap_fix_logic_correct,
        test_selected_lookup_is_constant_time,
        test_comments_are_not_modified,
        test_commits_require_real_changes,
        test_fix_categories_create_real_separate_undo_snapshots,
        test_empty_removal_is_safe,
        test_ass_aware_empty_detection_preserves_non_dialogue_content,
        test_delete_all_keeps_one_editable_line_and_precise_commit,
        test_chinese_translations,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} fix-common-errors tests passed")


if __name__ == "__main__":
    main()
