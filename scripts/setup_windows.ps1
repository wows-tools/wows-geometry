#Requires -Version 5.1
<#
.SYNOPSIS
    Sets up the build environment, builds wows-model-exporter, and optionally
    packages it as a Windows MSI installer.

.PARAMETER Qt6BinDir
    Path to the Qt6 bin directory (e.g. C:\Qt\6.7.0\msvc2022_64\bin).
    Required only if Qt6Quick.dll (or other Qt DLLs) must be bundled.
    If PySide6 is installed in Python, its Qt DLLs are picked up automatically.

.PARAMETER BuildType
    CMake build type: Release (default) or Debug.

.PARAMETER SkipMSI
    Build only; do not run CPack to generate the MSI.

.PARAMETER BuildDir
    Directory for CMake build artefacts. Defaults to "build" next to this script.

.EXAMPLE
    .\setup_windows.ps1
    .\setup_windows.ps1 -Qt6BinDir "C:\Qt\6.7.0\msvc2022_64\bin"
    .\setup_windows.ps1 -SkipMSI -BuildType Debug
#>
param(
    [string] $Qt6BinDir  = "",
    [string] $BuildType  = "Release",
    [switch] $SkipMSI,
    [string] $BuildDir   = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

function Assert-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        Write-Error "'$name' not found in PATH. See prerequisites below."
    }
}

function Test-Choco {
    return $null -ne (Get-Command choco -ErrorAction SilentlyContinue)
}

function Install-Choco {
    Write-Step "Installing Chocolatey"
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
    $env:PATH += ";$env:ALLUSERSPROFILE\chocolatey\bin"
}

function Ensure-ChocoPackage([string]$pkg) {
    if (-not (choco list --local-only $pkg 2>$null | Select-String "^$pkg ")) {
        Write-Step "Installing $pkg via Chocolatey"
        choco install $pkg -y --no-progress
    } else {
        Write-Host "  $pkg already installed" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# Root of the repo (parent of the scripts/ folder)
# ---------------------------------------------------------------------------
$RepoRoot = Split-Path $PSScriptRoot -Parent
if ($BuildDir -eq "") { $BuildDir = Join-Path $RepoRoot "build" }

Write-Step "Repository root : $RepoRoot"
Write-Step "Build directory : $BuildDir"

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------
Write-Step "Checking / installing prerequisites"

if (-not (Test-Choco)) { Install-Choco }

Ensure-ChocoPackage "cmake"
Ensure-ChocoPackage "git"
Ensure-ChocoPackage "python311"   # provides python311.dll

# WiX Toolset (needed for MSI packaging)
if (-not $SkipMSI) { Ensure-ChocoPackage "wixtoolset" }

# Refresh PATH so newly installed tools are visible
$env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("PATH","User")

Assert-Command cmake
Assert-Command git
Assert-Command python

# ---------------------------------------------------------------------------
# Locate Python and collect its root dir (for DLL bundling at CPack time)
# ---------------------------------------------------------------------------
$PythonExe = (Get-Command python).Source
$PythonBinDir = Split-Path $PythonExe -Parent
Write-Host "  Python: $PythonExe"

# Detect Qt from PySide6 / PyQt6 installed in the active Python env, or from
# an explicit -Qt6BinDir argument.
if ($Qt6BinDir -eq "") {
    try {
        $psidePath = python -c "import PySide6; import os; print(os.path.dirname(PySide6.__file__))" 2>$null
        if ($psidePath -and (Test-Path $psidePath)) {
            $Qt6BinDir = $psidePath
            Write-Host "  Qt6 DLLs from PySide6: $Qt6BinDir" -ForegroundColor Yellow
        }
    } catch {}
}
if ($Qt6BinDir -ne "") {
    Write-Host "  Qt6 bin dir: $Qt6BinDir"
} else {
    Write-Host "  Qt6 not detected - Qt6Quick.dll will not be bundled automatically" -ForegroundColor Yellow
    Write-Host "  Use -Qt6BinDir <path> to specify the Qt6 bin directory" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# vcpkg – install C++ dependencies
# ---------------------------------------------------------------------------
Write-Step "Setting up vcpkg dependencies"

$VcpkgRoot = "C:\vcpkg"
if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    Write-Host "  vcpkg not found at $VcpkgRoot – cloning and bootstrapping"
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
}

$packages = @("zlib:x64-windows", "pcre2:x64-windows", "meshoptimizer:x64-windows")
foreach ($pkg in $packages) {
    Write-Host "  vcpkg install $pkg"
    & "$VcpkgRoot\vcpkg.exe" install $pkg | Out-Null
}

# ---------------------------------------------------------------------------
# Update git submodules
# ---------------------------------------------------------------------------
Write-Step "Updating submodules"
Push-Location $RepoRoot
git submodule update --init --recursive
Pop-Location

# ---------------------------------------------------------------------------
# CMake configure
# ---------------------------------------------------------------------------
Write-Step "Configuring CMake ($BuildType)"

$cmakeArgs = @(
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DBUILD_TESTS=OFF",
    "-DBUNDLE_WOWS_DEPACK=ON",
    "-DBUNDLE_TINYGLTF=ON",
    "-DPython3_ROOT_DIR=$PythonBinDir"
)
if ($Qt6BinDir -ne "") {
    $cmakeArgs += "-DQT6_BIN_DIR=$Qt6BinDir"
}

Push-Location $RepoRoot
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Pop-Location; exit $LASTEXITCODE }
Pop-Location

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Write-Step "Building"
cmake --build $BuildDir --config $BuildType
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---------------------------------------------------------------------------
# CPack – generate MSI
# ---------------------------------------------------------------------------
if (-not $SkipMSI) {
    Write-Step "Packaging MSI with CPack"
    Push-Location $BuildDir
    cpack -G WIX -C $BuildType -V
    $rc = $LASTEXITCODE
    Pop-Location

    if ($rc -ne 0) { exit $rc }

    $msi = Get-ChildItem $BuildDir -Filter "*.msi" | Select-Object -First 1
    if ($msi) {
        Write-Host "`nMSI ready: $($msi.FullName)" -ForegroundColor Green
    } else {
        Write-Warning "CPack finished but no .msi file found in $BuildDir"
    }
} else {
    Write-Host "`nBuild complete (MSI packaging skipped)" -ForegroundColor Green
    Write-Host "Binaries in: $BuildDir\$BuildType"
}
