using System.ComponentModel;
using System.Diagnostics;

namespace Nimbo.Core;

// One-time preparation of the Virtual Display Driver from the app, instead of PowerShell:
// nimbo_win.exe --setup-displays, elevated (Windows asks for administrator permission). The
// installer runs it too; with the driver shipped in the "driver" folder it installs it first.
internal static class DisplaySetup
{
    public const string DriverUrl = "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/latest";

    public static bool DriverInstalled => File.Exists(@"C:\VirtualDisplayDriver\vdd_settings.xml");
    // The installer's copy of the driver, next to the app: the setup installs it when missing.
    public static bool DriverShipped => File.Exists(Path.Combine(AppContext.BaseDirectory, "driver", "MttVDD.inf"));

    // Runs the setup and returns a message for the user.
    public static async Task<(bool Ok, string Message)> RunAsync()
    {
        var exe = Path.Combine(AppContext.BaseDirectory, "nimbo_win.exe");
        if (!DriverInstalled && !DriverShipped) return (false, "El driver Virtual Display Driver no está instalado: instala Nimbo con su instalador, que lo trae.");
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
                0 => (true, "Listo: 6 monitores preparados, solo existen mientras Nimbo está abierto. No hace falta reiniciar Windows."),
                7 => (false, "Driver preparado, pero no se pudieron crear las tareas para encenderlo y apagarlo con Nimbo."),
                8 => (false, "No se pudo instalar el driver de pantallas virtuales; el registro de Nimbo dice por qué."),
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
