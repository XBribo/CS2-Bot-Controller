// Recording storage facade for CounterStrikeSharp.

using BotControllerApi;

namespace BotControllerImpl;

public static class MotionStore
{
    // Save a stopped recording in the shared frame format; -1 means no frames.
    public static int SaveToFile(int slot, string path, int tickrate = 64)
        => MotionRecordingStore.SaveToFile(slot, path, tickrate);

    // Load unified frames or convert a supported legacy recording.
    public static ReplayData LoadFromFile(string path)
        => MotionRecordingStore.LoadFromFile(path);
}
