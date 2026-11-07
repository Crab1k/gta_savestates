# Usage: run.ps1 <query-file> <out-file>   (read-only, on the analysed 1.0 US program of gta_sa_opcodes)
param([string]$In, [string]$Out)
$gso = "E:\Claude Projects\gta_sa_opcodes"
& "$gso\scripts\headless.ps1" "$gso\ghidra_proj" gta_sa -process gta_sa_10.exe -noanalysis -readOnly -scriptPath "$PSScriptRoot" -postScript Decomp.java $In $Out 2>&1 | Select-String -Pattern "ERROR|Exception|SCRIPT" | Select-Object -First 20
