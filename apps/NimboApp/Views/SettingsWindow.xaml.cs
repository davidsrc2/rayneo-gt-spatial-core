using System.Diagnostics;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
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
    // "Buscar actualizaciones" / "Actualizar a …": the app checks or installs.
    public event Action? UpdateRequested;

    public void ShowUpdate(string text, string button, bool enabled = true)
    {
        UpdateText.Text = text;
        UpdateButton.Content = button;
        UpdateButton.IsEnabled = enabled;
    }

    private void Update_Click(object sender, RoutedEventArgs e) => UpdateRequested?.Invoke();

    private void Shortcuts_Click(object sender, RoutedEventArgs e) => ShortcutsWindow.ShowShared();

    internal SettingsWindow(AppModel model, ServerController server)
    {
        _model = model;
        _server = server;
        // XAML raises ValueChanged / Toggled while it builds the controls (a slider's minimum
        // moves its value): saving then read controls not created yet and crashed the app.
        _updating = true;
        InitializeComponent();
        _updating = false;
        Title = "Nimbo · Ajustes";
        // Diagnóstico: the running server's figures, every second.
        var diagnostics = DispatcherQueue.CreateTimer();
        diagnostics.Interval = TimeSpan.FromSeconds(1);
        diagnostics.Tick += (_, _) => LiveDiagnosticsText.Text = ServerController.Describe(_server.LastStatus, _server.IsRunning);
        diagnostics.Start();
        LiveDiagnosticsText.Text = ServerController.Describe(_server.LastStatus, _server.IsRunning);
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
        ResetCalibrationBtn.Visibility = _model.HasCalibration ? Visibility.Visible : Visibility.Collapsed;
        StartupToggle.IsOn = Startup.Enabled;
        DrawLayout();
        DrawPlacement();
        DrawFloating();
        DimToggle.IsOn = _model.DimOthers;
        AnchorToggle.IsOn = _model.AnchorAtRest;
        LaptopToggle.IsOn = _model.LaptopScreen;
        VehicleToggle.IsOn = _model.VehicleMode;
        FlatWallToggle.IsOn = _model.FlatWall;
        GrowGazeToggle.IsOn = _model.GrowByGaze;
        PinnedToggle.IsOn = _model.PinnedMode;
        FovSlider.Value = _model.Fov;
        GapSlider.Value = _model.Gap;
        TextSizeSlider.Value = _model.TextSize;
        DistanceSlider.Value = _model.Distance;
        CurvatureSlider.Value = _model.Curvature;
        SizeSlider.Value = _model.Size;
        TiltSlider.Value = _model.Tilt;
        if (BackgroundCombo.Items.Count == 0) foreach (var name in AppModel.Backgrounds) BackgroundCombo.Items.Add(name);
        BackgroundCombo.SelectedIndex = _model.Background;
        BackgroundSlider.Value = _model.BackgroundBrightness * 100;
        ShowBackgroundImage();
        GazeToggle.IsOn = _model.GazeCursor;
        GatherToggle.IsOn = _model.GatherWindows;
        K1Slider.Value = _model.K1;
        K2Slider.Value = _model.K2;
        DiagnosticsText.Text =
            $"Gafas: {(NimboNative.nimbo_glasses_connected() != 0 ? "conectadas" : "no detectadas")}\n" +
            $"Driver de monitores virtuales: {(NimboNative.nimbo_virtual_driver_available() != 0 ? "listo" : "no responde (instala Parsec VDD: builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe)")}\n" +
            $"Ajustes: {AppModel.SettingsPath}";
        _updating = false;
    }

    // Ctrl+Alt+G switched the glasses' pinned mode: the switch here shows it (it stayed off).
    public void SyncPinned()
    {
        _updating = true;
        PinnedToggle.IsOn = _model.PinnedMode;
        _updating = false;
        DrawPlacement();  // the wall is off in the pinned mode
    }

    private void Save()
    {
        if (_updating) return;
        _model.DimOthers = DimToggle.IsOn;
        _model.AnchorAtRest = AnchorToggle.IsOn;
        _model.LaptopScreen = LaptopToggle.IsOn;
        _model.VehicleMode = VehicleToggle.IsOn;
        _model.FlatWall = FlatWallToggle.IsOn;
        _model.GrowByGaze = GrowGazeToggle.IsOn;
        _model.PinnedMode = PinnedToggle.IsOn;
        _model.Fov = (float)FovSlider.Value;
        _model.Gap = (float)GapSlider.Value;
        _model.TextSize = (int)Math.Round(TextSizeSlider.Value);
        _model.Distance = (float)DistanceSlider.Value;
        _model.Curvature = (float)CurvatureSlider.Value;
        _model.Size = (float)SizeSlider.Value;
        _model.Tilt = (float)TiltSlider.Value;
        if (BackgroundCombo.SelectedIndex >= 0) _model.Background = BackgroundCombo.SelectedIndex;
        _model.BackgroundBrightness = (float)BackgroundSlider.Value / 100f;
        _model.GazeCursor = GazeToggle.IsOn;
        _model.GatherWindows = GatherToggle.IsOn;
        _model.K1 = (float)K1Slider.Value;
        _model.K2 = (float)K2Slider.Value;
        _server.SettingsChanged();
    }

    private void Changed(object sender, RoutedEventArgs e)
    {
        Save();
        if (ReferenceEquals(sender, FlatWallToggle))
        {
            DrawLayout();  // the preview shows the wall or the gaps
            DrawPlacement();  // the wall's block or the hand placement
        }
    }

    private void BackgroundChanged(object sender, SelectionChangedEventArgs e)
    {
        Save();
        ShowBackgroundImage();
    }

    private void ShowBackgroundImage()
    {
        bool picture = BackgroundCombo.SelectedIndex == 5;
        BackgroundImagePanel.Visibility = picture ? Visibility.Visible : Visibility.Collapsed;
        BackgroundSlider.Visibility = BackgroundCombo.SelectedIndex > 0 ? Visibility.Visible : Visibility.Collapsed;
        string path = _model.BackgroundImage;
        BackgroundImageText.Text = string.IsNullOrEmpty(path) ? "Ninguna elegida" : System.IO.Path.GetFileName(path);
    }

    private async void ChooseBackground_Click(object sender, RoutedEventArgs e)
    {
        var picker = new Windows.Storage.Pickers.FileOpenPicker();
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        foreach (var ext in new[] { ".jpg", ".jpeg", ".png", ".bmp", ".heic", ".webp", ".tif", ".tiff" }) picker.FileTypeFilter.Add(ext);
        var file = await picker.PickSingleFileAsync();
        if (file == null) return;
        _model.BackgroundImage = file.Path;
        _model.Save();
        _server.SettingsChanged();
        ShowBackgroundImage();
    }

    private void SliderChanged(object sender, RangeBaseValueChangedEventArgs e) => Save();

    // The driver prepared once (pool of monitors, duplicates removed, restarted): Nimbo stops
    // first, the driver restarts underneath it.
    private async void SetupDisplays_Click(object sender, RoutedEventArgs e)
    {
        SetupDisplaysButton.IsEnabled = false;
        if (_server.IsRunning) await _server.StopAsync();
        SetupDisplaysText.Text = "Preparando… (acepta el permiso de administrador)";
        var (_, message) = await DisplaySetup.RunAsync();
        SetupDisplaysText.Text = message;
        SetupDisplaysButton.IsEnabled = true;
    }

    private void Report_Click(object sender, RoutedEventArgs e)
    {
        var zip = _server.MakeReport();
        if (zip == null)
        {
            LiveDiagnosticsText.Text = "No se pudo crear el informe (detalles en crash.log).";
            return;
        }
        Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{zip}\"") { UseShellExecute = true });
    }

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (File.Exists(ServerController.LogPath))
        {
            Process.Start(new ProcessStartInfo(ServerController.LogPath) { UseShellExecute = true });
        }
    }

    private void Done_Click(object sender, RoutedEventArgs e) => AppWindow.Hide();

    private void Recalibrate_Click(object sender, RoutedEventArgs e) => RecalibrateRequested?.Invoke();

    private void ResetCalibration_Click(object sender, RoutedEventArgs e)
    {
        _model.ResetCalibration();
        Load();
        _server.Restart();
    }

    private void Startup_Toggled(object sender, RoutedEventArgs e)
    {
        if (!_updating) Startup.Enabled = StartupToggle.IsOn;
    }

    // --- layout editor ----------------------------------------------------------------------------

    // The one-click layouts of the macOS window (Templates in MainView.swift).
    private void Template_Click(object sender, RoutedEventArgs e)
    {
        (int, int)[] cells = ((sender as Button)?.Tag as string) switch
        {
            "one" => new[] { (0, 0) },
            "twoStacked" => new[] { (0, 0), (0, 1) },
            "threeArc" => new[] { (0, 0), (1, 0), (2, 0) },
            _ => new[] { (0, 0), (1, 0) },
        };
        _model.Cells = cells;
        _server.SettingsChanged();
        DrawLayout();
        DrawPlacement();
    }

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

    // --- hand placement (SpatialEditor.swift on macOS) -------------------------------------------

    private const float SpanDeg = 150f;  // horizontal degrees across the canvas
    private const float SnapDeg = 1.5f;
    private readonly List<Border> _placeViews = new();
    private int _placeSelected;
    private int _dragging = -1;
    private Windows.Foundation.Point _dragStart;
    private PanelPos[] _dragPoses = Array.Empty<PanelPos>();
    private PanelPos[]? _dragPlaced;           // the model's hand placement when the drag began (null: the grid)
    private PanelPos _dragPose;                // where the pointer has the card (unsnapped)
    private PanelPos? _dragTarget;             // where it lands; null: nowhere, the drop is undone
    private (int Col, int Row)? _dragCell;     // on the wall: the cell it lands on
    private Microsoft.UI.Xaml.Shapes.Rectangle? _placeGhost;

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
        // Where the dragged card will land (shown while dragging).
        _placeGhost = new Microsoft.UI.Xaml.Shapes.Rectangle
        {
            StrokeThickness = 1.5,
            StrokeDashArray = new DoubleCollection { 4, 3 },
            RadiusX = 6,
            RadiusY = 6,
            Visibility = Visibility.Collapsed,
        };
        PlaceCanvas.Children.Add(_placeGhost);

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
        PlaceScale.IsEnabled = !OnWall(_placeSelected);
        PlaceReset.IsEnabled = _model.HasPlacement && !_model.WallActive;
        PlaceHint.Text = _model.WallActive
            ? "Pared continua: los monitores forman un bloque. Arrastra uno a otra casilla (o sobre otro para intercambiarlos); las ventanas flotantes se colocan libres."
            : "Arrastra cada monitor a su sitio; se mueve en las gafas a la vez. Se alinea solo con los demás (mantén Alt para colocarlo libre). Elige uno para cambiar su tamaño.";
    }

    // The wall's monitors move between cells; floating windows are always free.
    private bool OnWall(int i) => _model.WallActive && i < MonitorCount;

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
        _dragPlaced = _model.HasPlacement ? _model.Poses : null;
        _dragPose = _dragPoses[i];
        _dragTarget = _dragPoses[i];
        _dragCell = null;
        view.CapturePointer(e.Pointer);
        Microsoft.UI.Xaml.Controls.Canvas.SetZIndex(view, 1);
        var poses = PlaceItems();
        for (int k = 0; k < poses.Length; ++k) PositionView(k, poses[k]);
        PlaceSelected.Text = i < MonitorCount ? $"Monitor {i + 1}" : $"Ventana {PlaceLabel(i)}";
        _updating = true;
        PlaceScale.Value = poses[i].Scale;
        PlaceScale.IsEnabled = !OnWall(i);
        _updating = false;
        e.Handled = true;
    }

    // The card stays under the pointer (the start pose plus the whole translation, never added up
    // step by step); the outline shows where it lands, and the glasses follow that.
    private void PlaceMoved(int i, PointerRoutedEventArgs e)
    {
        if (_dragging != i) return;
        var pt = e.GetCurrentPoint(PlaceCanvas).Position;
        var poses = (PanelPos[])_dragPoses.Clone();
        var p = poses[i] with
        {
            Yaw = poses[i].Yaw + (float)((pt.X - _dragStart.X) / Ppd),
            Pitch = poses[i].Pitch - (float)((pt.Y - _dragStart.Y) / Ppd),
        };
        _dragPose = p;
        PositionView(i, p);
        if (OnWall(i))
        {
            var cell = WallCell(p);
            _dragCell = _model.MoveCell(i, cell, apply: false) ? cell : null;
            _dragTarget = _dragCell is { } c ? WallPose(c) : null;
            ShowGhost(_dragTarget ?? _dragPoses[i], _dragTarget != null);
            e.Handled = true;
            return;
        }
        bool free = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        var snapped = free ? p : Snap(p, i, poses);
        poses[i] = snapped;
        bool fits = i >= MonitorCount || !_model.PoseOverlaps(poses.Take(MonitorCount).ToArray(), i);
        _dragTarget = fits ? snapped : null;
        ShowGhost(fits ? snapped : _dragPoses[i], fits);
        if (fits)
        {
            PlaceWrite(poses, i);
            _server.SettingsChanged();  // live on the glasses
        }
        e.Handled = true;
    }

    // Lands where the outline was; with no outline (over another monitor, or a cell that would
    // split the wall) the card glides back and the glasses go back to how they were.
    private void PlaceReleased(UIElement view, PointerRoutedEventArgs e)
    {
        int i = _dragging;
        _dragging = -1;
        view.ReleasePointerCapture(e.Pointer);
        if (i < 0) return;
        if (_placeGhost != null) _placeGhost.Visibility = Visibility.Collapsed;
        var land = _dragTarget ?? _dragPoses[i];
        if (OnWall(i))
        {
            if (_dragCell is { } cell && _model.MoveCell(i, cell)) _server.SettingsChanged();
        }
        else if (_dragTarget == null)
        {
            if (i < MonitorCount)
            {
                if (_dragPlaced != null) _model.Poses = _dragPlaced;
                else _model.ResetPlacement();
            }
            else PlaceWrite(_dragPoses, i);
            _server.SettingsChanged();
        }
        _model.SaveFloating();
        var c = ToCanvas(land.Yaw, land.Pitch);
        var b = (FrameworkElement)view;
        GlideTo(view, c.X - b.Width / 2, c.Y - b.Height / 2, () =>
        {
            DrawPlacement();
            DrawLayout();
        });
    }

    private void ShowGhost(PanelPos p, bool lands)
    {
        if (_placeGhost == null) return;
        _placeGhost.Width = _model.Size * p.Scale * Ppd;
        _placeGhost.Height = AppModel.VerticalArc(_model.Size * p.Scale) * Ppd;
        byte a = lands ? (byte)230 : (byte)140;
        _placeGhost.Stroke = new SolidColorBrush(lands ? Microsoft.UI.ColorHelper.FromArgb(a, 140, 220, 200)
                                                       : Microsoft.UI.ColorHelper.FromArgb(a, 170, 170, 170));
        _placeGhost.Fill = new SolidColorBrush(lands ? Microsoft.UI.ColorHelper.FromArgb(30, 140, 220, 200)
                                                     : Microsoft.UI.ColorHelper.FromArgb(25, 170, 170, 170));
        var c = ToCanvas(p.Yaw, p.Pitch);
        Microsoft.UI.Xaml.Controls.Canvas.SetLeft(_placeGhost, c.X - _placeGhost.Width / 2);
        Microsoft.UI.Xaml.Controls.Canvas.SetTop(_placeGhost, c.Y - _placeGhost.Height / 2);
        _placeGhost.Visibility = Visibility.Visible;
    }

    // A card moved softly to its place (0.35 s, as Theme.ease on macOS), then `done`.
    private static void GlideTo(UIElement view, double left, double top, Action done)
    {
        var sb = new Storyboard();
        foreach (var (property, to) in new[] { ("(Canvas.Left)", left), ("(Canvas.Top)", top) })
        {
            var a = new DoubleAnimation
            {
                To = to,
                Duration = new Duration(TimeSpan.FromMilliseconds(350)),
                EnableDependentAnimation = true,
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut },
            };
            Storyboard.SetTarget(a, view);
            Storyboard.SetTargetProperty(a, property);
            sb.Children.Add(a);
        }
        sb.Completed += (_, _) => done();
        sb.Begin();
    }

    // The wall's cell (normalised grid) whose place is nearest a pose; may lie beyond the block.
    private (int Col, int Row) WallCell(PanelPos p)
    {
        var (midC, midR) = WallMid();
        float w = _model.Size, h = AppModel.VerticalArc(_model.Size);
        return ((int)MathF.Round(p.Yaw / w + midC), (int)MathF.Round(midR - p.Pitch / h));
    }

    private PanelPos WallPose((int Col, int Row) c)
    {
        var (midC, midR) = WallMid();
        float w = _model.Size, h = AppModel.VerticalArc(_model.Size);
        return new PanelPos((c.Col - midC) * w, (midR - c.Row) * h);
    }

    private (float, float) WallMid()
    {
        var cells = _model.Cells;
        if (cells.Length == 0) return (0, 0);
        int c0 = cells.Min(c => c.Col), r0 = cells.Min(c => c.Row);
        return ((cells.Max(c => c.Col) - c0) / 2f, (cells.Max(c => c.Row) - r0) / 2f);
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
        if (_updating || OnWall(_placeSelected)) return;
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

    // The grid of monitors as seen from the front, numbered in panel order. On the flat wall
    // (the default) they touch edge to edge, a thin line between them; otherwise apart.
    private void DrawLayout()
    {
        LayoutPreview.Children.Clear();
        double inset = _model.FlatWall ? 0.5 : 3;
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
                Width = cw - 2 * inset,
                Height = ch - 2 * inset,
                RadiusX = _model.FlatWall ? 0 : 4,
                RadiusY = _model.FlatWall ? 0 : 4,
                Fill = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(140, 165, 205, 200)),
            };
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(rect, ox + (cells[i].Col - c0) * cw + inset);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(rect, oy + (cells[i].Row - r0) * ch + inset);
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
