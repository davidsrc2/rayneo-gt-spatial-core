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
        InitializeComponent();
        DispatcherShutdownMode = DispatcherShutdownMode.OnExplicitShutdown;
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _single = new Mutex(true, @"Local\NimboApp", out bool first);
        if (!first)
        {
            Exit();  // already running: its icon is in the tray
            return;
        }
        NimboNative.nimbo_restore_displays();  // a previous run that crashed

        _model = new AppModel();
        _server = new ServerController(_model, DispatcherQueue.GetForCurrentThread());
        _panel = new PanelWindow(_model, _server, OpenSettings);
        _server.GlassesChanged += OnGlassesChanged;

        // Ctrl+Alt+1 / 2 / 3: the one-click layouts without opening the panel. Ctrl+Alt+N: the panel.
        _hotKeys = new HotKeys();
        _hotKeys.Register('N', () => _panel.Toggle());
        // Ctrl+Alt+[ / ]: the glasses' brightness (their mode stays on their own button).
        foreach (var (vk, step) in new[] { (0xDBu, -1), (0xDDu, 1) })
        {
            _hotKeys.Register(vk, () =>
            {
                int current = _model.Brightness == 0 ? 8 : _model.Brightness;
                _model.Brightness = Math.Clamp(current + step, 1, 8);
                _server.SettingsChanged();
                _panel.Refresh();
            });
        }
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
        _tray.ForceCreate();

        bool background = Environment.GetCommandLineArgs().Contains(Startup.BackgroundArg);
        if (!_model.OnboardingDone) OpenOnboarding(OnboardingWindow.Step.Welcome, singleStep: false);
        else if (!background) _panel.ShowNearTray();
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
