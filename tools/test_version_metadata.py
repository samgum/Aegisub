#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_project_version_is_patch_hotfix():
    meson = (ROOT / "meson.build").read_text(encoding="utf-8")
    assert "version: '3.4.2.1'" in meson


def test_version_scripts_accept_four_part_versions():
    ps1 = (ROOT / "tools" / "version.ps1").read_text(encoding="utf-8")
    sh = (ROOT / "tools" / "version.sh").read_text(encoding="utf-8")
    assert r"(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?" in ps1
    assert "$mesonVersionParts = $null" in ps1
    assert "if ($mesonVersionParts) {" in ps1
    assert "$versionParts = @($Matches[1], $Matches[2], $Matches[3])" in ps1
    assert "if ($Matches[4]) { $versionParts += $Matches[4] }" in ps1
    assert "RESOURCE_BASE_VERSION'] = 0, 0, 0, 0" in ps1
    assert r"[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?" in sh
    assert "meson_version=$(sed -n" in sh
    assert 'installer_version="${meson_version:-0.0.0.0}"' in sh


def main():
    tests = [
        test_project_version_is_patch_hotfix,
        test_version_scripts_accept_four_part_versions,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} version metadata tests passed")


if __name__ == "__main__":
    main()
