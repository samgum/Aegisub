# Aegisub SGMY

English | [中文说明](README_CN.md)

[Aegisub](http://aegisub.org) 的定制分支，专注于实际字幕制作工作流——
HDR/杜比视界预览、中日韩文本工具、音频稳定性，以及批量时间轴修复。

预编译二进制文件（Windows 安装版/便携版、macOS Intel/Apple Silicon DMG）
可在 [Releases 页面](https://github.com/samgum/Aegisub/releases) 下载。

---

## 本分支新增功能

### HDR / 杜比视界视频支持
- **打开 HDR/BT.2020 视频**——原版 Aegisub 会以"Unknown video color space"拒绝
  4K HDR 片源。本分支识别 BT.2020 及所有现代色彩空间，UHD/HDR/杜比视界文件
  可正常打开。
- **CPU HDR 色调映射**——PQ（SMPTE ST 2084）和 HLG（ARIB STD-B67）源以 16-bit
  精度解码，并色调映射为可查看的 SDR/BT.709 预览，HDR 片源不再发暗或发灰。
  色调映射使用预计算查找表（每帧循环中无 `std::pow`），全程 float 运算以利
  于编译器自动向量化。
- **解码时降采样**——4K HDR 源在解码时降采样到 1920 宽的预览分辨率，CPU 和
  内存开销降低约 4 倍，播放保持流畅，不再卡死整机。
- SDR 视频完全不受影响（零开销，与上游行为一致）。

### 修复常见错误（参考 Subtitle Edit 的批量修复）
新增工具栏按钮（工具 → 修复常见错误），一次性批量修复最常见的字幕问题，
每项可通过复选框独立勾选：
- 修复重叠显示时间（截到下一行开始）
- 修复行间过短间隔（可配置最小值）
- 修复过短/过长时长（可配置阈值）
- 删除空行或仅含空白的行
- 去除尾部空白（含全角空格 U+3000）

影响全部行或选中行。每类修复是独立的撤销步骤，使用正确的提交类型
（时间/文本/增删行）。

### 中日韩文本工具（工具菜单）
- **简繁中文互转**——基于 ICU，支持按样式过滤和选择范围。
- **字幕文本清理**——修复全角逗号、句号间距、智能引号、连续空格。
- **配对标点检查**——验证引号、括号、中日韩书名号/直角引号/角括号的配对问题。
- **日语假名标注**——在汉字上方或下方添加可编辑的假名注音，支持逐词读音和
  按样式应用。

### 音乐滚动歌词生成器
生成音乐软件式逐行滚动歌词字幕，支持实时预览、单语/双语模式、分辨率预设
（2160p/1080p/720p/竖屏/自定义），以及当前行和未显示行的独立样式设置。

### 音频播放改进
- **播放速度控制**——保调变速（SoundTouch），0.25×–4.0×，支持 Ctrl 快捷键覆盖。
- **macOS PortAudio 稳定性**——惰性路由切换检测（仅在默认设备真正变化时重建流），
  异常安全的重开流，耳机/蓝牙/扬声器切换不再崩溃或无声。
- **DirectSound 位置抖动修复**——Windows 上播放位置不再倒退跳动。
- **96 kHz / 24-bit Hi-Res 音频**支持。

### 其他
- **字幕溢出高亮**——高亮渲染文本超出视频帧的行，使用 libass 边界。
- **AI 字幕分析**——可选的 AI 辅助行分析（在偏好设置中配置）。
- **31 种语言本地化**——所有新功能在简体中文、繁体中文及其它 22 种语言中
  均有完整翻译。
- **稳健的 Windows 安装包**——容错的语言文件下载，能经受上游 Inno Setup 变更。

---

## 构建 Aegisub

### Windows

前置条件：

1. Visual Studio（任意较新版本的 Community 版即可，需包含 Windows SDK）
2. Python 3
3. Meson
4. CMake

以下可选依赖需安装并在 PATH 中：

1. msgfmt，用于构建翻译（从 https://mlocati.github.io/articles/gettext-iconv-windows.html 安装最简单）
2. InnoSetup，用于构建安装包（PATH 中需有 iscc.exe）
3. 7zip，用于构建安装包（PATH 中需有 7z.exe）
4. Moonscript，用于构建安装包（PATH 中需有 moonc.exe）

其余依赖要么存储在仓库中，要么作为子项目包含。

构建步骤：

1. 克隆仓库：`git clone https://github.com/samgum/Aegisub.git`
2. 在 Visual Studio "x64 Native Tools Command Prompt" 中生成构建目录：
   `meson build -Ddefault_library=static`（如构建 Release 版，加 `--buildtype=release`）
3. 构建：`cd build` 然后 `ninja`

现在你应该得到了二进制文件：`aegisub.exe`。

安装包：

构建成功后可用 `ninja win-installer` 生成安装包。需要网络连接和可选依赖。

构建成功后可用 `ninja win-portable` 生成便携版 zip。

### macOS

需要较新版本的 Xcode 及对应的命令行工具。

个人使用时，可用 pip 和 homebrew 安装几乎所有依赖：

    pip3 install meson      # 如果通过 brew 安装的 Python，用 brew install meson
    brew install cmake ninja pkg-config libass boost zlib ffms2 fftw hunspell uchardet
    export LDFLAGS="-L/usr/local/opt/icu4c/lib"
    export CPPFLAGS="-I/usr/local/opt/icu4c/include"
    export PKG_CONFIG_PATH="/usr/local/opt/icu4c/lib/pkgconfig"

Apple Silicon 上编译时，将 `/usr/local` 替换为 `/opt/homebrew`。

依赖安装完成后，用 `meson build && meson compile -C build` 构建。

#### 构建 dmg

```bash
meson build_static -Ddefault_library=static -Dbuildtype=debugoptimized -Dbuild_osx_bundle=true -Dlocal_boost=true
meson compile -C build_static
meson test -C build_static --verbose
meson compile osx-bundle -C build_static
meson compile osx-build-dmg -C build_static
```

### Linux

#### Debian 系系统的构建依赖

```
编译器:       build-essential
pkgconfig:    pkg-config 或 pkgconf
meson:        meson ninja-build
gettext:      gettext intltool
fontconfig:   libfontconfig1-dev
libass:       libass-dev
boost:        libboost-chrono-dev libboost-locale-dev libboost-regex-dev libboost-system-dev libboost-thread-dev
zlib:         zlib1g-dev
WxWidgets:    wx3.2-headers libwxgtk3.2-dev
ICU:          icu-devtools libicu-dev
pulse-audio:  libpulse-dev
ALSA:         libasound2-dev
OpenAL:       libopenal-dev
ffms2:        libffms2-dev
fftw3:        libfftw3-dev
hunspell:     libhunspell-dev
uchardet:     libuchardet-dev
libcurl:      libcurl4-openssl-dev 或 libcurl4-gnutls-dev
opengl:       libgl1-mesa-dev
gtest:        libgtest-dev
gmock:        libgmock-dev
libportal:    libportal-gtk3-dev
```

在 Ubuntu 24.04 上一键安装：
``` bash
sudo apt install build-essential pkg-config meson ninja-build gettext intltool libfontconfig1-dev libass-dev libboost-chrono-dev libboost-locale-dev libboost-regex-dev libboost-system-dev libboost-thread-dev zlib1g-dev wx3.2-headers libwxgtk3.2-dev icu-devtools libicu-dev libpulse-dev libasound2-dev libopenal-dev libffms2-dev libfftw3-dev libhunspell-dev libuchardet-dev libcurl4-gnutls-dev libgl1-mesa-dev libgtest-dev libgmock-dev libportal-gtk3-dev
```

#### 构建 Aegisub

``` bash
meson setup build --prefix=/usr/local --buildtype=release --strip -Dsystem_luajit=false -Ddefault_library=static
meson compile -C build
meson install -C build --skip-subprojects luajit
```

#### 打包注意事项
如果你要为 Linux 发行版打包 Aegisub，需注意以下几点：
- Aegisub 不能用 LTO 构建（见 https://github.com/TypesettingTools/Aegisub/issues/290）。
- Aegisub 依赖 LuaJIT，且**要求** LuaJIT 以 Lua 5.2 兼容模式编译。大多数发行版
  不带此标志编译 LuaJIT，这会使打包复杂化，详见
  https://github.com/TypesettingTools/Aegisub/issues/239。
- 链接 libstdc++ 时，Aegisub 需要 libstdc++ 6.0.32 或更高版本（见
  https://gcc.gnu.org/bugzilla/show_bug.cgi?id=95048）。
- Aegisub 通过 wxWidgets 使用 OpenGL。要在 Wayland 上原生运行（而非 Xwayland），
  wxWidgets 需启用 EGL。缺失 EGL 支持时 Aegisub 会自动回退到 X11。

## 更新 Moonscript

在 Moonscript 仓库中运行 `bin/moon bin/splat.moon -l moonscript moonscript/ > bin/moonscript.lua`。
打开生成的 `bin/moonscript.lua`，做以下修改：

1. 将文件最后一行 `package.preload["moonscript"]()` 前加 `return`，变为
   `return package.preload["moonscript"]()`。
2. 在 `package.preload['moonscript.base']` 函数中，移除对 `moon_loader`、
   `insert_loader` 和 `remove_loader` 的引用（声明、定义和返回表中的条目）。
3. 在 `package.preload['moonscript']` 函数中，移除 `_with_0.insert_loader()` 行。

修改后的文件放入 Aegisub 仓库的 `automation/include` 目录即可使用。

## 开源协议

本仓库中所有文件均在各种 GPL 兼容的 BSD 风格协议下授权；详见 LICENCE 和各源文件。
官方 Windows 和 macOS 构建因包含 fftw3 而为 GPLv2。
