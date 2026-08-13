# Two-instance LAN harness. Launches a host and a joiner with their own determinism logs,
# lets them run for -Seconds, then kills both and compares the logs frame-for-frame.
#
#   powershell -ExecutionPolicy Bypass -File port/tools/lan_test.ps1 -Seconds 180
#   ... -Mode legacy            the stage-1c direct peer path (TJ_NET=host|join)
#   ... -Mode lan               the stage-3 session path (TJ_LAN=host|join, discovery+JOIN)
#   ... -Pw secret              host password (the joiner is given the same one)
#   ... -BadPw                  joiner offers the WRONG password (expect a clean rejection)
#   ... -Arena 5 -Rounds 3      lobby rules broadcast by the host
#   ... -Drop 5 -Burst 7        packet-loss fault injection on both peers
#   ... -Dump 9000              raw state dump at that lockstep frame on both peers
param(
  [int]$Seconds = 180,
  [string]$Mode = "lan",
  [string]$Pw = "",
  [switch]$BadPw,
  [int]$Arena = -1,
  [int]$Meat = 0,          # MEAT RUSH: 5/10/15/20 target, 255 = unlimited, 0 = off
  [int]$Rounds = 0,
  [int]$Time = -1,
  [int]$Drop = 0,
  [int]$Burst = 1,
  [int]$Lag = 0,
  [int]$Players = 2,
  [int]$Dump = -1,
  [string]$NetCap = "",
  [switch]$Fast,
  [switch]$Wander,
  [switch]$Items,
  [switch]$Contest,
  [switch]$Tier2,             # 64 sub-block digests per frame: bisects a divergence to bytes
  [string]$Tag = "lan",
  # Which folder to run FROM. Point this at dist\TomJerryWOW-LAN to test the copyable build
  # exactly as it will be shipped; logs still land in the build output, not in the package.
  [string]$Bin = ""
)

$root = "D:\Projects\Tom and Jerry in War of the Whiskers (U)"
$bin  = $(if ($Bin -ne "") { $Bin } else { "$root\port\build\bin\Release" })
$out  = "$root\port\build\bin\Release"

Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Get-ChildItem "$out\$Tag*.txt","$out\$Tag*.bin" -ErrorAction SilentlyContinue | Remove-Item -Force

function Launch([string]$who, [hashtable]$extra) {
  $env:TJ_UNLOCK      = "1"
  $env:TJ_NOINPUT     = "1"          # determinism runs must ignore physical pads
  # Screen-gated presses walk both instances from the boot screens to the main menu, which
  # is a launch-capable screen; the session machinery does the rest.
  $env:TJ_INPUT       = "@20:start,@1:start"
  $env:TJ_NET_PLAYERS = "$Players"
  $env:TJ_DETLOG      = "$out\$Tag`_$who.txt"
  $env:TJ_FAST        = $(if ($Fast) { "1" } else { $null })
  $env:TJ_NET_DROP    = $(if ($Drop -gt 0) { "$Drop" } else { $null })
  $env:TJ_NET_BURST   = $(if ($Drop -gt 0) { "$Burst" } else { $null })
  $env:TJ_NET_LAG     = $(if ($Lag -gt 0) { "$Lag" } else { $null })
  $env:TJ_DETDUMP     = $(if ($Dump -ge 0) { "$Dump" } else { $null })
  $env:TJ_DETDUMP_PATH= "$out\$Tag`_$who.bin"
  $env:TJ_NETCAP      = $(if ($NetCap -ne "") { "$NetCap" } else { $null })
  $env:TJ_NETCAP_PATH = "$out\$Tag`_$who.bmp"
  # -Contest walks the two fighters together (2) so the item placed between them is inside
  # both pickup radii; -Wander alone just chases items (1).
  $env:TJ_WANDER      = $(if ($Contest) { "2" } elseif ($Wander) { "1" } else { $null })
  $env:TJ_ITEMTEST    = $(if ($Contest) { "1" } else { $null })
  $env:TJ_DETLOG2     = $(if ($Tier2) { "1" } else { $null })
  $env:TJ_MEAT        = $(if ($Meat -gt 0) { "$Meat" } else { $null })
  $env:TJ_ITEMLOG     = $(if ($Items) { "$out\$Tag`_$who.items.txt" } else { $null })
  $env:TJ_NET = $null; $env:TJ_LAN = $null; $env:TJ_LAN_PW = $null
  $env:TJ_LAN_NAME = $null; $env:TJ_LAN_ARENA = $null; $env:TJ_LAN_ROUNDS = $null
  $env:TJ_LAN_TIME = $null; $env:TJ_SEED = $null
  foreach ($k in $extra.Keys) { Set-Item -Path "env:$k" -Value $extra[$k] }
  Start-Process -FilePath "$bin\tj_loader.exe" -ArgumentList "m4" `
    -WorkingDirectory $bin -RedirectStandardOutput "$out\$Tag`_$who.log" -PassThru
}

if ($Mode -eq "legacy") {
  $h = @{ TJ_NET = "host"; TJ_SEED = "5150" }
  $j = @{ TJ_NET = "join:127.0.0.1"; TJ_SEED = "1" }
} else {
  $h = @{ TJ_LAN = "host"; TJ_LAN_NAME = "HOSTPC" }
  $j = @{ TJ_LAN = "join"; TJ_LAN_NAME = "JOINPC" }
  if ($Pw -ne "") { $h["TJ_LAN_PW"] = $Pw; $j["TJ_LAN_PW"] = $(if ($BadPw) { "WRONGPW" } else { $Pw }) }
  if ($Arena -ge 0)  { $h["TJ_LAN_ARENA"]  = "$Arena" }
  if ($Rounds -gt 0) { $h["TJ_LAN_ROUNDS"] = "$Rounds" }
  if ($Time -ge 0)   { $h["TJ_LAN_TIME"]   = "$Time" }
}

Write-Host "== launching host =="
$ph = Launch "host" $h
Start-Sleep -Seconds 3
Write-Host "== launching joiner =="
$pj = Launch "join" $j

Write-Host "== running for $Seconds s =="
Start-Sleep -Seconds $Seconds
Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Write-Host ""
Write-Host "== [lan]/[net] lines, host =="
Select-String -Path "$out\$Tag`_host.log" -Pattern "^\[lan\]|^\[net\]|LAUNCH|ARMED|WARN|slot " |
  Select-Object -First 40 | ForEach-Object { $_.Line }
Write-Host ""
Write-Host "== [lan]/[net] lines, joiner =="
Select-String -Path "$out\$Tag`_join.log" -Pattern "^\[lan\]|^\[net\]|LAUNCH|ARMED|WARN|slot " |
  Select-Object -First 40 | ForEach-Object { $_.Line }
Write-Host ""
Write-Host "== determinism diff =="
python "$root\port\tools\det_diff.py" "$out\$Tag`_host.txt" "$out\$Tag`_join.txt"
if ($Items) {
  Write-Host ""
  Write-Host "== item ownership diff =="
  python "$root\port\tools\item_diff.py" "$out\$Tag`_host.items.txt" "$out\$Tag`_join.items.txt"
}
