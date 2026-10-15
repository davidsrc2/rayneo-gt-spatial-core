using System.Runtime.InteropServices;
using System.Text;

namespace Nimbo.Native;

// P/Invoke over nimbo.dll. The structs mirror include/rayneo/platform/nimbo_win.h field by
// field and are blittable (fixed buffers, C bool as byte), so they cross by value as is;
// keep both in sync.
internal static unsafe class NimboNative
{
    private const string Dll = "nimbo.dll";
    public const int MaxPanels = 8;

    [StructLayout(LayoutKind.Sequential)]
    public struct Options
    {
        public int Displays;
        public int LayoutCount;
        public fixed int LayoutCols[MaxPanels];
        public fixed int LayoutRows[MaxPanels];
        public int VdWidth, VdHeight;
        public int VdScale;
        public float ArcDeg;
        public float GapDeg;
        public float DistanceM;
        public float Curvature;
        public byte Fit;
        public byte GatherWindows;
        public byte GazeCursor;
        public float DimOthers;
        public int SideBySide;
        public float IpdMm;
        public float FovYDeg;
        public float K1, K2;
        public fixed byte CalibrationPath[260];
        public byte CalibrationRequired;
        public float DeadZoneDeg;
        public byte Sim;
        public float SimSweepDeg;
        public byte Windowed;
        public fixed byte Source[32];
        public byte Refresh120;
        public byte Hotkeys;
        public int TrackingAxes;  // 0 both, 1 left/right, 2 up/down
        public byte GlassesPinned;
    }

    public enum State
    {
        Stopped = 0,
        Starting = 1,
        Calibrating = 2,
        Tracking = 3,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public State State;
        public int PanelCount;
        public int GazePanel;
        public float FovYDeg;
        public float RenderFps;
        public float CaptureFps;
        public float LatencyMs;
        public fixed byte Message[256];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Settings
    {
        public int CellCount;
        public fixed int Cols[MaxPanels];
        public fixed int Rows[MaxPanels];
        public int TextSize;
        public float DistanceM, GapDeg, Curvature, SizeDeg;
        public byte AutoStart;
        public float FovYDeg, IpdMm;
        public byte Stereo;
        public float DeadZoneDeg;
        public byte GazeCursor;
        public byte DimOthers;
        public byte GatherWindows;
        public float K1, K2;
        public byte OnboardingDone;
        public int Tracking;
        public byte GlassesPinned;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Layout
    {
        public IntPtr Id;
        public int CellCount;
        public fixed int Cols[MaxPanels];
        public fixed int Rows[MaxPanels];
        public float Curvature, SizeDeg, GapDeg;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Profile
    {
        public IntPtr Id;
        public IntPtr LayoutId;
        public float DistanceM;
        public int TextSize;
        public float Curvature;
        public float SizeDeg;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LogFn(IntPtr ctx, IntPtr line);

    public enum CalibStep
    {
        Bias = 0,
        Center,
        Up,
        Down,
        Left,
        Right,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CalibStatus
    {
        public CalibStep Step;
        public byte Returning;
        public IntPtr Hint;
        public float RateDegs;
        public float RotationDeg;
        public byte BiasOk;
        public float CapturedDeg;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void CalibFn(IntPtr ctx, CalibStatus* status);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr nimbo_calibration_create();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_calibration_destroy(IntPtr c);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_calibration_cancel(IntPtr c);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int nimbo_calibration_run(IntPtr c, byte* outPath, IntPtr fn, IntPtr ctx, byte* error, int errorSize);

    // Blocking guided calibration; `onStatus` runs on the calling thread. Returns 0 ok, 1 stopped,
    // 2 rejected, with the reason in `error`.
    public static int RunCalibration(IntPtr c, string outPath, Action<CalibStatus> onStatus, out string error)
    {
        CalibFn fn = (_, st) => onStatus(*st);
        var path = Encoding.UTF8.GetBytes(outPath + "\0");
        var err = new byte[256];
        int code;
        fixed (byte* p = path)
        fixed (byte* e = err)
        {
            code = nimbo_calibration_run(c, p, Marshal.GetFunctionPointerForDelegate(fn), IntPtr.Zero, e, err.Length);
            error = GetField(e, err.Length);
        }
        GC.KeepAlive(fn);
        return code;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern Options nimbo_default_options();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr nimbo_create();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_destroy(IntPtr s);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_start(IntPtr s, Options* options);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_stop(IntPtr s);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_apply(IntPtr s, Options* options);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_recenter(IntPtr s);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_adjust_fov(IntPtr s, float delta);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_get_status(IntPtr s, Status* status);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_set_log(IntPtr fn, IntPtr ctx);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_glasses_connected();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_virtual_driver_available();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_restore_displays();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern Settings nimbo_default_settings();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_load_settings(byte* path, Settings* settings);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_save_settings(byte* path, Settings* settings);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void nimbo_options_from_settings(Settings* settings, Options* options);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int nimbo_layout_count();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_layout_at(int index, Layout* layout);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int nimbo_profile_count();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern byte nimbo_profile_at(int index, Profile* profile);

    // Fixed UTF-8 field <- string (truncated, NUL-terminated).
    public static void SetField(byte* field, int size, string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        int n = Math.Min(bytes.Length, size - 1);
        for (int i = 0; i < n; ++i) field[i] = bytes[i];
        field[n] = 0;
    }

    public static string GetField(byte* field, int size)
    {
        int n = 0;
        while (n < size && field[n] != 0) ++n;
        return Encoding.UTF8.GetString(field, n);
    }

    public static string FromPtr(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";

    // Safe wrappers for async callers (no pointers across awaits).
    public static bool Start(IntPtr server, Options options) => nimbo_start(server, &options) != 0;

    public static void Apply(IntPtr server, Options options) => nimbo_apply(server, &options);

    public static Status GetStatus(IntPtr server)
    {
        Status st;
        nimbo_get_status(server, &st);
        return st;
    }

    public static string MessageOf(Status st) => GetField(st.Message, 256);

    public static bool LoadSettings(string path, out Settings settings)
    {
        var bytes = Encoding.UTF8.GetBytes(path + "\0");
        Settings s;
        byte ok;
        fixed (byte* p = bytes) ok = nimbo_load_settings(p, &s);
        settings = s;
        return ok != 0;
    }

    public static bool SaveSettings(string path, Settings settings)
    {
        var bytes = Encoding.UTF8.GetBytes(path + "\0");
        fixed (byte* p = bytes) return nimbo_save_settings(p, &settings) != 0;
    }
}
