#!/usr/bin/env python3
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


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


def generate_version(build, source):
    subprocess.run(["sh", (ROOT / "tools" / "version.sh").as_posix(), build.as_posix(), source.as_posix()], check=True)
    return (build / "git_version.h").read_text(encoding="utf-8")


def build_commit():
    if (ROOT / ".git").exists():
        return subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    header = (ROOT / "git_version.h").read_text(encoding="utf-8")
    return re.search(r'#define BUILD_GIT_COMMIT "([0-9a-f]{40})"', header)[1]


def test_build_commit_survives_source_archive():
    commit = build_commit()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        build = root / "build"
        build.mkdir()
        header = generate_version(build, ROOT)
        assert f'#define BUILD_GIT_COMMIT "{commit}"' in header
        archive = root / "archive"
        archive.mkdir()
        shutil.copyfile(build / "git_version.h", archive / "git_version.h")
        rebuilt = root / "rebuilt"
        rebuilt.mkdir()
        assert generate_version(rebuilt, archive) == header


def test_build_commit_refreshes_cached_metadata():
    with tempfile.TemporaryDirectory() as directory:
        build = Path(directory)
        header = generate_version(build, ROOT)
        commit = build_commit()
        stale = header.replace(commit, "0" * 40)
        (build / "git_version.h").write_text(stale, encoding="utf-8")
        assert generate_version(build, ROOT) == header
        # Also exercise the Windows generator when PowerShell is installed.
        if shutil.which("pwsh") and (ROOT / ".git").exists():
            (build / "git_version.h").write_text(stale, encoding="utf-8")
            subprocess.run(["pwsh", "-File", str(ROOT / "tools" / "version.ps1"), str(build), str(ROOT)], check=True)
            assert f'#define BUILD_GIT_COMMIT "{commit}"' in (build / "git_version.h").read_text(encoding="utf-8-sig")


def main():
    tests = [
        test_project_version_is_patch_hotfix,
        test_version_scripts_accept_four_part_versions,
        test_build_commit_survives_source_archive,
        test_build_commit_refreshes_cached_metadata,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} version metadata tests passed")


if __name__ == "__main__":
    main()
