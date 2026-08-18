# Gate S5d Windows-null reference leg (companion to arm_run.ps1 for the ARM side).
#
# Runs the null-gfx tj_hybrid (build-null) under engine+EXACT with the MEAT script and
# captures TJ_DRAWLOG — the recorded draw-call stream compared against the ARM-null leg.
# Mirrors eng_run.ps1's save protection (LOCALAPPDATA -> scratch: the _SAVES rule).
#
# Prereq (one-time): configure the null-gfx build dir, then build it:
#   cmake -S port -B port/build-null -A Win32 -DTJ_NULL_GFX=ON
#   cmake --build port/build-null --config Release --target tj_hybrid tj_loader
#
#   draw_run.ps1 -Tag drawWin -Seconds 150
# Output: %TEMP%\tj_eng_runs\<Tag>.{draw,det,items.txt,log}
# Compare vs the ARM-null leg (arm.draw), matching per-frame "frame N draws=K verts=V hash=H":
#   - draws/verts must match on every common frame (structural identity, the gate);
#   - vertex-payload HASH diverges once 3D rendering starts — that is native host FP in the
#     bridge transform (x86 vs ARM), by design (ANDROID_PLAN §2.7), not an engine fault.
param([string]$Tag = "drawWin", [int]$Seconds = 150, [int]$Arena = 3)

$root = Split-Path (Split-Path $PSScriptRoot)          # port/tools -> project root
$bin  = "$root\port\build-null\bin\Release"
$out  = "$env:TEMP\tj_eng_runs"
$scratch = "$out\lad_$Tag"
New-Item -ItemType Directory -Force $scratch | Out-Null

if (-not (Test-Path "$bin\tj_loader.exe")) {
    Write-Host "build-null not built. Configure with -DTJ_NULL_GFX=ON and build tj_hybrid tj_loader (see header)."
    exit 1
}

Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$log = "$out\$Tag.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue

$env:LOCALAPPDATA = $scratch
$env:TJ_FAST = "1"; $env:TJ_UNLOCK = "1"; $env:TJ_NOINPUT = "1"
$env:TJ_LAN = $null; $env:TJ_NET = $null; $env:TJ_NETCAP = $null
$env:TJ_NETCAP_PATH = $null; $env:TJ_WANDER = $null; $env:TJ_CAPTURE = $null; $env:TJ_PROF = $null
$env:TJ_MEAT = "1"
$env:TJ_ITEMLOG = "$out\$Tag.items.txt"
$env:TJ_ENGINE = "1"
$env:TJ_ENG_EXACT = "1"
$env:TJ_DETLOG = "$out\$Tag.det"
$env:TJ_DRAWLOG = "$out\$Tag.draw"
$env:TJ_INPUT = "@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:$Arena"

Write-Host "tag=$Tag bin=$bin seconds=$Seconds input=$($env:TJ_INPUT)"
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
Write-Host "== drawlog tail =="
if (Test-Path "$out\$Tag.draw") { Get-Content "$out\$Tag.draw" -Tail 6 } else { Write-Host "NO DRAWLOG" }
