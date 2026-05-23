#Requires -Version 5.1
<#
.SYNOPSIS
    Builds wows-model-exporter on Windows using the bundled vcpkg submodule.

.PARAMETER BuildType
    CMake build type: Release (default) or Debug.

.PARAMETER BuildDir
    Directory for CMake build artefacts. Defaults to "build" next to this script.

.EXAMPLE
    .\setup_windows.ps1
    .\setup_windows.ps1 -BuildType Debug
#>
param(
    [string] $BuildType  = "Release",
    [string] $BuildDir   = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step([string]$msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

function Assert-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        Write-Error "'$name' not found in PATH. See prerequisites in README.md."
    }
}

$RepoRoot = Split-Path $PSScriptRoot -Parent
if ($BuildDir -eq "") { $BuildDir = Join-Path $RepoRoot "build" }
$VcpkgRoot = Join-Path $RepoRoot "deps\vcpkg"

Write-Step "Repository root : $RepoRoot"
Write-Step "Build directory : $BuildDir"

Assert-Command cmake
Assert-Command git

Write-Step "Initializing submodules"
Push-Location $RepoRoot
git submodule update --init --recursive
Pop-Location

if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    Write-Step "Bootstrapping vcpkg (deps/vcpkg)"
    Push-Location $VcpkgRoot
    & .\bootstrap-vcpkg.bat -disableMetrics
    if ($LASTEXITCODE -ne 0) { Pop-Location; exit $LASTEXITCODE }
    Pop-Location
}

Write-Step "Configuring CMake ($BuildType)"
Push-Location $RepoRoot
cmake -B $BuildDir `
    -DCMAKE_BUILD_TYPE=$BuildType `
    -DBUILD_TESTS=OFF `
    -DBUNDLE_WOWS_DEPACK=ON `
    -DBUNDLE_TINYGLTF=ON
if ($LASTEXITCODE -ne 0) { Pop-Location; exit $LASTEXITCODE }
Pop-Location

Write-Step "Building"
cmake --build $BuildDir --config $BuildType
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "`nBuild complete" -ForegroundColor Green
Write-Host "Binaries in: $BuildDir\$BuildType"
Write-Host "Python runtime (for distribution): vcpkg_installed\x64-windows\tools\python3\"
