# Local virtual AMPS-style switch for Simonulator firmware testing.
# It transports high-level registration and call state only; it does not model
# the physical RF layer or carry voice audio.

param(
    [int]$Port = 5555,
    [int]$FirstNumber = 1001,
    [ValidateSet('HOME1','HOME2','HOME3','HOME4','ROAM','ALTROAM','OFFLINE','NOSERVICE')]
    [string]$DefaultRegistration = 'HOME1',
    [ValidateRange(0, 6)]
    [int]$DefaultSignal = 6,
    [string]$DefaultOperator = 'Virtual AMPS',
    [switch]$TraceRF,
    [switch]$Headless
)

$ErrorActionPreference = 'Stop'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
$clients = @{}
$nextNumber = $FirstNumber
$running = $true

function Send-Line([object]$Client, [string]$Line) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`n")
    $Client.Stream.Write($bytes, 0, $bytes.Length)
    $Client.Stream.Flush()
}

function Test-ClientClosed([object]$Client) {
    # NetworkStream.DataAvailable remains false both for an idle connection
    # and for a socket whose peer has closed.  Poll/Available distinguishes
    # those cases so stale Simon entries cannot survive until a later call and
    # make a failed write to the old peer look like the answering unit died.
    $socket = $Client.Tcp.Client
    return $socket.Poll(0, [System.Net.Sockets.SelectMode]::SelectRead) -and $socket.Available -eq 0
}

function Send-Profile([object]$Client) {
	Send-Line $Client "V $($Client.Number)"
    Send-Line $Client "S $($Client.Registration) $($Client.Signal) $($Client.Operator)"
}

function Disconnect-Call([object]$Client, [switch]$NotifyClient) {
    $peerNumber = $Client.Peer
    if ($NotifyClient) { Send-Line $Client 'H' }
    $Client.Peer = $null

    if ($null -ne $peerNumber -and $clients.ContainsKey([int]$peerNumber)) {
        $peer = $clients[[int]$peerNumber]
        Send-Line $peer 'H'
        $peer.Peer = $null
        Write-Host "Call ended: $($Client.Number) <-> $peerNumber"
    }
}

function Route-Line([object]$Source, [string]$Line) {
    if ($Line -match '^T\s+[0-9A-Fa-f]{2}$') {
        if ($TraceRF) { Write-Host "[$($Source.Number)] $Line" }
        return
    }
    Write-Host "[$($Source.Number)] $Line"
    if ($Line -eq 'N') {
        Send-Profile $Source
        Write-Host "RF deck ready: $($Source.Number); profile sent ($($Source.Registration), signal $($Source.Signal), $($Source.Operator))"
    }
    elseif ($Line -match '^P\s+') {
        Write-Host "Profile acknowledged by Simon $($Source.Number)"
    }
    elseif ($Line -match '^O\s+([0-9*#]+)$') {
        $targetNumber = [int]$Matches[1]
        if ($clients.ContainsKey($targetNumber)) {
            $target = $clients[$targetNumber]
            Send-Line $target "R $($Source.Number)"
            $Source.Peer = $targetNumber
            $target.Peer = $Source.Number
            Write-Host "Call: $($Source.Number) -> $targetNumber (ringing)"
        }
        else {
            Send-Line $Source 'I 83'
            Write-Warning "Number $targetNumber is not connected"
        }
    }
    elseif ($Line -eq 'A' -and $null -ne $Source.Peer) {
        $peerNumber = [int]$Source.Peer
        if ($clients.ContainsKey($peerNumber)) {
            $peer = $clients[$peerNumber]
            Send-Line $Source 'C'
            Send-Line $peer 'C'
            Write-Host "Call connected: $($Source.Number) <-> $peerNumber"
        }
    }
    elseif ($Line -eq 'H' -and $null -ne $Source.Peer) {
        Disconnect-Call $Source
    }
}

#控制台头输出
Write-Host "Simon virtual AMPS switch listening on 127.0.0.1:$Port"
Write-Host "Repo:https://github.com/INTERINIT/Simonulator"
Write-Host "Commands: list | set <number> <home1..home4|roam|altroam|offline> <signal 0..6> [operator]"
Write-Host "          ring <from> <to> | connect <number1> <number2> | hang <number> | clear | quit"
Write-Host "For detailed usage instructions, please see the repository readme.md"
Write-Host "中文使用方法请阅读存储库中的readme_ch.md"

try {
    while ($running) {
        while ($listener.Pending()) {
            $tcp = $listener.AcceptTcpClient()
            $tcp.NoDelay = $true
            $client = [pscustomobject]@{
                Number = $nextNumber
                Tcp = $tcp
                Stream = $tcp.GetStream()
                Buffer = ''
                Peer = $null
                Registration = $DefaultRegistration
                Signal = $DefaultSignal
                Operator = $DefaultOperator
            }
            $clients[$nextNumber] = $client
            Write-Host "Simon connected; virtual number: $nextNumber"
            Send-Profile $client
            $nextNumber++
        }

        foreach ($number in @($clients.Keys)) {
            $client = $clients[$number]
            try {
                if (Test-ClientClosed $client) { throw 'Remote socket closed' }
                while ($client.Stream.DataAvailable) {
                    $buffer = New-Object byte[] 512
                    $count = $client.Stream.Read($buffer, 0, $buffer.Length)
                    if ($count -le 0) { throw 'Connection closed' }
                    $client.Buffer += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $count)
                }
                while ($client.Buffer.Contains("`n")) {
                    $split = $client.Buffer.IndexOf("`n")
                    $line = $client.Buffer.Substring(0, $split).Trim()
                    $client.Buffer = $client.Buffer.Substring($split + 1)
                    if ($line) { Route-Line $client $line }
                }
            }
            catch {
                $reason = $_.Exception.Message
                Write-Warning "Simon $number disconnected: $reason"
                $peerNumber = $client.Peer
                $client.Tcp.Dispose()
                $clients.Remove($number)
                if ($null -ne $peerNumber -and $clients.ContainsKey([int]$peerNumber)) {
                    $peer = $clients[[int]$peerNumber]
                    $peer.Peer = $null
                    try { Send-Line $peer 'H' } catch { }
                }
            }
        }

        if (-not $Headless -and [Console]::KeyAvailable) {
            $command = (Read-Host).Trim()
            if ($command -eq 'list') {
                $clients.Values | Sort-Object Number | Format-Table Number, Peer, Registration, Signal, Operator
            }
            elseif ($command -eq 'clear' -or $command -eq 'cls') {
                Clear-Host
                Write-Host "Simon virtual 1G switch listening on 127.0.0.1:$Port"
                Write-Host "Connected Simon units: $($clients.Count)"
            }
            elseif ($command -match '^ring\s+(\d+)\s+(\d+)$') {
                $from = [int]$Matches[1]; $to = [int]$Matches[2]
                if ($clients.ContainsKey($from) -and $clients.ContainsKey($to)) {
                    $source = $clients[$from]; $target = $clients[$to]
                    $source.Peer = $to; $target.Peer = $from
                    Send-Line $target "R $from"
                    Write-Host "Call: $from -> $to (ringing)"
                }
                else { Write-Warning 'Both Simon numbers must be connected' }
            }
            elseif ($command -match '^connect\s+(\d+)\s+(\d+)$') {
                $a = [int]$Matches[1]; $b = [int]$Matches[2]
                if ($clients.ContainsKey($a) -and $clients.ContainsKey($b)) {
                    Send-Line $clients[$a] 'C'; Send-Line $clients[$b] 'C'
                    $clients[$a].Peer = $b; $clients[$b].Peer = $a
                }
            }
            elseif ($command -match '^hang\s+(\d+)$') {
                $number = [int]$Matches[1]
                if ($clients.ContainsKey($number)) { Disconnect-Call $clients[$number] -NotifyClient }
            }
            elseif ($command -match '^set\s+(\d+)\s+(home1|home2|home3|home4|roam|altroam|offline|noservice)\s+([0-6])(?:\s+(.+))?$') {
                $number = [int]$Matches[1]
                if ($clients.ContainsKey($number)) {
                    $client = $clients[$number]
                    $client.Registration = $Matches[2].ToUpperInvariant()
                    $client.Signal = [int]$Matches[3]
                    if ($Matches[4]) { $client.Operator = $Matches[4].Trim() }
                    Send-Profile $client
                    Write-Host "Profile $number`: $($client.Registration), signal $($client.Signal), operator $($client.Operator)"
                }
                else { Write-Warning "Number $number is not connected" }
            }
            elseif ($command -eq 'quit') { $running = $false }
        }
        Start-Sleep -Milliseconds 20
    }
}
finally {
    foreach ($client in $clients.Values) { $client.Tcp.Dispose() }
    $listener.Stop()
}
