#!/usr/bin/env powershell

param (
  [Parameter(Position = 0)]
  [string]$BuildRoot,
  [Parameter(Position = 1)]
  [string]$SourceRoot
)

$InstallerDir = Join-Path $SourceRoot "packages\win_installer" | Resolve-Path
$DepsDir = Join-Path $BuildRoot "installer-deps"
if (!(Test-Path $DepsDir)) {
	New-Item -ItemType Directory -Path $DepsDir
}

$Env:BUILD_ROOT = $BuildRoot
$Env:SOURCE_ROOT = $SourceRoot

Set-Location $DepsDir

$GitHeaders = @{}
if (Test-Path 'Env:GITHUB_TOKEN') {
	$GitHeaders = @{ 'Authorization' = 'Bearer ' + $Env:GITHUB_TOKEN }
}

# DepCtrl
if (!(Test-Path DependencyControl)) {
	git clone https://github.com/TypesettingTools/DependencyControl.git
	Set-Location DependencyControl
	git checkout v0.6.3-alpha
	Set-Location $DepsDir
}

# YUtils
if (!(Test-Path YUtils)) {
	git clone https://github.com/TypesettingTools/YUtils.git
}

# luajson
if (!(Test-Path luajson)) {
	git clone https://github.com/harningt/luajson.git
}

# Avisynth
# if (!(Test-Path AviSynthPlus64)) {
# 	$avsReleases = Invoke-WebRequest "https://api.github.com/repos/AviSynth/AviSynthPlus/releases/latest" -Headers $GitHeaders -UseBasicParsing | ConvertFrom-Json
# 	$avsUrl = $avsReleases.assets[0].browser_download_url
# 	Invoke-WebRequest $avsUrl -OutFile AviSynthPlus.7z -UseBasicParsing
# 	7z x AviSynthPlus.7z
# 	Rename-Item (Get-ChildItem -Filter "AviSynthPlus_*" -Directory) AviSynthPlus64
# 	Remove-Item AviSynthPlus.7z
# }

# VSFilter
if (!(Test-Path VSFilter)) {
	$vsFilterDir = New-Item -ItemType Directory VSFilter
	Set-Location $vsFilterDir
	$vsFilterReleases = Invoke-WebRequest "https://api.github.com/repos/pinterf/xy-VSFilter/releases/latest" -Headers $GitHeaders -UseBasicParsing | ConvertFrom-Json
	$vsFilterUrl = $vsFilterReleases.assets[0].browser_download_url
	Invoke-WebRequest $vsFilterUrl -OutFile VSFilter.7z -UseBasicParsing
	7z x VSFilter.7z
	Remove-Item VSFilter.7z
	Set-Location $DepsDir
}

# ffi-experiments
if (!(Test-Path ffi-experiments)) {
	Get-Command "moonc" # check to ensure Moonscript is present
	git clone https://github.com/TypesettingTools/ffi-experiments.git
	Set-Location ffi-experiments
	meson build -Ddefault_library=static
	if(!$?) { Exit $LASTEXITCODE }
	meson compile -C build
	if(!$?) { Exit $LASTEXITCODE }
	Set-Location $DepsDir
}

# VC++ redistributable
if (!(Test-Path VC_redist)) {
	$redistDir = New-Item -ItemType Directory VC_redist
	Invoke-WebRequest https://aka.ms/vs/17/release/VC_redist.x64.exe -OutFile "$redistDir\VC_redist.x64.exe" -UseBasicParsing
}

# Dictionaries
if (!(Test-Path dictionaries)) {
	New-Item -ItemType Directory dictionaries
	Invoke-WebRequest https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.aff -OutFile dictionaries/en_US.aff -UseBasicParsing
	Invoke-WebRequest https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.dic -OutFile dictionaries/en_US.dic -UseBasicParsing
}

# Installer localization
#
# These language files live upstream in the jrsoftware/issrc repo under
# Files/Languages/Unofficial. They are NOT version-pinned and individual files
# disappear without warning — both ChineseSimplified.isl and
# ChineseTraditional.isl have been removed upstream at different times, and each
# removal previously hard-failed the entire Windows installer build.
#
# Design rule: no single language file is ever load-bearing. We try a chain of
# candidate fallback sources, and if every network source fails we write a tiny
# self-contained English .isl so iscc always has something valid to read. A
# missing upstream language may downgrade that one installer language to
# English, but it can never break the build.
if (!(Test-Path innosetup-langs)) {
	New-Item -ItemType Directory innosetup-langs | Out-Null

	# All unofficial languages we reference from fragment_setupbase.iss, in the
	# order we want to try them. The first one that downloads successfully
	# becomes the fallback content used for any later missing file.
	$Langs = @(
		'Greek',
		'Basque',
		'Galician',
		'Indonesian',
		'SerbianCyrillic',
		'SerbianLatin',
		'ChineseSimplified',
		'ChineseTraditional'
	)

	$FallbackFile = $null  # path to the first .isl we successfully obtained

	foreach ($Lang in $Langs) {
		$OutFile = Join-Path 'innosetup-langs' "$Lang.isl"
		$Url = "https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/Unofficial/$Lang.isl"
		try {
			Invoke-WebRequest $Url -OutFile $OutFile -UseBasicParsing -ErrorAction Stop
			if (-not $FallbackFile) { $FallbackFile = $OutFile }
		}
		catch {
			if ($FallbackFile) {
				Write-Warning ("Language file {0}.isl could not be downloaded from upstream ({1}). " -f $Lang, $Url)
				Write-Warning ("Falling back to an already-downloaded language so the installer still builds. Error: {0}" -f $_.Exception.Message)
				Copy-Item $FallbackFile $OutFile -Force
			}
			else {
				Write-Warning ("Language file {0}.isl could not be downloaded and no fallback is available yet. Will retry later languages. Error: {1}" -f $Lang, $_.Exception.Message)
			}
		}
	}

	# Absolute last-resort fallback: if nothing downloaded (e.g. upstream repo
	# moved, runner is offline, etc.), synthesize a minimal valid English .isl
	# so iscc can always parse the [Languages] entries. The text below is the
	# minimum iscc needs: a [LangOptions] section with the language name.
	if (-not $FallbackFile) {
		Write-Warning "No upstream language file could be downloaded; writing a minimal English fallback."
		$Minimal = @"
[LangOptions]
LanguageName=English
LanguageID=$0409
LanguageCodePage=0
"@
		$MinimalFile = Join-Path 'innosetup-langs' 'Greek.isl'
		$Minimal | Out-File -FilePath $MinimalFile -Encoding UTF8
		$FallbackFile = $MinimalFile
		# Ensure every referenced language file exists so iscc finds them all.
		foreach ($Lang in $Langs) {
			$OutFile = Join-Path 'innosetup-langs' "$Lang.isl"
			if (-not (Test-Path $OutFile)) {
				Copy-Item $FallbackFile $OutFile -Force
			}
		}
	}
}

# Aegisub localization
Set-Location $BuildRoot
meson compile aegisub-gmo
if(!$?) { Exit $LASTEXITCODE }

# Invoke InnoSetup
$IssUrl = Join-Path $InstallerDir "aegisub_depctrl.iss"
iscc $IssUrl
if(!$?) { Exit $LASTEXITCODE }
