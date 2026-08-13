# PrintWindow capture of a window by title substring -> PNG (works w/o focus, grabs D3D11 swapchain)
param([string]$titleLike = "Tom", [string]$out = "shot.png")
Add-Type @"
using System;using System.Runtime.InteropServices;using System.Drawing;
public class W {
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
}
"@
Add-Type -AssemblyName System.Drawing
$p = Get-Process | Where-Object { $_.MainWindowTitle -like "*$titleLike*" } | Select-Object -First 1
if(-not $p){ Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object W+RECT
[W]::GetWindowRect($h,[ref]$r) | Out-Null
$w = $r.R-$r.L; $ht = $r.B-$r.T
if($w -le 0 -or $ht -le 0){ Write-Output "bad rect"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w,$ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[W]::PrintWindow($h,$dc,3) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($out,[System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $out ($w x $ht) title='$($p.MainWindowTitle)'"
