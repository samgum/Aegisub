#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TIMING_TOOLS = ROOT / "src" / "dialog_timing_tools.cpp"
AI = ROOT / "src" / "dialog_ai_analysis.cpp"
COMMANDS = ROOT / "src" / "command" / "tool.cpp"
MENU = ROOT / "src" / "libresrc" / "default_menu.json"
CONFIG = ROOT / "src" / "libresrc" / "default_config.json"
EDIT_BOX = ROOT / "src" / "subs_edit_box.cpp"


def test_chinese_conversion_uses_icu_transliterator_and_plain_blocks_only():
    source = TIMING_TOOLS.read_text(encoding="utf-8")
    assert "#include <unicode/translit.h>" in source
    assert "Simplified-Traditional" in source
    assert "Traditional-Simplified" in source
    assert "AssDialogueBlockPlain" in source
    assert "line->UpdateText(blocks)" in source
    config = CONFIG.read_text(encoding="utf-8")
    assert '"Chinese Conversion"' in config
    assert '"Style Only"' in config


def test_pair_check_ignores_tags_and_covers_common_cjk_pairs():
    source = TIMING_TOOLS.read_text(encoding="utf-8")
    assert "line->GetStrippedText()" in source
    for token in ["book-title brackets", "corner brackets", "Chinese double quotes", "Unpaired ASCII double quote"]:
        assert token in source
    config = CONFIG.read_text(encoding="utf-8")
    assert '"Pair Check"' in config
    assert '"Style Only"' in config


def test_tool_commands_and_menu_are_registered():
    commands = COMMANDS.read_text(encoding="utf-8")
    menu = MENU.read_text(encoding="utf-8")
    for command in [
        "tool/text/chinese_convert",
        "tool/text/pair_check",
        "tool/ai/analysis_settings",
    ]:
        assert command in commands
        assert command in menu
    assert "ShowChineseConversionDialog(c)" in commands
    assert "ShowPairCheckDialog(c)" in commands
    assert "ShowAIAnalysisSettingsDialog(c->parent)" in commands


def test_ai_button_and_openai_compatible_request_are_present():
    edit_box = EDIT_BOX.read_text(encoding="utf-8")
    ai = AI.read_text(encoding="utf-8")
    assert "ShowAIAnalysisDialog(c, from_wx(edit_ctrl->GetText()))" in edit_box
    assert "API key (kept in memory only)" in ai
    assert "Authorization: Bearer " in ai
    assert "/chat/completions" in ai
    assert "reasoning_effort" in ai
    assert 'thinking\\":{\\"type\\":\\"enabled\\"}' in ai
    for base_url in [
        "https://api.deepseek.com",
        "https://openrouter.ai/api/v1",
        "https://generativelanguage.googleapis.com/v1beta/openai",
        "https://dashscope.aliyuncs.com/compatible-mode/v1",
    ]:
        assert base_url in ai
    config = CONFIG.read_text(encoding="utf-8")
    assert '"Temperature" : 1.0' in config


def test_ai_analysis_request_is_backgrounded_and_target_language_driven():
    ai = AI.read_text(encoding="utf-8")
    assert "#include <thread>" in ai
    assert "std::thread([alive_token" in ai
    assert "wxTheApp->CallAfter" in ai
    assert "wxSafeYield" not in ai
    assert "Write every heading and every explanation in the requested target language" in ai
    for section in [
        "source meaning",
        "grammar structure",
        "word/phrase notes",
        "nuance and tone",
        "subtitle-localization notes",
        "recommended translation",
        "optional alternatives",
    ]:
        assert section in ai


def test_ai_cache_does_not_include_api_key():
    ai = AI.read_text(encoding="utf-8")
    cache_line = next(line for line in ai.splitlines() if "cache_key = " in line)
    assert "api_key" not in cache_line
    assert "session_api_key" not in cache_line
    assert "response_cache[cache_key]" in ai


def main():
    tests = [
        test_chinese_conversion_uses_icu_transliterator_and_plain_blocks_only,
        test_pair_check_ignores_tags_and_covers_common_cjk_pairs,
        test_tool_commands_and_menu_are_registered,
        test_ai_button_and_openai_compatible_request_are_present,
        test_ai_analysis_request_is_backgrounded_and_target_language_driven,
        test_ai_cache_does_not_include_api_key,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} text tools and AI tests passed")


if __name__ == "__main__":
    main()
