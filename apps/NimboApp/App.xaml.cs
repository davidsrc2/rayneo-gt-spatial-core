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

        // Ctrl+Alt+1 / 2 / 3: the one-click layouts without opening the panel.
        _hotKeys = new HotKeys();
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

        // First run: show the panel so the app is found (the wizard comes in a later version).
        if (!_model.OnboardingDone)
        {
            _model.OnboardingDone = true;
            _model.Save();
            _panel.ShowNearTray();
        }
    }

    private void OpenSettings()
    {
        if (_model == null || _server == null) return;
        _settings ??= new SettingsWindow(_model, _server);
        _settings.ShowCentered();
    }

    private void Quit()
    {
        _hotKeys?.Dispose();
        _server?.Dispose();  // stops the server: displays restored
        _tray?.Dispose();
        _panel?.Destroy();
        _settings?.Destroy();
        _single?.ReleaseMutex();
        Exit();
    }
}
