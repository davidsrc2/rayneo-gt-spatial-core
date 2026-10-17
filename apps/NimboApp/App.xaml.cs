using H.NotifyIcon;
using H.NotifyIcon.Core;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Nimbo.Core;
using Nimbo.Native;
using Nimbo.Views;

namespace Nimbo;

// Tray app: the icon, the compact panel under it and the settings window. Lives until
// "Salir"; closing a window only hides it.
public partial class App : Application
{
    private Mutex? _single;
    private AppModel? _model;
    private ServerController? _server;
    private HotKeys? _hotKeys;
    private TaskbarIcon? _tray;
    private PanelWindow? _panel;
    private SettingsWindow? _settings;
    private OnboardingWindow? _onboarding;

    public App()
    {
        // A crash at start-up closes a tray app without a word: write why to crash.log.
        AppDomain.CurrentDomain.UnhandledException += (_, e) => CrashLog("unhandled", e.ExceptionObject as Exception);
        UnhandledException += (_, e) => CrashLog("xaml", e.Exception);
        Trace("app: loading App.xaml");
        InitializeComponent();
        Trace("app: App.xaml loaded");
        DispatcherShutdownMode = DispatcherShutdownMode.OnExplicitShutdown;
    }

    // Start-up steps, so a silent exit shows how far it got.
    internal static void Trace(string step) => CrashLog(step, null);

    public static string CrashLogPath => Path.Combine(AppModel.DataDirectory, "crash.log");

    internal static void CrashLog(string where, Exception? e)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nimbo");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "crash.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{where}]{(e != null ? " " + e : "")}{Environment.NewLine}");
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            Launch();
        }
        catch (Exception e)
        {
            CrashLog("launch", e);
            throw;
        }
    }

    private void Launch()
    {
        Trace("launch: start");
        _single = new Mutex(true, @"Local\NimboApp", out bool first);
        if (!first)
        {
            Exit();  // already running: its icon is in the tray
            return;
        }
        Trace("launch: nimbo_server.dll");
        NimboNative.nimbo_restore_displays();  // a previous run that crashed
        Trace("launch: nimbo_server.dll ok, settings");

        _model = new AppModel();
        _server = new ServerController(_model, DispatcherQueue.GetForCurrentThread());
        Trace("launch: panel");
        _panel = new PanelWindow(_model, _server, OpenSettings);
        Trace("launch: panel ok");
        _server.GlassesChanged += OnGlassesChanged;

        // Ctrl+Alt+1 / 2 / 3: the one-click layouts without opening the panel. Ctrl+Alt+N: the panel.
        _hotKeys = new HotKeys();
        _hotKeys.Register('N', () => _panel.Toggle());
        // Ctrl+Alt+Down / Up: the glasses' own screen brightness, 8 steps (8 = factory).
        foreach (var (vk, step) in new[] { (0x28u, -1), (0x26u, 1) })
        {
            _hotKeys.Register(vk, () =>
            {
                int current = _model.Brightness == 0 ? 8 : _model.Brightness;
                _model.Brightness = Math.Clamp(current + step, 1, 8);
                _server.SettingsChanged();
                _server.ShowMessage($"Brillo de las gafas {_model.Brightness} de 8");
                _panel.Refresh();
            });
        }
        // Ctrl+Alt+M: in the glasses' pinned mode, the panel in use follows the mouse only (or the
        // gaze too again). Saved, like the switch in Settings.
        _hotKeys.Register('M', () =>
        {
            _model.GrowByGaze = !_model.GrowByGaze;
            _model.Save();
            _server.SettingsChanged();
            _server.ShowMessage(_model.GrowByGaze ? "Pantalla activa: mirada y ratón" : "Pantalla activa: solo con el ratón");
            _panel.Refresh();
        });
        // Ctrl+Alt+S: in the space at real size, the glasses in steady mode instead of follow (or
        // back), live, to compare. For this run.
        _hotKeys.Register('S', () =>
        {
            if (_model.PinnedMode) return;
            _model.GlassesSteady = !_model.GlassesSteady;
            _server.SettingsChanged();
            _server.ShowMessage(_model.GlassesSteady ? "Gafas en modo steady (prueba)" : "Gafas en modo follow");
        });
        // Ctrl+Alt+G: the glasses' own pinned mode or back to the space at real size.
        _hotKeys.Register('G', () =>
        {
            _model.PinnedMode = !_model.PinnedMode;
            _model.Save();
            _server.SettingsChanged();
            _tray?.ShowNotification(_model.PinnedMode ? "Modo fijo de las gafas" : "Espacio a tamaño real",
                                    _model.PinnedMode ? "Las gafas sostienen la imagen (Ctrl+Alt+G para volver)" : "");
            _panel.Refresh();
        });
        for (int i = 0; i < Math.Min(3, AppModel.Layouts.Count); ++i)
        {
            var preset = AppModel.Layouts[i];
            _hotKeys.Register((char)('1' + i), () =>
            {
                if (_model.ActiveLayout == preset.Id) return;
                _model.Apply(preset);
                _server.SettingsChanged();
                _panel.Refresh();
                _tray?.ShowNotification(preset.Title, _server.IsRunning ? "Preparando los monitores…" : "Se aplicará al empezar");
            });
        }

        var menu = new MenuFlyout();
        menu.Items.Add(new MenuFlyoutItem { Text = "Abrir Nimbo", Command = new RelayCommand(() => _panel.ShowNearTray()) });
        menu.Items.Add(new MenuFlyoutItem { Text = "Empezar / parar", Command = new RelayCommand(() => _ = _server.ToggleAsync()) });
        menu.Items.Add(new MenuFlyoutItem { Text = "Recentrar vista", Command = new RelayCommand(() => _server.Recenter()) });
        menu.Items.Add(new MenuFlyoutItem { Text = "Más opciones…", Command = new RelayCommand(OpenSettings) });
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(new MenuFlyoutItem { Text = "Salir", Command = new RelayCommand(Quit) });

        _tray = new TaskbarIcon
        {
            ToolTipText = "Nimbo",
            Icon = new System.Drawing.Icon(Path.Combine(AppContext.BaseDirectory, "Assets", "Nimbo.ico")),
            NoLeftClickDelay = true,
            LeftClickCommand = new RelayCommand(() => _panel.Toggle()),
            ContextMenuMode = ContextMenuMode.PopupMenu,
            ContextFlyout = menu,
        };
        Trace("launch: tray icon");
        _tray.ForceCreate();
        Trace("launch: tray icon ok");

        bool background = Environment.GetCommandLineArgs().Contains(Startup.BackgroundArg);
        if (!_model.OnboardingDone) OpenOnboarding(OnboardingWindow.Step.Welcome, singleStep: false);
        else if (!background) _panel.ShowNearTray();
        Trace("launch: done");
    }

    private void OpenOnboarding(OnboardingWindow.Step step, bool singleStep)
    {
        if (_model == null || _server == null) return;
        if (_onboarding == null)
        {
            _onboarding = new OnboardingWindow(_model, _server);
            _onboarding.Finished += () => _panel?.ShowNearTray();
        }
        _onboarding.Open(step, singleStep);
    }

    // Notices when the glasses come and go (connect-and-forget).
    private void OnGlassesChanged(bool connected, bool wasRunning)
    {
        if (_tray == null || _model == null) return;
        if (connected)
        {
            _tray.ShowNotification("Gafas conectadas", _model.AutoStart ? "Empezando…" : "Abre Nimbo para empezar");
        }
        else if (wasRunning)
        {
            _tray.ShowNotification("Gafas desconectadas", "Todo ha vuelto a la normalidad");
        }
    }

    private void OpenSettings()
    {
        if (_model == null || _server == null) return;
        if (_settings == null)
        {
            _settings = new SettingsWindow(_model, _server);
            _settings.RecalibrateRequested += () => OpenOnboarding(OnboardingWindow.Step.Calibration, singleStep: true);
        }
        _settings.ShowCentered();
    }

    private void Quit()
    {
        _hotKeys?.Dispose();
        _server?.Dispose();  // stops the server: displays restored
        _tray?.Dispose();
        _panel?.Destroy();
        _settings?.Destroy();
        _onboarding?.Destroy();
        _single?.ReleaseMutex();
        Exit();
    }
}
