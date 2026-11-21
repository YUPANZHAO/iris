param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath
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
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $CliPath
    $info.Arguments = ($Arguments | ForEach-Object {
        '"' + ($_ -replace '"', '\"') + '"'
    }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true

    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    return $process
}

$serverPort = Get-FreeUdpPort
do {
    $clientPort = Get-FreeUdpPort
} while ($clientPort -eq $serverPort)

$common = @(
    "--media-address", "127.0.0.1",
    "--audio-input", "null",
    "--video-input", "null",
    "--no-video",
    "--no-audio-render",
    "--no-video-render",
    "--log-level", "3"
)

$server = $null
$client = $null
try {
    $server = Start-Iris (
        @("--sip-port", "$serverPort", "--auto-answer") + $common)
    $client = Start-Iris (@("--sip-port", "$clientPort") + $common)

    Start-Sleep -Milliseconds 800
    $server.StandardInput.WriteLine(
        "account add sip:bob@127.0.0.1:$serverPort")
    $client.StandardInput.WriteLine(
        "account add sip:alice@127.0.0.1:$clientPort")
    Start-Sleep -Seconds 1

    $client.StandardInput.WriteLine(
        "call start account-1 sip:bob@127.0.0.1:$serverPort")
    Start-Sleep -Seconds 3
    $client.StandardInput.WriteLine("call hold call-1")
    Start-Sleep -Seconds 1
    $client.StandardInput.WriteLine("call resume call-1")
    Start-Sleep -Seconds 2
    $client.StandardInput.WriteLine("media stats call-1")
    Start-Sleep -Milliseconds 500
    $client.StandardInput.WriteLine("call hangup call-1")
    Start-Sleep -Milliseconds 700

    $client.StandardInput.WriteLine("quit")
    $server.StandardInput.WriteLine("quit")
    $client.StandardInput.Close()
    $server.StandardInput.Close()

    if (-not $client.WaitForExit(7000)) {
        throw "iris client did not stop within 7 seconds"
    }
    if (-not $server.WaitForExit(7000)) {
        throw "iris server did not stop within 7 seconds"
    }

    $clientOutput = $client.StandardOutput.ReadToEnd()
    $clientError = $client.StandardError.ReadToEnd()
    $serverOutput = $server.StandardOutput.ReadToEnd()
    $serverError = $server.StandardError.ReadToEnd()

    $requiredClientOutput = @(
        'Started call call-1',
        'state=confirmed',
        'reason="locally held"',
        'reason="resumed"',
        'send-errors=0'
    )
    foreach ($expected in $requiredClientOutput) {
        if (-not $clientOutput.Contains($expected)) {
            throw "client output is missing: $expected`n$clientOutput`n$clientError`nserver:`n$serverOutput`n$serverError"
        }
    }
    if (-not $serverOutput.Contains('Auto-answered call-1')) {
        throw "server did not auto-answer the call"
    }

    foreach ($diagnostic in @($clientError, $serverError)) {
        if ($diagnostic -match '(?i)\berror\b|\bfailed\b|invalid operation') {
            throw "iris process reported an error: $diagnostic"
        }
    }

    Write-Output "SIP call, hold/resume, and media transport passed"
} finally {
    if ($client -and -not $client.HasExited) {
        $client.Kill()
    }
    if ($server -and -not $server.HasExited) {
        $server.Kill()
    }
}
