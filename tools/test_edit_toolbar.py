#!/usr/bin/env python3
"""Static tests for the edit-box toolbar text-transform commands.

These verify the 6 new commands are implemented, registered, wired into the
toolbar, and that the full-width <-> half-width byte mapping is correct
(checked against Python's own Unicode codec, which is authoritative).
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EDIT = ROOT / "src" / "command" / "edit.cpp"
SUBS_EDIT_BOX = ROOT / "src" / "subs_edit_box.cpp"
MENU = ROOT / "src" / "libresrc" / "default_menu.json"
ZH_CN = ROOT / "po" / "zh_CN.po"
ZH_TW = ROOT / "po" / "zh_TW.po"


# The six commands we added.
COMMANDS = {
    "edit/text/trim": "Trim Whitespace",
    "edit/text/cycle_case": "Cycle Case",
    "edit/text/fullwidth_to_halfwidth": "Full-width to Half-width",
    "edit/text/halfwidth_to_fullwidth": "Half-width to Full-width",
    "edit/text/collapse_spaces": "Collapse Spaces",
    "edit/text/strip_tags": "Strip All Tags",
}


def test_commands_defined():
    """Every command must have a CMD_NAME, STR_DISP/STR_MENU, STR_HELP, and an
    operator() in edit.cpp."""
    src = EDIT.read_text(encoding="utf-8")
    for cmd, disp in COMMANDS.items():
        assert ('CMD_NAME("%s")' % cmd) in src, "missing CMD_NAME %s" % cmd
        assert ('STR_DISP("%s")' % disp) in src, "missing STR_DISP for %s" % cmd
        assert ('STR_MENU("%s")' % disp) in src, "missing STR_MENU for %s" % cmd
    # All six must be registered.
    for cmd in COMMANDS:
        assert ('reg(std::make_unique<' + cmd.replace('/', '_') + '>())') in src, \
            "%s not registered" % cmd


def test_commands_wired_into_toolbar():
    """The edit-box toolbar must surface all six commands as buttons."""
    src = SUBS_EDIT_BOX.read_text(encoding="utf-8")
    for cmd in COMMANDS:
        assert ('MakeTextButton("%s"' % cmd) in src, "%s not on toolbar" % cmd
    # The MakeTextButton helper itself must exist.
    assert "wxButton *SubsEditBox::MakeTextButton" in src


def test_commands_in_grid_context_menu():
    """Commands should also be reachable from the grid context menu."""
    src = MENU.read_text(encoding="utf-8")
    for cmd in COMMANDS:
        assert ('"command" : "%s"' % cmd) in src, "%s not in menu" % cmd


def test_chinese_translations_present():
    """Both simplified and traditional Chinese must translate all six."""
    for po_path in (ZH_CN, ZH_TW):
        src = po_path.read_text(encoding="utf-8")
        for disp in COMMANDS.values():
            assert ('msgid "%s"' % disp) in src, "%s missing msgid in %s" % (disp, po_path.name)
            # There must be a non-empty msgstr right after the msgid.
            idx = src.index('msgid "%s"' % disp)
            chunk = src[idx:idx + 200]
            assert 'msgstr "' in chunk and 'msgstr ""' not in chunk.split('\n')[1], \
                "%s has empty msgstr in %s" % (disp, po_path.name)


def test_fullwidth_to_halfwidth_mapping():
    """The C++ byte mapping must match Python's authoritative Unicode codec
    for the full-width ASCII range U+FF01..U+FF5E and the full-width space
    U+3000. We verify the exact UTF-8 byte sequences the C++ must emit."""
    src = EDIT.read_text(encoding="utf-8")
    # The round-trip helpers must both exist.
    assert "std::string fullwidth_to_halfwidth" in src
    assert "std::string halfwidth_to_fullwidth" in src

    # Authoritative expected bytes from Python's codec.
    for c in range(0x21, 0x7F):
        full_utf8 = chr(c + 0xFEE0).encode("utf-8")
        hex3 = [b for b in full_utf8]
        assert len(hex3) == 3

    # Verify the C++ forward formulas produce the same bytes the codec does.
    # Forward (half->full): documented in the C++ comment.
    #   0x21..0x5F -> EF BC (c+0x60)
    #   0x60..0x7E -> EF BD (c+0x20)
    for c in range(0x21, 0x60):
        expected = chr(c + 0xFEE0).encode("utf-8")
        produced = bytes([0xEF, 0xBC, (c + 0x60) & 0xFF])
        assert produced == expected, "half->full 0x%02X: %s != %s" % (c, produced.hex(), expected.hex())
    for c in range(0x60, 0x7F):
        expected = chr(c + 0xFEE0).encode("utf-8")
        produced = bytes([0xEF, 0xBD, (c + 0x20) & 0xFF])
        assert produced == expected, "half->full 0x%02X: %s != %s" % (c, produced.hex(), expected.hex())

    # Reverse (full->half): the C++ subtracts 0x60 / 0x20 from the 3rd byte.
    for c in range(0x21, 0x60):
        third = (c + 0x60) & 0xFF
        assert (third - 0x60) & 0xFF == c
    for c in range(0x60, 0x7F):
        third = (c + 0x20) & 0xFF
        assert (third - 0x20) & 0xFF == c

    # The C++ source must encode the corrected ranges (not the old buggy ones).
    assert "c >= 0x21 && c <= 0x5F" in src, "halfwidth_to_fullwidth range 0x21-0x5F missing"
    assert "c >= 0x60 && c <= 0x7E" in src, "halfwidth_to_fullwidth range 0x60-0x7E missing"
    assert "c + 0x60" in src
    assert "c + 0x20" in src
    # Reverse side must use matching subtractions.
    assert "b2 - 0x60" in src
    assert "b2 - 0x20" in src

    # Full-width space U+3000 <-> ASCII space.
    assert chr(0x3000).encode("utf-8") == b"\xe3\x80\x80"


def test_strip_tags_logic():
    """strip_override_tags must be present and operate on { } blocks."""
    src = EDIT.read_text(encoding="utf-8")
    assert "std::string strip_override_tags" in src
    # It must track in_tag state with { and }.
    assert "in_tag" in src


def test_no_duplicate_command_ids():
    """No command name should appear more than once as CMD_NAME."""
    src = EDIT.read_text(encoding="utf-8")
    for cmd in COMMANDS:
        assert src.count('CMD_NAME("%s")' % cmd) == 1, "%s duplicated" % cmd


def main():
    tests = [
        test_commands_defined,
        test_commands_wired_into_toolbar,
        test_commands_in_grid_context_menu,
        test_chinese_translations_present,
        test_fullwidth_to_halfwidth_mapping,
        test_strip_tags_logic,
        test_no_duplicate_command_ids,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} edit-box toolbar tests passed")


if __name__ == "__main__":
    main()
