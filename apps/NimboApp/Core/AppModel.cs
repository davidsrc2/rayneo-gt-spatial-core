using Nimbo.Native;

namespace Nimbo.Core;

// One-click layout or usage profile from the shared core (core/presets.h), with its title.
internal sealed record Preset(string Id, string Title, (int Col, int Row)[] Cells, float Curvature, float Size, float Gap);
internal sealed record ProfilePreset(string Id, string Title, string LayoutId, float Distance, int TextSize, float? Curvature,
                                     float? Size);

// Every user-facing setting, kept in %LOCALAPPDATA%\Nimbo\settings.json (the same file format
// as the macOS app), and how it maps to the server options.
internal sealed unsafe class AppModel
{
    public static string DataDirectory { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Nimbo");
    public static string SettingsPath => Path.Combine(DataDirectory, "settings.json");
    public static string CalibrationPath => Path.Combine(DataDirectory, "calibration.json");

    public static readonly string[] TextSizeLabels = { "Normal", "Algo más grande", "Grande", "Más grande", "Muy grande" };

    private static readonly Dictionary<string, string> LayoutTitles = new()
    {
        ["curve"] = "1 Pantalla Curva",
        ["dual"] = "Doble Monitor",
        ["triple"] = "Triple Ultrawide",
    };
    private static readonly Dictionary<string, string> ProfileTitles = new() { ["code"] = "Código", ["cinema"] = "Cine" };

    public static IReadOnlyList<Preset> Layouts { get; } = LoadLayouts();
    public static IReadOnlyList<ProfilePreset> Profiles { get; } = LoadProfiles();

    private NimboNative.Settings _s;

    public AppModel()
    {
        Directory.CreateDirectory(DataDirectory);
        if (!NimboNative.LoadSettings(SettingsPath, out _s)) _s = NimboNative.nimbo_default_settings();
        AdoptExistingCalibration();
    }

    public void Save() => NimboNative.SaveSettings(SettingsPath, _s);

    // --- settings -----------------------------------------------------------------------

    public (int Col, int Row)[] Cells
    {
        get
        {
            var cells = new (int, int)[_s.CellCount];
            for (int i = 0; i < _s.CellCount; ++i) cells[i] = (_s.Cols[i], _s.Rows[i]);
            return cells;
        }
        set
        {
            _s.CellCount = Math.Min(value.Length, NimboNative.MaxPanels);
            for (int i = 0; i < _s.CellCount; ++i)
            {
                _s.Cols[i] = value[i].Col;
                _s.Rows[i] = value[i].Row;
            }
        }
    }

    public int TextSize { get => _s.TextSize; set => _s.TextSize = Math.Clamp(value, 0, TextSizeLabels.Length - 1); }
    public float Distance { get => _s.DistanceM; set => _s.DistanceM = value; }
    public float Gap { get => _s.GapDeg; set => _s.GapDeg = value; }
    public float Curvature { get => _s.Curvature; set => _s.Curvature = value; }
    public float Size { get => _s.SizeDeg; set => _s.SizeDeg = value; }
    public bool AutoStart { get => _s.AutoStart != 0; set => _s.AutoStart = B(value); }
    public float Fov { get => _s.FovYDeg; set => _s.FovYDeg = value; }
    public float Ipd { get => _s.IpdMm; set => _s.IpdMm = value; }
    public bool Stereo { get => _s.Stereo != 0; set => _s.Stereo = B(value); }
    public float DeadZone { get => _s.DeadZoneDeg; set => _s.DeadZoneDeg = value; }
    public bool GazeCursor { get => _s.GazeCursor != 0; set => _s.GazeCursor = B(value); }
    public bool DimOthers { get => _s.DimOthers != 0; set => _s.DimOthers = B(value); }
    public bool GatherWindows { get => _s.GatherWindows != 0; set => _s.GatherWindows = B(value); }
    public float K1 { get => _s.K1; set => _s.K1 = value; }
    public float K2 { get => _s.K2; set => _s.K2 = value; }
    public bool OnboardingDone { get => _s.OnboardingDone != 0; set => _s.OnboardingDone = B(value); }
    public bool GlassesPinned { get => _s.GlassesPinned != 0; set => _s.GlassesPinned = B(value); }
    public int Tracking { get => _s.Tracking; set => _s.Tracking = Math.Clamp(value, 0, 2); }  // 0 both, 1 left/right, 2 up/down

    public bool HasCalibration => File.Exists(CalibrationPath);

    // --- layouts and profiles -------------------------------------------------------------

    private static (int, int)[] Normalized((int Col, int Row)[] cells)
    {
        if (cells.Length == 0) return cells;
        int c0 = cells.Min(c => c.Col), r0 = cells.Min(c => c.Row);
        return cells.Select(c => (c.Col - c0, c.Row - r0)).ToArray();
    }

    public string? ActiveLayout
    {
        get
        {
            var n = Normalized(Cells);
            return Layouts.FirstOrDefault(p => p.Cells.SequenceEqual(n))?.Id;
        }
    }

    public string? ActiveProfile => Profiles.FirstOrDefault(p =>
        ActiveLayout == p.LayoutId && Math.Abs(Distance - p.Distance) < 0.05f && TextSize == p.TextSize)?.Id;

    public void Apply(Preset p)
    {
        Cells = p.Cells;
        Curvature = p.Curvature;
        Size = p.Size;
        Gap = p.Gap;
    }

    public void Apply(ProfilePreset p)
    {
        var layout = Layouts.FirstOrDefault(l => l.Id == p.LayoutId);
        if (layout != null) Apply(layout);
        if (p.Curvature is float c) Curvature = c;
        if (p.Size is float s) Size = s;
        Distance = p.Distance;
        TextSize = p.TextSize;
    }

    // --- layout editor (LayoutEditor.swift) ---------------------------------------------------

    public const int MaxMonitors = 6;

    // Adds a monitor touching the arrangement: right of a row first, then below a column,
    // keeping the whole layout within 4 columns x 3 rows.
    public bool AddMonitor()
    {
        var cells = Cells.ToList();
        if (cells.Count >= MaxMonitors) return false;
        var candidates = new List<(int Col, int Row)>();
        foreach (var r in cells.Select(c => c.Row).Distinct().OrderBy(r => r))
            candidates.Add((cells.Where(c => c.Row == r).Max(c => c.Col) + 1, r));
        foreach (var c in cells.Select(c => c.Col).Distinct().OrderBy(c => c))
            candidates.Add((c, cells.Where(x => x.Col == c).Max(x => x.Row) + 1));
        foreach (var p in candidates)
        {
            if (cells.Contains(p)) continue;
            var all = cells.Append(p).ToList();
            if (all.Max(c => c.Col) - all.Min(c => c.Col) + 1 > 4 || all.Max(c => c.Row) - all.Min(c => c.Row) + 1 > 3) continue;
            Cells = all.ToArray();
            return true;
        }
        return false;
    }

    public bool RemoveMonitor()
    {
        var cells = Cells;
        if (cells.Length <= 1) return false;
        var next = cells.Take(cells.Length - 1).ToArray();
        if (!IsConnected(next)) return false;
        Cells = next;
        return true;
    }

    // Monitors must touch edge to edge, as Windows requires for an arrangement.
    public static bool IsConnected((int Col, int Row)[] cells)
    {
        if (cells.Length == 0) return true;
        var all = cells.ToHashSet();
        var seen = new HashSet<(int, int)> { cells[0] };
        var stack = new Stack<(int Col, int Row)>();
        stack.Push(cells[0]);
        while (stack.Count > 0)
        {
            var c = stack.Pop();
            foreach (var n in new[] { (c.Col + 1, c.Row), (c.Col - 1, c.Row), (c.Col, c.Row + 1), (c.Col, c.Row - 1) })
            {
                if (all.Contains(n) && seen.Add(n)) stack.Push(n);
            }
        }
        return seen.Count == all.Count && all.Count == cells.Length;
    }

    // --- server options -------------------------------------------------------------------

    public NimboNative.Options ToOptions()
    {
        var o = NimboNative.nimbo_default_options();
        var s = _s;
        NimboNative.nimbo_options_from_settings(&s, &o);
        NimboNative.SetField(o.CalibrationPath, 260, CalibrationPath);
        o.Hotkeys = 1;
        if (Environment.GetEnvironmentVariable("NIMBO_SIM") != null)  // development without the glasses
        {
            o.Sim = 1;
            o.Windowed = 1;
        }
        return o;
    }

    // Whether going from `a` to `b` needs the monitors rebuilt (stop + start).
    public static bool NeedsRestart(NimboNative.Options a, NimboNative.Options b)
    {
        if (a.Displays != b.Displays || a.LayoutCount != b.LayoutCount || a.VdScale != b.VdScale ||
            a.VdWidth != b.VdWidth || a.DeadZoneDeg != b.DeadZoneDeg || a.GatherWindows != b.GatherWindows)
        {
            return true;
        }
        for (int i = 0; i < a.LayoutCount; ++i)
        {
            if (a.LayoutCols[i] != b.LayoutCols[i] || a.LayoutRows[i] != b.LayoutRows[i]) return true;
        }
        return false;
    }

    // --- helpers ----------------------------------------------------------------------------

    private static byte B(bool v) => v ? (byte)1 : (byte)0;

    private static List<Preset> LoadLayouts()
    {
        var list = new List<Preset>();
        int n = NimboNative.nimbo_layout_count();
        for (int i = 0; i < n; ++i)
        {
            NimboNative.Layout l;
            if (NimboNative.nimbo_layout_at(i, &l) == 0) continue;
            var id = NimboNative.FromPtr(l.Id);
            var cells = new (int, int)[l.CellCount];
            for (int c = 0; c < l.CellCount; ++c) cells[c] = (l.Cols[c], l.Rows[c]);
            list.Add(new Preset(id, LayoutTitles.GetValueOrDefault(id, id), cells, l.Curvature, l.SizeDeg, l.GapDeg));
        }
        return list;
    }

    private static List<ProfilePreset> LoadProfiles()
    {
        var list = new List<ProfilePreset>();
        int n = NimboNative.nimbo_profile_count();
        for (int i = 0; i < n; ++i)
        {
            NimboNative.Profile p;
            if (NimboNative.nimbo_profile_at(i, &p) == 0) continue;
            var id = NimboNative.FromPtr(p.Id);
            list.Add(new ProfilePreset(id, ProfileTitles.GetValueOrDefault(id, id), NimboNative.FromPtr(p.LayoutId),
                                       p.DistanceM, p.TextSize, p.Curvature >= 0 ? p.Curvature : null,
                                       p.SizeDeg >= 0 ? p.SizeDeg : null));
        }
        return list;
    }

    // A calibration made earlier with imu_probe (next to the app or in the repository) is reused.
    private static void AdoptExistingCalibration()
    {
        if (File.Exists(CalibrationPath)) return;
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "calibration.json"),
            Path.Combine(Environment.CurrentDirectory, "calibration.json"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Desktop",
                         "rayneo-gt-spatial-core", "calibration.json"),
        };
        var src = candidates.FirstOrDefault(File.Exists);
        if (src != null) File.Copy(src, CalibrationPath);
    }
}
