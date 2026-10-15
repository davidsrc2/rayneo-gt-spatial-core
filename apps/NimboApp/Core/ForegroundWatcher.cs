using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Nimbo.Core;

// Tells which app comes to the front (executable name), from the UI thread's message loop.
internal sealed class ForegroundWatcher : IDisposable
{
    private const uint EventSystemForeground = 0x0003;
    private const uint WinEventOutOfContext = 0x0000;

    private readonly WinEventProc _proc;  // kept alive while the hook exists
    private readonly IntPtr _hook;
    private readonly Action<string> _onApp;

    public ForegroundWatcher(Action<string> onApp)
    {
        _onApp = onApp;
        _proc = OnEvent;
        _hook = SetWinEventHook(EventSystemForeground, EventSystemForeground, IntPtr.Zero, _proc, 0, 0, WinEventOutOfContext);
    }

    private void OnEvent(IntPtr hook, uint ev, IntPtr hwnd, int obj, int child, uint thread, uint time)
    {
        GetWindowThreadProcessId(hwnd, out uint pid);
        try
        {
            using var p = Process.GetProcessById((int)pid);
            _onApp(p.ProcessName);
        }
        catch (ArgumentException)
        {
            // The process ended meanwhile.
        }
        catch (InvalidOperationException)
        {
        }
    }

    public void Dispose()
    {
        if (_hook != IntPtr.Zero) UnhookWinEvent(_hook);
    }

    private delegate void WinEventProc(IntPtr hook, uint ev, IntPtr hwnd, int obj, int child, uint thread, uint time);

    [DllImport("user32.dll")]
    private static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr module, WinEventProc proc, uint pid, uint tid, uint flags);
    [DllImport("user32.dll")] private static extern bool UnhookWinEvent(IntPtr hook);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
}
