// AquaKeyLauncher - Windows Copilot-key entry point for aqua.
// If aqua is already running: focus its window (restoring it if
// minimized) and exit - never a second instance, two aquas would
// be two ffplay streams fighting over the speakers.
// If not running: start it on the configured library folder and exit.
// Built with the on-box .NET Framework compiler - no dependencies.
//
// %%AQUA_EXE%% / %%AQUA_DIR%% are substituted by build.ps1.
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

static class AquaKeyLauncher
{
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lp);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);

    const string AQUA_EXE = @"%%AQUA_EXE%%";
    const string AQUA_DIR = @"%%AQUA_DIR%%";

    static IntPtr found = IntPtr.Zero;

    // aqua's window title is "aqua" when launched from its Start-menu
    // shortcut, or its full exe path when launched directly - accept both
    static bool Callback(IntPtr h, IntPtr lp)
    {
        var title = new StringBuilder(256);
        GetWindowText(h, title, 256);
        var cls = new StringBuilder(256);
        GetClassName(h, cls, 256);
        string t = title.ToString().ToLowerInvariant();
        if (IsWindowVisible(h) && cls.ToString() == "CASCADIA_HOSTING_WINDOW_CLASS"
            && (t == "aqua" || t.EndsWith("\\aqua.exe")))
        {
            found = h;
            return false;
        }
        return true;
    }

    static void Main()
    {
        Process[] running = Process.GetProcessesByName("aqua");
        if (running.Length > 0)
        {
            EnumWindows(Callback, IntPtr.Zero);
            if (found != IntPtr.Zero)
            {
                if (IsIconic(found)) ShowWindowAsync(found, 9); // SW_RESTORE
                SetForegroundWindow(found);
            }
            return;
        }
        Process.Start(new ProcessStartInfo
        {
            FileName = AQUA_EXE,
            Arguments = "\"" + AQUA_DIR + "\"",
            WorkingDirectory = AQUA_DIR
        });
    }
}
