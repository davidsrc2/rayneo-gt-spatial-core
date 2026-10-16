using System.Diagnostics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Nimbo.Core;
using Nimbo.Native;

namespace Nimbo.Views;

// First-run wizard; also opened from Settings to calibrate again (starting at that step).
public sealed partial class OnboardingWindow : Window
{
    public enum Step
    {
        Welcome = 0,
        Driver,
        Calibration,
        Scale,
    }

    private const string DriverUrl = "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/latest";

    private readonly AppModel _model;
    private readonly ServerController _server;
    private readonly DispatcherQueue _ui;
    private Step _step;
    private bool _calibrating;
    private IntPtr _calibration;
    private bool _destroying;
    private bool _singleStep;  // opened to recalibrate: only that step

    public event Action? Finished;

    internal OnboardingWindow(AppModel model, ServerController server)
    {
        _model = model;
        _server = server;
        _ui = DispatcherQueue.GetForCurrentThread();
        InitializeComponent();
        Title = "Nimbo";
        SystemBackdrop = new MicaBackdrop();
        AppWindow.SetIcon(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "Nimbo.ico"));
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.IsMaximizable = false;
            p.IsResizable = false;
        }
        AppWindow.Closing += (_, e) =>
        {
            if (_destroying) return;
            e.Cancel = true;
            CancelCalibration();
            AppWindow.Hide();
        };
        foreach (var label in AppModel.TextSizeLabels) TextSizes.Items.Add(label);
    }

    public void Open(Step step, bool singleStep)
    {
        _singleStep = singleStep;
        Show(step);
        WindowHelpers.Resize(this, 640, 520);
        WindowHelpers.Center(this);
        WindowHelpers.BringToFront(this);
    }

    public void Destroy()
    {
        _destroying = true;
        CancelCalibration();
        Close();
    }

    private void Show(Step step)
    {
        _step = step;
        WelcomeStep.Visibility = step == Step.Welcome ? Visibility.Visible : Visibility.Collapsed;
        DriverStep.Visibility = step == Step.Driver ? Visibility.Visible : Visibility.Collapsed;
        CalibrationStep.Visibility = step == Step.Calibration ? Visibility.Visible : Visibility.Collapsed;
        ScaleStep.Visibility = step == Step.Scale ? Visibility.Visible : Visibility.Collapsed;
        StepCounter.Text = _singleStep ? "" : $"Paso {(int)step + 1} de 4";
        BackButton.Visibility = step > Step.Welcome && !_singleStep ? Visibility.Visible : Visibility.Collapsed;
        SkipButton.Visibility = step == Step.Calibration && !_singleStep ? Visibility.Visible : Visibility.Collapsed;
        NextButton.Content = step == Step.Scale || _singleStep ? "Terminar" : "Continuar";
        if (step == Step.Driver) RefreshDriver();
        if (step == Step.Calibration) ResetCalibration();
        if (step == Step.Scale) TextSizes.SelectedIndex = _model.TextSize;
    }

    private void Next_Click(object sender, RoutedEventArgs e)
    {
        if (_step == Step.Scale || _singleStep)
        {
            _model.OnboardingDone = true;
            _model.Save();
            AppWindow.Hide();
            Finished?.Invoke();
            return;
        }
        Show(_step + 1);
    }

    private void Back_Click(object sender, RoutedEventArgs e)
    {
        CancelCalibration();
        if (_step > Step.Welcome) Show(_step - 1);
    }

    private void Skip_Click(object sender, RoutedEventArgs e)
    {
        CancelCalibration();
        Show(_step + 1);
    }

    // --- driver -----------------------------------------------------------------------------------

    private void RefreshDriver()
    {
        // Installed counts too while switched off: Nimbo switches it on when it starts.
        bool ok = NimboNative.nimbo_virtual_driver_available() != 0 || DisplaySetup.DriverInstalled;
        DriverInfo.Severity = ok ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
        DriverInfo.Title = ok ? "Instalado" : "No instalado";
        DriverInfo.Message = ok
            ? "Nimbo puede crear los monitores virtuales."
            : DisplaySetup.DriverShipped
                ? "Pulsa «Preparar pantallas virtuales»: instala el driver que viene con Nimbo (pide permiso de administrador)."
                : "Descárgalo, ejecuta «VDD Control» y pulsa Install. Luego vuelve aquí y pulsa «Comprobar de nuevo».";
    }

    private void DownloadDriver_Click(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(DriverUrl) { UseShellExecute = true });

    private void CheckDriver_Click(object sender, RoutedEventArgs e) => RefreshDriver();

    private async void SetupDisplays_Click(object sender, RoutedEventArgs e)
    {
        SetupDisplaysButton.IsEnabled = false;
        var (ok, message) = await DisplaySetup.RunAsync();
        SetupDisplaysButton.IsEnabled = true;
        RefreshDriver();
        DriverInfo.Severity = ok ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
        DriverInfo.Message = message;
    }

    // --- calibration ------------------------------------------------------------------------------

    private static string Prompt(NimboNative.CalibStep step) => step switch
    {
        NimboNative.CalibStep.Bias => "Deja las gafas sobre la mesa, sin tocarlas…",
        NimboNative.CalibStep.Center => "Póntelas y mira al frente, con la cabeza recta",
        NimboNative.CalibStep.Up => "Mira hacia ARRIBA (unos 30°) y quédate quieto",
        NimboNative.CalibStep.Down => "Mira hacia ABAJO (unos 30°) y quédate quieto",
        NimboNative.CalibStep.Left => "Gira a la IZQUIERDA (unos 30°) y quédate quieto",
        NimboNative.CalibStep.Right => "Gira a la DERECHA (unos 30°) y quédate quieto",
        _ => "",
    };

    private static string HintText(string hint) => hint switch
    {
        "waiting for rest" => "Esperando a que estén quietas",
        "put the glasses on" => "Ponte las gafas",
        "move" => "Muévete",
        "further" => "Un poco más",
        "hold" => "Quieto…",
        _ => hint,
    };

    private void ResetCalibration()
    {
        CalibProgress.Value = 0;
        CalibPrompt.Text = "";
        CalibHint.Text = "";
        CalibResult.IsOpen = false;
        CalibStart.Visibility = Visibility.Visible;
        CalibCancel.Visibility = Visibility.Collapsed;
        NextButton.IsEnabled = true;
    }

    private async void CalibStart_Click(object sender, RoutedEventArgs e)
    {
        if (_calibrating) return;
        if (_server.IsRunning) await _server.StopAsync();  // both need the glasses' IMU
        if (NimboNative.nimbo_glasses_connected() == 0)
        {
            ShowResult(InfoBarSeverity.Warning, "Conecta las gafas", "Conecta las RayNeo GT y vuelve a intentarlo.");
            return;
        }
        _calibrating = true;
        CalibStart.Visibility = Visibility.Collapsed;
        CalibCancel.Visibility = Visibility.Visible;
        CalibResult.IsOpen = false;
        NextButton.IsEnabled = false;
        _calibration = NimboNative.nimbo_calibration_create();
        var handle = _calibration;
        string error = "";
        int code = await Task.Run(() => NimboNative.RunCalibration(handle, AppModel.CalibrationPath,
                                                                   st => _ui.TryEnqueue(() => OnStatus(st)), out error));
        NimboNative.nimbo_calibration_destroy(handle);
        _calibration = IntPtr.Zero;
        _calibrating = false;
        CalibCancel.Visibility = Visibility.Collapsed;
        CalibStart.Visibility = Visibility.Visible;
        CalibStart.Content = code == 0 ? "Calibrar otra vez" : "Reintentar";
        NextButton.IsEnabled = true;
        if (code == 0)
        {
            CalibProgress.Value = 5;
            CalibPrompt.Text = "";
            CalibHint.Text = "";
            ShowResult(InfoBarSeverity.Success, "Listo", "Calibración guardada.");
        }
        else
        {
            ShowResult(InfoBarSeverity.Error, code == 2 ? "Calibración descartada" : "Calibración interrumpida",
                       code == 2 ? "Los movimientos no eran claros: vuelve a intentarlo, despacio y sin inclinar la cabeza." : error);
        }
    }

    private void OnStatus(NimboNative.CalibStatus st)
    {
        CalibProgress.Value = (int)st.Step;
        CalibPrompt.Text = st.Returning != 0 ? $"{st.CapturedDeg:0}°. Vuelve al centro y quédate quieto" : Prompt(st.Step);
        CalibHint.Text = HintText(NimboNative.FromPtr(st.Hint)) + (st.BiasOk != 0 ? "" : " · midiendo el sensor");
    }

    private void ShowResult(InfoBarSeverity severity, string title, string message)
    {
        CalibResult.Severity = severity;
        CalibResult.Title = title;
        CalibResult.Message = message;
        CalibResult.IsOpen = true;
    }

    private void CalibCancel_Click(object sender, RoutedEventArgs e) => CancelCalibration();

    private void CancelCalibration()
    {
        if (_calibrating && _calibration != IntPtr.Zero) NimboNative.nimbo_calibration_cancel(_calibration);
    }

    // --- text size ---------------------------------------------------------------------------------

    private void TextSizes_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TextSizes.SelectedIndex < 0) return;
        _model.TextSize = TextSizes.SelectedIndex;
        _server.SettingsChanged();
    }
}
