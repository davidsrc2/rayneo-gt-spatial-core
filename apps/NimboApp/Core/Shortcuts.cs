using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Nimbo.Core;

// Every shortcut (Windows and macOS keys), for the cheat sheet in the quick panel. The same
// list as the one in Settings and on macOS (SettingsView.swift, ShortcutSheet).
internal static class Shortcuts
{
    public static readonly (string Win, string Mac, string What)[] Rows =
    {
        ("Ctrl+Alt+N", "⌘⌥N", "Abrir o cerrar el panel de Nimbo"),
        ("Ctrl+Alt+H", "—", "Esta lista de atajos, en grande"),
        ("Ctrl+Alt+1", "⌘⌥1", "Disposición «1 Pantalla Curva»"),
        ("Ctrl+Alt+2", "⌘⌥2", "Disposición «Doble Monitor»"),
        ("Ctrl+Alt+3", "⌘⌥3", "Disposición «Triple Ultrawide»"),
        ("Ctrl+Alt+C", "⌘⌥C", "Recentrar: las pantallas vuelven delante de ti"),
        ("Ctrl+Alt+G", "⌘⌥G", "Modo fijo de las gafas: ellas sostienen la imagen, quietísima, todo dentro de su imagen (otra vez: espacio a tamaño real)"),
        ("Ctrl+Alt+J", "⌘⌥J", "Llevar el cursor a la pantalla que miras"),
        ("Ctrl+Alt+K", "⌘⌥K", "Modo código: las pantallas mucho más cerca, zoom 170 % (otra vez: como estaban)"),
        ("Ctrl+Alt++  -", "⌘⌥+  ⌘⌥-", "Zoom: acercar / alejar las pantallas"),
        ("Ctrl+Alt+0", "⌘⌥0", "Zoom al 100 %"),
        ("Ctrl+Alt+A", "⌘⌥A", "Ver de lejos todas las pantallas (otra vez: como estabas)"),
        ("Ctrl+Alt+F", "⌘⌥F", "Fondo alrededor de las pantallas: ninguno, lluvia, estrellas, atardecer, aurora o tu imagen"),
        ("Ctrl+Alt+O", "⌘⌥O", "Modo enfoque: solo la pantalla que miras (otra vez: todas)"),
        ("Ctrl+Alt+P", "⌘⌥P", "Modo cine: la pantalla que miras, delante y grande"),
        ("Ctrl+Alt+T", "⌘⌥T", "Cine en 3D con IA: modo cine con relieve (gafas en modo 3D)"),
        ("—", "⌘⌥R", "Grabar lo que ves en las gafas (solo Mac por ahora)"),
        ("Ctrl+Alt+↓  ↑", "⌘⌥↓  ⌘⌥↑", "Brillo de la pantalla de las gafas: bajar / subir (8 niveles)"),
        ("Ctrl+Alt+Mayús+-  +", "⌘⌥⇧-  ⌘⌥⇧+", "Calibrar el campo de visión: si las pantallas se mueven con la cabeza (-) o contra ella (+)"),
        ("Ctrl+Alt+Q", "⌘⌥Q", "Parar Nimbo (todo vuelve a la normalidad)"),
    };

    // A three-column table: Windows key, macOS key, what it does. large: the shortcuts window
    // (bigger text, the Windows keys as key caps).
    public static UIElement Sheet(bool large = false)
    {
        double size = large ? 17 : 13;
        var grid = new Grid { ColumnSpacing = large ? 28 : 16, RowSpacing = large ? 14 : 6, MaxWidth = large ? double.PositiveInfinity : 560 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        void Add(int row, int col, string text, bool strong = false, double opacity = 1, bool wrap = false)
        {
            var t = new TextBlock { Text = text, Opacity = opacity, FontSize = size, VerticalAlignment = VerticalAlignment.Center };
            if (strong) t.FontWeight = FontWeights.SemiBold;
            if (wrap) t.TextWrapping = TextWrapping.Wrap;
            UIElement e = t;
            if (strong && large)  // a key cap
            {
                e = new Border
                {
                    Child = t,
                    Padding = new Thickness(12, 6, 12, 6),
                    CornerRadius = new CornerRadius(8),
                    Background = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(0x26, 0x80, 0x80, 0x80)),
                    HorizontalAlignment = HorizontalAlignment.Left,
                    VerticalAlignment = VerticalAlignment.Center,
                };
            }
            Grid.SetRow((FrameworkElement)e, row);
            Grid.SetColumn((FrameworkElement)e, col);
            grid.Children.Add(e);
        }
        for (int i = 0; i <= Rows.Length; ++i) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Add(0, 0, "Windows", opacity: 0.6);
        Add(0, 1, "Mac", opacity: 0.6);
        Add(0, 2, "Qué hace", opacity: 0.6);
        for (int i = 0; i < Rows.Length; ++i)
        {
            Add(i + 1, 0, Rows[i].Win, strong: true);
            Add(i + 1, 1, Rows[i].Mac, opacity: 0.6);
            Add(i + 1, 2, Rows[i].What, wrap: true);
        }
        return grid;
    }
}
