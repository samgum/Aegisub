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


def test_readme_bilingual_switchers_and_docs():
    en_readme = (ROOT / "README.md").read_text(encoding="utf-8")
    cn_readme = (ROOT / "README_CN.md").read_text(encoding="utf-8")
    switcher = "**[English](README.md)** | **[简体中文](README_CN.md)**"
    assert switcher in en_readme
    assert switcher in cn_readme
    for doc in (en_readme, cn_readme):
        assert "SudachiPy" in doc
        assert "Stitch Timings" in doc
        assert "tools/run_source_regressions.py" in doc
        assert ("0.25x" in doc) or ("0.25×" in doc)
    assert "Check Style Overlaps" in en_readme
    assert ("检查样式重叠" in cn_readme) or ("Check Style Overlaps" in cn_readme)


def test_pair_check_format_specifiers_and_no_fuzzy_chinese_entries():
    for path in (ZH_CN, ZH_TW):
        text = path.read_text(encoding="utf-8")
        assert "#, fuzzy" not in text
        for prefix in ("Unexpected closing %s at character %llu", "Unclosed %s at character %llu"):
            assert f'msgid "{prefix}"\n' in text
            block = text.split(f'msgid "{prefix}"\n', 1)[1].split("\n\n", 1)[0]
            assert "%s" in block and "%llu" in block
            assert block.index("%s") < block.index("%llu")


def main():
    tests = [
        test_apply_is_translated_in_chinese_catalogs,
        test_standard_apply_buttons_use_aegisub_translation,
        test_readme_bilingual_switchers_and_docs,
        test_pair_check_format_specifiers_and_no_fuzzy_chinese_entries,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} Chinese localization tests passed")


if __name__ == "__main__":
    main()
