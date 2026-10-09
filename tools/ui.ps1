# Drives a running VolcanoClient window for testing: focus it, send keys,
# or save a screenshot. Usage:
#   powershell -File tools\ui.ps1 shot  <path.png>
#   powershell -File tools\ui.ps1 keys  "<SendKeys string>"   e.g. "{ESC}", "{DOWN}", " "
#   powershell -File tools\ui.ps1 key   Down [Down Space ...]     (real scancodes; use for arrows/nav)
#   powershell -File tools\ui.ps1 click "x,y"                      (F2 screenshot pixel coords)
#   powershell -File tools\ui.ps1 move  "x,y"                      (hover without clicking)
#   powershell -File tools\ui.ps1 look  "dx,dy"                    (relative mouse motion; in-game camera)
#   powershell -File tools\ui.ps1 focus
#
# Prefer `key` for navigation: SendKeys sends arrows without the extended-key
# flag, which GLFW (scancode-based) reads as numpad keys.
param(
    [Parameter(Mandatory)][ValidateSet('shot','keys','key','click','move','look','focus')] [string]$Action,
    [string]$Arg = ''
)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    public struct POINT { public int X, Y; }

    // Move the cursor to client-area coordinates (= F2 screenshot pixels).
    public static void Move(IntPtr h, int x, int y)
    {
        POINT p = new POINT { X = x, Y = y };
        ClientToScreen(h, ref p);
        SetCursorPos(p.X, p.Y);
    }

    public static void Click(IntPtr h, int x, int y)
    {
        Move(h, x, y);
        System.Threading.Thread.Sleep(120); // let a frame see the hover
        mouse_event(0x2, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        mouse_event(0x4, 0, 0, 0, UIntPtr.Zero);
    }
    public struct RECT { public int L, T, R, B; }

    // Press + release with the real scancode; arrows/nav keys get the
    // extended flag so GLFW doesn't read them as numpad keys.
    public static void Tap(byte vk, bool extended, int holdMs = 80)
    {
        byte scan = (byte)MapVirtualKey(vk, 0);
        uint ext = extended ? 0x1u : 0u;
        keybd_event(vk, scan, ext, UIntPtr.Zero);
        System.Threading.Thread.Sleep(holdMs); // held across a few frames
        keybd_event(vk, scan, ext | 0x2u, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
    }
}
'@
[W]::SetProcessDPIAware() | Out-Null
$p = Get-Process VolcanoClient -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output 'VolcanoClient window not found'; exit 1 }
$h = $p.MainWindowHandle

switch ($Action)
{
    'focus' { [W]::SetForegroundWindow($h) | Out-Null }
    'move'
    {
        $xy = $Arg -split '[ ,]+'
        [W]::SetForegroundWindow($h) | Out-Null
        [W]::Move($h, [int]$xy[0], [int]$xy[1])
    }
    'look'
    {
        # Relative mouse motion (what raw input sees), in small steps.
        $d = $Arg -split '[ ,]+'
        [W]::SetForegroundWindow($h) | Out-Null
        $steps = 20
        for ($i = 0; $i -lt $steps; $i++)
        {
            [W]::mouse_event(0x1, [int]([int]$d[0] / $steps), [int]([int]$d[1] / $steps), 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 15
        }
    }
    'click'
    {
        $xy = $Arg -split '[ ,]+'
        [W]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 150
        [W]::Click($h, [int]$xy[0], [int]$xy[1])
    }
    'key'
    {
        $table = @{
            Up = @(0x26, $true); Down = @(0x28, $true); Left = @(0x25, $true); Right = @(0x27, $true)
            Tab = @(0x09, $false); Space = @(0x20, $false); Enter = @(0x0D, $false); Esc = @(0x1B, $false)
            F2 = @(0x71, $false); F3 = @(0x72, $false); F11 = @(0x7A, $false)
            W = @(0x57, $false); A = @(0x41, $false); S = @(0x53, $false); D = @(0x44, $false)
            Shift = @(0x10, $false); Ctrl = @(0x11, $false)
        }
        [W]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 150
        # "Name" taps; "Name:1500" holds the key for 1500 ms.
        foreach ($item in ($Arg -split '\s+' | Where-Object { $_ }))
        {
            $name, $holdMs = $item -split ':'
            if (-not $table.ContainsKey($name)) { Write-Output "unknown key: $name"; continue }
            $hold = if ($holdMs) { [int]$holdMs } else { 80 }
            [W]::Tap([byte]$table[$name][0], [bool]$table[$name][1], $hold)
        }
    }
    'keys'
    {
        [W]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 150
        [System.Windows.Forms.SendKeys]::SendWait($Arg)
    }
    'shot'
    {
        $r = New-Object W+RECT
        [W]::GetWindowRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        # PW_RENDERFULLCONTENT (2) reads DWM's composited window content.
        $hdc = $g.GetHdc()
        $ok = [W]::PrintWindow($h, $hdc, 2)
        $g.ReleaseHdc($hdc)
        if (-not $ok) { $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size) }
        $bmp.Save($Arg, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Output "saved $Arg ($($bmp.Width)x$($bmp.Height))"
    }
}
