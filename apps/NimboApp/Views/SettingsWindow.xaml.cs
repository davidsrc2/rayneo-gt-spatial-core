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
            : "Aún no has calibrado tus gafas (se usa la orientación por defecto). La calibración guiada llega en la próxima versión; " +
              "mientras, imu_probe --calibrate y copia calibration.json a " + AppModel.DataDirectory + ".";
        DimToggle.IsOn = _model.DimOthers;
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

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (File.Exists(ServerController.LogPath))
        {
            Process.Start(new ProcessStartInfo(ServerController.LogPath) { UseShellExecute = true });
        }
    }

    private void Done_Click(object sender, RoutedEventArgs e) => AppWindow.Hide();
}
