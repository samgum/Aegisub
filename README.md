# Aegisub (SGMY Fork)

**[English](README.md)** | **[简体中文](README_CN.md)**

[![Meson CI](https://github.com/samgum/Aegisub/actions/workflows/ci.yml/badge.svg)](https://github.com/samgum/Aegisub/actions/workflows/ci.yml)

**Aegisub (SGMY Fork)** (`version 3.4.2.1`) is an enhanced cross-platform fork of [Aegisub](https://github.com/TypesettingTools/Aegisub) tailored for CJK subtitle production, Japanese karaoke & furigana workflows, HDR10/HLG video previewing, LRC/TTML lyric importing, subtitle overflow checking, AI-assisted grammar inspection, and macOS/Windows/Linux stability fixes.

---

## Key Enhancements in This Fork

### 1. Lyric Import (LRC, Enhanced LRC, TTML / DFXP)
* **LRC & Enhanced LRC (`.lrc`)**: Imports standard timestamped lyrics as well as Enhanced LRC word/syllable karaoke tags (`<mm:ss.xx>`) converted to ASS `\kf` sweep karaoke tags, with trailing line-end timestamp handling, automatic overlap clamping, empty spacer line clearing, and untimed LRC fallback.
* **TTML / DFXP (`.ttml`, `.dfxp`, `.xml`)**: Imports timed `<p>` lines and `<span begin="..." end="...">` word-level karaoke timing as `\kf`, splits `ttm:role="x-bg"` background-vocal / harmony spans, handles untimed `itunes:timing="None"` transcripts, and supports drag-and-drop onto Aegisub.

### 2. HDR10 / HLG & Dolby Vision Preview Rendering
* **16-bit BT.2100 CPU Preview Pipeline**: Decodes HDR10 (PQ) and HLG streams via `yuv444p16le` (with `rgb48le` fallback) to reconstruct signal ordering cleanly before inverse PQ EOTF / HLG inverse OETF conversion (`hdr_tonemap.h`).
* **Colorimetry & Dithering**: Performs BT.2020-to-BT.709 gamut compression and soft highlight roll-off with 8x8 Bayer ordered dithering into 8-bit SDR preview frames.
* **Responsive 4K Playback & Dolby Vision Handling**: Automatically caps CPU preview decoding width to 1920 px for smooth HDR scrubbing; previews Dolby Vision sources that carry a standard PQ/HLG base layer (without RPU reshaping) and cleanly logs unsupported Profile 5 / IPT-ICtCp streams.

### 3. Subtitle Overflow Highlighting (Rendered Width & Character Limit)
* **Dual Detection Modes** (`Preferences -> Interface` / `Colour`):
  * **Rendered subtitle width** (default): Uses `libass` rendered pixel bounds for authoritative grid/line overflow detection (accounting for `\move`, `\pos`, `\fsp`, `\b`/`\i`, and skipping `\t(...)` transforms), paired with cached `wxDC` per-character highlights in the subtitle edit box.
  * **Plain text character count**: Highlights lines exceeding a configurable character limit (`Subtitle/Overflow Highlight/Character Limit`).
* Customizable highlight colors (`Overlong line highlight` in the edit box and `Overlong line background` in the subtitle grid).

### 4. Batch Timing, CJK Punctuation, Style Overlap & Furigana Tools
All fork tools are accessible under the **Tools** menu:
* **Fix Common Errors** (`Tools -> Fix Common Errors...`, also on the main toolbar): Subtitle Edit-inspired batch processor for overlapping timings, minimum gaps, short/long durations (clamped chronologically), empty lines, and trailing whitespace with persisted thresholds.
* **Stitch Timings** (`Tools -> Stitch Timings...`): Snaps adjacent subtitle gaps using configurable millisecond or frame thresholds and anchor modes.
* **Check Style Overlaps** (`Tools -> Check Style Overlaps...`): Finds overlapping dialogue intervals within each style.
* **Chinese Simplified/Traditional Conversion** (`Tools -> Chinese Simplified/Traditional Conversion...`): Powered by ICU transliteration for fast script conversion across selection or full script.
* **Subtitle Text Cleanup** (`Tools -> Subtitle Text Cleanup...`) & **Check Paired Punctuation** (`Tools -> Check Paired Punctuation...`): Cleans CJK punctuation/spaces and audits unmatched quotes, brackets, and CJK book-title marks with apostrophe-aware UTF-8 indexing.
* **Japanese Furigana Annotation** (`Tools -> Japanese Furigana Annotation...`): Generates editable ruby annotations above or below kanji with Hiragana/Katakana mode switching, per-occurrence reading overrides, width-aware multi-line auto-wrap handling, style-scoped apply, and optional one-click **SudachiPy** auto-fill (`python -m pip install --upgrade sudachipy sudachidict_core`).

### 5. Music Lyrics Scroll Generator
* **Live Scrolling Lyric Builder** (`Tools -> Music Lyrics Scroll...`): Creates smooth `\move`-animated scrolling lyric scenes with cubic-out easing, live preview, resolution presets, active/inactive styling, and single-language or bilingual fuzzy lyric timing matching.

### 6. AI Subtitle Grammar Analysis
* **Interactive Line Inspector**: Accessible via the optional `AI` button beside the subtitle edit box (`Enable AI button beside the subtitle text box`) and configured via `Tools -> AI Grammar Analysis Settings...`.
* **Built-in OpenAI-Compatible Presets**: DeepSeek, OpenRouter, Gemini (OpenAI-compatible), Alibaba Cloud Bailian (Qwen), or any custom OpenAI-compatible base URL and model.
* **Safety & Control**: Session-only in-memory API key handling (not written to plaintext config), optional response caching, configurable reasoning parameter toggle, and robust JSON/UTF-16 surrogate-pair unescaping.

### 7. Audio Playback & Cross-Platform Audio Fixes
* **Playback Speed Range & Hotkeys**: Pitch-preserving SoundTouch tempo playback across `0.25x` to `4.0x` (`25, 50, 75, 100, 125, 150, 175, 200, 250, 300, 400`), adjustable from the audio box or via `Ctrl+Shift+Up` / `Ctrl+Shift+Down`.
* **Soft-Clip Peak Limiting**: `ApplyGainLimiter` prevents harsh digital clipping during amplified preview playback across PortAudio, OpenAL, and SoundTouch paths.
* **Platform Audio Reliability**:
  * **macOS (PortAudio / CoreAudio)**: Handles live audio output route switching safely and keeps low-latency output (`suggestedLatency` tuned with a safe floor) for accurate timing work.
  * **Windows (DirectSound2)**: Accurately tracks single-shot and ring-buffer end-of-stream drain so short clips and trailing samples finish cleanly.
  * **OpenAL**: Reports playback cursor in source samples clamped cleanly to `[start_frame, end_frame]` and resumes automatically after buffer underruns.

### 8. Localization & SGMY Release Checker
* **31 Languages** (`po/LINGUAS`): Complete Simplified Chinese (`zh_CN`) and Traditional Chinese (`zh_TW`) translations covering all fork-specific dialogs, menus, and preferences alongside 29 upstream translations.
* **Update Checker**: Checks `samgum/Aegisub` GitHub Releases (`Aegisub-SGMY update checker`).

### 9. Synced Upstream Improvements & Security Fixes
Merged and adapted fixes from [TypesettingTools/Aegisub](https://github.com/TypesettingTools/Aegisub):
* **Security Hardening**: Bounds/integer-overflow and parser fixes across ASS parser/attachments, Matroska/SRT/VobSub/YUV4MPEG readers, AviSynth wrapper, file mapping, VFR timecodes, and CoreText memory management.
* **Subtitle & Video Compatibility**: `LayoutResX` / `LayoutResY` script properties support, detached video pan/zoom scaling fixes, audio video-position marker snapping fixes, and UTF-8 / BOM charset detection improvements.
* **Automation & Lua**: `aegisub.version` API table fixes, MoonScript `0.7.0` support, `lfs` / Lua error reporting updates, and `-fexceptions` unwinding flag for C sources called by LuaJIT.
* **macOS Polish**: Native IME marked-text handling without spurious `NSBeep()`, Retina color picker and multi-monitor `CGDisplay` / ScreenCaptureKit coordinate fixes, `RawCtrl-` hotkey formatting, floating panel observer cleanup, locale normalization, and macOS bundle/rpath packaging improvements.

---

## Building from Source

Prebuilt installers and macOS `.dmg` bundles for tagged releases are available on the [Releases](https://github.com/samgum/Aegisub/releases) page.

### Common Prerequisites
* C++20 compiler, [Meson](https://mesonbuild.com/) `>= 0.60.0`, Ninja, Python 3
* wxWidgets `>= 3.2.2`, ICU (`icu-uc`, `icu-i18n`), Boost, libass, FFMS2 `>= 5.0`, libcurl, uchardet, Hunspell, FFTW3, SoundTouch

### macOS (Apple Silicon / Intel)
Install build dependencies with Homebrew (matches `.github/workflows/ci.yml`):

```bash
brew install ninja libass zlib ffms2 fftw hunspell pulseaudio sound-touch portaudio
```

Configure, build, test, and package `Aegisub.app` / `.dmg`:

```bash
meson setup build \
  -Dbuildtype=release \
  -Ddefault_library=static \
  -Dbuild_osx_bundle=true \
  -Dlocal_boost=true \
  --force-fallback-for=ffms2
meson compile -C build
meson test -C build --verbose --suite Aegisub
meson compile osx-bundle -C build
meson compile osx-build-dmg -C build
```

### Linux (Ubuntu / Debian)
Install dependencies (matches `.github/workflows/ci.yml`):

```bash
sudo apt-get update
sudo apt-get install -y \
  ninja-build build-essential libx11-dev libfreetype6-dev pkg-config \
  libfontconfig1-dev libass-dev libasound2-dev libffms2-dev intltool \
  libboost-all-dev libhunspell-dev libcurl4-openssl-dev libuchardet-dev \
  libgtest-dev libgmock-dev libwxgtk3.2-dev libportal-gtk3-dev libsoundtouch-dev
pip install meson
```

Configure, build, test, and install:

```bash
meson setup build -Dbuildtype=release -Db_pch=false
meson compile -C build
meson test -C build --verbose --suite Aegisub
sudo meson install -C build
```

> **Note for Linux packagers**: Aegisub requires LuaJIT compiled in Lua 5.2 compatibility mode (`-DLUAJIT_ENABLE_LUA52COMPAT`). By default, Meson automatically falls back to the built-in `luajit` subproject if the system LuaJIT lacks 5.2 mode; passing `-Dsystem_luajit=true` enforces strict checking against the system library.

### Windows (MSVC)
Run inside an **x64 Native Tools Command Prompt for VS** with Chocolatey dependencies (`choco install ninja innosetup`), `gettext`, and `moonscript` on `PATH`:

```powershell
meson setup build `
  -Dbuildtype=release `
  -Ddefault_library=static `
  --force-fallback-for=zlib,harfbuzz,freetype2,fribidi,libpng `
  -Dfallback_nasm=true `
  -Dfreetype2:harfbuzz=disabled `
  -Dharfbuzz:freetype=disabled `
  -Dharfbuzz:cairo=disabled `
  -Dharfbuzz:glib=disabled `
  -Dharfbuzz:gobject=disabled `
  -Dharfbuzz:tests=disabled `
  -Dharfbuzz:docs=disabled `
  -Dfribidi:tests=false `
  -Dfribidi:docs=false `
  -Dlibass:fontconfig=disabled

meson compile -C build
meson test -C build --verbose --suite Aegisub
# Optional installers:
meson compile win-installer -C build
ninja -C build win-portable
```

### Source Regression Checks
Run the lightweight Python source-regression suite anytime without a full native build:

```bash
python3 tools/run_source_regressions.py
```

---

## License

Aegisub is licensed under the **BSD 3-Clause License**. See [LICENCE](LICENCE) for details.
