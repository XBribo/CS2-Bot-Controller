using System.Runtime.InteropServices;
#if BOT_CONTROLLER_SW2
using System.Diagnostics;
#else
using CounterStrikeSharp.API;
#endif

namespace BotControllerApi;

internal static class NativeBridge
{
    private static ModuleHandle? _module;
    internal static nint? Handle => _module?.DangerousGetHandle();

#if BOT_CONTROLLER_SW2
    private const int RtldNow = 0x2;
    private const int RtldNoLoad = 0x4;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern bool GetModuleHandleExW(uint flags, string path, out nint handle);

    [DllImport("libdl.so.2", EntryPoint = "dlopen", CallingConvention = CallingConvention.Cdecl)]
    private static extern nint Dlopen([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int flags);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int IsLoaded();

    internal static void Initialize(string gameFilesDirectory)
    {
        if (_module != null) return;

        bool windows = OperatingSystem.IsWindows();
        string path = Path.GetFullPath(Path.Combine(gameFilesDirectory, "addons", "BotController", "bin",
            windows ? "win64" : "linuxsteamrt64", windows ? "BotController.dll" : "BotController.so"));
        _module = FindInitializedModule(path, windows);
    }

    private static ModuleHandle FindInitializedModule(string packagedPath, bool windows)
    {
        var module = TryOpenInitialized(packagedPath, windows, out string? error);
        if (module != null) return module;

        // A VDF may point elsewhere.
        using var process = Process.GetCurrentProcess();
        var comparison = windows ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        string name = Path.GetFileName(packagedPath);
        foreach (ProcessModule mapped in process.Modules)
        {
            string path = mapped.FileName;
            if (!string.Equals(Path.GetFileName(path), name, comparison) ||
                string.Equals(path, packagedPath, comparison)) continue;

            module = TryOpenInitialized(path, windows, out string? candidateError);
            if (module != null) return module;
            error ??= candidateError;
        }
        throw new InvalidOperationException(error ??
            $"BotController not loaded by Metamod: '{packagedPath}'.");
    }

    private static ModuleHandle? TryOpenInitialized(string path, bool windows, out string? error)
    {
        error = null;
        nint handle = OpenLoadedModule(path, windows);
        if (handle == 0) return null;

        var module = new ModuleHandle(handle, ownsHandle: true);
        if (!NativeLibrary.TryGetExport(handle, "BotController_IsLoaded", out nint probe))
            error = $"BotController_IsLoaded missing: '{path}'.";
        else if (Marshal.GetDelegateForFunctionPointer<IsLoaded>(probe)() == 0)
            error = $"BotController not initialized: '{path}'.";
        else
            return module;

        module.Dispose();
        return null;
    }

    private static nint OpenLoadedModule(string path, bool windows)
    {
        if (windows)
            return GetModuleHandleExW(0, path, out nint handle) ? handle : 0;
        return Dlopen(path, RtldNoLoad | RtldNow);
    }

#else
    private const string InterfaceName = "BotController_ModuleHandle001";
    internal const string Unavailable = InterfaceName + " unavailable; check the Metamod package.";

    internal static bool TryBind()
    {
        if (_module != null) return true;
        if (Utilities.MetaFactory(InterfaceName) is not { } handle) return false;
        _module = new ModuleHandle(handle, ownsHandle: false);
        return true;
    }
#endif

    private sealed class ModuleHandle : SafeHandle
    {
        internal ModuleHandle(nint value, bool ownsHandle) : base(0, ownsHandle) => SetHandle(value);
        public override bool IsInvalid => handle == 0;
        protected override bool ReleaseHandle()
        {
            NativeLibrary.Free(handle);
            return true;
        }
    }
}
