param(
    [int]$TestPort = 5655,
    [string]$Executable = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = if ($Executable) { (Resolve-Path -LiteralPath $Executable).Path } else { Join-Path $root 'Simon_AMPS_Switch.exe' }
$testDirectory = Join-Path $root ('test-output\' + $TestPort + '-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $testDirectory -Force)
$process = Start-Process -FilePath $exe -ArgumentList '--port', $TestPort, '--data-dir', '.' -WorkingDirectory $testDirectory -WindowStyle Hidden -PassThru

function New-TestClient {
    $client = [System.Net.Sockets.TcpClient]::new()
    $client.Connect('127.0.0.1', $script:testPort)
    $client.NoDelay = $true
    return $client
}

function Send-Line($client, [string]$line) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($line + "`n")
    $client.GetStream().Write($bytes, 0, $bytes.Length)
}

function Drain($client, [int]$milliseconds = 300) {
    $until = [DateTime]::UtcNow.AddMilliseconds($milliseconds)
    $builder = [Text.StringBuilder]::new()
    $buffer = [byte[]]::new(8192)
    while ([DateTime]::UtcNow -lt $until) {
        $stream = $client.GetStream()
        while ($stream.DataAvailable) {
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if ($count -le 0) { break }
            [void]$builder.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $count))
        }
        Start-Sleep -Milliseconds 10
    }
    return $builder.ToString()
}

function Hex-Line([string]$text) {
    return 'M X ' + (($text.ToCharArray() | ForEach-Object { ([byte][char]$_).ToString('X2') }) -join '')
}

try {
    Start-Sleep -Milliseconds 500
    $a = New-TestClient
    $b = New-TestClient
    $null = Drain $a
    $null = Drain $b
    Send-Line $a 'N'
    Send-Line $b 'N'
    $null = Drain $a
    $null = Drain $b

    Send-Line $a 'O 1002'
    $caller = Drain $a 750
    $called = Drain $b 750
    if ($called -notmatch '(?m)^D 1 \d+ (5970|6000|6030)$' -or $called -notmatch '(?m)^R 1001$') {
        throw "AMPS page/assignment failed:`n$called"
    }
    if ($caller -notmatch '(?m)^Y [0-9A-F]+$') { throw 'Network ringback PCM was not delivered' }

    Send-Line $b 'A'
    $caller = Drain $a 250
    $called = Drain $b 250
    if ($caller -notmatch '(?m)^C$' -or $called -notmatch '(?m)^C$') { throw 'Conversation order missing' }

    # Send a real 770+1336 Hz digit 5 over the unsigned 8 kHz PCM bearer.
    for ($packet = 0; $packet -lt 60; $packet++) {
        $hex = [Text.StringBuilder]::new(32)
        for ($sample = 0; $sample -lt 16; $sample++) {
            $n = $packet * 16 + $sample
            $value = 128 + [Math]::Round(40 * [Math]::Sin(2 * [Math]::PI * 770 * $n / 8000) + 40 * [Math]::Sin(2 * [Math]::PI * 1336 * $n / 8000))
            [void]$hex.Append(([Math]::Max(0, [Math]::Min(255, $value))).ToString('X2'))
        }
        Send-Line $a "Y $hex"
    }
    $dtmf = Drain $b 300
    if ($dtmf -notmatch '(?m)^E DTMF 1001 5$') { throw "In-band DTMF detection failed: $dtmf" }

    Send-Line $a 'H'
    $null = Drain $a 150
    $hang = Drain $b 250
    if ($hang -notmatch '(?m)^H$') { throw 'Release order missing' }
    if ($hang -match '(?m)^[SV] ') { throw 'Call release incorrectly repeated registration' }

    # A page is a network-originated voice transaction.  The target is paged
    # independently and, after answering, receives #4242# as in-band DTMF.
    Send-Line $a 'Q 1002 4242'
    $pageAlert = Drain $b 750
    if ($pageAlert -notmatch '(?m)^R 5552000 P$') { throw "Pager network alert failed:`n$pageAlert" }
    Send-Line $b 'A'
    $pageAudio = Drain $b 4200
    if ($pageAudio -notmatch '(?m)^C$' -or $pageAudio -notmatch '(?m)^Y [0-9A-F]+$' -or $pageAudio -notmatch '(?m)^H$') {
        throw "Pager DTMF bearer/release failed:`n$pageAudio"
    }
    if ($pageAudio -match '(?m)^[SV] ') { throw 'Pager release incorrectly repeated registration' }

    $mailA = New-TestClient # 1003
    $mailB = New-TestClient # 1004
    $null = Drain $mailA
    $null = Drain $mailB
    foreach ($name in @('1003','1004')) {
        Send-Line $mailA 'M D 5551003'
        $null = Drain $mailA 100
        $body = "user=$name&password=TEST"
        Send-Line $mailA (Hex-Line ("POST http://simon.local/register HTTP/1.0`r`nContent-Type: application/x-www-form-urlencoded`r`nContent-Length: " + $body.Length + "`r`n`r`n" + $body))
        $null = Drain $mailA 1600
        Send-Line $mailA 'M H'
        $null = Drain $mailA 100
    }
    Send-Line $mailA 'M D 5551001'
    $greeting = Drain $mailA 300
    if ($greeting -notmatch 'M C 2400') { throw 'Mail handshake failed' }
    foreach ($line in @('USER 1003', 'PASS TEST', 'SEND', 'TO: 1004', 'SUBJECT: SWITCH TEST', '', 'HELLO SIMON', '.')) {
        Send-Line $mailA (Hex-Line ($line + "`r`n"))
        $null = Drain $mailA 80
    }
    Send-Line $mailB 'M D 5551001'
    $null = Drain $mailB 200
    Send-Line $mailB (Hex-Line "USER 1004`r`n")
    $null = Drain $mailB 120
    Send-Line $mailB (Hex-Line "PASS TEST`r`n")
    $null = Drain $mailB 120
    Send-Line $mailB (Hex-Line "LIST`r`n")
    $listing = Drain $mailB 300
    if ($listing -notmatch 'M X ' -or $listing -notmatch '5357495443482054455354') {
        throw "Mail relay/list failed:`n$listing"
    }

    Write-Host 'PASS: control channel, assignment, ringback PCM, connect, in-band DTMF, release, pager and mail relay'
}
finally {
    foreach ($client in @($a, $b, $mailA, $mailB)) { if ($null -ne $client) { $client.Dispose() } }
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
}
