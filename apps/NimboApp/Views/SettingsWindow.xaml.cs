using System.Diagnostics;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
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
        DrawPlacement();
        DrawFloating();
        DrawRules();
        DimToggle.IsOn = _model.DimOthers;
        AnchorToggle.IsOn = _model.AnchorAtRest;
        LaptopToggle.IsOn = _model.LaptopScreen;
        FovSlider.Value = _model.Fov;
        IpdSlider.Value = _model.Ipd;
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
        _model.AnchorAtRest = AnchorToggle.IsOn;
        _model.LaptopScreen = LaptopToggle.IsOn;
        _model.Fov = (float)FovSlider.Value;
        _model.Ipd = (float)IpdSlider.Value;
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
        DrawPlacement();
    }

    private void RemoveMonitor_Click(object sender, RoutedEventArgs e)
    {
        if (_model.RemoveMonitor()) _server.SettingsChanged();
        DrawLayout();
        DrawPlacement();
    }

    // --- floating windows ------------------------------------------------------------------------

    private List<(ulong Hwnd, string Title, string App)> _windows = new();

    private void DrawFloating()
    {
        FloatList.Children.Clear();
        for (int i = 0; i < _model.Floating.Count; ++i)
        {
            int index = i;
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            row.Children.Add(new TextBlock { Text = $"V{i + 1}", Width = 30, VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(new TextBlock
            {
                Text = _model.Floating[i].Title,
                Width = 360,
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center,
            });
            var remove = new Button { Content = "Quitar" };
            remove.Click += (_, _) =>
            {
                _model.Floating.RemoveAt(index);
                _model.SaveFloating();
                _server.SettingsChanged();
                DrawFloating();
                DrawPlacement();
            };
            row.Children.Add(remove);
            FloatList.Children.Add(row);
        }
        _windows = NimboNative.ListWindows().Where(w => _model.Floating.All(f => f.Hwnd != w.Hwnd)).ToList();
        FloatChoice.ItemsSource = _windows.Select(w => $"{w.App} — {w.Title}").ToList();
    }

    private void FloatRefresh_Click(object sender, RoutedEventArgs e) => DrawFloating();

    private void FloatAdd_Click(object sender, RoutedEventArgs e)
    {
        int i = FloatChoice.SelectedIndex;
        if (i < 0 || i >= _windows.Count || _model.Floating.Count + _model.Cells.Length >= NimboNative.MaxPanels) return;
        var w = _windows[i];
        _model.Floating.Add(new AppModel.FloatingWin
        {
            Hwnd = w.Hwnd, Title = $"{w.App} — {w.Title}", Pose = _model.DefaultFloatingPose(), App = w.App, WindowTitle = w.Title,
        });
        _model.SaveFloating();
        _server.SettingsChanged();
        DrawFloating();
        DrawPlacement();
    }

    // --- per-app profiles ------------------------------------------------------------------------

    private void DrawRules()
    {
        RuleList.Children.Clear();
        foreach (var rule in _model.AppRules.ToList())
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            row.Children.Add(new TextBlock { Text = rule.Name, Width = 220, VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(new TextBlock
            {
                Text = AppModel.Profiles.FirstOrDefault(p => p.Id == rule.Profile)?.Title ?? rule.Profile,
                Width = 120,
                VerticalAlignment = VerticalAlignment.Center,
            });
            var remove = new Button { Content = "Quitar" };
            remove.Click += (_, _) =>
            {
                _model.AppRules.Remove(rule);
                _model.SaveRules();
                DrawRules();
            };
            row.Children.Add(remove);
            RuleList.Children.Add(row);
        }
        RuleApp.ItemsSource = NimboNative.ListWindows().Select(w => w.App).Where(a => a.Length > 0)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Where(a => _model.AppRules.All(r => !r.App.Equals(a, StringComparison.OrdinalIgnoreCase)))
            .OrderBy(a => a).ToList();
        RuleProfile.ItemsSource = AppModel.Profiles.Select(p => p.Title).ToList();
        if (RuleProfile.SelectedIndex < 0 && AppModel.Profiles.Count > 0) RuleProfile.SelectedIndex = 0;
    }

    private void RuleAdd_Click(object sender, RoutedEventArgs e)
    {
        if (RuleApp.SelectedItem is not string app || RuleProfile.SelectedIndex < 0) return;
        var profile = AppModel.Profiles[RuleProfile.SelectedIndex];
        _model.AppRules.Add(new AppModel.AppRule(app, app, profile.Id));
        _model.SaveRules();
        DrawRules();
    }

    // --- hand placement (SpatialEditor.swift on macOS) -------------------------------------------

    private const float SpanDeg = 150f;  // horizontal degrees across the canvas
    private const float SnapDeg = 1.5f;
    private readonly List<Border> _placeViews = new();
    private int _placeSelected;
    private int _dragging = -1;
    private Windows.Foundation.Point _dragStart;
    private PanelPos[] _dragPoses = Array.Empty<PanelPos>();

    private double Ppd => PlaceCanvas.Width / SpanDeg;

    private Windows.Foundation.Point ToCanvas(float yaw, float pitch) =>
        new(PlaceCanvas.Width / 2 + yaw * Ppd, PlaceCanvas.Height / 2 - pitch * Ppd);

    // Degrees the glasses show around straight ahead, in editor units.
    private (float W, float H) ViewFrame()
    {
        float fovY = _model.Fov;
        float hfov = 2f * MathF.Atan(MathF.Tan(fovY * MathF.PI / 360f) * AppModel.EyeAspect) * 180f / MathF.PI;
        return (hfov, fovY);
    }

    // Monitors first, then floating windows: one list for drawing, snapping and sizing.
    private PanelPos[] PlaceItems() => _model.EffectivePoses.Concat(_model.Floating.Select(f => f.Pose)).ToArray();

    private int MonitorCount => _model.EffectivePoses.Length;

    private string PlaceLabel(int i) => i < MonitorCount ? (i + 1).ToString() : $"V{i - MonitorCount + 1}";

    // Monitors back to the settings, windows to the floating list.
    private void PlaceWrite(PanelPos[] poses, int i)
    {
        int m = MonitorCount;
        if (i < m) _model.Poses = poses.Take(m).ToArray();
        else if (i - m < _model.Floating.Count) _model.Floating[i - m].Pose = poses[i];
    }

    private void DrawPlacement()
    {
        PlaceCanvas.Children.Clear();
        _placeViews.Clear();
        var view = ViewFrame();
        var frame = new Microsoft.UI.Xaml.Shapes.Rectangle
        {
            Width = view.W * Ppd,
            Height = view.H * Ppd,
            Stroke = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(150, 200, 200, 200)),
            StrokeDashArray = new DoubleCollection { 4, 3 },
            RadiusX = 6,
            RadiusY = 6,
        };
        var c = ToCanvas(0, 0);
        Microsoft.UI.Xaml.Controls.Canvas.SetLeft(frame, c.X - frame.Width / 2);
        Microsoft.UI.Xaml.Controls.Canvas.SetTop(frame, c.Y - frame.Height / 2);
        PlaceCanvas.Children.Add(frame);

        var poses = PlaceItems();
        _placeSelected = Math.Clamp(_placeSelected, 0, Math.Max(0, poses.Length - 1));
        for (int i = 0; i < poses.Length; ++i)
        {
            int index = i;
            var b = new Border
            {
                CornerRadius = new CornerRadius(6),
                BorderThickness = new Thickness(1.5),
                Child = new TextBlock
                {
                    Text = PlaceLabel(i),
                    FontSize = 18,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                },
            };
            b.PointerPressed += (s, e) => PlacePressed(index, (UIElement)s, e);
            b.PointerMoved += (s, e) => PlaceMoved(index, e);
            b.PointerReleased += (s, e) => PlaceReleased((UIElement)s, e);
            PlaceCanvas.Children.Add(b);
            _placeViews.Add(b);
            PositionView(i, poses[i]);
        }
        PlaceSelected.Text = poses.Length == 0 ? "" : _placeSelected < MonitorCount ? $"Monitor {_placeSelected + 1}" : $"Ventana {PlaceLabel(_placeSelected)}";
        PlaceScale.Value = poses.Length > 0 ? poses[_placeSelected].Scale : 1;
        PlaceReset.IsEnabled = _model.HasPlacement;
    }

    private void PositionView(int i, PanelPos p)
    {
        var b = _placeViews[i];
        b.Width = _model.Size * p.Scale * Ppd;
        b.Height = AppModel.VerticalArc(_model.Size * p.Scale) * Ppd;
        bool selected = i == _placeSelected, window = i >= MonitorCount;
        b.Background = new SolidColorBrush(window
            ? Microsoft.UI.ColorHelper.FromArgb(selected ? (byte)230 : (byte)170, 150, 120, 190)
            : Microsoft.UI.ColorHelper.FromArgb(selected ? (byte)230 : (byte)160, 120, 160, 156));
        b.BorderBrush = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(selected ? (byte)230 : (byte)0, 140, 220, 200));
        var c = ToCanvas(p.Yaw, p.Pitch);
        Microsoft.UI.Xaml.Controls.Canvas.SetLeft(b, c.X - b.Width / 2);
        Microsoft.UI.Xaml.Controls.Canvas.SetTop(b, c.Y - b.Height / 2);
    }

    private void PlacePressed(int i, UIElement view, PointerRoutedEventArgs e)
    {
        _dragging = i;
        _placeSelected = i;
        _dragStart = e.GetCurrentPoint(PlaceCanvas).Position;
        _dragPoses = PlaceItems();
        view.CapturePointer(e.Pointer);
        var poses = PlaceItems();
        for (int k = 0; k < poses.Length; ++k) PositionView(k, poses[k]);
        PlaceSelected.Text = i < MonitorCount ? $"Monitor {i + 1}" : $"Ventana {PlaceLabel(i)}";
        _updating = true;
        PlaceScale.Value = poses[i].Scale;
        _updating = false;
        e.Handled = true;
    }

    private void PlaceMoved(int i, PointerRoutedEventArgs e)
    {
        if (_dragging != i) return;
        var pt = e.GetCurrentPoint(PlaceCanvas).Position;
        var poses = (PanelPos[])_dragPoses.Clone();
        var p = poses[i];
        p = p with
        {
            Yaw = p.Yaw + (float)((pt.X - _dragStart.X) / Ppd),
            Pitch = p.Pitch - (float)((pt.Y - _dragStart.Y) / Ppd),
        };
        bool free = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        if (!free) p = Snap(p, i, poses);
        poses[i] = p;
        PlaceWrite(poses, i);
        PositionView(i, p);
        _server.SettingsChanged();  // live on the glasses
        e.Handled = true;
    }

    private void PlaceReleased(UIElement view, PointerRoutedEventArgs e)
    {
        _dragging = -1;
        _model.SaveFloating();
        view.ReleasePointerCapture(e.Pointer);
        PlaceReset.IsEnabled = _model.HasPlacement;
        DrawLayout();
    }

    // Edges touching, centres and tops/bottoms aligned with the other monitors, and the horizon.
    private PanelPos Snap(PanelPos p, int i, PanelPos[] poses)
    {
        float size = _model.Size, g = _model.Gap;
        float halfW = size * p.Scale / 2, halfH = AppModel.VerticalArc(size * p.Scale) / 2;
        var xs = new List<float> { 0 };
        var ys = new List<float> { 0 };
        for (int j = 0; j < poses.Length; ++j)
        {
            if (j == i) continue;
            var o = poses[j];
            float oW = size * o.Scale / 2, oH = AppModel.VerticalArc(size * o.Scale) / 2;
            xs.AddRange(new[] { o.Yaw, o.Yaw + oW + g + halfW, o.Yaw - oW - g - halfW });
            ys.AddRange(new[] { o.Pitch, o.Pitch + oH - halfH, o.Pitch - oH + halfH, o.Pitch + oH + g + halfH, o.Pitch - oH - g - halfH });
        }
        float yaw = p.Yaw, pitch = p.Pitch, bx = SnapDeg, by = SnapDeg;
        foreach (var t in xs)
        {
            if (MathF.Abs(p.Yaw - t) < bx) { bx = MathF.Abs(p.Yaw - t); yaw = t; }
        }
        foreach (var t in ys)
        {
            if (MathF.Abs(p.Pitch - t) < by) { by = MathF.Abs(p.Pitch - t); pitch = t; }
        }
        return p with { Yaw = yaw, Pitch = pitch };
    }

    private void PlaceScale_Changed(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_updating) return;
        var poses = PlaceItems();
        if (_placeSelected >= poses.Length) return;
        poses[_placeSelected] = poses[_placeSelected] with { Scale = (float)PlaceScale.Value };
        PlaceWrite(poses, _placeSelected);
        PositionView(_placeSelected, poses[_placeSelected]);
        PlaceReset.IsEnabled = true;
        _server.SettingsChanged();
    }

    private void PlaceReset_Click(object sender, RoutedEventArgs e)
    {
        _model.ResetPlacement();
        _server.SettingsChanged();
        DrawPlacement();
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
