using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace Nimbo;

// Start-up written out instead of the XAML-generated Main, so a failure before the first
// window (which a tray app hides completely) leaves its steps and the error in crash.log.
public static class Program
{
    [DllImport("Microsoft.ui.xaml.dll")]
    private static extern void XamlCheckProcessRequirements();

    [STAThread]
    public static void Main(string[] args)
    {
        App.Trace("main: start");
        try
        {
            XamlCheckProcessRequirements();
            App.Trace("main: xaml requirements ok");
            WinRT.ComWrappersSupport.InitializeComWrappers();
            Application.Start(_ =>
            {
                try
                {
                    var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
                    SynchronizationContext.SetSynchronizationContext(context);
                    App.Trace("main: creating the app");
                    _ = new App();
                }
                catch (Exception e)
                {
                    App.CrashLog("app", e);
                    throw;
                }
            });
        }
        catch (Exception e)
        {
            App.CrashLog("main", e);
            throw;
        }
    }
}
