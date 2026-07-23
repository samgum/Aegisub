# Aegisub SGMY

[中文说明](README_CN.md) | English

A customized fork of [Aegisub](http://aegisub.org) focused on practical
subtitle-production workflows — HDR/Dolby Vision preview, CJK text tools,
audio stability, and batch timing fixes.

Pre-built binaries (Windows installer/portable, macOS DMG for Intel and Apple
Silicon) are available on the [Releases page](https://github.com/samgum/Aegisub/releases).

---

## What's new in this fork

### HDR & Dolby Vision video support
- **Open HDR/BT.2020 video** — the original Aegisub rejected 4K HDR sources
  with "Unknown video color space". This fork recognises BT.2020 and all
  modern color spaces so UHD/HDR/Dolby Vision files open normally.
- **CPU HDR tone-mapping** — PQ (SMPTE ST 2084) and HLG (ARIB STD-B67) sources
  are decoded at 16-bit precision and tone-mapped to a viewable SDR/BT.709
  preview, so HDR footage no longer appears near-black or washed out. The
  tone-map uses precomputed lookup tables (no `std::pow` in the per-frame loop)
  and runs entirely in `float` for autovectorization.
- **Decode-time downscaling** — 4K HDR sources are subsampled to a 1920-wide
  preview at decode time, cutting CPU/memory ~4× so playback stays responsive
  instead of freezing the whole machine.
- SDR video is completely untouched (zero overhead, same behavior as upstream).

### Fix Common Errors (Subtitle Edit-style batch fixer)
A new toolbar tool (Tools → Fix Common Errors) that batch-fixes the most
common subtitle problems in one pass, each opt-in via checkbox:
- Fix overlapping display times (trim end to next start)
- Fix short gaps between lines (configurable minimum)
- Fix short / long durations (configurable thresholds)
- Remove empty / whitespace-only lines
- Strip trailing whitespace (including full-width U+3000)

Affects all rows or selected rows. Each category is its own undo step with
the correct commit type (timing / text / add-remove).

### CJK text tools (Tools menu)
- **Chinese Simplified ↔ Traditional conversion** — ICU-based, with per-style
  filtering and selection scope.
- **Subtitle text cleanup** — fix full-width commas, spacing around periods,
  smart quotes, and double spaces.
- **Paired punctuation checker** — validates quotes, brackets, and CJK
  book-title / corner / angle brackets for pairing problems.
- **Japanese furigana annotation** — add editable ruby-text annotations above
  or below kanji, with per-occurrence readings and style-scoped application.

### Music lyrics scroll generator
Generate music-player-style scrolling lyric subtitles with live preview,
multi-language (single / bilingual) support, resolution presets (2160p /
1080p / 720p / vertical / custom), and independent styling for current and
upcoming lines.

### Audio playback improvements
- **Playback speed control** with pitch preservation (SoundTouch), 0.25×–4.0×,
  with Ctrl+hotkey overrides.
- **macOS PortAudio stability** — lazy route-change detection (only rebuild
  the stream when the default device actually moves), exception-safe reopen,
  so headphone/Bluetooth/speaker switching no longer crashes or silences.
- **DirectSound position jitter fix** — playback position no longer jumps
  backward on Windows.
- **96 kHz / 24-bit Hi-Res audio** support.

### Other
- **Subtitle overflow highlighting** — highlights lines whose rendered text
  exceeds the video frame, using libass bounds.
- **AI subtitle analysis** — optional AI-assisted line analysis (configurable
  in Preferences).
- **31-language localization** — complete translations for all new features
  in Simplified Chinese, Traditional Chinese, and 22 other languages.
- **Robust Windows installer** — fault-tolerant language-file download that
  survives upstream Inno Setup changes.

---

## Building Aegisub

### Windows

Prerequisites:

1. Visual Studio (Community edition of any recent version is fine, needs the Windows SDK included)
2. Python 3
3. Meson
4. CMake

There are a few optional dependencies that must be installed and on your PATH:

1. msgfmt, to build the translations (installing from https://mlocati.github.io/articles/gettext-iconv-windows.html seems to be the easiest option)
2. InnoSetup, to build the regular installer (iscc.exe on your PATH)
3. 7zip, to build the regular installer (7z.exe on your PATH)
4. Moonscript, to build the regular installer (moonc.exe on your PATH)

All other dependencies are either stored in the repository or are included as submodules.

Building:

1. Clone Aegisub's repository: `git clone https://github.com/samgum/Aegisub.git`
2. From the Visual Studio "x64 Native Tools Command Prompt", generate the build directory: `meson build -Ddefault_library=static` (if building for release, add `--buildtype=release`)
3. Build with `cd build` and `ninja`

You should now have a binary: `aegisub.exe`.

Installer:

You can generate the installer with `ninja win-installer` after a successful build. This assumes a working internet connection and installation of the optional dependencies.

You can generate the portable zip with `ninja win-portable` after a successful build.

### macOS

A vaguely recent version of Xcode and the corresponding command-line tools are required.

For personal usage, you can use pip and homebrew to install almost all of Aegisub's dependencies:

    pip3 install meson      # or brew install meson if you installed Python via brew
    brew install cmake ninja pkg-config libass boost zlib ffms2 fftw hunspell uchardet
    export LDFLAGS="-L/usr/local/opt/icu4c/lib"
    export CPPFLAGS="-I/usr/local/opt/icu4c/include"
    export PKG_CONFIG_PATH="/usr/local/opt/icu4c/lib/pkgconfig"

When compiling on Apple Silicon, replace `/usr/local` with `/opt/homebrew`.

Once the dependencies are installed, build Aegisub with `meson build && meson compile -C build`.

#### Build dmg

```bash
meson build_static -Ddefault_library=static -Dbuildtype=debugoptimized -Dbuild_osx_bundle=true -Dlocal_boost=true
meson compile -C build_static
meson test -C build_static --verbose
meson compile osx-bundle -C build_static
meson compile osx-build-dmg -C build_static
```

### Linux

#### Build dependencies for Debian-based systems

```
compiler:    build-essential
pkgconfig:   pkg-config  or  pkgconf
meson:       meson ninja-build
gettext:     gettext intltool
fontconfig:  libfontconfig1-dev
libass:      libass-dev
boost:       libboost-chrono-dev libboost-locale-dev libboost-regex-dev libboost-system-dev libboost-thread-dev
zlib:        zlib1g-dev
WxWidgets:   wx3.2-headers libwxgtk3.2-dev
ICU:         icu-devtools libicu-dev
pulse-audio: libpulse-dev
ALSA:        libasound2-dev
OpenAL:      libopenal-dev
ffms2:       libffms2-dev
fftw3:       libfftw3-dev
hunspell:    libhunspell-dev
uchardet:    libuchardet-dev
libcurl:     libcurl4-openssl-dev  or  libcurl4-gnutls-dev
opengl:      libgl1-mesa-dev
gtest:       libgtest-dev
gmock:       libgmock-dev
libportal:   libportal-gtk3-dev
```

I.e. to install on Ubuntu 24.04 run this command:
``` bash
sudo apt install build-essential pkg-config meson ninja-build gettext intltool libfontconfig1-dev libass-dev libboost-chrono-dev libboost-locale-dev libboost-regex-dev libboost-system-dev libboost-thread-dev zlib1g-dev wx3.2-headers libwxgtk3.2-dev icu-devtools libicu-dev libpulse-dev libasound2-dev libopenal-dev libffms2-dev libfftw3-dev libhunspell-dev libuchardet-dev libcurl4-gnutls-dev libgl1-mesa-dev libgtest-dev libgmock-dev libportal-gtk3-dev
```

#### Build Aegisub

``` bash
meson setup build --prefix=/usr/local --buildtype=release --strip -Dsystem_luajit=false -Ddefault_library=static
meson compile -C build
meson install -C build --skip-subprojects luajit
```

#### Packaging
If you are packaging Aegisub for a Linux distribution, here are a few things you may need to know:
- Aegisub cannot be built with LTO (See: https://github.com/TypesettingTools/Aegisub/issues/290).
- Aegisub depends on LuaJIT and *requires* LuaJIT to be build with Lua 5.2 compatibility enabled.
  We are aware that most distributions do not compile LuaJIT with this flag, and that this complicates packaging for them, see https://github.com/TypesettingTools/Aegisub/issues/239 for a detailed discussion of the situation.

  Like for its other dependencies, Aegisub includes a meson subproject for LuaJIT that can be used to statically link a version of LuaJIT with 5.2 compatibility.
  For distributions that do not allow downloading additional sources at build time, the downloaded LuaJIT subproject is included in the source tarballs distributed with releases.
- When linked against libstdc++, Aegisub needs libstdc++ 6.0.32 or later due to https://gcc.gnu.org/bugzilla/show_bug.cgi?id=95048.
  Aegisub's tests will detect this bug, but if you're not running tests on packaging you'll need to make sure the libstdc++ version is recent enough.
- Aegisub uses OpenGL through wxWidgets. For Aegisub to work directly on Wayland (as opposed to Xwayland), wxWidgets needs to be built with EGL enabled.
  Aegisub will automatically fall back to X11 when it detects missing EGL support.

The following commands are an example for how to build Aegisub with the goal of creating a distribution package:

```bash
meson subprojects download luajit              # Or use the tarball
meson subprojects packagefiles --apply luajit

meson setup builddir --wrap-mode=nodownload --prefix=/usr --buildtype=release -Dsystem_luajit=false -Ddefault_library=static -Dtests=false

meson compile -C builddir
meson install -C builddir --skip-subprojects luajit
```

## Updating Moonscript

From within the Moonscript repository, run `bin/moon bin/splat.moon -l moonscript moonscript/ > bin/moonscript.lua`.
Open the newly created `bin/moonscript.lua`, and within it make the following changes:

1. Prepend the final line of the file, `package.preload["moonscript"]()`, with a `return`, producing `return package.preload["moonscript"]()`.
2. Within the function at `package.preload['moonscript.base']`, remove references to `moon_loader`, `insert_loader`, and `remove_loader`. This means removing their declarations, definitions, and entries in the returned table.
3. Within the function at `package.preload['moonscript']`, remove the line `_with_0.insert_loader()`.

The file is now ready for use, to be placed in `automation/include` within the Aegisub repo.

## License

All files in this repository are licensed under various GPL-compatible BSD-style licenses; see LICENCE and the individual source files for more information.
The official Windows and OS X builds are GPLv2 due to including fftw3.
