param(
    [ValidateSet("x64-windows", "x64-windows-static")]
    [string]$Triplet = "x64-windows"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $repoRoot ".tools"
$vcpkgRoot = Join-Path $toolsDir "vcpkg"
$installedRoot = Join-Path $toolsDir "vcpkg-installed"
$downloadsRoot = Join-Path $toolsDir "vcpkg-downloads"

New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null

if (-not (Test-Path (Join-Path $vcpkgRoot "vcpkg.exe"))) {
    if (-not (Test-Path $vcpkgRoot)) {
        git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkgRoot
    }
    & (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

& (Join-Path $vcpkgRoot "vcpkg.exe") install `
    "--triplet=$Triplet" `
    "--x-install-root=$installedRoot" `
    "--downloads-root=$downloadsRoot" `
    "--clean-after-build"

exit $LASTEXITCODE

