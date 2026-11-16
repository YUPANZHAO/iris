param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$MediaAddress = "auto",
    [string]$NshPath,
    [switch]$Check,
    [switch]$SkipCoreBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $repoRoot "apps/iris-ui"
$bridgeDir = Join-Path $repoRoot "build/windows/Release"
$cargoBin = Join-Path $env:USERPROFILE ".cargo/bin"

. (Join-Path $PSScriptRoot "resolve-nsh.ps1")
$nsh = Resolve-NshExecutable -ExplicitPath $NshPath

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
