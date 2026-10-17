using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Nimbo.Core;

// Every shortcut (Windows and macOS keys), for the cheat sheet in the quick panel. The same
// list as the one in Settings and on macOS (SettingsView.swift, ShortcutSheet).
internal static class Shortcuts
{
    public static readonly (string Win, string Mac, string What)[] Rows =
    {
        ("Ctrl+Alt+N", "⌘⌥N", "Abrir o cerrar el panel de Nimbo"),
        ("Ctrl+Alt+1", "⌘⌥1", "Disposición «1 Pantalla Curva»"),
        ("Ctrl+Alt+2", "⌘⌥2", "Disposición «Doble Monitor»"),
        ("Ctrl+Alt+3", "⌘⌥3", "Disposición «Triple Ultrawide»"),
        ("Ctrl+Alt+C", "⌘⌥C", "Recentrar: las pantallas vuelven delante de ti"),
        ("Ctrl+Alt+G", "⌘⌥G", "Modo fijo de las gafas: ellas sostienen la imagen, quietísima, todo dentro de su imagen (otra vez: espacio a tamaño real)"),
        ("Ctrl+Alt+J", "⌘⌥J", "Llevar el cursor a la pantalla que miras"),
        ("Ctrl+Alt+M", "⌘⌥M", "Modo fijo: la pantalla activa cambia solo con el ratón / también mirándola"),
        ("Ctrl+Alt+S", "⌘⌥S", "Prueba: gafas en modo steady en vez de follow (modo normal)"),
        ("Ctrl+Alt+K", "⌘⌥K", "Modo código: las pantallas mucho más cerca, zoom 170 % (otra vez: como estaban)"),
        ("Ctrl+Alt++  -", "⌘⌥+  ⌘⌥-", "Zoom: acercar / alejar las pantallas"),
        ("Ctrl+Alt+0", "⌘⌥0", "Zoom al 100 %"),
        ("Ctrl+Alt+A", "⌘⌥A", "Ver de lejos todas las pantallas (otra vez: como estabas)"),
        ("Ctrl+Alt+O", "⌘⌥O", "Modo enfoque: solo la pantalla que miras (otra vez: todas)"),
        ("Ctrl+Alt+P", "⌘⌥P", "Modo cine: la pantalla que miras, delante y grande"),
        ("Ctrl+Alt+T", "⌘⌥T", "Cine en 3D con IA: modo cine con relieve (gafas en modo 3D)"),
        ("—", "⌘⌥R", "Grabar lo que ves en las gafas (solo Mac por ahora)"),
        ("Ctrl+Alt+↓  ↑", "⌘⌥↓  ⌘⌥↑", "Brillo de la pantalla de las gafas: bajar / subir (8 niveles)"),
        ("Ctrl+Alt+Mayús+-  +", "⌘⌥⇧-  ⌘⌥⇧+", "Calibrar el campo de visión: si las pantallas se mueven con la cabeza (-) o contra ella (+)"),
        ("Ctrl+Alt+Q", "⌘⌥Q", "Parar Nimbo (todo vuelve a la normalidad)"),
    };

    // A three-column table: Windows key, macOS key, what it does.
    public static UIElement Sheet()
    {
        var grid = new Grid { ColumnSpacing = 16, RowSpacing = 6, MaxWidth = 560 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        void Add(int row, int col, string text, bool strong = false, double opacity = 1, bool wrap = false)
        {
            var t = new TextBlock { Text = text, Opacity = opacity, FontSize = 13 };
            if (strong) t.FontWeight = FontWeights.SemiBold;
            if (wrap) t.TextWrapping = TextWrapping.Wrap;
            Grid.SetRow(t, row);
            Grid.SetColumn(t, col);
            grid.Children.Add(t);
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
