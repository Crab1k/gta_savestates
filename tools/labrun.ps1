# Copy the fresh build into the lab launcher folder, start the lab game through GTASA-Savestates.exe (the game folder
# itself stays untouched), wait for the autotest to finish (or a timeout / crash), then print the log tail.
# Usage: labrun.ps1 [-Seconds 240] [-Keep]   (-Keep leaves the game running)
param([int]$Seconds = 240, [switch]$Keep)
$root = Split-Path -Parent $PSScriptRoot
$lab = "$root\lab\GTASA"
$ls = "$root\lab\launcher"
Copy-Item "$root\build\savestate.dll", "$root\build\GTASA-Savestates.exe" $ls -Force
Remove-Item "$ls\savestate.log" -ErrorAction SilentlyContinue
$game = (Resolve-Path "$lab\gta_sa.exe").Path
$launcher = Start-Process -FilePath "$ls\GTASA-Savestates.exe" -WorkingDirectory $ls -PassThru
$p = $null
for ($i = 0; $i -lt 60 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 250
    $p = Get-Process gta_sa -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $game } | Select-Object -First 1
}
if (-not $p) { "game did not start (launcher exit code $($launcher.ExitCode))"; return }
$null = $p.Handle   # keep a handle open so the exit code can be read later
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
