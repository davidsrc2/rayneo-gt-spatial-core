using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Nimbo.Core;

namespace Nimbo.Views;

// The shortcut sheet in a window of its own, large and easy to read (the flyout under the
// panel was too small). One instance; closing it only hides it.
internal sealed class ShortcutsWindow : Window
{
    private static ShortcutsWindow? _shared;
    private bool _destroying;

    public static void ShowShared()
    {
        _shared ??= new ShortcutsWindow();
        WindowHelpers.Resize(_shared, 980, Math.Min(860, WindowHelpers.RoomNearTray(_shared, 80)));
        WindowHelpers.Center(_shared);
        WindowHelpers.BringToFront(_shared);
    }

    public static void DestroyShared()
    {
        if (_shared == null) return;
        _shared._destroying = true;
        _shared.Close();
        _shared = null;
    }

    private ShortcutsWindow()
    {
        Title = "Nimbo · Atajos de teclado";
        SystemBackdrop = new MicaBackdrop();
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "Nimbo.ico"));
        if (AppWindow.Presenter is OverlappedPresenter p) p.IsMinimizable = false;
        AppWindow.Closing += (_, e) =>
        {
            if (_destroying) return;
            e.Cancel = true;
            AppWindow.Hide();
        };
        var page = new StackPanel { Padding = new Thickness(40, 32, 40, 36), Spacing = 22 };
        page.Children.Add(new TextBlock { Text = "Atajos de teclado", FontSize = 30, FontWeight = FontWeights.SemiBold });
        page.Children.Add(new TextBlock
        {
            Text = "Funcionan con cualquier app delante mientras Nimbo está en marcha; el panel, el brillo y las disposiciones, también con Nimbo parado.",
            FontSize = 15,
            Opacity = 0.7,
            TextWrapping = TextWrapping.Wrap,
        });
        page.Children.Add(Shortcuts.Sheet(large: true));
        Content = new ScrollViewer { Content = page, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    }
}
