# Drive a single instance into a QUICK GAME match with MEAT RUSH forced on, and prove the
# turkey leg is the only thing that ever spawns.
#
#   powershell -ExecutionPolicy Bypass -File port/tools/meat_shot.ps1 -Arena 3
#   ... -Arena 9 -Seconds 90 -Tag west
#   ... -Off              same run with MEAT RUSH off, for an A/B of the item log
#
# Writes <Tag>_<frame>.bmp + <Tag>.log + <Tag>.items.txt next to the exe and prints the
# item census: with the mode on, every spawn must be cat=1 type=0 and nothing else.
param([int]$Arena = 3, [int]$Seconds = 90, [string]$Tag = "meat",
      [int]$Every = 150, [int]$Width = 640, [int]$Height = 480, [switch]$Off)

$root = "D:\Projects\Tom and Jerry in War of the Whiskers (U)"
$bin  = "$root\port\build\bin\Release"

Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Get-ChildItem "$bin\capture_*.bmp" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem "$bin\$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force

$ini = "$bin\tomjerry.ini"
$txt = if (Test-Path $ini) { Get-Content $ini -Raw } else { "" }
if ($txt -match '(?m)^Width=')  { $txt = $txt -replace '(?m)^Width=.*',  "Width=$Width" }
if ($txt -match '(?m)^Height=') { $txt = $txt -replace '(?m)^Height=.*', "Height=$Height" }
Set-Content -Path $ini -Value $txt -Encoding ascii

$env:TJ_FAST = "1"; $env:TJ_UNLOCK = "1"; $env:TJ_NOINPUT = "1"
$env:TJ_LAN = $null; $env:TJ_NET = $null; $env:TJ_DETLOG = $null
$env:TJ_NETCAP = $null; $env:TJ_NETCAP_PATH = $null; $env:TJ_WANDER = $null
$env:TJ_MEAT = $(if ($Off) { $null } else { "1" })
$env:TJ_ITEMLOG = "$bin\$Tag.items.txt"
$env:TJ_CAPTURE = "$Every+"
# MEAT RUSH IS A MENU CHOICE NOW, NOT JUST THE SETTINGS BYTE. `@qg:1` walked the OLD main
# menu and silently never reached a match on the new one -- the run still produced a log and
# an item census, but with the mode OFF, which reads exactly like a passing test. Drive the
# real path instead: MAIN MENU -> MULTIPLAYER -> (QUICK GAME | MEAT RUSH) -> setup -> arena.
# Direction presses fire once; activation presses re-fire until the screen actually changes.
$pick = if ($Off) { "@14:a" } else { "@14:down,@14:down,@14:a" }
$env:TJ_INPUT = "@20:start,@1:start,@4:a,$pick,@11:start,@map:$Arena"
Write-Host "arena $Arena  meat=$(if ($Off) {'OFF'} else {'ON'})  script: $($env:TJ_INPUT)"

Start-Process -FilePath "$bin\tj_loader.exe" -ArgumentList "m4" -WorkingDirectory $bin `
  -RedirectStandardOutput "$bin\$Tag.log" -PassThru | Out-Null
Start-Sleep -Seconds $Seconds
Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

Get-ChildItem "$bin\capture_*.bmp" -ErrorAction SilentlyContinue |
  ForEach-Object { Rename-Item $_.FullName ($Tag + "_" + $_.Name.Substring(8)) }

# Fail LOUDLY if the mode never armed. Without this the census below is still printed and
# still looks plausible, which is how a mode-off run passed for a mode-on test.
if (-not $Off) {
  $armed = Select-String -Path "$bin\$Tag.log" -Pattern "\[meat\] arena $Arena armed"
  if (-not $armed) {
    Write-Host ""
    Write-Host "*** MEAT RUSH NEVER ARMED -- this run tested NOTHING. ***" -ForegroundColor Red
    Write-Host "    No '[meat] arena $Arena armed' line. Check the menu walk reached a match:" -ForegroundColor Red
    Select-String -Path "$bin\$Tag.log" -Pattern '\[fe\] screen' | Select-Object -Last 6 |
      ForEach-Object { "      " + $_.Line }
  }
}

Write-Host ""
Write-Host "== log =="
Select-String -Path "$bin\$Tag.log" -Pattern "\[meat\] (arena|installed|scores)|Couldn't find geometry" |
  Select-Object -Last 12 | ForEach-Object { $_.Line }
$banked = Select-String -Path "$bin\$Tag.log" -Pattern "banked meat"
Write-Host "  banked-meat events: $($banked.Count)"
if ($banked) { $banked | Select-Object -Last 6 | ForEach-Object { "   " + $_.Line } }

Write-Host ""
Write-Host "== item census (cat/type of every spawn) =="
if (Test-Path "$bin\$Tag.items.txt") {
  $spawns = Select-String -Path "$bin\$Tag.items.txt" -Pattern 'st=0->' |
            ForEach-Object { if ($_.Line -match 'type=(\d+) cat=(\d+)') { "cat$($Matches[2]) type$($Matches[1])" } }
  if ($spawns) { $spawns | Group-Object | Sort-Object Name |
                 ForEach-Object { "  {0,-14} x{1}" -f $_.Name, $_.Count } }
  else { Write-Host "  (no spawns logged)" }
  $n = (Get-Content "$bin\$Tag.items.txt" | Where-Object { $_ -notmatch '^#' }).Count
  Write-Host "  $n item transitions total"
}
