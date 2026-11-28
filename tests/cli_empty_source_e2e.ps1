param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath,
    [switch]$AudioOnly
)

$ErrorActionPreference = "Stop"
$CliPath = (Resolve-Path -LiteralPath $CliPath).Path

function Get-FreeUdpPort {
    $socket = [Net.Sockets.UdpClient]::new(
        0, [Net.Sockets.AddressFamily]::InterNetwork)
    try {
        return ([Net.IPEndPoint]$socket.Client.LocalEndPoint).Port
    } finally {
        $socket.Dispose()
    }
}

function Start-Iris([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $CliPath
    $info.Arguments = ($Arguments | ForEach-Object {
        '"' + ($_ -replace '"', '\"') + '"'
    }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    [void]$process.Start()
    return [pscustomobject]@{
        Process = $process
        StdoutTask = $process.StandardOutput.ReadToEndAsync()
        StderrTask = $process.StandardError.ReadToEndAsync()
    }
}

function Read-Output($Endpoint) {
    $Endpoint.Process.StandardInput.Close()
    if (-not $Endpoint.Process.WaitForExit(10000)) {
        throw "iris-cli process $($Endpoint.Process.Id) did not stop"
    }
    return [pscustomobject]@{
        Stdout = $Endpoint.StdoutTask.GetAwaiter().GetResult()
        Stderr = $Endpoint.StderrTask.GetAwaiter().GetResult()
        ExitCode = $Endpoint.Process.ExitCode
    }
}

function Get-Tail([string]$Text, [int]$Length = 8000) {
    if ($Text.Length -le $Length) {
        return $Text
    }
    return $Text.Substring($Text.Length - $Length)
}

$emptyPort = Get-FreeUdpPort
do {
    $lavfiPort = Get-FreeUdpPort
} while ($lavfiPort -eq $emptyPort)

$empty = $null
$lavfi = $null
try {
    $emptyArgs = @(
        "--sip-port", "$emptyPort",
        "--media-address", "127.0.0.1",
        "--audio-input", "null",
        "--video-input", "null",
        "--video-width", "320",
        "--video-height", "180",
        "--video-fps", "15",
        "--auto-answer",
        "--log-level", "3"
    )
    $lavfiArgs = @(
        "--sip-port", "$lavfiPort",
        "--media-address", "127.0.0.1",
        "--audio-input", "null",
        "--video-input", "null",
        "--video-width", "320",
        "--video-height", "180",
        "--video-fps", "15",
        "--log-level", "3"
    )
    if ($AudioOnly) {
        $emptyArgs += "--no-video"
        $lavfiArgs += "--no-video"
    }
    $empty = Start-Iris $emptyArgs
    $lavfi = Start-Iris $lavfiArgs

    Start-Sleep -Milliseconds 900
    $empty.Process.StandardInput.WriteLine(
        "account add sip:empty@127.0.0.1:$emptyPort")
    $lavfi.Process.StandardInput.WriteLine(
        "account add sip:lavfi@127.0.0.1:$lavfiPort")
    Start-Sleep -Seconds 1

    $lavfi.Process.StandardInput.WriteLine(
        "call start account-1 sip:empty@127.0.0.1:$emptyPort")
    Start-Sleep -Seconds 5
    $lavfi.Process.StandardInput.WriteLine("call list")
    Start-Sleep -Seconds 2
    $empty.Process.StandardInput.WriteLine("media stats call-1")
    $lavfi.Process.StandardInput.WriteLine("media stats call-1")
    Start-Sleep -Milliseconds 800
    $lavfi.Process.StandardInput.WriteLine("call hangup call-1")
    Start-Sleep -Milliseconds 700
    $empty.Process.StandardInput.WriteLine("quit")
    $lavfi.Process.StandardInput.WriteLine("quit")

    $emptyResult = Read-Output $empty
    $lavfiResult = Read-Output $lavfi

    $results = @(
        [pscustomobject]@{ Name = "empty"; Output = $emptyResult },
        [pscustomobject]@{ Name = "lavfi"; Output = $lavfiResult }
    )
    foreach ($entry in $results) {
        $result = $entry.Output
        if ($result.ExitCode -ne 0) {
            throw "iris-cli exited with $($result.ExitCode):`n$($result.Stderr)"
        }
        if (-not ($result.Stdout -match 'audio-frames=[1-9][0-9]*')) {
            throw "No remote audio frames were decoded ($($entry.Name)):`nempty:`n$(Get-Tail $emptyResult.Stdout)`n$(Get-Tail $emptyResult.Stderr)`nlavfi:`n$(Get-Tail $lavfiResult.Stdout)`n$(Get-Tail $lavfiResult.Stderr)"
        }
        if (-not $AudioOnly -and
            -not ($result.Stdout -match 'video-frames=[1-9][0-9]*')) {
            throw "No remote video frames were decoded ($($entry.Name)):`nempty:`n$(Get-Tail $emptyResult.Stdout)`n$(Get-Tail $emptyResult.Stderr)`nlavfi:`n$(Get-Tail $lavfiResult.Stdout)`n$(Get-Tail $lavfiResult.Stderr)"
        }
        if (-not ($result.Stdout -match 'send-errors=0')) {
            throw "Media send errors occurred:`n$(Get-Tail $result.Stdout)`n$(Get-Tail $result.Stderr)"
        }
    }
    if (-not $emptyResult.Stdout.Contains('Auto-answered call-1')) {
        throw "Empty-source endpoint did not answer the call"
    }
    if (-not $lavfiResult.Stdout.Contains('Started call call-1')) {
        throw "lavfi endpoint did not start the call"
    }

Write-Output "Empty-source direct SIP call passed"
} finally {
    if ($empty -and -not $empty.Process.HasExited) {
        $empty.Process.Kill()
    }
    if ($lavfi -and -not $lavfi.Process.HasExited) {
        $lavfi.Process.Kill()
    }
}
