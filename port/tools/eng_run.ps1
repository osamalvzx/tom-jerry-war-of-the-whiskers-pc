# Engine-mode / native scripted run with LOCALAPPDATA redirected to scratch (the _SAVES
# rule: no run that could touch saves may see the real LOCALAPPDATA). Session 24 tool.
#
#   eng_run.ps1 -Tag natA -Seconds 130 -Detlog              native leg (~2 min to match end)
#   eng_run.ps1 -Tag engA -Seconds 1800 -Engine -Detlog     interpreted leg (~6-10 fps in-match)
#   ... -Meat            MEAT RUSH instead of QUICK GAME (+TJ_ITEMLOG)
#   ... -MtTrace "A-B"   log MT-draw callers for frames A..B ([mtt] lines)
#
# Outputs land in %TEMP%\tj_eng_runs: <Tag>.log, <Tag>.det, <Tag>.items.txt. Compare two
# .det files with port/tools/det_diff.py (offline runs are a single segment).
# ⚠ EXCLUSIVE: this kills every running tj_loader at start — never overlap two runs.
# ⚠ The game opens the .det file deny-share: diff only after the run's process exits.
param([string]$Tag = "engrun", [int]$Seconds = 120, [switch]$Engine, [int]$Arena = 3,
      [switch]$Detlog, [string]$Input = "", [string]$MtTrace = "", [switch]$Meat,
      [switch]$Prof)

$root  = Split-Path (Split-Path $PSScriptRoot)   # port/tools -> project root
$bin   = "$root\port\build\bin\Release"
$out   = "$env:TEMP\tj_eng_runs"
$scratch = "$out\lad_$Tag"
New-Item -ItemType Directory -Force $scratch | Out-Null

Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$log = "$out\$Tag.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue

$env:LOCALAPPDATA = $scratch
$env:TJ_FAST = "1"; $env:TJ_UNLOCK = "1"; $env:TJ_NOINPUT = "1"
$env:TJ_LAN = $null; $env:TJ_NET = $null; $env:TJ_NETCAP = $null
$env:TJ_NETCAP_PATH = $null; $env:TJ_WANDER = $null; $env:TJ_CAPTURE = $null
$env:TJ_PROF = $(if ($Prof) { "$out\$Tag.prof" } else { $null })
$env:TJ_MEAT = $(if ($Meat) { "1" } else { $null })
$env:TJ_ITEMLOG = $(if ($Meat) { "$out\$Tag.items.txt" } else { $null })
$env:TJ_ENGINE = $(if ($Engine) { "1" } else { $null })
$env:TJ_DETLOG = $(if ($Detlog) { "$out\$Tag.det" } else { $null })
$env:TJ_MTTRACE = $(if ($MtTrace -ne "") { $MtTrace } else { $null })
if ($Input -ne "") { $env:TJ_INPUT = $Input }
elseif ($Meat) { $env:TJ_INPUT = "@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:$Arena" }
else { $env:TJ_INPUT = "@20:start,@1:start,@4:a,@14:a,@11:start,@map:$Arena" }

Write-Host "tag=$Tag engine=$($Engine.IsPresent) seconds=$Seconds out=$out input=$($env:TJ_INPUT)"
$t0 = Get-Date
$p = Start-Process -FilePath "$bin\tj_loader.exe" -ArgumentList "m4" -WorkingDirectory $bin `
  -RedirectStandardOutput $log -PassThru
$deadline = $t0.AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
  if ($p.HasExited) { Write-Host "EXITED code=$($p.ExitCode) after $([int]((Get-Date)-$t0).TotalSeconds)s"; break }
  Start-Sleep -Seconds 2
}
if (-not $p.HasExited) { Write-Host "still running at deadline; killing"; Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
Write-Host "== tail =="
Get-Content $log -Tail 25
