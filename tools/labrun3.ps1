# Lab run for GTA III / Vice City: copy the fresh build into lab\launcher_<game>, start the untouched lab game through
# its launcher, wait for the autotest to finish (or a timeout / crash), then print the log tail.
# Usage: labrun3.ps1 -Game iii|vc [-Seconds 240] [-Keep] [-Dll <another savestate.dll, e.g. a released one>]
param([string]$Game = "iii", [int]$Seconds = 240, [switch]$Keep, [string]$Dll = "")
$root = Split-Path -Parent $PSScriptRoot
if ($Game -eq "vc") { $lab = "$root\lab\GTAVC"; $exe = "gta-vc.exe"; $launcher = "GTAVC-Savestates.exe"; $proc = "gta-vc" }
else { $lab = "$root\lab\GTA3"; $exe = "gta3.exe"; $launcher = "GTA3-Savestates.exe"; $proc = "gta3" }
$ls = "$root\lab\launcher_$Game"
Copy-Item $(if ($Dll) { $Dll } else { "$root\build\savestate3.dll" }) "$ls\savestate.dll" -Force
Copy-Item "$root\build\$launcher" $ls -Force
Remove-Item "$ls\savestate.log" -ErrorAction SilentlyContinue
$gamePath = (Resolve-Path "$lab\$exe").Path
$l = Start-Process -FilePath "$ls\$launcher" -WorkingDirectory $ls -PassThru
$p = $null
for ($i = 0; $i -lt 60 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 250
    $p = Get-Process $proc -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $gamePath } | Select-Object -First 1
}
if (-not $p) { "game did not start (launcher exit code $($l.ExitCode))"; return }
$null = $p.Handle
"PID $($p.Id)"
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Seconds 2
    if ((Test-Path "$ls\savestate.log") -and (Select-String -Path "$ls\savestate.log" -Pattern "TEST done" -Quiet)) { break }
}
Start-Sleep -Seconds 3
if (-not $p.HasExited) {
    if ($Keep) { "still running (kept)" } else { Stop-Process -Id $p.Id -Force; "killed after timeout/test end" }
} else { "exited with code $($p.ExitCode)" }
Get-Content "$ls\savestate.log" | Select-Object -Last 60
