# Savestates across game restarts, in the lab copy: run 1 saves two slots in mission A of savefile 3 and quits;
# run 2 (a new process, savefile 1, mission B) loads them from disk, saves and reloads, loads savefile 4 through the
# menu and loads again. Each load is checked against what run 1 recorded (position, game clock, running scripts).
# Both runs start the untouched lab game through GTASA-Savestates.exe (lab\launcher).
#   persisttest.ps1 [-Version 10|101] [-PadEnvironment]
#   -PadEnvironment: start run 2 with a large extra environment variable, so the new process's early heap layout
#                    (CRT environment copy, per-thread data, file table) differs from run 1's.
param([string]$Version = "10", [switch]$PadEnvironment)
$root = Split-Path -Parent $PSScriptRoot
$lab = "$root\lab\GTASA"
$ls = "$root\lab\launcher"
$ini = "$ls\savestate.ini"

function Set-Ini($k, $v) {
    $c = Get-Content $ini
    if ($c -match "^$k=") { $c = $c -replace "^$k=.*", "$k=$v" } else { $c += "$k=$v" }
    $c | Set-Content $ini -Encoding ascii
}

Copy-Item "$lab\gta_sa_$Version.exe" "$lab\gta_sa.exe" -Force
Set-Ini AutoTest 1; Set-Ini AutoTestMode 5; Set-Ini AutoTestQuit 1; Set-Ini ForceRecreate 0; Set-Ini Census 0
Set-Ini MissionA 16; Set-Ini MissionB 23; Set-Ini SavefileC 4
Remove-Item "$ls\savestates" -Recurse -ErrorAction SilentlyContinue
Remove-Item "$lab\lab_marks.txt", "$lab\shots\*.png" -ErrorAction SilentlyContinue

"== run 1 (gta_sa $Version)"
Set-Ini PersistPhase 1; Set-Ini AutoLoadSlot 3
& "$PSScriptRoot\labrun.ps1" -Seconds 150 | Select-Object -First 2
Select-String -Path "$ls\savestate.log" -Pattern "save slot|CRASH|DISABLED|not usable" | ForEach-Object { $_.Line.Substring(0, [Math]::Min(160, $_.Line.Length)) }
Copy-Item "$ls\savestate.log" "$ls\savestate_run1.log" -Force

"== run 2" + $(if ($PadEnvironment) { " (padded environment)" } else { "" })
Set-Ini PersistPhase 2; Set-Ini AutoLoadSlot 1
if ($PadEnvironment) { $env:SAVESTATE_LAB_PAD = "x" * 6000 }
& "$PSScriptRoot\labrun.ps1" -Seconds 200 | Select-Object -First 2
Remove-Item Env:\SAVESTATE_LAB_PAD -ErrorAction SilentlyContinue
Select-String -Path "$ls\savestate.log" -Pattern "TEST p2|TEST done|CRASH|held locks|HEAP|DISABLED|cannot use|load slot" | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
Copy-Item "$lab\gta_sa_10.exe" "$lab\gta_sa.exe" -Force
