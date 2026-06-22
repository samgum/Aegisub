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
# Files/Languages/Unofficial. They are not version-pinned and can disappear
# at any time — ChineseTraditional.isl was removed upstream, which hard-failed
# the entire Windows installer build. Download each one defensively: on any
# failure (404, transient network error, etc.) log a warning and fall back to
# the Simplified Chinese file (same family, identical structure, UTF-8 text),
# so a single missing language can never break the whole Windows build again.
if (!(Test-Path innosetup-langs)) {
	New-Item -ItemType Directory innosetup-langs | Out-Null

	# Simplified Chinese is the designated fallback: it downloads reliably, is
	# structurally identical to every other unofficial .isl, and is the closest
	# match for the Traditional Chinese file that upstream deleted.
	$FallbackLang = 'ChineseSimplified'
	$FallbackUrl = "https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/Unofficial/$FallbackLang.isl"
	try {
		Invoke-WebRequest $FallbackUrl -OutFile "innosetup-langs/$FallbackLang.isl" -UseBasicParsing -ErrorAction Stop
	}
	catch {
		throw "Could not download the fallback language file $FallbackLang.isl from upstream: $($_.Exception.Message)"
	}

	# All remaining unofficial languages we reference from fragment_setupbase.iss.
	$Langs = @(
		'Greek',
		'Basque',
		'Galician',
		'Indonesian',
		'SerbianCyrillic',
		'SerbianLatin',
		'ChineseTraditional'
	)

	foreach ($Lang in $Langs) {
		$OutFile = Join-Path 'innosetup-langs' "$Lang.isl"
		$Url = "https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/Unofficial/$Lang.isl"
		try {
			Invoke-WebRequest $Url -OutFile $OutFile -UseBasicParsing -ErrorAction Stop
		}
		catch {
			Write-Warning ("Language file {0}.isl could not be downloaded from upstream ({1}). " -f $Lang, $Url)
			Write-Warning ("Falling back to $FallbackLang.isl so the Windows installer can still build. Error: {0}" -f $_.Exception.Message)
			Copy-Item "innosetup-langs/$FallbackLang.isl" $OutFile -Force
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
