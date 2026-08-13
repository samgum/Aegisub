#!/usr/bin/env python3
"""Keep network-fetched fallback dependencies reproducible."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WRAPS = ROOT / "subprojects"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
IMMUTABLE_REVISION = re.compile(r"(?:[0-9a-f]{40}|v?\d+(?:\.\d+){1,3})$")


def test_git_wrap_revisions_are_immutable():
    failures = []
    for wrap in sorted(WRAPS.glob("*.wrap")):
        source = wrap.read_text(encoding="utf-8")
        if "[wrap-git]" not in source:
            continue
        match = re.search(r"^revision\s*=\s*(\S+)\s*$", source, re.MULTILINE)
        if not match or not IMMUTABLE_REVISION.fullmatch(match.group(1)):
            failures.append(f"{wrap.name}: {match.group(1) if match else 'missing'}")
    assert not failures, "mutable git wrap revisions: " + ", ".join(failures)


def test_windows_ci_does_not_shadow_msvc_linker():
    source = CI_WORKFLOW.read_text(encoding="utf-8")
    windows = source[source.index("- name: Install dependencies (Windows)"):]
    assert 'Git\\usr\\bin' not in windows
    assert "$gitUsrBin | Out-File -FilePath $env:GITHUB_PATH" not in windows


def main():
    test_git_wrap_revisions_are_immutable()
    test_windows_ci_does_not_shadow_msvc_linker()
    print("2 dependency pin tests passed")


if __name__ == "__main__":
    main()
