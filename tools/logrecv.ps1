# SPDX-License-Identifier: GPL-2.0-or-later
#
# Receptor del log UDP de gc-ps3 para Windows (PowerShell 5.1 o 7), sin instalar nada.
# Mismo formato y comportamiento que tools/logrecv.py: un archivo por ejecucion en logs\,
# descarta duplicados y cuenta lineas perdidas.
#
#   tools\logrecv.cmd                      escucha hasta Ctrl+C
#   tools\logrecv.cmd -UntilResult         termina con el RESULT: 0 OK, 1 FAIL, 2 tiempo agotado
#
# Sirve para RPCS3 (la app envia a 127.0.0.1) y para la consola en la red local.
param(
    [int]$Port = 18194,
    [string]$Out = '',
    [switch]$UntilResult,
    [int]$Timeout = 120
)

$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$re = '^GCPS3 ([0-9a-f]{8}) (\d+) (\d+) ([IWER]) ([a-z0-9_-]{1,31}) \| (.*)$'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$sessions = @{}

function Get-Summary($s) {
    $result = if ($s.Result) { $s.Result } else { 'sin RESULT' }
    $lost = $s.MaxSeq + 1 - $s.Seen.Count
    return "$($s.Test): $result | $($s.Seen.Count) lineas, $lost perdidas, $($s.Dups) duplicadas -> $($s.Path)"
}

$udp = New-Object System.Net.Sockets.UdpClient($Port)
$udp.Client.ReceiveTimeout = 500
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
$start = Get-Date
$tail = if ($UntilResult) { '' } else { ' (Ctrl+C para salir)' }
Write-Host "Escuchando el log de gc-ps3 en UDP $Port; logs en $Out$tail"

$code = 0
try {
    while ($true) {
        if ($UntilResult -and ((Get-Date) - $start).TotalSeconds -gt $Timeout) {
            Write-Host "Tiempo agotado: no llego ningun RESULT en $Timeout s"
            $code = 2
            break
        }
        try {
            $bytes = $udp.Receive([ref]$remote)
        } catch {
            # Timeout de 500 ms (Windows PowerShell 5.1 lo envuelve en MethodInvocationException)
            $e = $_.Exception
            if ($e -is [System.Net.Sockets.SocketException] -or $e.InnerException -is [System.Net.Sockets.SocketException]) { continue }
            throw
        }
        $line = $utf8.GetString($bytes).TrimEnd("`r", "`n")
        $m = [regex]::Match($line, $re)
        if (-not $m.Success) {
            Write-Host "[?] $($remote.Address): $line"
            continue
        }
        $sid = $m.Groups[1].Value
        $seq = [uint32]$m.Groups[2].Value
        $tus = [uint64]$m.Groups[3].Value
        $level = $m.Groups[4].Value
        $test = $m.Groups[5].Value
        $msg = $m.Groups[6].Value

        if (-not $sessions.ContainsKey($sid)) {
            $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
            $sessions[$sid] = [pscustomobject]@{
                Test   = $test
                Path   = Join-Path $Out "$($stamp)_$($test)_$($sid).log"
                Seen   = New-Object 'System.Collections.Generic.HashSet[uint32]'
                MaxSeq = -1
                Dups   = 0
                Result = $null
            }
            Write-Host "== nueva ejecucion de '$test' desde $($remote.Address) (sesion $sid)"
        }
        $s = $sessions[$sid]
        if (-not $s.Seen.Add($seq)) {
            $s.Dups++
            continue
        }
        if ([int64]$seq -gt $s.MaxSeq) { $s.MaxSeq = [int64]$seq }
        [System.IO.File]::AppendAllText($s.Path, $line + "`n", $utf8)

        $mark = ''
        if ($level -eq 'W') { $mark = 'AVISO ' } elseif ($level -eq 'E') { $mark = 'ERROR ' }
        Write-Host ('[{0} {1,8:F3}s] {2}{3}' -f $test, ($tus / 1e6), $mark, $msg)

        if ($level -eq 'R') {
            $s.Result = $msg
            Write-Host ('== ' + (Get-Summary $s))
            if ($UntilResult) {
                if ($msg -eq 'RESULT: OK') { $code = 0 } else { $code = 1 }
                break
            }
        }
    }
} finally {
    $udp.Close()
    if (-not $UntilResult) {
        foreach ($s in $sessions.Values) { Write-Host ('== ' + (Get-Summary $s)) }
    }
}
exit $code
