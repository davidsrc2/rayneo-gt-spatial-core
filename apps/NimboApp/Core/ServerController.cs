using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Nimbo.Native;

namespace Nimbo.Core;

// Runs the spatial server (nimbo.dll) for the app and turns its state into calm status text.
// UI thread only; the blocking start / stop calls run on the thread pool.
internal sealed unsafe class ServerController : IDisposable
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

    // Keeps the log callback alive while nimbo.dll holds it.
    private static readonly NimboNative.LogFn LogCallback = OnLog;

    public event Action? Changed;

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
        bool ok = await Task.Run(() =>
        {
            var o = options;  // a local of the lambda: captured variables cannot be pinned
            return NimboNative.nimbo_start(_server, &o) != 0;
        });
        _busy = false;
        if (ok)
        {
            _running = options;
        }
        else
        {
            NimboNative.Status st;
            NimboNative.nimbo_get_status(_server, &st);
            State = NimboNative.State.Stopped;
            Say(Friendly(NimboNative.GetField(st.Message, 256)));
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

    public void Recenter()
    {
        if (IsRunning) NimboNative.nimbo_recenter(_server);
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
            NimboNative.nimbo_apply(_server, &next);
            _running = next;
        }
    }

    private async Task RestartAsync()
    {
        if (!IsRunning || _busy) return;
        await StopAsync();
        await StartAsync();
    }

    private void Refresh()
    {
        if (++_glassesCheck % 5 == 0)  // ~1 s: display enumeration is not free
        {
            bool connected = NimboNative.nimbo_glasses_connected() != 0 || IsRunning;
            if (connected != GlassesConnected)
            {
                GlassesConnected = connected;
                if (!connected) _autoStartArmed = true;
            }
        }
        if (!_busy)
        {
            NimboNative.Status st;
            NimboNative.nimbo_get_status(_server, &st);
            var previous = State;
            State = _running != null ? st.State : NimboNative.State.Stopped;
            GazePanel = st.GazePanel;
            if (st.FovYDeg > 0 && Math.Abs(st.FovYDeg - _model.Fov) > 0.01f && _running != null)
            {
                _model.Fov = st.FovYDeg;  // tuned with Ctrl+Alt+- / =
                _model.Save();
            }
            FovY = st.FovYDeg;
            if (_running != null && st.State == NimboNative.State.Stopped)  // stopped on its own
            {
                _running = null;
                var why = NimboNative.GetField(st.Message, 256);
                Say(why.Contains("disconnected") ? "Las gafas se han desconectado; todo ha vuelto a la normalidad"
                                                 : Friendly(why));
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
