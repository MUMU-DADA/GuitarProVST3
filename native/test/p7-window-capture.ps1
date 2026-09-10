# Test-only capture of the observed editor owned by the test Guitar Pro PID.
# Qt render cannot include some third-party native HWND renderers.
Add-Type -AssemblyName System.Drawing
if (-not ('Gpvst3EditorCapture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Gpvst3EditorCapture {
    public delegate bool EnumWindow(IntPtr hwnd, IntPtr argument);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindow callback, IntPtr argument);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int capacity);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
    public static string Inventory(uint processId) {
        var result = new StringBuilder();
        EnumWindows((hwnd, argument) => {
            uint pid; GetWindowThreadProcessId(hwnd, out pid);
            if (pid == processId) { var title = new StringBuilder(2048); GetWindowText(hwnd, title, title.Capacity); result.AppendLine(hwnd.ToInt64() + " : " + title.ToString()); }
            return true;
        }, IntPtr.Zero);
        return result.ToString();
    }
    public static IntPtr Find(uint processId, string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, argument) => {
            uint pid; GetWindowThreadProcessId(hwnd, out pid);
            if (pid != processId) return true;
            var text = new StringBuilder(2048); GetWindowText(hwnd, text, text.Capacity);
            if (text.ToString() == title || text.ToString() == title + " - Guitar Pro 8") { found = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
}
function Save-P7NativeWindowCapture([int]$ProcessId, [string]$Title, [string]$Path) {
    $window = [Gpvst3EditorCapture]::Find([uint32]$ProcessId, $Title)
    if ($window -eq [IntPtr]::Zero) { throw ('The observed native editor HWND was not found. Expected: ' + $Title + '. Observed: ' + [Gpvst3EditorCapture]::Inventory([uint32]$ProcessId)) }
    $rectangle = [Gpvst3EditorCapture+Rect]::new()
    if (-not [Gpvst3EditorCapture]::GetWindowRect($window, [ref]$rectangle)) { throw 'Cannot read native editor bounds.' }
    $width = $rectangle.Right - $rectangle.Left; $height = $rectangle.Bottom - $rectangle.Top
    if ($width -lt 100 -or $height -lt 100 -or $width -gt 4096 -or $height -gt 4096) { throw 'Unexpected native editor dimensions.' }
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try { $printed = [Gpvst3EditorCapture]::PrintWindow($window, $dc, 2) }
        finally { $graphics.ReleaseHdc($dc) }
        $mode = 'PrintWindow'
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
        $style = [Gpvst3EditorCapture]::GetWindowLongPtr($window, -16).ToInt64()
        return @{pid=$ProcessId;title=$Title;hwnd=$window.ToInt64();visible=[Gpvst3EditorCapture]::IsWindowVisible($window);
            has_caption=(($style -band 0x00C00000) -eq 0x00C00000);has_system_menu=([bool]($style -band 0x00080000));
            mode=$mode;print_window_return=$printed;width=$width;height=$height;path=$Path}
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
