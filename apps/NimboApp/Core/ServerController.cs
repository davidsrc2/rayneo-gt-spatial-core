using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Nimbo.Native;

namespace Nimbo.Core;

// Runs the spatial server (nimbo_server.dll) for the app and turns its state into calm status text.
// UI thread only; the blocking start / stop calls run on the thread pool.
internal sealed class ServerController : IDisposable
{
    private readonly AppModel _model;
    private readonly DispatcherQueue _ui;
    private readonly IntPtr _server = NimboNative.nimbo_create();
    private readonly DispatcherQueueTimer _poll;
    private readonly DispatcherQueueTimer _restart;  // debounces changes that rebuild the monitors
    private NimboNative.Options? _running;
    private bool _busy;
    private bool _autoStartArmed = true;  // auto-start once per connection
    private int _glassesCheck;
    private bool _stoppedByUnplug;

    // Keeps the log callback alive while nimbo_server.dll holds it.
    private static readonly NimboNative.LogFn LogCallback = OnLog;

    public event Action? Changed;
    // Glasses plugged in (true) or out (false), for the notices. `wasRunning`: unplugged mid-session.
    public event Action<bool, bool>? GlassesChanged;

    public NimboNative.State State { get; private set; } = NimboNative.State.Stopped;
    public bool GlassesConnected { get; private set; }
    public int GazePanel { get; private set; } = -1;
    public float FovY { get; private set; }
    public string? Notice { get; private set; }
    public bool IsRunning => State != NimboNative.State.Stopped;
    public bool IsBusy => _busy;

    public string StatusText => State switch
    {
        NimboNative.State.Starting => "Preparando los monitores…",
        NimboNative.State.Calibrating => "Calibrando (deja las gafas en la mesa 5 s)",
        NimboNative.State.Tracking => "Sensores estabilizados",
        _ => GlassesConnected ? "Listas para empezar" : "Conecta las RayNeo GT",
    };

    public ServerController(AppModel model, DispatcherQueue ui)
    {
        _model = model;
        _ui = ui;
        NimboNative.nimbo_set_log(Marshal.GetFunctionPointerForDelegate(LogCallback), IntPtr.Zero);
        GlassesConnected = NimboNative.nimbo_glasses_connected() != 0;
        _poll = ui.CreateTimer();
        _poll.Interval = TimeSpan.FromMilliseconds(200);
        _poll.Tick += (_, _) => Refresh();
        _poll.Start();
        _restart = ui.CreateTimer();
        _restart.Interval = TimeSpan.FromMilliseconds(700);
        _restart.IsRepeating = false;
        _restart.Tick += async (_, _) => await RestartAsync();
    }

    public static string LogPath => Path.Combine(AppModel.DataDirectory, "nimbo.log");

    // The last status read (diagnostics in Settings).
    public NimboNative.Status LastStatus { get; private set; }

    // The diagnostics in plain terms (Settings, the report).
    public static string Describe(NimboNative.Status s, bool running)
    {
        if (!running) return "Con Nimbo en marcha verás aquí cómo van la imagen y el sensor.";
        string lvl(bool good, bool fair) => good ? "bien" : fair ? "regular" : "mal";
        return
            $"Latencia (cabeza → imagen): {s.LatencyMs:0} ms ({lvl(s.LatencyMs <= 22, s.LatencyMs <= 30)})\n" +
            $"Imágenes que llegan tarde: {s.MissedPerS:0} por segundo ({lvl(s.MissedPerS <= 2, s.MissedPerS <= 10)})\n" +
            $"Refresco: {s.RenderFps:0} Hz · GPU por imagen: {s.GpuMs:0.0} ms\n" +
            $"Nitidez del texto: {(s.Supersample >= 2 ? "máxima" : "reducida un momento (fluidez)")}\n" +
            $"Ruido del sensor: {s.GyroNoise:0.00} °/s ({lvl(s.GyroNoise < 0.25f, s.GyroNoise < 0.5f)})\n" +
            $"Temperatura de las gafas: {s.TempC:0.0} °C{(s.TempC > 0 && s.TempC < 28 ? " (frías: se estabiliza en 2-3 min)" : "")}\n" +
            $"Estabilidad del sensor (último minuto): {s.BiasChange:0.00} °/s ({lvl(s.BiasChange < 0.05f, s.BiasChange < 0.15f)})\n" +
            $"Gafas puestas: {(s.Worn != 0 ? "sí" : "no (en pausa)")} · Cortes del sensor recuperados: {s.Stalls}";
    }

    // "Enviar informe": the log's last 5000 lines, the settings, the calibration, the bias map,
    // the last 30 s of the IMU (replayable) and the diagnostics, zipped in Downloads.
    public string? MakeReport()
    {
        try
        {
            var name = $"Nimbo informe {DateTime.Now:yyyy-MM-dd HH.mm.ss}";
            var downloads = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
            var dir = Path.Combine(Path.GetTempPath(), name);
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            if (File.Exists(LogPath))
            {
                using var fs = new FileStream(LogPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var reader = new StreamReader(fs);
                var lines = reader.ReadToEnd().Split('\n');
                File.WriteAllText(Path.Combine(dir, "nimbo.log"), string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 5000))));
            }
            foreach (var f in new[] { AppModel.SettingsPath, AppModel.CalibrationPath,
                                      Path.Combine(AppModel.DataDirectory, "rayneo_biasmap.json"),
                                      Path.Combine(AppModel.DataDirectory, "crash.log") })
            {
                if (File.Exists(f)) File.Copy(f, Path.Combine(dir, Path.GetFileName(f)), true);
            }
            if (IsRunning) NimboNative.nimbo_dump_recent_imu(_server, Path.Combine(dir, "imu-last-30s.imu"));
            File.WriteAllText(Path.Combine(dir, "diagnostico.txt"),
                              $"Nimbo {typeof(ServerController).Assembly.GetName().Version} · {Environment.OSVersion}\n{Describe(LastStatus, IsRunning)}\n");
            Directory.CreateDirectory(downloads);
            var zip = Path.Combine(downloads, name + ".zip");
            if (File.Exists(zip)) File.Delete(zip);
            System.IO.Compression.ZipFile.CreateFromDirectory(dir, zip);
            Directory.Delete(dir, true);
            return zip;
        }
        catch (Exception e)
        {
            App.CrashLog("report", e);
            return null;
        }
    }

    private static void OnLog(IntPtr ctx, IntPtr line)
    {
        try
        {
            File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss} {NimboNative.FromPtr(line)}{Environment.NewLine}");
        }
        catch (IOException)
        {
        }
    }

    public async Task StartAsync()
    {
        if (_busy || IsRunning) return;
        var options = _model.ToOptions();
        if (options.Windowed == 0 && NimboNative.nimbo_glasses_connected() == 0)
        {
            Say("Conecta las gafas para empezar");
            return;
        }
        _busy = true;
        State = NimboNative.State.Starting;
        Notice = null;
        Changed?.Invoke();
        bool ok = await Task.Run(() => NimboNative.Start(_server, options));
        _busy = false;
        if (ok)
        {
            _running = options;
        }
        else
        {
            var st = NimboNative.GetStatus(_server);
            State = NimboNative.State.Stopped;
            Say(Friendly(NimboNative.MessageOf(st)));
        }
        Refresh();
    }

    public async Task StopAsync()
    {
        if (_busy || !IsRunning) return;
        _busy = true;
        Changed?.Invoke();
        await Task.Run(() => NimboNative.nimbo_stop(_server));
        _busy = false;
        _running = null;
        State = NimboNative.State.Stopped;
        Refresh();
    }

    public Task ToggleAsync() => IsRunning ? StopAsync() : StartAsync();

    public void ToggleFocus(bool cinema)
    {
        if (IsRunning) NimboNative.nimbo_toggle_focus(_server, cinema ? (byte)1 : (byte)0);
    }

    // A short message on the glasses.
    public void ShowMessage(string text)
    {
        if (IsRunning) NimboNative.nimbo_show_message(_server, text);
    }

    public void Recenter()
    {
        if (IsRunning) NimboNative.nimbo_recenter(_server);  // in pinned mode: the glasses' anchor
    }

    // Settings changed in the UI: saved, then applied live or by rebuilding the monitors.
    public void SettingsChanged()
    {
        _model.Save();
        if (_running is not NimboNative.Options running) return;
        var next = _model.ToOptions();
        if (AppModel.NeedsRestart(running, next))
        {
            _restart.Stop();
            _restart.Start();
        }
        else
        {
            NimboNative.Apply(_server, next);
            _running = next;
        }
    }

    public void Restart()
    {
        _restart.Stop();
        _restart.Start();
    }

    private async Task RestartAsync()
    {
        if (!IsRunning || _busy) return;
        await StopAsync();
        // Windows settles the displays a moment after they are restored (on macOS starting
        // straight away found them half way and laid the monitors out wrong).
        await Task.Delay(1500);
        await StartAsync();
    }

    private void Refresh()
    {
        if (++_glassesCheck % 5 == 0)  // ~1 s: display enumeration is not free
        {
            // While running, display reconfigurations can hide the glasses for a moment.
            bool connected = NimboNative.nimbo_glasses_connected() != 0 || IsRunning || _busy;
            if (connected != GlassesConnected)
            {
                GlassesConnected = connected;
                if (!connected) _autoStartArmed = true;
                GlassesChanged?.Invoke(connected, _stoppedByUnplug);
                _stoppedByUnplug = false;
            }
        }
        if (!_busy)
        {
            var st = NimboNative.GetStatus(_server);
            LastStatus = st;
            var previous = State;
            State = _running != null ? st.State : NimboNative.State.Stopped;
            GazePanel = st.GazePanel;
            if (st.FovYDeg > 0 && Math.Abs(st.FovYDeg - _model.Fov) > 0.01f && _running != null)
            {
                _model.Fov = st.FovYDeg;  // tuned with Ctrl+Alt+Shift+- / +
                _model.Save();
            }
            FovY = st.FovYDeg;
            if (st.Zoom > 0 && Math.Abs(st.Zoom - _model.Zoom) > 0.001f && _running != null)
            {
                _model.Zoom = st.Zoom;  // Ctrl+Alt++ / - / 0 / K: the same zoom next time
                _model.Save();
            }
            if (_running != null && st.State == NimboNative.State.Stopped)  // stopped on its own
            {
                _running = null;
                var why = NimboNative.MessageOf(st);
                _stoppedByUnplug = why.Contains("disconnected");
                Say(_stoppedByUnplug ? "Las gafas se han desconectado; todo ha vuelto a la normalidad" : Friendly(why));
            }
            if (previous != State) Changed?.Invoke();
        }
        if (_model.AutoStart && _autoStartArmed && GlassesConnected && !IsRunning && !_busy)
        {
            _autoStartArmed = false;
            _ = StartAsync();
        }
        Changed?.Invoke();
    }

    private static string Friendly(string message)
    {
        if (string.IsNullOrEmpty(message)) return "";
        if (message.Contains("SmartGlasses")) return "No encuentro la pantalla de las gafas";
        if (message.Contains("Ctrl+Alt+Q")) return "";
        return message;
    }

    private void Say(string? text)
    {
        Notice = string.IsNullOrEmpty(text) ? null : text;
        Changed?.Invoke();
    }

    public void Dispose()
    {
        _poll.Stop();
        _restart.Stop();
        NimboNative.nimbo_destroy(_server);  // stops and restores the displays
        NimboNative.nimbo_set_log(IntPtr.Zero, IntPtr.Zero);
    }
}
