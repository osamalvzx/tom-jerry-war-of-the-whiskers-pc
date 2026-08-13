# Scripted two-instance walk of the LAN UI with the pad only -- main menu -> LAN GAME ->
# HOST / browser row -> LOBBY -> START MATCH. Captures both windows at each step so the
# screens can be eyeballed at 4:3 and 16:9 without a human driving.
#
#   powershell -ExecutionPolicy Bypass -File port/tools/lan_ui_walk.ps1
param([int]$Seconds = 90, [string]$Tag = "walk")

$root = "D:\Projects\Tom and Jerry in War of the Whiskers (U)"
$bin  = "$root\port\build\bin\Release"

Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Get-ChildItem "$bin\$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force

function Launch([string]$who, [string]$script, [string]$cap) {
  $env:TJ_FAST = "1"; $env:TJ_UNLOCK = "1"; $env:TJ_NOINPUT = "1"
  $env:TJ_LAN = $null; $env:TJ_NET = $null; $env:TJ_DETLOG = $null
  $env:TJ_ITEMLOG = $null; $env:TJ_WANDER = $null; $env:TJ_ITEMTEST = $null
  $env:TJ_CAPTURE = $null
  # Capture through the lockstep-frame path so each instance writes its OWN files -- both
  # processes share a working directory, and TJ_CAPTURE's fixed filename makes them collide.
  $env:TJ_NETCAP = $cap
  $env:TJ_NETCAP_PATH = "$bin\$Tag`_$who.bmp"
  $env:TJ_INPUT = $script
  Start-Process -FilePath "$bin\tj_loader.exe" -ArgumentList "m4" -WorkingDirectory $bin `
    -RedirectStandardOutput "$bin\$Tag`_$who.log" -PassThru | Out-Null
}

# HOST: menu -> down x3 -> A (LAN GAME) -> A (HOST A GAME, the default selection)
Launch "host" "2200:start,3500:start,3900:down,4300:down,4700:down,5100:a,5900:a" "2000+"
Start-Sleep -Seconds 6
# JOINER: same to the LAN screen, then down x2 to the first browser row -> A (join)
Launch "join" "2200:start,3500:start,3900:down,4300:down,4700:down,5100:a,6100:down,6500:down,6900:a" "2000+"

Start-Sleep -Seconds $Seconds
Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

foreach ($w in @("host", "join")) {
  Write-Host "== $w =="
  Select-String -Path "$bin\$Tag`_$w.log" -Pattern "\[fe\] screen|\[lan\]|captured" |
    Select-Object -Last 14 | ForEach-Object { $_.Line }
}
