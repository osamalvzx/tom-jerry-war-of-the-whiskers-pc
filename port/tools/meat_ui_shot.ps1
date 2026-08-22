# Drive the frontend to MAIN MENU -> OPTIONS -> FIGHT SETTINGS, walk down onto the new
# MEAT RUSH row, cycle it right a few times and confirm.  Writes a BMP film strip so the row's
# LAYOUT can actually be looked at -- the only thing that catches a layout fault.
#
#   powershell -ExecutionPolicy Bypass -File port/tools/meat_ui_shot.ps1
#   ... -Width 1280 -Height 720        the 16:9 worst case
param([int]$Seconds = 55, [string]$Tag = "mui", [int]$Every = 60,
      [int]$Width = 640, [int]$Height = 480, [int]$Rights = 2)

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

# ⚠ THIS COMMENT WAS STALE AND THE SCRIPT WALKED INTO THE WRONG SCREEN. The main menu is no
# longer CHALLENGE / QUICK GAME / TOURNAMENT / LAN GAME / OPTIONS / EXIT: meat_menu.cpp
# splices a MULTIPLAYER row in and moves QUICK GAME and TOURNAMENT into it, so the menu is
# CHALLENGE / MULTIPLAYER / LAN GAME / OPTIONS / EXIT with MULTIPLAYER as the default row
# (a single A on screen 4 enters it -- proven by the device legs). OPTIONS is therefore TWO
# DOWNs, not four; four landed on CHALLENGE (screen 10) and the capture never saw screen 7.
# Screen 6's first row is FIGHT SETTINGS (returns 7). On screen 7 the rows are TIME, ROUNDS,
# DIFFICULTY, VIBRATION, MARKERS, [MAX MEAT], [SCORES], CONFIRM.
$nav = "@20:start,@1:start,@4:down,@4:down,@4:a,@6:a"
$down = (1..5 | ForEach-Object { "@7:down" }) -join ","
$right = if ($Rights -gt 0) { "," + ((1..$Rights | ForEach-Object { "@7:right" }) -join ",") } else { "" }
$env:TJ_INPUT = "$nav,$down$right,@7:down,@7:a"

$env:TJ_FAST = "1"; $env:TJ_UNLOCK = "1"; $env:TJ_NOINPUT = "1"
$env:TJ_LAN = $null; $env:TJ_NET = $null; $env:TJ_DETLOG = $null; $env:TJ_MEAT = $null
$env:TJ_ITEMLOG = $null; $env:TJ_CAPTURE = "$Every+"
Write-Host "script: $($env:TJ_INPUT)"

Start-Process -FilePath "$bin\tj_loader.exe" -ArgumentList "m4" -WorkingDirectory $bin `
  -RedirectStandardOutput "$bin\$Tag.log" -PassThru | Out-Null
Start-Sleep -Seconds $Seconds
Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

Get-ChildItem "$bin\capture_*.bmp" -ErrorAction SilentlyContinue |
  ForEach-Object { Rename-Item $_.FullName ($Tag + "_" + $_.Name.Substring(8)) }
Select-String -Path "$bin\$Tag.log" -Pattern "\[meat\]|\[fe\] screen" |
  Select-Object -Last 20 | ForEach-Object { $_.Line }
