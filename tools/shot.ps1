param([string]$out = "shot")

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SC {
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
}
"@
$w = [SC]::GetSystemMetrics(0); $h = [SC]::GetSystemMetrics(1)
$vsW = [SC]::GetSystemMetrics(76); $vsH = [SC]::GetSystemMetrics(77)
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(0, 0, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$g.Dispose()
$path = "D:\pmy\Com_UI\output\$out.png"
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $path ${w}x${h}"
