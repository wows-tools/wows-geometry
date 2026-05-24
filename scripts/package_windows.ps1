#Requires -Version 5.1
<#
.SYNOPSIS
    Collect Release binaries and the vcpkg Python runtime into dist/ and zip them.
#>
param(
    [string] $BuildDir = "build",
    [string] $Config   = "Release",
    [string] $OutZip   = "wows-model-exporter-windows-x86_64.zip"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$BuildDir = Join-Path $RepoRoot $BuildDir
$BinDir = Join-Path $BuildDir $Config
$Dist = Join-Path $RepoRoot "dist"

if (-not (Test-Path $BinDir)) {
    Write-Error "Build output not found: $BinDir"
}

# Locate Python runtime: prefer vcpkg-installed copy, fall back to system Python.
# The release archive bundles this Python runtime so the executables can auto-discover
# it without requiring PYTHONHOME or PYTHONPATH to be manually set.
$PyRoot = Join-Path $RepoRoot "vcpkg_installed\x64-windows\tools\python3"
if (-not (Test-Path $PyRoot)) {
    Write-Host "vcpkg Python not found at $PyRoot — searching system Python..."
    $PythonExe = $null
    foreach ($cmd in @('python', 'python3')) {
        $found = Get-Command $cmd -ErrorAction SilentlyContinue
        if ($found) { $PythonExe = $found.Source; break }
    }
    if (-not $PythonExe) {
        Write-Error "Python not found: neither vcpkg nor a system Python could be located"
    }
    $PyRoot = & $PythonExe -c "import sys; print(sys.prefix)"
    Write-Host "Using system Python at $PyRoot"
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
Copy-Item "$BinDir\*.exe", "$BinDir\*.dll" $Dist
Copy-Item "$PyRoot\*.dll" $Dist
Copy-Item "$PyRoot\Lib" (Join-Path $Dist "Lib") -Recurse
if (Test-Path "$PyRoot\DLLs") {
    Copy-Item "$PyRoot\DLLs" (Join-Path $Dist "DLLs") -Recurse
}
Copy-Item (Join-Path $RepoRoot "LICENSE"), (Join-Path $RepoRoot "README.md") $Dist

if (Test-Path $OutZip) { Remove-Item $OutZip }
Compress-Archive -Path "$Dist\*" -DestinationPath (Join-Path $RepoRoot $OutZip)
Write-Host "Created $(Join-Path $RepoRoot $OutZip)"
