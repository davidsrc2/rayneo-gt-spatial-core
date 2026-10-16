using System.ComponentModel;
using System.Diagnostics;

namespace Nimbo.Core;

// One-time preparation of the Virtual Display Driver from the app, instead of PowerShell:
// nimbo_win.exe --setup-displays, elevated (Windows asks for administrator permission).
internal static class DisplaySetup
{
    public const string DriverUrl = "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/latest";

    public static bool DriverInstalled => File.Exists(@"C:\VirtualDisplayDriver\vdd_settings.xml");

    // Runs the setup and returns a message for the user.
    public static async Task<(bool Ok, string Message)> RunAsync()
    {
        var exe = Path.Combine(AppContext.BaseDirectory, "nimbo_win.exe");
        if (!DriverInstalled) return (false, "El driver Virtual Display Driver no está instalado: descárgalo e instálalo primero.");
        if (!File.Exists(exe)) return (false, "Falta nimbo_win.exe junto a la app.");
        try
        {
            using var p = Process.Start(new ProcessStartInfo(exe, "--setup-displays")
            {
                UseShellExecute = true,
                Verb = "runas",  // administrator
                WindowStyle = ProcessWindowStyle.Hidden,
            });
            if (p == null) return (false, "No se pudo lanzar la preparación.");
            await p.WaitForExitAsync();
            return p.ExitCode switch
            {
                0 => (true, "Listo: el driver tiene 6 monitores preparados. No hace falta reiniciar Windows."),
                3 => (false, "El driver Virtual Display Driver no está instalado."),
                5 => (false, "No se encontró el dispositivo del driver: reinstálalo con VDD Control."),
                6 => (false, "No se pudo reiniciar el driver: reinicia Windows una vez."),
                _ => (false, $"La preparación no terminó (código {p.ExitCode})."),
            };
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223)
        {
            return (false, "Cancelado: hace falta aceptar el permiso de administrador.");
        }
    }
}
