using Nimbo.Native;

namespace Nimbo.Core;

// One-click layout or usage profile from the shared core (core/presets.h), with its title.
// A monitor placed by hand (layout editor): degrees right / up at 2 m with the common panel
// size, and its own size (core::PanelPose).
internal record struct PanelPos(float Yaw, float Pitch, float Scale = 1f);

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
            _s.PoseCount = 0;  // a new arrangement from the grid editor or a preset starts from the grid
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
    // The space at real size (the app's one mode, as on macOS): panels above / below face you (1)
    // or stand upright (0); frozen while the head holds still (anchor); the laptop's own screen
    // as panel 1.
    public float Tilt { get => _s.Tilt; set => _s.Tilt = Math.Clamp(value, 0f, 1f); }
    public bool AnchorAtRest { get => _s.AnchorAtRest != 0; set => _s.AnchorAtRest = B(value); }
    public bool LaptopScreen { get => _s.LaptopScreen != 0; set => _s.LaptopScreen = B(value); }
    // AI 3D in cinema mode: strength = parallax at the nearest depth (panel widths), plane = depth on the panel.
    public bool Depth3D { get => _s.Depth3D != 0; set => _s.Depth3D = B(value); }
    public float DepthStrength { get => _s.DepthStrength; set => _s.DepthStrength = Math.Clamp(value, 0.005f, 0.08f); }
    public float DepthPlane { get => _s.DepthPlane; set => _s.DepthPlane = Math.Clamp(value, 0f, 1f); }
    public int Brightness { get => _s.Brightness; set => _s.Brightness = Math.Clamp(value, 0, 8); }  // 1..8, 0 = leave
    public int Tracking { get => _s.Tracking; set => _s.Tracking = Math.Clamp(value, 0, 2); }  // 0 both, 1 left/right, 2 up/down

    public bool HasCalibration => File.Exists(CalibrationPath);

    // --- hand placement (layout editor) --------------------------------------------------

    public const float EyeAspect = 16f / 9f;

    // Vertical size (deg) of a flat panel `arc` degrees wide (core::panel_vertical_arc).
    public static float VerticalArc(float arc) => 2f * MathF.Atan(MathF.Tan(arc * MathF.PI / 360f) / EyeAspect) * 180f / MathF.PI;

    public PanelPos[] Poses
    {
        get
        {
            var p = new PanelPos[_s.PoseCount];
            for (int i = 0; i < p.Length; ++i) p[i] = new PanelPos(_s.PoseYaw[i], _s.PosePitch[i], _s.PoseScale[i]);
            return p;
        }
        set
        {
            _s.PoseCount = Math.Min(value.Length, NimboNative.MaxPanels);
            for (int i = 0; i < _s.PoseCount; ++i)
            {
                _s.PoseYaw[i] = value[i].Yaw;
                _s.PosePitch[i] = value[i].Pitch;
                _s.PoseScale[i] = value[i].Scale;
            }
        }
    }

    public bool HasPlacement => _s.PoseCount > 0 && _s.PoseCount == _s.CellCount;

    // The grid as hand placements: where the monitors are now (core::grid_layout).
    public PanelPos[] GridPoses()
    {
        var n = Normalized(Cells);
        if (n.Length == 0) return Array.Empty<PanelPos>();
        float midC = (n.Min(c => c.Item1) + n.Max(c => c.Item1)) / 2f, midR = (n.Min(c => c.Item2) + n.Max(c => c.Item2)) / 2f;
        float w = Size + Gap, h = VerticalArc(Size) + Gap;
        return n.Select(c => new PanelPos((c.Item1 - midC) * w, (midR - c.Item2) * h)).ToArray();
    }

    public PanelPos[] EffectivePoses => HasPlacement ? Poses : GridPoses();

    // --- floating windows (this session: window handles do not survive the app) ------------

    public sealed class FloatingWin
    {
        public ulong Hwnd;
        public string Title = "";        // "app — window title", shown in Settings
        public PanelPos Pose;
        public string App = "";          // remembered between sessions: found again by app and title
        public string WindowTitle = "";
    }

    // Floating windows, remembered in floating.json by app and title (window handles do not
    // survive the app: found again when the app starts).
    public List<FloatingWin> Floating { get; } = LoadFloating();

    private sealed record SavedFloating(string App, string Title, float Yaw, float Pitch, float Scale);

    public static string FloatingPath => Path.Combine(DataDirectory, "floating.json");

    public void SaveFloating() => File.WriteAllText(FloatingPath, System.Text.Json.JsonSerializer.Serialize(
        Floating.Select(f => new SavedFloating(f.App, f.WindowTitle, f.Pose.Yaw, f.Pose.Pitch, f.Pose.Scale))));

    // Saved windows that are open now: the same app and title, else that app's first window.
    private static List<FloatingWin> LoadFloating()
    {
        List<SavedFloating> saved;
        try
        {
            saved = File.Exists(FloatingPath)
                ? System.Text.Json.JsonSerializer.Deserialize<List<SavedFloating>>(File.ReadAllText(FloatingPath)) ?? new()
                : new();
        }
        catch (Exception e) when (e is IOException or System.Text.Json.JsonException)
        {
            return new();
        }
        var open = NimboNative.ListWindows();
        var list = new List<FloatingWin>();
        foreach (var s in saved)
        {
            var same = open.Where(w => w.App.Equals(s.App, StringComparison.OrdinalIgnoreCase) && list.All(f => f.Hwnd != w.Hwnd)).ToList();
            if (same.Count == 0) continue;
            var w = same.FirstOrDefault(x => x.Title == s.Title);
            if (w.Hwnd == 0) w = same[0];
            list.Add(new FloatingWin
            {
                Hwnd = w.Hwnd, Title = $"{w.App} — {w.Title}", Pose = new PanelPos(s.Yaw, s.Pitch, s.Scale),
                App = w.App, WindowTitle = w.Title,
            });
        }
        return list;
    }

    // Where a new floating window goes: centred above the monitors, one monitor wide.
    public PanelPos DefaultFloatingPose()
    {
        float v = VerticalArc(Size);
        var monitors = EffectivePoses;
        float top = monitors.Length > 0 ? monitors.Max(p => p.Pitch + v * p.Scale / 2) : v / 2;
        float above = Floating.Count == 0 ? 0 : Floating.Max(f => f.Pose.Pitch + v * f.Pose.Scale / 2) - top + Gap;
        return new PanelPos(0, top + Gap + v / 2 + Math.Max(0, above));
    }

    // Ctrl+Alt+G: the glasses' own pinned mode instead of the space at real size, as left last time.
    public bool PinnedMode { get => _s.GlassesMode == 1; set => _s.GlassesMode = value ? 1 : 0; }
    // Ctrl+Alt++ / - / 0 / K: the zoom, as left last time (the server reports it).
    public float Zoom { get => _s.Zoom > 0 ? _s.Zoom : 1f; set => _s.Zoom = value; }
    // Ctrl+Alt+M / Settings: in the glasses' pinned mode the panel in use follows the mouse only.
    public bool GrowByGaze { get => _s.GrowByGaze != 0; set => _s.GrowByGaze = B(value); }

    public void ResetPlacement() => _s.PoseCount = 0;

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
        o.FloatingCount = Math.Min(Floating.Count, NimboNative.MaxPanels);
        for (int i = 0; i < o.FloatingCount; ++i)
        {
            o.FloatingWindow[i] = Floating[i].Hwnd;
            o.FloatingYaw[i] = Floating[i].Pose.Yaw;
            o.FloatingPitch[i] = Floating[i].Pose.Pitch;
            o.FloatingScale[i] = Floating[i].Pose.Scale;
        }
        o.Depth3D = 0;  // AI 3D only with Ctrl+Alt+T, for the run
        if (PinnedMode)
        {
            // The glasses hold the image: panels fitted inside it, the one looked at large.
            o.GlassesMode = 1;
            o.GlassesPinned = 1;
            o.RecenterGlasses = 1;
            o.RealSize = 0;
            o.DeadZoneDeg = DeadZone > 0 ? DeadZone : 1.0f;
        }
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
            a.VdWidth != b.VdWidth || a.DeadZoneDeg != b.DeadZoneDeg || a.GatherWindows != b.GatherWindows ||
            a.LaptopScreen != b.LaptopScreen)
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
