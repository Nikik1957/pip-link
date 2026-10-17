# pb_install.ps1 - upload a local file to the Pip-Boy SD card over the Espruino REPL.
# Recipe that survives the 2 KB RX buffer: echo(0), ~1200-char base64 chunks, each
# chunk ends with a print() sentinel we wait for, then verify size and retry on mismatch.
#   .\pb_install.ps1 -LocalFile ..\pipboy\TERM\APP.js -RemotePath HOLO/TERM/APP.js
param(
  [string]$Port = "COM6",
  [int]$Baud = 115200,
  [Parameter(Mandatory=$true)][string]$LocalFile,
  [Parameter(Mandatory=$true)][string]$RemotePath,
  [int]$Chunk = 1200,
  [int]$Retries = 3
)
$ErrorActionPreference = "Stop"
$Q = [string][char]34   # literal double quote; avoids -f choking on braces

$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $LocalFile))
$b64 = [Convert]::ToBase64String($bytes)
if ($Chunk % 4 -ne 0) { $Chunk = [math]::Floor($Chunk / 4) * 4 }
$chunks = @()
for ($i = 0; $i -lt $b64.Length; $i += $Chunk) { $chunks += $b64.Substring($i, [math]::Min($Chunk, $b64.Length - $i)) }

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 500; $sp.WriteTimeout = 8000
$sp.DtrEnable = $true; $sp.RtsEnable = $true

function Drain() { Start-Sleep -Milliseconds 60; if ($sp.BytesToRead -gt 0) { [void]$sp.ReadExisting() } }
function SendWait($cmd, $needle, $maxMs) {
  $sp.Write($cmd + "`n")
  $buf = ""; $start = Get-Date
  while (((Get-Date) - $start).TotalMilliseconds -lt $maxMs) {
    if ($sp.BytesToRead -gt 0) { $buf += $sp.ReadExisting() } else { Start-Sleep -Milliseconds 12 }
    if ($buf.Contains($needle)) { return $buf }
  }
  return $null
}

$fs = 'require(' + $Q + 'fs' + $Q + ')'
$success = $false
try {
  $sp.Open(); Start-Sleep -Milliseconds 250; Drain
  $sp.Write("`n"); Start-Sleep -Milliseconds 300; Drain       # prime
  [void](SendWait "echo(0)" "" 300); Drain

  # make parent directories (ignore "exists")
  $parts = $RemotePath.Split('/')
  for ($d = 1; $d -lt $parts.Count; $d++) {
    $dir = ($parts[0..($d - 1)] -join '/')
    [void](SendWait ('try{' + $fs + '.mkdirSync(' + $Q + $dir + $Q + ')}catch(e){}print(' + $Q + 'MK' + $Q + ')') "MK" 2000)
  }

  for ($attempt = 1; $attempt -le $Retries -and -not $success; $attempt++) {
    Drain
    $ok = $true
    for ($i = 0; $i -lt $chunks.Count; $i++) {
      $fn = if ($i -eq 0) { "writeFileSync" } else { "appendFileSync" }
      $tag = "@" + $i + "#"
      $cmd = $fs + '.' + $fn + '(' + $Q + $RemotePath + $Q + ',atob(' + $Q + $chunks[$i] + $Q + '));print(' + $Q + $tag + $Q + ')'
      if ($null -eq (SendWait $cmd $tag 8000)) { $ok = $false; break }
      Start-Sleep -Milliseconds 15
    }
    Drain
    $szResp = SendWait ('print(' + $Q + 'SZ=' + $Q + '+' + $fs + '.statSync(' + $Q + $RemotePath + $Q + ').size)') "SZ=" 3000
    $devSize = -1
    if ($szResp -and $szResp -match "SZ=(\d+)") { $devSize = [int]$Matches[1] }
    if ($ok -and $devSize -eq $bytes.Length) {
      $success = $true
      Write-Output ("OK    " + $RemotePath + "  " + $bytes.Length + " bytes  (" + $chunks.Count + " chunks, attempt " + $attempt + ")")
    } else {
      Write-Output ("RETRY " + $RemotePath + " attempt " + $attempt + ": ok=" + $ok + " devSize=" + $devSize + " expected=" + $bytes.Length)
      [void](SendWait ('try{' + $fs + '.unlinkSync(' + $Q + $RemotePath + $Q + ')}catch(e){}print(' + $Q + 'DEL' + $Q + ')') "DEL" 2000)
      Start-Sleep -Milliseconds 250
    }
  }
} finally {
  if ($sp.IsOpen) { try { $sp.Write("echo(1)`n") } catch {}; Start-Sleep -Milliseconds 60; $sp.Close() }
}
if (-not $success) { Write-Output ("FAILED " + $RemotePath); exit 1 }
