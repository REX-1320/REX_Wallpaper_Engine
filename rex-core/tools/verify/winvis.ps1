Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WinVis {
  public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc e, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc e, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder b, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@
$found = New-Object System.Collections.ArrayList
$childCb = {
  param($h, $l)
  $sb = New-Object Text.StringBuilder 256
  [WinVis]::GetClassName($h, $sb, 256) | Out-Null
  if ($sb.ToString() -eq 'REXRenderWindow') { $found.Add($h) | Out-Null }
  return $true
}
$topCb = {
  param($h, $l)
  $sb = New-Object Text.StringBuilder 256
  [WinVis]::GetClassName($h, $sb, 256) | Out-Null
  if ($sb.ToString() -eq 'REXRenderWindow') { $found.Add($h) | Out-Null }
  [WinVis]::EnumChildWindows($h, $childCb, [IntPtr]::Zero) | Out-Null
  return $true
}
[WinVis]::EnumWindows($topCb, [IntPtr]::Zero) | Out-Null
if ($found.Count -eq 0) { Write-Output 'REXRenderWindow: NONE FOUND'; exit 0 }
foreach ($h in $found) {
  $v = [WinVis]::IsWindowVisible($h)
  Write-Output ("REXRenderWindow 0x" + $h.ToString('X') + ' visible=' + $v)
}
