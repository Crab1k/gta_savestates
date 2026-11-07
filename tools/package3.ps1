# Package the GTA III and Vice City savestates: dist3\<game> folders and GTA3-Savestates.zip / GTAVC-Savestates.zip in
# the project root. With -Install, also copy them to D:\GTA III Savestates and D:\GTA Vice City Savestates with the
# game paths filled in (an existing savestate.ini there is kept).
param([switch]$Install, [string[]]$Games = @("iii", "vc"))
$root = Split-Path -Parent $PSScriptRoot
$all = @(
    @{ Key = "iii"; Exe = "GTA3-Savestates.exe"; Readme = "README-III.md"; Zip = "GTA3-Savestates.zip"
       Target = "D:\GTA III Savestates"; Game = "D:\GTA3\gta3.exe" },
    @{ Key = "vc"; Exe = "GTAVC-Savestates.exe"; Readme = "README-VC.md"; Zip = "GTAVC-Savestates.zip"
       Target = "D:\GTA Vice City Savestates"; Game = "E:\Grand Theft Auto Vice City JP\gta-vc.exe" }
)
foreach ($g in ($all | Where-Object { $Games -contains $_.Key })) {
    $out = "$root\dist3\$($g.Key)"
    New-Item -ItemType Directory -Force $out | Out-Null
    Copy-Item "$root\build\$($g.Exe)" $out -Force
    Copy-Item "$root\build\savestate3.dll" "$out\savestate.dll" -Force
    Copy-Item "$root\dist3\savestate.ini" "$out\savestate.ini" -Force
    Copy-Item "$root\dist3\$($g.Readme)" "$out\README.md" -Force
    Compress-Archive -Path "$out\*" -DestinationPath "$root\$($g.Zip)" -Force
    "packaged $($g.Zip)"
    if ($Install) {
        New-Item -ItemType Directory -Force $g.Target | Out-Null
        Copy-Item "$out\$($g.Exe)", "$out\savestate.dll", "$out\README.md" $g.Target -Force
        $ini = Join-Path $g.Target "savestate.ini"
        if (-not (Test-Path $ini)) {
            (Get-Content "$out\savestate.ini") -replace '^Game=.*', "Game=$($g.Game)" | Set-Content $ini -Encoding ascii
        }
        "installed to $($g.Target)"
    }
}
