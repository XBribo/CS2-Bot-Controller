using System.Collections.Concurrent;
using BotControllerApi;

#if BOT_CONTROLLER_SW2
namespace BotControllerImplSW2;
#else
namespace BotControllerImpl;
#endif

// Shared recording sessions and file jobs.
internal sealed class RecordingSessions(
    string recordingsDir,
    Func<int, string?> registerPawn,
    Action<int, ulong, string> notify,
    Action<string> logError)
{
    private const int Tickrate = 64;
    private readonly Dictionary<int, (ulong SteamId, string File)> _recordingFiles = new();
    private readonly HashSet<int> _savingSlots = new();
    private readonly Dictionary<int, ulong> _loadingSlots = new();
    private readonly ConcurrentQueue<Action> _completedJobs = new();
    private readonly List<Task> _jobs = new();
    private ulong _mapGeneration;

    internal void Reset()
    {
        ++_mapGeneration;
        _loadingSlots.Clear();
        _recordingFiles.Clear();
        BotController.ResetPawns();
    }

    internal void Disconnect(int slot)
    {
        _loadingSlots.Remove(slot);
        _recordingFiles.Remove(slot);
        BotController.ResetPawn(slot);
    }

    internal void Tick()
    {
        BotController.RefreshPawns();
        while (_completedJobs.TryDequeue(out Action? completion)) completion();
        _jobs.RemoveAll(job => job.IsCompleted);
    }

    // Drains native buffer readers before unload.
    internal void Drain()
    {
        Reset();
        Task.WaitAll(_jobs.ToArray());
        _jobs.Clear();
        _completedJobs.Clear();
        _savingSlots.Clear();
    }

    internal string StartRecord(int slot, ulong steamId, string? fileName)
    {
        if (_savingSlots.Contains(slot)) return "Previous recording is still saving.";
        if (!TryGetRecordingFile(fileName, steamId, out string file)) return "Usage: !record [fileName]";
        if (registerPawn(slot) is { } failure) return failure;
        if (!BotController.StartRecord(slot)) return "Failed to start recording.";
        _recordingFiles[slot] = (steamId, file);
        return "Recording. Use !stoprecord to finish.";
    }

    internal string StopRecord(int slot, ulong steamId)
    {
        if (_savingSlots.Contains(slot)) return "Recording is still saving.";
        if (!_recordingFiles.TryGetValue(slot, out var recording) || recording.SteamId != steamId)
            return "No recording.";
        _recordingFiles.Remove(slot);
        BotController.StopRecord(slot);
        ulong mapGeneration = _mapGeneration;
        _savingSlots.Add(slot);
        _jobs.Add(Task.Run(() =>
        {
            try
            {
                int saved = MotionStore.SaveToFile(slot, recording.File, Tickrate);
                _completedJobs.Enqueue(() =>
                {
                    _savingSlots.Remove(slot);
                    if (mapGeneration != _mapGeneration) return;
                    notify(slot, steamId, saved > 0 ? $"Saved {saved} ticks." : "Nothing recorded.");
                });
            }
            catch (Exception ex)
            {
                _completedJobs.Enqueue(() =>
                {
                    _savingSlots.Remove(slot);
                    if (mapGeneration != _mapGeneration) return;
                    logError($"Save failed: {ex.Message}");
                    notify(slot, steamId, "Recording save failed.");
                });
            }
        }));
        return "Saving recording...";
    }

    internal string StartReplay(int slot, string? fileName, int requesterSlot, ulong requesterSteamId)
    {
        if (!TryGetRecordingFile(fileName, requesterSteamId, out string file))
            return "Usage: !replay <slot> [fileName]";
        if (!File.Exists(file)) return "No recording found. Use !record first.";
        if (_loadingSlots.ContainsKey(slot)) return "Replay is still loading on that slot.";
        if (BotController.IsReplaying(slot)) return $"Slot {slot}: replay already running.";
        if (registerPawn(slot) is { } failure) return failure;
        ulong generation = BotController.PawnGeneration(slot);
        if (generation == 0) return "Pawn session unavailable.";
        _loadingSlots.Add(slot, generation);
        _jobs.Add(Task.Run(() =>
        {
            try
            {
                MotionRecording rec = MotionStore.LoadFromFile(file);
                bool loaded = rec.Ticks.Length > 0 && BotController.LoadReplay(
                    slot, rec.Ticks, rec.Subticks, rec.Commands, rec.InputHistory, generation);
                _completedJobs.Enqueue(() =>
                {
                    if (!_loadingSlots.TryGetValue(slot, out ulong current) || current != generation) return;
                    _loadingSlots.Remove(slot);
                    if (BotController.PawnGeneration(slot) != generation)
                    {
                        notify(requesterSlot, requesterSteamId, "Replay cancelled: pawn changed.");
                        return;
                    }
                    if (rec.Tickrate != Tickrate)
                        notify(requesterSlot, requesterSteamId, $"WARN tickrate mismatch: recorded {rec.Tickrate}, server {Tickrate}.");
                    if (loaded && BotController.StartReplay(slot))
                        notify(requesterSlot, requesterSteamId, $"Replaying on slot {slot}.");
                    else notify(requesterSlot, requesterSteamId, "Failed to start replay.");
                });
            }
            catch (Exception ex)
            {
                _completedJobs.Enqueue(() =>
                {
                    if (!_loadingSlots.TryGetValue(slot, out ulong current) || current != generation) return;
                    _loadingSlots.Remove(slot);
                    logError($"Replay load failed: {ex.Message}");
                    notify(requesterSlot, requesterSteamId, "Replay load failed.");
                });
            }
        }));
        return "Loading replay...";
    }

    internal string StopReplay(int slot)
    {
        _loadingSlots.Remove(slot);
        BotController.StopReplay(slot);
        return $"Stopped replay on slot {slot}.";
    }

    // Resolves a plugin-local recording path.
    private bool TryGetRecordingFile(string? fileName, ulong steamId, out string file)
    {
        string name = string.IsNullOrWhiteSpace(fileName) ? steamId.ToString() : fileName;
        if (name != Path.GetFileName(name) || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
        {
            file = string.Empty;
            return false;
        }
        if (name.EndsWith(".json.br", StringComparison.OrdinalIgnoreCase)) name = name[..^8];
        if (string.IsNullOrWhiteSpace(name))
        {
            file = string.Empty;
            return false;
        }
        file = Path.Combine(recordingsDir, $"{name}.json.br");
        return true;
    }
}
