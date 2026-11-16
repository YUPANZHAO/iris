function Resolve-NshExecutable {
    param(
        [string]$ExplicitPath
    )

    $candidates = [System.Collections.Generic.List[string]]::new()
    $candidates.Add($ExplicitPath)

    if ($env:NSH_HOME) {
        $candidates.Add((Join-Path $env:NSH_HOME "bin/nsh.exe"))
        $candidates.Add((Join-Path $env:NSH_HOME "nsh.exe"))
    }

    Get-Command nsh.exe, nsh -CommandType Application -ErrorAction SilentlyContinue |
        ForEach-Object { $candidates.Add($_.Source) }

    if ($env:LOCALAPPDATA) {
        $candidates.Add((Join-Path $env:LOCALAPPDATA "nsh/bin/nsh.exe"))
        $candidates.Add((Join-Path $env:LOCALAPPDATA "Microsoft/WindowsApps/nsh.exe"))

        $packagesDir = Join-Path $env:LOCALAPPDATA "Packages"
        if (Test-Path -LiteralPath $packagesDir) {
            Get-ChildItem -LiteralPath $packagesDir -Directory -ErrorAction SilentlyContinue |
                ForEach-Object {
                    $candidates.Add((Join-Path $_.FullName "LocalCache/Local/nsh/bin/nsh.exe"))
                }
        }
    }

    if ($env:USERPROFILE) {
        $candidates.Add((Join-Path $env:USERPROFILE "AppData/Local/nsh/bin/nsh.exe"))
        $candidates.Add((Join-Path $env:USERPROFILE ".cargo/bin/nsh.exe"))
    }

    $candidates.Add((Join-Path $env:ProgramFiles "nsh/bin/nsh.exe"))

    $searched = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    foreach ($candidate in $candidates) {
        if (-not $candidate) {
            continue
        }

        $fullPath = [IO.Path]::GetFullPath($candidate)
        if (-not $searched.Add($fullPath)) {
            continue
        }
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            return $fullPath
        }
    }

    $locations = ($searched | ForEach-Object { "  $_" }) -join [Environment]::NewLine
    throw @"
nsh was not found. Install NSH, add it to PATH, or pass -NshPath with the full path to nsh.exe.
Searched:
$locations
"@
}
