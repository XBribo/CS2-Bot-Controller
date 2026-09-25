// Recording model and JSON load/save — framework-agnostic.

using System.Text.Json;
using System.Text.Json.Serialization;
using BotControllerApi;

namespace BotControllerImplSW2;

// Recorded motion plus the tickrate it was captured at
public sealed class MotionRecording
{
    public required int Tickrate { get; set; }
    public required ReplayTick[] Ticks { get; set; }
    public required SubtickMove[] Subticks { get; set; }
    public required ReplayCommandFrame[] Commands { get; set; }
}

// File + capture-buffer on top of the native calls
public static class MotionStore
{
    // IncludeFields
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = false,
        IncludeFields = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };

    // Save a slot's recorded motion to a JSON file. Returns tick count, or -1
    // if nothing was recorded
    public static int SaveToFile(int slot, string path, int tickrate = 64)
    {
        var (ticks, subs, commands) = BotController.GetRecordedMotionExtended(slot);
        if (ticks.Length == 0) return -1;
        var rec = new MotionRecording
        {
            Tickrate = tickrate,
            Ticks = ticks,
            Subticks = subs,
            Commands = commands,
        };
        File.WriteAllText(path, JsonSerializer.Serialize(rec, JsonOpts));
        return ticks.Length;
    }

    // Load a JSON recording from disk
    public static MotionRecording LoadFromFile(string path)
    {
        MotionRecording recording = JsonSerializer.Deserialize<MotionRecording>(
            File.ReadAllText(path), JsonOpts)
            ?? throw new InvalidDataException("Recording JSON is empty.");
        if (recording.Ticks is null || recording.Subticks is null || recording.Commands is null)
            throw new InvalidDataException("Recording JSON is missing required data.");
        if (recording.Commands.Length != recording.Ticks.Length)
            throw new InvalidDataException("Recording commands must match the tick count.");

        const uint eventDrop = 1U << 0;
        const uint dropReleasePose = 1U << 3;
        foreach (ReplayTick tick in recording.Ticks)
        {
            if ((tick.EventFlags & eventDrop) != 0 && (tick.EventDropVectorFlags & dropReleasePose) == 0)
                throw new InvalidDataException("Recording lacks the required drop release pose.");
        }

        const uint commandFieldWeaponSelectDef = 1U << 8;
        foreach (ReplayCommandFrame command in recording.Commands)
        {
            if ((command.Fields & commandFieldWeaponSelectDef) == 0)
                throw new InvalidDataException("Recording uses an unsupported command JSON format.");
        }

        return recording;
    }
}
