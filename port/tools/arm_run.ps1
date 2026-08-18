# On-device Gate-S5c leg: push tj_headless + the game data to the phone and run the
# scripted MEAT detlog leg there. Companion to eng_run.ps1 (same env contract).
#
#   arm_run.ps1 -Adb 192.168.31.173:44019           first time: pushes assets (~232 MB)
#   arm_run.ps1 -Adb <ip:port> -SkipAssets          re-run: binary + env only
#   arm_run.ps1 ... -Seconds 2400 -Tag armA
#
# Output: /data/local/tmp/tj/<Tag>.det + .log, pulled back into %TEMP%\tj_eng_runs.
# Compare with:  python port\tools\det_diff.py %TEMP%\tj_eng_runs\exS27.det %TEMP%\tj_eng_runs\<Tag>.det
# (the Windows reference MUST be an -Exact engine leg - S5c is EXACT-vs-EXACT.)
param([string]$Adb = "", [string]$Tag = "armA", [int]$Seconds = 2400,
      [switch]$SkipAssets, [int]$Arena = 3)

$adbExe = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$root   = Split-Path (Split-Path $PSScriptRoot)
$bin    = "$root\port\build-arm64\tj_headless"
$assets = "$root\extracted"
$dev    = "/data/local/tmp/tj"
$out    = "$env:TEMP\tj_eng_runs"
New-Item -ItemType Directory -Force $out | Out-Null

if ($Adb -ne "") { & $adbExe connect $Adb }
$devs = & $adbExe devices | Select-String "device$"
if (-not $devs) { Write-Host "NO DEVICE - pair wireless adb first (Developer options -> Wireless debugging)"; exit 1 }

& $adbExe shell mkdir -p $dev
& $adbExe push $bin "$dev/tj_headless"
& $adbExe shell chmod 755 "$dev/tj_headless"
if (-not $SkipAssets) {
    Write-Host "pushing assets (~232 MB over wireless adb; several minutes)..."
    & $adbExe push $assets "$dev/extracted"
}

$inp = "@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:$Arena"
$cmd = "cd $dev && TJ_FAST=1 TJ_NOINPUT=1 TJ_UNLOCK=1 TJ_MEAT=1 " +
       "TJ_INPUT='$inp' TJ_DETLOG=$dev/$Tag.det TJ_ITEMLOG=$dev/$Tag.items.txt " +
       "timeout $Seconds ./tj_headless $dev/extracted/default.xbe > $dev/$Tag.log 2>&1; " +
       "echo DONE"
Write-Host "running $Seconds s on-device..."
& $adbExe shell $cmd
& $adbExe pull "$dev/$Tag.det" "$out\$Tag.det"
& $adbExe pull "$dev/$Tag.items.txt" "$out\$Tag.items.txt"
& $adbExe pull "$dev/$Tag.log" "$out\$Tag.log"
Write-Host "== tail =="
Get-Content "$out\$Tag.log" -Tail 15
