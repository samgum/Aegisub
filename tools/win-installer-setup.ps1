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
# These language files come from jrsoftware/issrc. A given language can be in
# either the Official or the Unofficial directory, and upstream moves files
# between them without notice: ChineseSimplified.isl and ChineseTraditional.isl
# were promoted from Unofficial/ to Official/, which 404'd the old Unofficial-
# only download and previously made the installer fall back to the wrong
# language (Chinese UI showed Greek) or hard-failed the build.
#
# Design rules:
#   1. Each language downloads from its own real content. We never fill a
#      missing Chinese file with Greek content again — that produces a broken,
#      misleading installer.
#   2. Each language has a list of candidate URLs (Official, then Unofficial,
#      then the jrsoftware istrans mirror) tried in order.
#   3. If a language genuinely cannot be obtained from any source, it falls
#      back to English content (not another random language), and the build
#      still succeeds.
if (!(Test-Path innosetup-langs)) {
	New-Item -ItemType Directory innosetup-langs | Out-Null

	$Base = "https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages"
	$Mirror = "https://jrsoftware.org/files/istrans"

	# Each entry: language name -> ordered candidate URLs. Languages promoted
	# to Official are listed Official-first; purely-Unofficial ones still try
	# Official first harmlessly (404) before hitting their real location.
	$LangUrls = @{
		'Greek'              = @("$Base/Greek.isl", "$Base/Unofficial/Greek.isl", "$Mirror/Greek.isl")
		'Basque'             = @("$Base/Basque.isl", "$Base/Unofficial/Basque.isl", "$Mirror/Basque.isl")
		'Galician'           = @("$Base/Galician.isl", "$Base/Unofficial/Galician.isl", "$Mirror/Galician.isl")
		'Indonesian'         = @("$Base/Indonesian.isl", "$Base/Unofficial/Indonesian.isl", "$Mirror/Indonesian.isl")
		'SerbianCyrillic'    = @("$Base/SerbianCyrillic.isl", "$Base/Unofficial/SerbianCyrillic.isl", "$Mirror/SerbianCyrillic.isl")
		'SerbianLatin'       = @("$Base/SerbianLatin.isl", "$Base/Unofficial/SerbianLatin.isl", "$Mirror/SerbianLatin.isl")
		'ChineseSimplified'  = @("$Base/ChineseSimplified.isl", "$Base/Unofficial/ChineseSimplified.isl", "$Mirror/ChineseSimplified.isl")
		'ChineseTraditional' = @("$Base/ChineseTraditional.isl", "$Base/Unofficial/ChineseTraditional.isl", "$Mirror/ChineseTraditional.isl")
	}

	# Minimal English .isl used only if every source for a language fails —
	# keeps iscc happy and the build green without ever showing the wrong
	# language's strings.
	$EnglishFallback = @"
[LangOptions]
LanguageName=English
LanguageID=`$0409
LanguageCodePage=0
"@

	foreach ($Lang in $LangUrls.Keys) {
		$OutFile = Join-Path 'innosetup-langs' "$Lang.isl"
		$Got = $false
		foreach ($Url in $LangUrls[$Lang]) {
			try {
				Invoke-WebRequest $Url -OutFile $OutFile -UseBasicParsing -ErrorAction Stop
				$Got = $true
				break
			}
			catch {
				# try next candidate URL
			}
		}
		if (-not $Got) {
			Write-Warning ("Could not download $Lang.isl from any source; using English fallback so the installer still builds.")
			$EnglishFallback | Out-File -FilePath $OutFile -Encoding UTF8
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
