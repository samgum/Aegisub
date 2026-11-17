# Aegisub (SGMY 分支)

**[English](README.md)** | **[简体中文](README_CN.md)**

[![Meson CI](https://github.com/samgum/Aegisub/actions/workflows/ci.yml/badge.svg)](https://github.com/samgum/Aegisub/actions/workflows/ci.yml)

**Aegisub (SGMY 分支)**（版本号 `3.4.2.1`）是基于 [Aegisub](https://github.com/TypesettingTools/Aegisub) 的增强型跨平台分支，针对中日文字幕制作、日语卡拉 OK 与振假名注音、HDR10/HLG 视频预览、LRC/TTML 歌词导入、字幕超画幅检测、AI 辅助语法分析以及 macOS/Windows/Linux 稳定性进行了深度优化与修复。

---

## 本分支核心增强功能

### 1. 歌词导入（LRC、增强型 LRC、TTML / DFXP）
* **LRC 与增强型 LRC (`.lrc`)**：支持普通时间戳歌词及逐字增强型 LRC（`<mm:ss.xx>`）自动转换为 ASS `\kf` 扫字卡拉 OK 标签，包含行尾时间戳终止、相邻时间重叠自动钳位、空占位时间行清理，以及无时间戳 LRC 纯文本回退导入。
* **TTML / DFXP (`.ttml`, `.dfxp`, `.xml`)**：支持将 `<p>` 对白及 `<span begin="..." end="...">` 逐词时间轴转换为 `\kf`，支持 `ttm:role="x-bg"` 背景和声拆行与时间约束、`itunes:timing="None"` 无时间轴回退导入，以及直接拖拽文件导入。

### 2. HDR10 / HLG 与杜比视界（Dolby Vision）预览渲染
* **16-bit BT.2100 CPU 预览管线**：通过 `yuv444p16le`（并以 `rgb48le` 作为回退格式）解码 HDR10（PQ）与 HLG 视频源，在执行逆 PQ EOTF / HLG 逆 OETF 转换（`hdr_tonemap.h`）前完整保留原始传递编码信号序。
* **色域压缩与有序抖动**：执行 BT.2020 到 BT.709 的色域压缩与高光柔和压缩（Soft Roll-off），并使用 8×8 Bayer 有序抖动输出到 8-bit SDR 预览画面。
* **4K 流畅预览与杜比视界识别**：宽度超过 1920 像素的 HDR 视频会自动缩放至 1920 px 进行预览，避免卡顿；对携带标准 PQ/HLG 基础层的杜比视界片源可正常预览基础层（不应用 RPU 重塑），对 Profile 5 / IPT-ICtCp 片源则安全记录日志而不错误偏色渲染。

### 3. 超过画面检测（渲染宽度与字符数双模式）
* **双检测模式**（位于 `首选项 -> 界面` / `颜色`）：
  * **按字幕渲染宽度（默认）**：基于 `libass` 实际像素包围盒进行权威判断（精确处理 `\move`、`\pos`、`\fsp`、`\b`/`\i` 并跳过 `\t(...)` 动态变换块），同时在编辑框内借助带缓存的 `wxDC` 测量实现逐字实时高亮。
  * **按纯文本字符数**：当纯文本超过自定义字符上限（`Subtitle/Overflow Highlight/Character Limit`）时自动标黄提醒。
* 支持分别自定义编辑框高亮色（超长行高亮）与字幕列表背景色（超长行背景）。

### 4. 批量打轴、CJK 文本处理、样式重叠与日语振假名工具
以下工具均位于 **工具（Tools）** 菜单下：
* **修复常见错误**（`工具 -> 修复常见错误...`，主工具栏亦提供快捷图标）：参考 Subtitle Edit 设计，支持批量修复时间重叠、最小间隔、过短/过长持续时间（按时间先后顺序钳位防止反向覆盖）、空白行以及行首尾空格，参数自动持久化保存。
* **时间轴缝合（Stitch Timings）**（`工具 -> 缝合时间轴...`）：按毫秒或帧阈值自动吸附相邻字幕间隙，支持多种对齐锚点与选中范围模式。
* **检查样式重叠**（`工具 -> 检查样式重叠...`）：按样式分组排查同一样式内的时间重叠行。
* **中文简繁转换**（`工具 -> 中文简繁转换...`）：基于 ICU 转写器实现选中行或全本的简体/繁体互转。
* **字幕文本清理** & **成对标点检查**：清理多余全角句号与连续空格，检查中英文引号、书名号及括号配对异常（跳过英文词内撇号如 `don't`，并报告准确的 UTF-8 字符位置）。
* **日语振假名标注**（`工具 -> 日语振假名标注...`）：支持在汉字上方或下方生成可编辑的振假名注音，支持平假名/片假名切换、同一词汇按出现位置分别设置读音、考虑 `\fsp` 字距与换行模式的多行自动对齐、按样式筛选应用，并支持 **SudachiPy** 一键自动填充读音（需执行 `python -m pip install --upgrade sudachipy sudachidict_core`）。

### 5. 音乐滚动歌词生成器
* **滚动歌词生成**（`工具 -> 音乐滚动歌词...`）：使用 `\move` 与三次缓出（Cubic-Out）曲线生成平滑滚动歌词场景，支持实时样式预览、多种分辨率预设、当前行/非当前行独立样式，以及单语或双语模糊时间轴匹配。

### 6. AI 字幕语法分析
* **快捷分析面板与设置**：通过字幕编辑框旁的可选 `AI` 按钮调用（需勾选 `在字幕文本框旁启用 AI 按钮`），并在 `工具 -> AI 语法分析设置...` 中配置。
* **内置 OpenAI 兼容预设**：支持 **DeepSeek**、**OpenRouter**、**Gemini（OpenAI 兼容接口）**、**阿里云百炼（通义千问）** 以及任意自定义 OpenAI 兼容端点与模型。
* **安全与体验**：API Key 仅保存在当前会话内存中（不写入明文配置文件）；支持响应缓存、可选思考/推理参数开关、跨平台线程安全回调以及完整的 JSON/UTF-16 代理对反转义解析。

### 7. 音频播放、变速与跨平台稳定性优化
* **宽范围无级变调/保调变速**：通过 SoundTouch 支持 `0.25×` 至 `4.0×` 播放倍率（`25, 50, 75, 100, 125, 150, 175, 200, 250, 300, 400`），可在音频控制栏选择或使用快捷键 `Ctrl+Shift+Up` / `Ctrl+Shift+Down` 快速切换。
* **预览峰值限幅保护**：内置 `ApplyGainLimiter` 软限幅保护，覆盖 PortAudio、OpenAL 与 SoundTouch 预览通路，防止增益放大导致削波爆音。
* **全平台音频后端修复**：
  * **macOS（PortAudio / CoreAudio）**：支持系统默认输出设备热切换而不断流崩溃，并通过合理延迟下限保持低延迟打轴同步。
  * **Windows（DirectSound2）**：准确跟踪单次短音频与环形缓冲区尾部静音排空状态，避免末尾音频被提前截断。
  * **OpenAL**：准确报告处于 `[start_frame, end_frame]` 范围内的源采样进度，并在缓冲区欠载后自动恢复播放。

### 8. 多语言本地化与 SGMY 更新检查
* **31 种界面语言**（`po/LINGUAS`）：内置完整的简体中文（`zh_CN`）与繁体中文（`zh_TW`）翻译，全面覆盖本分支新增的所有对话框、菜单项与偏好设置，并保留 29 种上游语言包。
* **版本更新检查**：已将内置更新检查指向 `samgum/Aegisub` 的 GitHub Releases。

### 9. 同步上游（TypesettingTools/Aegisub）的重要改进与安全修复
已整合来自上游 [TypesettingTools/Aegisub](https://github.com/TypesettingTools/Aegisub) 的多项核心修复与特性：
* **安全加固补丁**：修复 ASS 解析器/附件管理、Matroska/SRT/VobSub/YUV4MPEG 解析器、AviSynth 封装、文件映射、VFR 时间码越界/整数溢出问题以及 macOS CoreText 字体列举的内存泄漏。
* **字幕与视频功能修复**：新增 `LayoutResX` / `LayoutResY` 布局分辨率属性支持、修复独立视频窗口缩放平移比例问题、修复音频面板视频当前帧指示线吸附问题，并改进 UTF-8 / BOM 字符集检测。
* **Automation 与 Lua 更新**：同步 `aegisub.version` 接口表修复、MoonScript `0.7.0` 兼容、`lfs` 与 Lua 错误栈修复，以及确保 LuaJIT 外部栈展开正常工作的 `-fexceptions` 编译选项。
* **macOS 专项优化**：修复中文输入法组字期间误触发系统提示音（`NSBeep()`）与光标按键捕获问题、修复 Retina 屏幕取色器象限偏移及多显示器坐标换算、修复 `RawCtrl-` 快捷键显示、清理窗口浮动通知观察者并优化 `.app` 捆绑与动态库 `rpath` 处理。

---

## 从源码编译

已发布的各平台安装包与 macOS `.dmg` 镜像可在 [Releases](https://github.com/samgum/Aegisub/releases) 页面下载。

### 通用依赖环境
* 支持 C++20 的编译器、[Meson](https://mesonbuild.com/) `>= 0.60.0`、Ninja、Python 3
* wxWidgets `>= 3.2.2`、ICU（`icu-uc`、`icu-i18n`）、Boost、libass、FFMS2 `>= 5.0`、libcurl、uchardet、Hunspell、FFTW3、SoundTouch

### macOS（Apple Silicon / Intel）
使用 Homebrew 安装编译依赖（与 `.github/workflows/ci.yml` 保持一致）：

```bash
brew install ninja libass zlib ffms2 fftw hunspell pulseaudio sound-touch portaudio
```

配置、编译、运行单元测试并生成 `Aegisub.app` 与 `.dmg`：

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

### Linux（Ubuntu / Debian）
安装构建依赖（与 `.github/workflows/ci.yml` 保持一致）：

```bash
sudo apt-get update
sudo apt-get install -y \
  ninja-build build-essential libx11-dev libfreetype6-dev pkg-config \
  libfontconfig1-dev libass-dev libasound2-dev libffms2-dev intltool \
  libboost-all-dev libhunspell-dev libcurl4-openssl-dev libuchardet-dev \
  libgtest-dev libgmock-dev libwxgtk3.2-dev libportal-gtk3-dev libsoundtouch-dev
pip install meson
```

配置、编译、测试并安装：

```bash
meson setup build -Dbuildtype=release -Db_pch=false
meson compile -C build
meson test -C build --verbose --suite Aegisub
sudo meson install -C build
```

> **Linux 发行版打包提示**：Aegisub 需要开启 Lua 5.2 兼容模式（`-DLUAJIT_ENABLE_LUA52COMPAT`）的 LuaJIT。默认情况下，如果系统 LuaJIT 未开启该模式，Meson 会自动回退到内置 `luajit` 子项目进行编译；传入 `-Dsystem_luajit=true` 则可强制检验系统库是否满足要求。

### Windows（MSVC）
在 **x64 Native Tools Command Prompt for VS** 中运行（需通过 `choco install ninja innosetup` 安装基础工具，并将 `gettext` 与 `moonscript` 加入 `PATH`）：

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
# 可选：构建安装程序与绿色便携包
meson compile win-installer -C build
ninja -C build win-portable
```

### 源码回归检查
无需完整本地编译即可运行轻量化 Python 源码回归验证脚本：

```bash
python3 tools/run_source_regressions.py
```

---

## 开源协议

Aegisub 采用 **BSD 3-Clause** 开源协议，详见 [LICENCE](LICENCE)。
