using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;

namespace Nimbo.Views;

internal static class WindowHelpers
{
    public static IntPtr Hwnd(Window w) => WinRT.Interop.WindowNative.GetWindowHandle(w);

    public static double Scale(Window w) => GetDpiForWindow(Hwnd(w)) / 96.0;

    // Size in DIPs (AppWindow works in physical pixels).
    public static void Resize(Window w, double width, double height)
    {
        double s = Scale(w);
        w.AppWindow.Resize(new Windows.Graphics.SizeInt32((int)Math.Round(width * s), (int)Math.Round(height * s)));
    }

    // Bottom-right of the work area of the display with the cursor: where the tray is.
    public static void MoveNearTray(Window w, int margin = 12)
    {
        GetCursorPos(out var cursor);
        var area = DisplayArea.GetFromPoint(new Windows.Graphics.PointInt32(cursor.X, cursor.Y), DisplayAreaFallback.Primary);
        var work = area.WorkArea;
        var size = w.AppWindow.Size;
        int m = (int)Math.Round(margin * Scale(w));
        w.AppWindow.Move(new Windows.Graphics.PointInt32(work.X + work.Width - size.Width - m,
                                                         work.Y + work.Height - size.Height - m));
    }

    public static void Center(Window w)
    {
        var area = DisplayArea.GetFromWindowId(w.AppWindow.Id, DisplayAreaFallback.Primary);
        var work = area.WorkArea;
        var size = w.AppWindow.Size;
        w.AppWindow.Move(new Windows.Graphics.PointInt32(work.X + (work.Width - size.Width) / 2,
                                                         work.Y + (work.Height - size.Height) / 2));
    }

    public static void BringToFront(Window w)
    {
        w.AppWindow.Show();
        w.Activate();
        SetForegroundWindow(Hwnd(w));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X, Y;
    }

    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point p);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hwnd);
}
