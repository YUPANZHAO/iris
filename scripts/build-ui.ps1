param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$MediaAddress = "auto",
    [switch]$Check,
    [switch]$SkipCoreBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $repoRoot "apps/iris-ui"
$bridgeDir = Join-Path $repoRoot "build/windows/Release"
$nsh = Join-Path $env:LOCALAPPDATA "nsh/bin/nsh.exe"
$cargoBin = Join-Path $env:USERPROFILE ".cargo/bin"

if (-not (Test-Path -LiteralPath $nsh)) {
    throw "nsh was not found. Run the NSH Windows installer first."
}

if (-not $SkipCoreBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$bridge = Join-Path $bridgeDir "iris_bridge.dll"
if (-not (Test-Path -LiteralPath $bridge)) {
    throw "iris_bridge.dll was not built: $bridge"
}

$env:Path = "$cargoBin;$bridgeDir;$env:Path"
$env:IRIS_MEDIA_ADDRESS = $MediaAddress

if ($Check) {
    & $nsh check $appDir
    exit $LASTEXITCODE
}

& $nsh run $appDir
exit $LASTEXITCODE
