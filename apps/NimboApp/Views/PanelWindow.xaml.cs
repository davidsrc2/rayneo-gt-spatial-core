using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Nimbo.Core;
using Nimbo.Native;

namespace Nimbo.Views;

// Compact glass panel under the tray icon (QuickPanel.swift on macOS).
public sealed partial class PanelWindow : Window
{
    private const double PanelWidth = 380;

    private readonly AppModel _model;
    private readonly ServerController _server;
    private readonly Action _openSettings;
    private readonly List<(Preset Preset, Button Button, Canvas Glyph)> _cards = new();
    private bool _updating;
    private bool _destroying;
    private DateTime _hiddenAt;

    internal PanelWindow(AppModel model, ServerController server, Action openSettings)
    {
        _model = model;
        _server = server;
        _openSettings = openSettings;
        _updating = true;  // no saving while XAML builds the controls (see SettingsWindow)
        InitializeComponent();
        _updating = false;
        Title = "Nimbo";
        SystemBackdrop = new DesktopAcrylicBackdrop();

        var presenter = OverlappedPresenter.Create();
        presenter.IsResizable = false;
        presenter.IsMaximizable = false;
        presenter.IsMinimizable = false;
        presenter.IsAlwaysOnTop = true;
        presenter.SetBorderAndTitleBar(true, false);
        AppWindow.SetPresenter(presenter);
        AppWindow.IsShownInSwitchers = false;
        AppWindow.SetIcon(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "Nimbo.ico"));
        AppWindow.Closing += (_, e) =>
        {
            if (_destroying) return;
            e.Cancel = true;
            Hide();
        };
        Activated += (_, e) =>
        {
            if (e.WindowActivationState == WindowActivationState.Deactivated) Hide();
        };

        BuildCards();
        _server.Changed += Refresh;
        Refresh();
    }

    public void Toggle()
    {
        if (AppWindow.IsVisible) Hide();
        else if ((DateTime.Now - _hiddenAt).TotalMilliseconds > 300) ShowNearTray();  // the click that hid it
    }

    public void ShowNearTray()
    {
        Refresh();
        // Before the window has been shown once its content is not in a live tree yet and
        // Measure throws E_UNEXPECTED (it did at start-up): show it first at a default height,
        // then fit it once the content is measurable.
        double room = WindowHelpers.RoomNearTray(this);
        double height = room;  // not measurable yet: as tall as the screen allows (it scrolls)
        try
        {
            Root.Measure(new Windows.Foundation.Size(PanelWidth, double.PositiveInfinity));
            if (Root.DesiredSize.Height > 200) height = Math.Min(Root.DesiredSize.Height, room);
        }
        catch (System.Runtime.InteropServices.COMException)
        {
            DispatcherQueue.TryEnqueue(FitHeight);
        }
        WindowHelpers.Resize(this, PanelWidth, height);
        WindowHelpers.MoveNearTray(this);
        WindowHelpers.BringToFront(this);
    }

    // Once shown: the panel's real height, kept near the tray.
    private void FitHeight()
    {
        try
        {
            Root.Measure(new Windows.Foundation.Size(PanelWidth, double.PositiveInfinity));
            if (Root.DesiredSize.Height <= 200) return;
            WindowHelpers.Resize(this, PanelWidth, Math.Min(Root.DesiredSize.Height, WindowHelpers.RoomNearTray(this)));
            WindowHelpers.MoveNearTray(this);
        }
        catch (System.Runtime.InteropServices.COMException)
        {
        }
    }

    public void Destroy()
    {
        _destroying = true;
        _server.Changed -= Refresh;
        Close();
    }

    private void Hide()
    {
        if (!AppWindow.IsVisible) return;
        AppWindow.Hide();
        _hiddenAt = DateTime.Now;
    }

    // --- state -> UI ------------------------------------------------------------------------

    public void Refresh()
    {
        _updating = true;
        Light.Fill = (Brush)Application.Current.Resources[_server.State switch
        {
            NimboNative.State.Tracking => "NimboGreen",
            NimboNative.State.Calibrating or NimboNative.State.Starting => "NimboAmber",
            _ => "NimboGrey",
        }];
        StatusLine.Text = _server.StatusText;
        Power.IsOn = _server.IsRunning;
        Power.IsEnabled = !_server.IsBusy;
        RecenterButton.IsEnabled = _server.IsRunning;
        RecenterButton.Background = new SolidColorBrush(ColorHelper.FromArgb(_server.IsRunning ? (byte)82 : (byte)31, 140, 184, 179));

        var active = _model.ActiveLayout;
        foreach (var (preset, button, glyph) in _cards)
        {
            bool selected = preset.Id == active;
            button.Background = new SolidColorBrush(ColorHelper.FromArgb(selected ? (byte)36 : (byte)13, 255, 255, 255));
            button.BorderBrush = new SolidColorBrush(ColorHelper.FromArgb(selected ? (byte)90 : (byte)20, 255, 255, 255));
            DrawGlyph(glyph, preset, selected && _server.IsRunning ? _server.GazePanel : -1);
        }

        CurvatureSlider.Value = _model.Curvature;
        TiltSlider.Value = _model.Tilt;
        TextSlider.Value = _model.TextSize;
        UpdateValueLabels();

        AutoStart.IsOn = _model.AutoStart;
        BrightnessSlider.Value = _model.Brightness == 0 ? 8 : _model.Brightness;
        NoticeText.Text = _server.Notice ?? "";
        _updating = false;
    }

    private void UpdateValueLabels()
    {
        CurvatureValue.Text = CurvatureSlider.Value < 0.02 ? "Plana" : $"{CurvatureSlider.Value * 100:0} %";
        TiltValue.Text = TiltSlider.Value < 0.02 ? "Rectas" : TiltSlider.Value > 0.98 ? "Hacia ti" : $"{TiltSlider.Value * 100:0} %";
        TextValue.Text = AppModel.TextSizeLabels[(int)Math.Round(TextSlider.Value)];
    }

    // --- layout cards -------------------------------------------------------------------------

    private void BuildCards()
    {
        for (int i = 0; i < AppModel.Layouts.Count; ++i)
        {
            var preset = AppModel.Layouts[i];
            Cards.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var glyph = new Canvas { Width = 90, Height = 30, HorizontalAlignment = HorizontalAlignment.Center };
            var content = new StackPanel { Spacing = 8 };
            content.Children.Add(glyph);
            content.Children.Add(new TextBlock
            {
                Text = preset.Title,
                FontSize = 12,
                TextAlignment = TextAlignment.Center,
                TextWrapping = TextWrapping.Wrap,
                HorizontalAlignment = HorizontalAlignment.Center,
            });
            var button = new Button
            {
                Content = content,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                VerticalAlignment = VerticalAlignment.Stretch,
                MinHeight = 84,
                CornerRadius = new CornerRadius(14),
                BorderThickness = new Thickness(1),
                Padding = new Thickness(4, 8, 4, 8),
            };
            button.Click += (_, _) =>
            {
                _model.Apply(preset);
                _server.SettingsChanged();
                Refresh();
            };
            Grid.SetColumn(button, i);
            Cards.Children.Add(button);
            _cards.Add((preset, button, glyph));
        }
    }

    // Top view: an arc of screens in front of the viewer; `lit` is the screen you look at.
    private static void DrawGlyph(Canvas canvas, Preset preset, int lit)
    {
        canvas.Children.Clear();
        int n = preset.Cells.Length;
        double each = n == 1 ? 50 : 26, gap = 3, total = n * each + (n - 1) * gap;
        for (int i = 0; i < n; ++i)
        {
            double x = (canvas.Width - total) / 2 + i * (each + gap);
            double offset = preset.Curvature > 0.2 && n > 1 ? Math.Abs(i - (n - 1) / 2.0) * 4 : 0;
            var rect = new Rectangle
            {
                Width = each,
                Height = 16,
                RadiusX = 3,
                RadiusY = 3,
                Fill = new SolidColorBrush(ColorHelper.FromArgb(lit == i ? (byte)230 : (byte)130, 165, 205, 200)),
            };
            Canvas.SetLeft(rect, x);
            Canvas.SetTop(rect, 4 + offset);
            canvas.Children.Add(rect);
        }
    }

    // --- input ----------------------------------------------------------------------------------

    private async void Power_Toggled(object sender, RoutedEventArgs e)
    {
        if (_updating || Power.IsOn == _server.IsRunning) return;
        if (Power.IsOn) await _server.StartAsync();
        else await _server.StopAsync();
        Refresh();
    }

    private void Recenter_Click(object sender, RoutedEventArgs e) => _server.Recenter();

    private void Shortcuts_Click(object sender, RoutedEventArgs e)
    {
        var flyout = new Flyout
        {
            Content = new ScrollViewer { Content = Shortcuts.Sheet(), MaxHeight = 480, VerticalScrollBarVisibility = ScrollBarVisibility.Auto },
            Placement = FlyoutPlacementMode.Top,
        };
        flyout.ShowAt(ShortcutsButton);
    }

    private void Slider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_updating) return;
        _model.Curvature = (float)CurvatureSlider.Value;
        _model.Tilt = (float)TiltSlider.Value;
        _model.TextSize = (int)Math.Round(TextSlider.Value);
        UpdateValueLabels();
        _server.SettingsChanged();
    }

    private void Brightness_Changed(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_updating) return;
        _model.Brightness = (int)Math.Round(BrightnessSlider.Value);
        _server.SettingsChanged();
    }

    private void AutoStart_Toggled(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        _model.AutoStart = AutoStart.IsOn;
        _model.Save();
    }

    private void More_Click(object sender, RoutedEventArgs e)
    {
        Hide();
        _openSettings();
    }
}
