param(
    [string]$MediaAddress = "auto"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $repoRoot "apps/iris-ui"
$releaseDir = Join-Path $repoRoot "build/windows/Release"
$packageDir = Join-Path $appDir ".nsh/out/Iris"
$nsh = Join-Path $env:LOCALAPPDATA "nsh/bin/nsh.exe"
$cargoBin = Join-Path $env:USERPROFILE ".cargo/bin"

if (-not (Test-Path -LiteralPath $nsh)) {
    throw "nsh was not found. Run the NSH Windows installer first."
}

& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$env:Path = "$cargoBin;$releaseDir;$env:Path"
$env:IRIS_MEDIA_ADDRESS = $MediaAddress
Push-Location $appDir
try {
    & $nsh package --target windows
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $packageDir)) {
    throw "nsh package did not create $packageDir"
}

$runtimePatterns = @(
    "avcodec-*.dll",
    "avdevice-*.dll",
    "avfilter-*.dll",
    "avformat-*.dll",
    "avutil-*.dll",
    "swresample-*.dll",
    "swscale-*.dll",
    "libssl-*.dll",
    "libcrypto-*.dll",
    "openh264-*.dll",
    "opus.dll",
    "iris_bridge.dll"
)
foreach ($pattern in $runtimePatterns) {
    Get-ChildItem -LiteralPath $releaseDir -Filter $pattern |
        Copy-Item -Destination $packageDir -Force
}

$zipPath = Join-Path $appDir ".nsh/out/Iris.zip"
Compress-Archive -Path (Join-Path $packageDir "*") `
    -DestinationPath $zipPath -CompressionLevel Optimal -Force

Write-Host "Packaged: $packageDir"
Write-Host "Archive: $zipPath"
