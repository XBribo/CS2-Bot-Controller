// Per-tick replay: release the AI lock when a replay ends

using BotControllerApi;

namespace BotControllerImplSW2;

public sealed class ReplayDriver
{
    // Bot slots we're driving
    private readonly HashSet<int> _active = new();

    // Begin tracking a bot slot once its replay has been started natively.
    public void Track(int slot) => _active.Add(slot);

    // Stop tracking and release the AI lock for one slot
    public void Release(int slot)
    {
        BotController.Unlock(slot, LockKind.All);
        _active.Remove(slot);
    }

    public bool IsActive => _active.Count > 0;

    public void Tick()
    {
        if (_active.Count == 0) return;

        // Snapshot so we can prune finished replays without mutating mid-loop
        foreach (int slot in new List<int>(_active))
        {
            if (!BotController.IsReplaying(slot))
                Release(slot); // replay ended
        }
    }
}
