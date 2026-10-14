using Microsoft.Win32;

namespace Nimbo.Core;

// "Abrir Nimbo al iniciar Windows": the per-user Run key, the same switch as Settings > Apps > Startup.
internal static class Startup
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string Name = "Nimbo";
    public const string BackgroundArg = "--background";  // started with Windows: no panel

    private static string Command => $"\"{Environment.ProcessPath}\" {BackgroundArg}";

    public static bool Enabled
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(Name) is string v && v.Equals(Command, StringComparison.OrdinalIgnoreCase);
        }
        set
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value) key.SetValue(Name, Command);
            else key.DeleteValue(Name, throwOnMissingValue: false);
        }
    }
}
