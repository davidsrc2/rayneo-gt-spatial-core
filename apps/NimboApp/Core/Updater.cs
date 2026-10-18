using System.ComponentModel;
using System.Diagnostics;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace Nimbo.Core;

// Updates from the repository's GitHub Releases: the latest release's Nimbo-Setup-<version>.exe,
// checked against the SHA-256 GitHub publishes for it, run silently (Windows asks for
// administrator permission once); the installer keeps the settings and opens Nimbo again.
internal sealed record UpdateInfo(Version Version, string Url, string Sha256);

internal static class Updater
{
    private const string LatestApi = "https://api.github.com/repos/davidsrc2/rayneo-gt-spatial-core/releases/latest";

    private static readonly HttpClient Http = CreateClient();

    private static HttpClient CreateClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("Nimbo-Updater");
        c.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return c;
    }

    // 0.1.0 and 0.1.0.0 are the same version.
    private static Version Normalize(Version v) => new(v.Major, v.Minor, Math.Max(0, v.Build), Math.Max(0, v.Revision));

    public static Version Current => Normalize(Assembly.GetEntryAssembly()?.GetName().Version ?? new Version(0, 0));

    public static string CurrentText => $"{Current.Major}.{Current.Minor}.{Current.Build}";

    // A newer release with its installer, or null (none, offline, or GitHub not answering).
    public static async Task<UpdateInfo?> CheckAsync()
    {
        try
        {
            using var r = await Http.GetAsync(LatestApi);
            if (!r.IsSuccessStatusCode) return null;
            using var doc = JsonDocument.Parse(await r.Content.ReadAsStringAsync());
            var root = doc.RootElement;
            if (root.GetProperty("draft").GetBoolean() || root.GetProperty("prerelease").GetBoolean()) return null;
            var tag = root.GetProperty("tag_name").GetString()?.TrimStart('v', 'V');
            if (!Version.TryParse(tag, out var latest) || Normalize(latest) <= Current) return null;
            foreach (var a in root.GetProperty("assets").EnumerateArray())
            {
                var name = a.GetProperty("name").GetString() ?? "";
                if (!name.StartsWith("Nimbo-Setup-", StringComparison.OrdinalIgnoreCase) ||
                    !name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                // Without the digest there is nothing to check the download against: not offered.
                var digest = a.TryGetProperty("digest", out var d) ? d.GetString() ?? "" : "";
                if (!digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase)) return null;
                return new UpdateInfo(Normalize(latest), a.GetProperty("browser_download_url").GetString() ?? "",
                                      digest[7..].ToLowerInvariant());
            }
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or
                                      KeyNotFoundException or InvalidOperationException)
        {
            App.Trace($"update: check failed ({e.GetType().Name})");
        }
        return null;
    }

    // Downloads, checks and starts the installer. Null when it is running (quit Nimbo now),
    // else why not.
    public static async Task<string?> InstallAsync(UpdateInfo u)
    {
        try
        {
            return await InstallCoreAsync(u);
        }
        catch (Exception e)  // never left at "Descargando…": say it and leave a trace
        {
            App.CrashLog("update: install failed", e);
            return $"No se pudo actualizar ({e.GetType().Name}). Detalles en crash.log.";
        }
    }

    private static async Task<string?> InstallCoreAsync(UpdateInfo u)
    {
        App.Trace($"update: downloading {u.Url}");
        var dir = Path.Combine(Path.GetTempPath(), "Nimbo-Update");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, $"Nimbo-Setup-{u.Version.Major}.{u.Version.Minor}.{u.Version.Build}.exe");
        try
        {
            var bytes = await Http.GetByteArrayAsync(u.Url);
            App.Trace($"update: {bytes.Length} bytes downloaded");
            if (Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant() != u.Sha256)
            {
                App.Trace("update: SHA-256 mismatch");
                return "La descarga no coincide con la publicada; no se ha instalado.";
            }
            await File.WriteAllBytesAsync(path, bytes);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException or
                                      UnauthorizedAccessException)
        {
            App.CrashLog("update: download failed", e);
            return "No se pudo descargar la actualización.";
        }
        App.Trace($"update: running {path} /SILENT");
        try
        {
            Process.Start(new ProcessStartInfo(path, "/SILENT /SUPPRESSMSGBOXES /NORESTART")
            {
                UseShellExecute = true,
                Verb = "runas",
            });
            return null;
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223)
        {
            return "Cancelado: hace falta aceptar el permiso de administrador.";
        }
    }
}
