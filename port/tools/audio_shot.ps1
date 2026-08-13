# Drive a single instance to a place where the AUDIO control is on screen and dump BMPs.
#   -Where fe    : main menu -> OPTIONS -> AUDIO (frontend screen 8)
#   -Where pause : QUICK GAME match -> START -> AUDIO row -> A (the in-game pause badge)
# Writes <Tag>_<frame>.bmp next to the exe plus <Tag>.log.
param([string]$Where = "pause", [int]$Seconds = 70, [string]$Tag = "aud",
      [int]$Every = 150, [int]$Width = 640, [int]$Height = 480)

$root = "D:\Projects\Tom and Jerry in War of the Whiskers (U)"
$bin  = "$root\port\build\bin\Release"

Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Get-ChildItem "$bin\capture_*.bmp" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem "$bin\$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force

# resolution for this run (the loader reads tomjerry.ini at boot)
$ini = "$bin\tomjerry.ini"
$txt = if (Test-Path $ini) { Get-Content $ini -Raw } else { "" }
if ($txt -match '(?m)^Width=')  { $txt = $txt -replace '(?m)^Width=.*',  "Width=$Width" }
if ($txt -match '(?m)^Height=') { $txt = $txt -replace '(?m)^Height=.*', "Height=$Height" }
Set-Content -Path $ini -Value $txt -Encoding ascii

if ($Where -eq "fe") {
  # main menu (CHALLENGE / QUICK GAME* / TOURNAMENT / LAN GAME / OPTIONS / EXIT):
  # three DOWNs from the QUICK GAME default reaches OPTIONS, then A -> screen 6, A -> AUDIO.
  $script = "@20:start,@1:start,@4:down,@4:down,@4:down,@4:down,@4:a,@6:down,@6:down,@6:a,@8:left,@8:left,@8:left,@8:left,@8:left,@8:down,@8:right,@8:right,@8:right,@8:right,@8:down,@8:a,@6:b,@qg:1,@map:0"
} else {
  # boot -> QUICK GAME -> Kitchen -> match, then START to pause, DOWN to AUDIO, A to open it.
  $script = "@qg:1,@map:0,7000:start,7500:down,8000:a,8500:left,8900:left,9300:left,9700:down,10100:right,10500:right,10900:right,11300:down,11700:a"
}

$env:TJ_FAST = "1"; $env:TJ_UNLOCK = "1"; $env:TJ_NOINPUT = "1"
$env:TJ_LAN = $null; $env:TJ_NET = $null; $env:TJ_DETLOG = $null
$env:TJ_NETCAP = $null; $env:TJ_NETCAP_PATH = $null
$env:TJ_CAPTURE = "$Every+"
$env:TJ_INPUT = $script
Write-Host "script: $script"
Start-Process -FilePath "$bin\tj_loader.exe" -ArgumentList "m4" -WorkingDirectory $bin `
  -RedirectStandardOutput "$bin\$Tag.log" -PassThru | Out-Null

Start-Sleep -Seconds $Seconds
Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

Get-ChildItem "$bin\capture_*.bmp" -ErrorAction SilentlyContinue |
  ForEach-Object { Rename-Item $_.FullName ($Tag + "_" + $_.Name.Substring(8)) }
Select-String -Path "$bin\$Tag.log" -Pattern "\[fe\] screen|\[input\]|captured|\[aud\]" |
  Select-Object -Last 40 | ForEach-Object { $_.Line }
