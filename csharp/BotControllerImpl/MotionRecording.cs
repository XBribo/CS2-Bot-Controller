// Shared recording model and Brotli-compressed JSON storage.

using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Serialization;

using BotControllerApi;

#if BOT_CONTROLLER_SW2
namespace BotControllerImplSW2;
#else
namespace BotControllerImpl;
#endif

// Recorded motion and capture tickrate.
public sealed class MotionRecording
{
    public int FormatVersion { get; set; }
    public required int Tickrate { get; set; }
    public required ReplayTick[] Ticks { get; set; }
    public required SubtickMove[] Subticks { get; set; }
    public required ReplayCommandFrame[] Commands { get; set; }
    public byte[] InputHistory { get; set; } = Array.Empty<byte>();
}

// Recording file storage.
public static class MotionStore
{
    private const int CurrentFormatVersion = 1;

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = false,
        IncludeFields = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };

    // Saves a stopped recording; returns its tick count or -1.
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
                writer.WriteNumber(nameof(MotionRecording.FormatVersion), CurrentFormatVersion);
                writer.WriteNumber(nameof(MotionRecording.Tickrate), tickrate);
                WriteTicks(writer, slot, ticks);
                WriteSubticks(writer, slot, subticks);
                WriteCommands(writer, slot, commands);
                writer.WriteBase64String(nameof(MotionRecording.InputHistory), BotController.GetRecordedInputHistory(slot));
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

    // Streams bounded batches of ticks.
    private static void WriteTicks(Utf8JsonWriter writer, int slot, int total)
    {
        writer.WritePropertyName(nameof(MotionRecording.Ticks));
        writer.WriteStartArray();
        var batch = new ReplayTick[Math.Min(512, total)];
        for (int start = 0; start < total;)
        {
            int count = Math.Min(batch.Length, total - start);
            if (BotController.CopyRecordedTicksRange(slot, start, batch, count) != count)
                throw new InvalidDataException("Recording ticks changed during save.");
            for (int i = 0; i < count; ++i) JsonSerializer.Serialize(writer, batch[i], JsonOpts);
            start += count;
        }
        writer.WriteEndArray();
    }

    // Serializes subticks in bounded batches.
    private static void WriteSubticks(Utf8JsonWriter writer, int slot, int total)
    {
        writer.WritePropertyName(nameof(MotionRecording.Subticks));
        writer.WriteStartArray();
        var batch = new SubtickMove[Math.Min(512, total)];
        for (int start = 0; start < total;)
        {
            int count = Math.Min(batch.Length, total - start);
            if (BotController.CopyRecordedSubticksRange(slot, start, batch, count) != count)
                throw new InvalidDataException("Recording subticks changed during save.");
            for (int i = 0; i < count; ++i) JsonSerializer.Serialize(writer, batch[i], JsonOpts);
            start += count;
        }
        writer.WriteEndArray();
    }

    // Serializes commands in bounded batches.
    private static void WriteCommands(Utf8JsonWriter writer, int slot, int total)
    {
        writer.WritePropertyName(nameof(MotionRecording.Commands));
        writer.WriteStartArray();
        var batch = new ReplayCommandFrame[Math.Min(512, total)];
        for (int start = 0; start < total;)
        {
            int count = Math.Min(batch.Length, total - start);
            if (BotController.CopyRecordedCommandsRange(slot, start, batch, count) != count)
                throw new InvalidDataException("Recording commands changed during save.");
            for (int i = 0; i < count; ++i) JsonSerializer.Serialize(writer, batch[i], JsonOpts);
            start += count;
        }
        writer.WriteEndArray();
    }

    // Loads Brotli-compressed JSON.
    public static MotionRecording LoadFromFile(string path)
    {
        using var file = File.OpenRead(path);
        using var brotli = new BrotliStream(file, CompressionMode.Decompress);
        MotionRecording recording = JsonSerializer.Deserialize<MotionRecording>(
            brotli, JsonOpts)
            ?? throw new InvalidDataException("Recording is empty.");
        if (recording.Ticks is null || recording.Subticks is null || recording.Commands is null)
            throw new InvalidDataException("Recording JSON is missing required data.");
        recording.InputHistory ??= Array.Empty<byte>();
        if (recording.Commands.Length != recording.Ticks.Length)
            throw new InvalidDataException("Recording commands must match the tick count.");

        return recording;
    }
}
