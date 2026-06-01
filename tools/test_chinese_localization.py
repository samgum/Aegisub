#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ZH_CN = ROOT / "po" / "zh_CN.po"
ZH_TW = ROOT / "po" / "zh_TW.po"
STYLE_EDITOR = ROOT / "src" / "dialog_style_editor.cpp"
PREFERENCES = ROOT / "src" / "preferences.cpp"
SELECTION = ROOT / "src" / "dialog_selection.cpp"


def test_apply_is_translated_in_chinese_catalogs():
    zh_cn = ZH_CN.read_text(encoding="utf-8")
    zh_tw = ZH_TW.read_text(encoding="utf-8")
    assert 'msgid "Apply"\nmsgstr "应用"' in zh_cn
    assert 'msgid "Apply"\nmsgstr "套用"' in zh_tw
    assert 'msgid "Apply karaoke template"\nmsgstr "应用卡拉OK模板"' in zh_cn
    assert 'msgid "Apply karaoke template"\nmsgstr "套用卡拉OK範本"' in zh_tw
    assert 'msgstr "Apply karaoke template' not in zh_cn
    assert 'msgstr "Apply karaoke template' not in zh_tw


def test_standard_apply_buttons_use_aegisub_translation():
    style_editor = STYLE_EDITOR.read_text(encoding="utf-8")
    preferences = PREFERENCES.read_text(encoding="utf-8")
    selection = SELECTION.read_text(encoding="utf-8")
    assert 'apply_button->SetLabel(_("Apply"))' in style_editor
    assert 'applyButton->SetLabel(_("Apply"))' in preferences
    assert 'apply_button->SetLabel(_("Apply"))' in selection


def main():
    tests = [
        test_apply_is_translated_in_chinese_catalogs,
        test_standard_apply_buttons_use_aegisub_translation,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} Chinese localization tests passed")


if __name__ == "__main__":
    main()
