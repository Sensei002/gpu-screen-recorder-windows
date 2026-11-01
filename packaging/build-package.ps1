# =============================================================================
# build-package.ps1 - Phase 13 packaging: assembles the release layout from
# the `build` job artifact, validates it (executables, DLLs, license, icon +
# version resources, --help/--version/--info), and produces:
#
#   GPU-Screen-Recorder-Windows-x64-Portable.zip   (portable build)
#   GPU-Screen-Recorder-Windows-x64-Setup.exe      (Inno Setup installer)
#
# Runs on the plain `package` job runner (pwsh) - no MSYS2 needed. The
# binaries are statically linked; the only DLLs are the pango/fontconfig
# set for gsr-ui and the ANGLE DLLs (libEGL/libGLESv2) the engine dlopens,
# all bundled next to the exes by the build job and shipped in the artifact.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File packaging/build-package.ps1 `
#       -BinDir <artifact dir with exes + dlls> -OutDir dist -Version 6.0.0-w3
# =============================================================================
param(
    [Parameter(Mandatory = $true)][string]$BinDir,
    [string]$OutDir   = "dist",
    [string]$Version  = "6.0.0-w3"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$PackagingDir = $PSScriptRoot
$StageDir = Join-Path $OutDir "stage"
$ExeNames = @("gpu-screen-recorder.exe", "gsr-cli.exe", "gsr-ui.exe")

# Version forms: the resource/installer numeric form needs X.Y.Z.W. A dev
# suffix "-wN" maps to the 4th component (6.0.0-w3 -> 6.0.0.3) so the
# installer advertises the same numeric version the exes embed.
$VersionNum = ($Version -split "-")[0]
if (($VersionNum -split "\.").Count -lt 4) {
    $build = 1
    if ($Version -match '-w(\d+)$') { $build = [int]$Matches[1] }
    $VersionNum = "$VersionNum.$build"
}

Write-Host "== Phase 13 packaging =="
Write-Host "   binaries:  $BinDir"
Write-Host "   out:       $OutDir"
Write-Host "   version:   $Version (numeric $VersionNum)"
Write-Host ""

# ---------------------------------------------------------------------------
# Resource probes (no external tools): ExtractIconEx counts the icons in a PE
# (0 = no icon resource), VersionInfo reads the version resource.
# ---------------------------------------------------------------------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class GsrPe {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint ExtractIconEx(string szFileName, int nIconIndex, IntPtr[] phiconLarge, IntPtr[] phiconSmall, uint nIcons);
}
"@

function Get-IconCount([string]$Path) {
    $large = New-Object IntPtr[] 1
    $small = New-Object IntPtr[] 1
    return [GsrPe]::ExtractIconEx($Path, -1, $large, $small, 0)
}

function Assert-ExeResources([string]$Path, [string]$Label, [string]$ExpectedFileVersion = $Version) {
    if (-not (Test-Path $Path)) { throw "${Label}: missing $Path" }
    $icons = Get-IconCount $Path
    if ($icons -lt 1) { throw "$Label ($Path): no icon resource (ExtractIconEx = $icons)" }
    Write-Host "   [ok] $Label icon: $icons icon resource(s) embedded"
    $vi = (Get-Item $Path).VersionInfo
    # Trim: Inno Setup pads version-info strings to fixed width (trailing
    # spaces); windres-built exes are unpadded. Trim makes both compare equal.
    $productName = $vi.ProductName.Trim()
    $fileVersion = $vi.FileVersion.Trim()
    if ($productName -ne "GPU Screen Recorder") {
        throw "$Label ($Path): ProductName is '$productName', expected 'GPU Screen Recorder'"
    }
    # Our exes embed the full version string (e.g. 6.0.0-w1) as FileVersion;
    # the installer embeds the numeric X.Y.Z.W form (Inno VersionInfoVersion).
    if ($fileVersion -ne $ExpectedFileVersion) {
        throw "$Label ($Path): FileVersion is '$fileVersion', expected '$ExpectedFileVersion'"
    }
    Write-Host "   [ok] $Label version: $productName $fileVersion"
}

function Assert-Runs([string]$Path, [string[]]$ArgsList, [string]$Label, [int]$ExpectedExit = 0) {
    # 2>&1 on a native command turns stderr into ErrorRecords, which under
    # EAP=Stop would throw on any stderr write - suspend it for the call.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $Path @ArgsList 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($code -ne $ExpectedExit) {
        throw "${Label}: '$Path $ArgsList' exited $code (expected $ExpectedExit)`n$out"
    }
    Write-Host "   [ok] ${Label}: '$Path $ArgsList' (exit $ExpectedExit)"
}

function Invoke-Validation([string]$Dir, [string]$Label) {
    Write-Host "   validating $Label in $Dir"
    foreach ($exe in $ExeNames) {
        Assert-ExeResources (Join-Path $Dir $exe) $exe
    }
    $engine = Join-Path $Dir "gpu-screen-recorder.exe"
    $cli    = Join-Path $Dir "gsr-cli.exe"
    Assert-Runs $engine @("--version") "engine --version"
    # --help is an error path in the upstream args parser (usage printed,
    # ARGS_PARSE_RESULT_ERROR = exit 1); the exe's help must behave the same.
    Assert-Runs $engine @("--help")    "engine --help" 1
    Assert-Runs $cli    @("-h")        "gsr-cli -h"
    # --info needs the ANGLE DLLs next to the exe (dlopen'd) - they are in the
    # staged layout, which is exactly the portable/installer layout.
    Assert-Runs $engine @("--info")    "engine --info (ANGLE)"
    foreach ($required in @("LICENSE", "README.md", "NOTICE-WINDOWS-PORT.md")) {
        if (-not (Test-Path (Join-Path $Dir $required))) {
            throw "${Label}: missing $required"
        }
    }
    # The UI renders from <exe dir>/images/ + /translations/; without them
    # it exits with 'failed to load theme' and shows nothing. Assert the
    # resources the theme loader needs are present.
    foreach ($img in @("combobox_arrow.png", "gpu_screen_recorder_logo.png", "settings.png", "record.png")) {
        if (-not (Test-Path (Join-Path $Dir ("images\" + $img)))) {
            throw "${Label}: missing images/$img - the UI theme cannot load without it"
        }
    }
    $translations = Get-ChildItem (Join-Path $Dir "translations") -Filter *.txt -File -ErrorAction SilentlyContinue
    if ($translations.Count -eq 0) { throw "${Label}: missing translations/*.txt" }
    Write-Host "   [ok] $Label UI resources: images/ + translations/ present"
    Write-Host "   [ok] $Label validation passed"
    Write-Host ""
}

# ---------------------------------------------------------------------------
# 1. Stage the release layout.
# ---------------------------------------------------------------------------
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

foreach ($exe in $ExeNames) {
    $src = Join-Path $BinDir $exe
    if (-not (Test-Path $src)) { throw "artifact missing: $src" }
    Copy-Item $src (Join-Path $StageDir $exe)
}
# All bundled DLLs (pango set for gsr-ui + ANGLE for the engine).
$dlls = Get-ChildItem $BinDir -Filter *.dll -File
if ($dlls.Count -eq 0) { throw "no DLLs in artifact - the portable build needs the bundled pango/ANGLE DLLs" }
foreach ($dll in $dlls) { Copy-Item $dll.FullName (Join-Path $StageDir $dll.Name) }
# ANGLE is required for the engine; fail loudly rather than ship a broken zip.
foreach ($angle in @("libEGL.dll", "libGLESv2.dll")) {
    if (-not (Test-Path (Join-Path $StageDir $angle))) {
        throw "missing $angle in artifact - the engine dlopens ANGLE and the portable build cannot work without it"
    }
}
# License + docs + branding assets (banners/ico also used by the installer).
foreach ($doc in @("LICENSE", "README.md", "NOTICE-WINDOWS-PORT.md")) {
    Copy-Item (Join-Path $RepoRoot $doc) (Join-Path $StageDir $doc)
}
foreach ($asset in @("gsr.ico", "installer_banner.bmp", "installer_banner_small.bmp")) {
    Copy-Item (Join-Path $PackagingDir $asset) (Join-Path $StageDir $asset)
}
# The UI loads its theme/cursor/icon images from <exe dir>/images/ and
# translations from <exe dir>/translations/ (main.cpp resources_path), so the
# package MUST ship them or the UI fails with 'failed to load theme' and
# renders nothing. Vendor them straight from the repo (ui/images,
# ui/translations).
foreach ($resDir in @("images", "translations")) {
    $srcRes = Join-Path $RepoRoot ("ui\" + $resDir)
    if (-not (Test-Path $srcRes)) { throw "missing UI resource dir: $srcRes - the UI cannot render without it" }
    Copy-Item -Recurse $srcRes (Join-Path $StageDir $resDir)
}
if ((Get-ChildItem (Join-Path $StageDir "images") -File).Count -eq 0) {
    throw "images dir staged empty - the UI theme cannot load"
}
# fontconfig has MSYS2's builder sysconfdir (C:\msys64\etc\fonts) baked in,
# which does not exist on user machines - without a config it finds no fonts
# and the UI text renders blank. Ship a minimal fonts.conf that scans the
# Windows font directory; gsr-ui sets FONTCONFIG_PATH to its own dir at
# startup so this is picked up automatically (same trick as the CI tests).
@"
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<!-- Shipped with the Windows port: MSYS2's fontconfig has the builder's
     sysconfdir baked in, so the app sets FONTCONFIG_PATH to this dir and
     fontconfig scans the Windows font directory (Segoe UI etc.). -->
<fontconfig>
  <dir>C:/Windows/Fonts</dir>
</fontconfig>
"@ | Set-Content -Encoding ASCII (Join-Path $StageDir "fonts.conf")
# Build manifest so users can identify the build.
@"
GPU Screen Recorder - Windows port
Version: $Version
Upstream: gpu-screen-recorder (see NOTICE-WINDOWS-PORT.md for provenance)
Build: portable x64 (MinGW-w64), FFmpeg statically linked
"@ | Set-Content -Encoding ASCII (Join-Path $StageDir "VERSION.txt")

Write-Host "== staged layout =="
Get-ChildItem $StageDir | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host

# ---------------------------------------------------------------------------
# 2. Validate the staged layout (this is the portable/installer layout).
# ---------------------------------------------------------------------------
Invoke-Validation $StageDir "staged layout"

# ---------------------------------------------------------------------------
# 3. Portable ZIP.
# ---------------------------------------------------------------------------
$ZipPath = Join-Path $OutDir "GPU-Screen-Recorder-Windows-x64-Portable.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal
Write-Host "== wrote $ZipPath ($([math]::Round((Get-Item $ZipPath).Length / 1MB, 1)) MB)"

# Extract and re-validate - proves the archive round-trips and runs standalone.
$ExtractDir = Join-Path $OutDir "extract-check"
if (Test-Path $ExtractDir) { Remove-Item -Recurse -Force $ExtractDir }
Expand-Archive -Path $ZipPath -DestinationPath $ExtractDir
Invoke-Validation $ExtractDir "extracted zip"

# ---------------------------------------------------------------------------
# 4. Inno Setup installer (per-user, Start Menu + optional desktop/startup).
# ---------------------------------------------------------------------------
$ISCC = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $ISCC)) { throw "ISCC.exe not found at $ISCC - install Inno Setup first" }

# The .iss is generated here (not committed) so the stage path is embedded
# without ISPP path-quoting pitfalls.
$stageEsc = $StageDir.Replace("\", "\\")
$iss = @"
; GPU Screen Recorder - Windows x64 - generated by packaging/build-package.ps1
; (Phase 13: per-user install, Start Menu shortcut, optional desktop
; shortcut + autostart, uninstaller; config lives in %APPDATA% and is never
; touched by the installer or uninstaller.)
#define MyAppVersion "$Version"

[Setup]
AppId={{B9E4A7D2-3C15-4F6A-9B1E-8D0F2A5C6E71}
AppName=GPU Screen Recorder
AppVersion={#MyAppVersion}
AppPublisher=GPU Screen Recorder
DefaultDirName={localappdata}\Programs\GPU Screen Recorder
DefaultGroupName=GPU Screen Recorder
PrivilegesRequired=lowest
OutputBaseFilename=GPU-Screen-Recorder-Windows-x64-Setup
SetupIconFile=$stageEsc\gsr.ico
WizardImageFile=$stageEsc\installer_banner.bmp
WizardSmallImageFile=$stageEsc\installer_banner_small.bmp
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\gsr-ui.exe
UninstallDisplayName=GPU Screen Recorder
VersionInfoVersion=$VersionNum
VersionInfoProductName=GPU Screen Recorder
VersionInfoProductVersion=$VersionNum
DisableProgramGroupPage=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "startup";     Description: "Start GPU Screen Recorder &automatically at logon"; GroupDescription: "Additional options:"

[Files]
Source: "$stageEsc\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\GPU Screen Recorder"; Filename: "{app}\gsr-ui.exe"
Name: "{userdesktop}\GPU Screen Recorder";  Filename: "{app}\gsr-ui.exe"; Tasks: desktopicon

; Optional autostart - same HKCU Run entry the app's own autostart toggle writes.
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "GPU Screen Recorder"; ValueData: """{app}\gsr-ui.exe"" launch-daemon"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\gsr-ui.exe"; Description: "Launch GPU Screen Recorder"; Flags: nowait postinstall skipifsilent
"@
$IssPath = Join-Path $OutDir "installer.iss"
$iss | Set-Content -Encoding ASCII $IssPath

$SetupPath = Join-Path $OutDir "GPU-Screen-Recorder-Windows-x64-Setup.exe"
if (Test-Path $SetupPath) { Remove-Item -Force $SetupPath }

Write-Host "== running ISCC =="
& $ISCC /Qp "/O$OutDir" $IssPath
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

# ---------------------------------------------------------------------------
# 5. Validate the installer itself (icon + version resources).
# ---------------------------------------------------------------------------
Assert-ExeResources $SetupPath "installer" $VersionNum
Write-Host ""
Write-Host "== package complete =="
Get-ChildItem $OutDir -Filter "GPU-Screen-Recorder-*" | ForEach-Object { Write-Host "   $($_.Name) ($([math]::Round($_.Length / 1MB, 1)) MB)" }
