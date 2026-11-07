# Lab cross scenario (AutoTestMode 5) in three processes: crossrun3.ps1 -Game iii|vc -A 1 -B 2 -C 3 -MA 30 -MB 54 [-Dll x]
# Phase 1: savefile A + mission A, savestates, quit. Phase 2: savefile B + mission B, loads from disk, savefile C, ...
# Phase 3: new game, loads from both earlier processes. Never starts while any GTA game runs; the ini is restored.
param([string]$Game, [int]$A, [int]$B, [int]$C, [int]$MA, [int]$MB, [string]$Dll = "")
$root = "E:\Claude Projects\gta_sa_savestate"
$ini = "$root\lab\launcher_$Game\savestate.ini"
$bak = "$ini.crossbak"
Copy-Item $ini $bak -Force
try {
    foreach ($phase in 1, 2, 3) {
        $g = Get-Process | Where-Object { $_.ProcessName -match "^(gta|gta-vc|gta3|gta_sa|gta-sa)$" }
        if ($g) { "GAME RUNNING, phase $phase not started: " + ($g | ForEach-Object { "$($_.Id) $($_.Path)" }); return }
        python "$root\tools\labini.py" $ini AutoTest=3 AutoTestMode=5 AutoTestQuit=1 PersistPhase=$phase SavefileA=$A `
            SavefileB=$B SavefileC=$C MissionA=$MA MissionB=$MB
        "=== phase $phase"
        $args3 = @{ Game = $Game; Seconds = 300 }
        if ($Dll) { $args3.Dll = $Dll }
        & "$root\tools\labrun3.ps1" @args3 | Select-String -Pattern "PID|exited|killed|TEST|CRASH|load slot|ped comment|left as they are|cannot|audio: stream" |
            ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
        Copy-Item "$root\lab\launcher_$Game\savestate.log" "$root\lab\launcher_$Game\savestate_cross_p$phase.log" -Force
    }
} finally {
    Copy-Item $bak $ini -Force
    Remove-Item $bak
}
