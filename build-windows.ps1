#requires -Version 5.1
<#
.SYNOPSIS
  Builds the Northstar Master VST3 plug-in for Windows x64.

.PARAMETER Install
  After the build, copy the .vst3 bundle into the VST3 folder.
  Run PowerShell as Administrator to install into
  C:\Program Files\Common Files\VST3 (recommended, every DAW scans it).
  Without admin rights the per-user folder is used instead.

.PARAMETER BuildStandalone
  Also build the standalone .exe (handy for a quick sound check without a DAW).

.PARAMETER JuceDir
  Path to an already downloaded JUCE 8.0.12 source folder (skips the git download).

.PARAMETER Vst2SdkPath
  Optional path to your own, separately licensed VST2 SDK.

.PARAMETER Clean
  Delete the build folder first (use after installing a different Visual Studio).
#>
param(
    [string]$Vst2SdkPath = "",
    [string]$JuceDir = "",
    [switch]$BuildStandalone,
    [switch]$Install,
    [switch]$Clean,
    [int]$Jobs = [Math]::Min([Environment]::ProcessorCount, 4)
)

$ErrorActionPreference = "Stop"
$projectDir = $PSScriptRoot
$buildDir = Join-Path $projectDir "build-windows"

function Fail($message) {
    Write-Host ""
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------- CMake
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail ("CMake 3.22+ not found. Install it (winget install Kitware.CMake), " +
          "tick 'Add CMake to the system PATH', then open a NEW PowerShell window.")
}

# ------------------------------------------------------------------ Git
if (-not $JuceDir -and -not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail ("Git not found - CMake needs it to download JUCE. Install it " +
          "(winget install Git.Git) and open a NEW PowerShell window, " +
          "or pass -JuceDir <path to a downloaded JUCE 8.0.12 folder>.")
}
if ($JuceDir -and -not (Test-Path (Join-Path $JuceDir "CMakeLists.txt"))) {
    Fail "-JuceDir '$JuceDir' does not contain CMakeLists.txt (point it at the JUCE root folder)."
}

# ------------------------------------------------- Visual Studio / MSVC
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail ("Visual Studio with the C++ compiler was not found. Install 'Visual Studio 2022 Community' " +
          "(or 'Build Tools for Visual Studio') and select the workload " +
          "'Desktop development with C++'.")
}

$versions = & $vswhere -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationVersion
if (-not $versions) {
    Fail ("Visual Studio is installed, but without the C++ tools. Open the Visual Studio Installer, " +
          "click Modify and tick 'Desktop development with C++'.")
}

$generatorNames = @{ 16 = "Visual Studio 16 2019"; 17 = "Visual Studio 17 2022"; 18 = "Visual Studio 18 2026" }
$cmakeHelp = (& cmake --help) -join "`n"
$generator = $null
$majors = @($versions | ForEach-Object { [int]($_.ToString().Split('.')[0]) } | Sort-Object -Descending)
foreach ($major in $majors) {
    $name = $generatorNames[$major]
    if ($name -and $cmakeHelp.Contains($name)) { $generator = $name; break }
}
if (-not $generator) {
    Fail ("Your CMake does not know the installed Visual Studio version. " +
          "Update CMake (winget upgrade Kitware.CMake) and try again.")
}
Write-Host "Using generator: $generator" -ForegroundColor Cyan

# ------------------------------------------------------------ Configure
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing old build folder..."
    Remove-Item -Recurse -Force $buildDir
}

$configureArgs = @("-S", $projectDir, "-B", $buildDir, "-G", $generator, "-A", "x64")
if ($JuceDir)      { $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_JUCE=$JuceDir" }
if ($Vst2SdkPath)  { $configureArgs += "-DNORTHSTAR_VST2_SDK_PATH=$Vst2SdkPath" }

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    Fail ("CMake configuration failed. If the message mentions a different generator / cache, " +
          "re-run with -Clean. If it mentions git/JUCE, check your internet connection.")
}

# ---------------------------------------------------------------- Build
$targets = @("NorthstarMastering_VST3")
if ($BuildStandalone) { $targets += "NorthstarMastering_Standalone" }

& cmake --build $buildDir --config Release --target $targets --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Fail "Build failed (see the compiler errors above)." }

$artefacts = Join-Path $buildDir "NorthstarMastering_artefacts\Release"
$pluginPath = Join-Path $artefacts "VST3\Northstar Master.vst3"
$pluginDll = Join-Path $pluginPath "Contents\x86_64-win\Northstar Master.vst3"
if (-not (Test-Path $pluginDll)) {
    Fail "Build finished but the plug-in binary was not found at: $pluginDll"
}

Write-Host ""
Write-Host "VST3 bundle created:" -ForegroundColor Green
Write-Host "  $pluginPath"
if ($BuildStandalone) { Write-Host "Standalone app folder: $(Join-Path $artefacts 'Standalone')" }

# -------------------------------------------------------------- Install
if ($Install) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $isAdmin = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)

    if ($isAdmin) {
        $destination = Join-Path $env:CommonProgramFiles "VST3"
    } else {
        $destination = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3"
        Write-Host ""
        Write-Host ("Not running as Administrator - installing to the per-user folder. " +
                    "Some DAWs do not scan it; re-run as Administrator to install system-wide.") -ForegroundColor Yellow
    }

    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -Recurse -Force -Path $pluginPath -Destination $destination
    Write-Host ""
    Write-Host "Installed to: $destination" -ForegroundColor Green
    Write-Host "Now rescan plug-ins in your DAW."
} else {
    Write-Host ""
    Write-Host "Next: copy the whole 'Northstar Master.vst3' folder to"
    Write-Host "  C:\Program Files\Common Files\VST3"
    Write-Host "(or re-run this script as Administrator with -Install), then rescan plug-ins in your DAW."
}
