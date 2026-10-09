// Recording storage facade for SwiftlyS2.

using BotControllerApi;

namespace BotControllerImplSW2;

public static class MotionStore
{
    // Save a stopped recording in the shared frame format; -1 means no frames.
    public static int SaveToFile(int slot, string path, int tickrate = 64)
        => MotionRecordingStore.SaveToFile(slot, path, tickrate);

    // Load unified frames or convert a supported legacy recording.
    public static ReplayData LoadFromFile(string path)
        => MotionRecordingStore.LoadFromFile(path);
}
