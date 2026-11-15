param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$Clean,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/windows"
$vcpkgRoot = Join-Path $repoRoot ".tools/vcpkg"
$vcpkgInstalled = Join-Path $repoRoot ".tools/vcpkg-installed"
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio with the C++ workload."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio C++ toolchain was found."
}
$vsVersion = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationVersion
$vsMajor = [Version]$vsVersion
$generator = switch ([int]$vsMajor.Major) {
    18 { "Visual Studio 18 2026" }
    17 { "Visual Studio 17 2022" }
    default {
        throw "Unsupported Visual Studio major version: $vsVersion"
    }
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

# Prefer English diagnostics when the toolchain honors VSLANG. Dependency
# tracking itself is provided by MSBuild, so localized /showIncludes output
# does not affect incremental builds.
$env:VSLANG = "1033"

if (-not (Test-Path $vcpkgToolchain)) {
    throw "vcpkg was not found at $vcpkgRoot. Install it under .tools/vcpkg first."
}
$env:VCPKG_ROOT = $vcpkgRoot
$env:VCPKG_DOWNLOADS = Join-Path $repoRoot ".tools/vcpkg-downloads"

$cmakeCandidates = @(
    (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
    (Join-Path $vsPath "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
) | Where-Object { $_ -and (Test-Path $_) }

if (-not $cmakeCandidates) {
    throw "CMake 3.28 or newer was not found."
}

$cmake = $cmakeCandidates[0]
$cacheFile = Join-Path $buildDir "CMakeCache.txt"

if ($Clean -and (Test-Path $buildDir)) {
    $resolvedBuild = (Resolve-Path -LiteralPath $buildDir).Path
    $resolvedRoot = (Resolve-Path -LiteralPath $repoRoot).Path
    if (-not $resolvedBuild.StartsWith(
            $resolvedRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a build directory outside the repository."
    }
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

$fresh = $false
if (Test-Path $cacheFile) {
    $cachedGenerator = Select-String `
        -Path $cacheFile `
        -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' |
        Select-Object -First 1
    if ($cachedGenerator -and
        $cachedGenerator.Matches[0].Groups[1].Value -ne $generator) {
        $fresh = $true
    }
}

$configureArguments = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", $generator,
    "-A", "x64",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DVCPKG_INSTALLED_DIR=$vcpkgInstalled",
    "-DVCPKG_MANIFEST_MODE=ON",
    "-DIRIS_BUILD_CLI=ON",
    "-DIRIS_BUILD_TESTS=ON"
)
if ($fresh) {
    $configureArguments = @("--fresh") + $configureArguments
}

& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($RunTests) {
    & $cmake -E chdir $buildDir ctest -C $Configuration --output-on-failure
    exit $LASTEXITCODE
}

Write-Host "Built: $(Join-Path $buildDir 'iris-cli.exe')"

