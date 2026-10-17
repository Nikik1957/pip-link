# pb_probe.ps1 - send JS lines to the Pip-Boy's Espruino REPL and capture the output.
# Read-only by convention: never send reset()/Ctrl-C/flash writes from here.
#   .\pb_probe.ps1 -Cmd 'print(process.env.BOARD)'
#   .\pb_probe.ps1 -CmdFile cmds.txt -OutFile out.txt
param(
  [string]$Port = "COM6",
  [int]$Baud = 115200,
  [string[]]$Cmd,
  [string]$CmdFile,
  [string]$OutFile,
  [int]$IdleMs = 900,
  [int]$MaxMs = 8000
)
$ErrorActionPreference = "Stop"
$lines = @()
if ($CmdFile) { $lines += Get-Content $CmdFile }
if ($Cmd) { $lines += $Cmd }

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 500; $sp.WriteTimeout = 3000
$sp.DtrEnable = $true; $sp.RtsEnable = $true
$log = New-Object System.Text.StringBuilder

function Read-Idle($idleMs, $maxMs) {
  $sb = New-Object System.Text.StringBuilder
  $last = Get-Date; $start = Get-Date
  while ($true) {
    if ($sp.BytesToRead -gt 0) { [void]$sb.Append($sp.ReadExisting()); $last = Get-Date }
    else { Start-Sleep -Milliseconds 30 }
    if (((Get-Date) - $last).TotalMilliseconds -ge $idleMs) { break }
    if (((Get-Date) - $start).TotalMilliseconds -ge $maxMs) { break }
  }
  return $sb.ToString()
}

try {
  $sp.Open()
  Start-Sleep -Milliseconds 250
  if ($sp.BytesToRead -gt 0) { [void]$sp.ReadExisting() }
  $sp.Write("`n"); [void](Read-Idle 400 1500)          # prime: first line after open can be swallowed
  foreach ($line in $lines) {
    if ($line.Trim() -eq "") { continue }
    [void]$log.AppendLine("----- CMD: " + $line)
    $sp.Write($line + "`n")
    [void]$log.AppendLine((Read-Idle $IdleMs $MaxMs))
  }
} finally {
  if ($sp.IsOpen) { $sp.Close() }
}
$text = $log.ToString()
if ($OutFile) { Set-Content -Path $OutFile -Value $text -Encoding utf8 }
Write-Output $text
