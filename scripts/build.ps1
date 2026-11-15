param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$Clean,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/windows"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio with the C++ workload."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio C++ toolchain was found."
}

$vcvars = Join-Path $vsPath "VC/Auxiliary/Build/vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat was not found under $vsPath"
}

$envLines = & cmd.exe /d /s /c ('"' + $vcvars + '" >nul && set')
foreach ($line in $envLines) {
    $separator = $line.IndexOf("=")
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

# Keep compiler diagnostics and Ninja dependency parsing in one predictable
# language. Without this, localized /showIncludes output can flood builds on
# systems using a non-UTF-8 PowerShell code page.
$env:VSLANG = "1033"

$cmakeCandidates = @(
    (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
    (Join-Path $vsPath "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
) | Where-Object { $_ -and (Test-Path $_) }

if (-not $cmakeCandidates) {
    throw "CMake 3.28 or newer was not found."
}

$cmake = $cmakeCandidates[0]

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

& $cmake -S $repoRoot -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DIRIS_BUILD_CLI=ON" `
    "-DIRIS_BUILD_TESTS=ON"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($RunTests) {
    & $cmake -E chdir $buildDir ctest --output-on-failure
    exit $LASTEXITCODE
}

Write-Host "Built: $(Join-Path $buildDir 'iris-cli.exe')"

