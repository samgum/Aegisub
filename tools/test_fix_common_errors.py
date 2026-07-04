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
    # Must build the full 'all' list to find neighbours.
    assert "for (auto& line : context->ass->Events)" in proc


def test_empty_removal_is_safe():
    """Empty-line removal must erase from the list before deleting the object,
    and must clear the active line first to avoid a dangling selection."""
    src = DIALOG_CPP.read_text(encoding="utf-8")
    idx = src.index("DialogFixCommonErrors::Process")
    proc = src[idx:]
    assert "Events.erase" in proc
    assert "SetActiveLine(nullptr)" in proc


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
        test_chinese_translations,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} fix-common-errors tests passed")


if __name__ == "__main__":
    main()
