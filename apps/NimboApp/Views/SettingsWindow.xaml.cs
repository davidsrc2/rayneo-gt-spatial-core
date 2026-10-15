using System.Diagnostics;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Nimbo.Core;
using Nimbo.Native;

namespace Nimbo.Views;

// "Más opciones…" (SettingsView.swift on macOS).
public sealed partial class SettingsWindow : Window
{
    private readonly AppModel _model;
    private readonly ServerController _server;
    private bool _updating;
    private bool _destroying;

    public event Action? RecalibrateRequested;

    internal SettingsWindow(AppModel model, ServerController server)
    {
        _model = model;
        _server = server;
        InitializeComponent();
        Title = "Nimbo · Ajustes";
        SystemBackdrop = new MicaBackdrop();
        AppWindow.SetIcon(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "Nimbo.ico"));
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.IsMaximizable = false;
            p.IsMinimizable = false;
        }
        AppWindow.Closing += (_, e) =>
        {
            if (_destroying) return;
            e.Cancel = true;
            AppWindow.Hide();
        };
    }

    public void ShowCentered()
    {
        Load();
        WindowHelpers.Resize(this, 640, 760);
        WindowHelpers.Center(this);
        WindowHelpers.BringToFront(this);
    }

    public void Destroy()
    {
        _destroying = true;
        Close();
    }

    private void Load()
    {
        _updating = true;
        CalibrationText.Text = _model.HasCalibration
            ? "Tus gafas están calibradas."
            : "Aún no has calibrado tus gafas (se usa la orientación por defecto).";
        StartupToggle.IsOn = Startup.Enabled;
        DrawLayout();
        DimToggle.IsOn = _model.DimOthers;
        TrackingChoice.SelectedIndex = _model.Tracking switch { 1 => 0, 2 => 1, _ => 2 };
        FovSlider.Value = _model.Fov;
        IpdSlider.Value = _model.Ipd;
        DeadZoneSlider.Value = _model.DeadZone;
        GapSlider.Value = _model.Gap;
        StereoToggle.IsOn = _model.Stereo;
        GazeToggle.IsOn = _model.GazeCursor;
        GatherToggle.IsOn = _model.GatherWindows;
        K1Slider.Value = _model.K1;
        K2Slider.Value = _model.K2;
        DiagnosticsText.Text =
            $"Gafas: {(NimboNative.nimbo_glasses_connected() != 0 ? "conectadas" : "no detectadas")}\n" +
            $"Driver de monitores virtuales: {(NimboNative.nimbo_virtual_driver_available() != 0 ? "instalado" : "no instalado (se muestra una pantalla existente)")}\n" +
            $"Ajustes: {AppModel.SettingsPath}";
        _updating = false;
    }

    private void Save()
    {
        if (_updating) return;
        _model.DimOthers = DimToggle.IsOn;
        _model.Tracking = TrackingChoice.SelectedIndex switch { 0 => 1, 1 => 2, _ => 0 };
        _model.Fov = (float)FovSlider.Value;
        _model.Ipd = (float)IpdSlider.Value;
        _model.DeadZone = (float)DeadZoneSlider.Value;
        _model.Gap = (float)GapSlider.Value;
        _model.Stereo = StereoToggle.IsOn;
        _model.GazeCursor = GazeToggle.IsOn;
        _model.GatherWindows = GatherToggle.IsOn;
        _model.K1 = (float)K1Slider.Value;
        _model.K2 = (float)K2Slider.Value;
        _server.SettingsChanged();
    }

    private void Changed(object sender, RoutedEventArgs e) => Save();

    private void SliderChanged(object sender, RangeBaseValueChangedEventArgs e) => Save();

    private void TrackingChanged(object sender, Microsoft.UI.Xaml.Controls.SelectionChangedEventArgs e) => Save();

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (File.Exists(ServerController.LogPath))
        {
            Process.Start(new ProcessStartInfo(ServerController.LogPath) { UseShellExecute = true });
        }
    }

    private void Done_Click(object sender, RoutedEventArgs e) => AppWindow.Hide();

    private void Recalibrate_Click(object sender, RoutedEventArgs e) => RecalibrateRequested?.Invoke();

    private void Startup_Toggled(object sender, RoutedEventArgs e)
    {
        if (!_updating) Startup.Enabled = StartupToggle.IsOn;
    }

    // --- layout editor ----------------------------------------------------------------------------

    private void AddMonitor_Click(object sender, RoutedEventArgs e)
    {
        if (_model.AddMonitor()) _server.SettingsChanged();
        DrawLayout();
    }

    private void RemoveMonitor_Click(object sender, RoutedEventArgs e)
    {
        if (_model.RemoveMonitor()) _server.SettingsChanged();
        DrawLayout();
    }

    // The grid of monitors as seen from the front, numbered in panel order.
    private void DrawLayout()
    {
        LayoutPreview.Children.Clear();
        var cells = _model.Cells;
        if (cells.Length == 0) return;
        int c0 = cells.Min(c => c.Col), r0 = cells.Min(c => c.Row);
        int cols = cells.Max(c => c.Col) - c0 + 1, rows = cells.Max(c => c.Row) - r0 + 1;
        double cw = Math.Min(LayoutPreview.Width / cols, LayoutPreview.Height / rows * 16 / 9);
        double ch = cw * 9 / 16;
        double ox = (LayoutPreview.Width - cw * cols) / 2, oy = (LayoutPreview.Height - ch * rows) / 2;
        for (int i = 0; i < cells.Length; ++i)
        {
            var rect = new Microsoft.UI.Xaml.Shapes.Rectangle
            {
                Width = cw - 6,
                Height = ch - 6,
                RadiusX = 4,
                RadiusY = 4,
                Fill = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(140, 165, 205, 200)),
            };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(rect, ox + (cells[i].Col - c0) * cw + 3);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(rect, oy + (cells[i].Row - r0) * ch + 3);
            LayoutPreview.Children.Add(rect);
            var label = new Microsoft.UI.Xaml.Controls.TextBlock { Text = (i + 1).ToString(), FontSize = 13 };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(label, ox + (cells[i].Col - c0) * cw + cw / 2 - 4);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(label, oy + (cells[i].Row - r0) * ch + ch / 2 - 10);
            LayoutPreview.Children.Add(label);
        }
        AddMonitorButton.IsEnabled = cells.Length < AppModel.MaxMonitors;
        RemoveMonitorButton.IsEnabled = cells.Length > 1;
        var name = AppModel.Layouts.FirstOrDefault(l => l.Id == _model.ActiveLayout)?.Title;
        LayoutText.Text = $"{cells.Length} monitor(es)" + (name != null ? $" · {name}" : "") +
                          (_server.IsRunning ? " · se aplica al momento" : "");
    }
}
