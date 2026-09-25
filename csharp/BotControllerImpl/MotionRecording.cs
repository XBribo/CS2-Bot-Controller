// Recording model and Brotli-compressed JSON load/save

using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Serialization;

using BotControllerApi;

namespace BotControllerImpl;

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

    // Save a slot's recorded motion to a compressed JSON file. Returns tick count, or -1
    // if nothing was recorded
    public static int SaveToFile(int slot, string path, int tickrate = 64)
    {
        int ticks = BotController.RecordedTickCount(slot);
        if (ticks <= 0) return -1;
        int subticks = BotController.RecordedSubtickCount(slot);
        int commands = BotController.RecordedCommandCount(slot);
        if (subticks < 0 || commands != ticks)
            throw new InvalidDataException("Recording buffers are incomplete.");

        string temporaryPath = path + "." + Path.GetRandomFileName() + ".tmp";
        try
        {
            using (var file = File.Create(temporaryPath))
            using (var brotli = new BrotliStream(file, CompressionLevel.Optimal))
            using (var writer = new Utf8JsonWriter(brotli))
            {
                writer.WriteStartObject();
                writer.WriteNumber(nameof(MotionRecording.Tickrate), tickrate);
                WriteTicks(writer, slot, ticks);
                WriteSubticks(writer, slot, subticks);
                WriteCommands(writer, slot, commands);
                writer.WriteEndObject();
            }
            File.Move(temporaryPath, path, true);
            return ticks;
        }
        finally
        {
            if (File.Exists(temporaryPath)) File.Delete(temporaryPath);
        }
    }

    // Copies and serializes ticks in bounded batches without a full managed snapshot.
    private static void WriteTicks(Utf8JsonWriter writer, int slot, int total)
    {
        writer.WritePropertyName(nameof(MotionRecording.Ticks));
        writer.WriteStartArray();
        for (int start = 0; start < total;)
        {
            var batch = new ReplayTick[Math.Min(512, total - start)];
            if (BotController.CopyRecordedTicksRange(slot, start, batch) != batch.Length)
                throw new InvalidDataException("Recording ticks changed during save.");
            foreach (ReplayTick tick in batch) JsonSerializer.Serialize(writer, tick, JsonOpts);
            start += batch.Length;
        }
        writer.WriteEndArray();
    }

    // Copies and serializes subticks in bounded batches.
    private static void WriteSubticks(Utf8JsonWriter writer, int slot, int total)
    {
        writer.WritePropertyName(nameof(MotionRecording.Subticks));
        writer.WriteStartArray();
        for (int start = 0; start < total;)
        {
            var batch = new SubtickMove[Math.Min(512, total - start)];
            if (BotController.CopyRecordedSubticksRange(slot, start, batch) != batch.Length)
                throw new InvalidDataException("Recording subticks changed during save.");
            foreach (SubtickMove subtick in batch) JsonSerializer.Serialize(writer, subtick, JsonOpts);
            start += batch.Length;
        }
        writer.WriteEndArray();
    }

    // Copies and serializes commands in bounded batches.
    private static void WriteCommands(Utf8JsonWriter writer, int slot, int total)
    {
        writer.WritePropertyName(nameof(MotionRecording.Commands));
        writer.WriteStartArray();
        for (int start = 0; start < total;)
        {
            var batch = new ReplayCommandFrame[Math.Min(512, total - start)];
            if (BotController.CopyRecordedCommandsRange(slot, start, batch) != batch.Length)
                throw new InvalidDataException("Recording commands changed during save.");
            foreach (ReplayCommandFrame command in batch) JsonSerializer.Serialize(writer, command, JsonOpts);
            start += batch.Length;
        }
        writer.WriteEndArray();
    }

    // Load a Brotli-compressed JSON recording from disk
    public static MotionRecording LoadFromFile(string path)
    {
        using var file = File.OpenRead(path);
        using var brotli = new BrotliStream(file, CompressionMode.Decompress);
        MotionRecording recording = JsonSerializer.Deserialize<MotionRecording>(
            brotli, JsonOpts)
            ?? throw new InvalidDataException("Recording is empty.");
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
