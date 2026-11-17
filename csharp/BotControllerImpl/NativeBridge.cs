using System.Runtime.InteropServices;

namespace BotControllerApi;

internal static class NativeBridge
{
    private const int RtldNow = 0x2;
    private const int RtldNoLoad = 0x4;
    private static bool _initialized;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern bool GetModuleHandleExW(uint flags, string path, out nint handle);

    [DllImport("libdl.so.2", EntryPoint = "dlopen", CallingConvention = CallingConvention.Cdecl)]
    private static extern nint Dlopen([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int flags);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int IsLoaded();

    // Call on the game thread before the first native import.
    internal static void Initialize(string gameFilesDirectory)
    {
        if (_initialized) return;

        bool windows = OperatingSystem.IsWindows();
        string path = Path.GetFullPath(Path.Combine(gameFilesDirectory, "addons", "BotController", "bin",
            windows ? "win64" : "linuxsteamrt64", windows ? "BotController.dll" : "BotController.so"));
        nint handle = OpenLoadedModule(path, windows);
        if (handle == 0)
            throw new InvalidOperationException($"BotController not loaded by Metamod: '{path}'.");

        var module = new ModuleHandle(handle);
        try
        {
            if (!NativeLibrary.TryGetExport(handle, "BotController_IsLoaded", out nint probe))
                throw new InvalidOperationException($"BotController_IsLoaded missing: '{path}'. Update the Metamod package.");
            if (Marshal.GetDelegateForFunctionPointer<IsLoaded>(probe)() == 0)
                throw new InvalidOperationException($"BotController not initialized: '{path}'.");

            // Keep the loader reference alive with the resolver.
            NativeLibrary.SetDllImportResolver(typeof(BotController).Assembly,
                (name, _, _) => name == "BotController" ? module.DangerousGetHandle() : 0);
            _initialized = true;
        }
        catch
        {
            module.Dispose();
            throw;
        }
    }

    private static nint OpenLoadedModule(string path, bool windows)
    {
        if (windows)
            return GetModuleHandleExW(0, path, out nint handle) ? handle : 0;
        return Dlopen(path, RtldNoLoad | RtldNow);
    }

    private sealed class ModuleHandle : SafeHandle
    {
        internal ModuleHandle(nint value) : base(0, ownsHandle: true) => SetHandle(value);
        public override bool IsInvalid => handle == 0;
        protected override bool ReleaseHandle()
        {
            NativeLibrary.Free(handle);
            return true;
        }
    }
}
