// Shared Brotli-compressed recording storage for both managed providers.

using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace BotControllerApi;

internal static class MotionRecordingStore
{
    // Only the reader accepts legacy transport arrays; new files use ReplayData.
    private sealed class RecordingFile
    {
        public float? TickRate { get; set; }
        public ReplayFrame[]? Frames { get; set; }
        public int? Tickrate { get; set; }
        public ReplayTick[]? Ticks { get; set; }
        public SubtickMove[]? Subticks { get; set; }
        public NativeReplayInput[]? Commands { get; set; }
    }

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        IncludeFields = true,
        IgnoreReadOnlyProperties = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };

    // Save complete stopped-recording frames in bounded batches and replace only on success.
    internal static int SaveToFile(int slot, string path, int tickrate)
    {
        if (!BotController.IsCompatible()) throw new InvalidOperationException("BotController ABI mismatch.");
        int ticks = BotController.RecordedTickCount(slot);
        if (ticks <= 0) return -1;
        int subticks = BotController.RecordedSubtickCount(slot);
        if (tickrate <= 0 || subticks < 0)
            throw new InvalidDataException("Recording rate or subtick count is invalid.");

        string temporaryPath = path + "." + Path.GetRandomFileName() + ".tmp";
        try
        {
            using (var file = File.Create(temporaryPath))
            using (var brotli = new BrotliStream(file, CompressionLevel.Optimal))
            using (var writer = new Utf8JsonWriter(brotli))
            {
                writer.WriteStartObject();
                writer.WriteNumber(nameof(ReplayData.TickRate), tickrate);
                WriteFrames(writer, slot, ticks, subticks, tickrate);
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

    // Keep each frame's own subticks and source state without copying the whole recording.
    private static void WriteFrames(Utf8JsonWriter writer, int slot, int total, int totalSubticks, float tickRate)
    {
        writer.WritePropertyName(nameof(ReplayData.Frames));
        writer.WriteStartArray();
        int subtickOffset = 0;
        for (int start = 0; start < total;)
        {
            var batch = new ReplayFrameData[Math.Min(512, total - start)];
            if (BotController.CopyRecordedFramesRange(slot, start, batch) != batch.Length)
                throw new InvalidDataException("Recording must be stopped and unchanged during save.");
            int count = 0;
            foreach (var frame in batch)
            {
                if (frame.Tick.NumSubtick > 36)
                    throw new InvalidDataException("Recording subtick count is invalid.");
                count += (int)frame.Tick.NumSubtick;
            }
            if ((long)subtickOffset + count > totalSubticks)
                throw new InvalidDataException("Recording subticks changed during save.");
            var subs = new SubtickMove[count];
            if (count > 0 && BotController.CopyRecordedSubticksRange(slot, subtickOffset, subs) != count)
                throw new InvalidDataException("Recording subticks changed during save.");
            var replay = ReplayFrameCodec.FromRecording(tickRate, batch, subs);
            foreach (var frame in replay.Frames) JsonSerializer.Serialize(writer, frame, JsonOpts);
            subtickOffset += count;
            start += batch.Length;
        }
        if (subtickOffset != totalSubticks || BotController.RecordedTickCount(slot) != total ||
            BotController.RecordedSubtickCount(slot) != totalSubticks)
            throw new InvalidDataException("Recording buffers changed during save.");
        writer.WriteEndArray();
    }

    // Read either the unified frame model or the supported legacy arrays, never a mixture.
    internal static ReplayData LoadFromFile(string path)
    {
        using var file = File.OpenRead(path);
        using var brotli = new BrotliStream(file, CompressionMode.Decompress);
        var recording = JsonSerializer.Deserialize<RecordingFile>(brotli, JsonOpts)
            ?? throw new InvalidDataException("Recording is empty.");
        if (recording.TickRate.HasValue || recording.Frames is not null)
        {
            if (recording.TickRate is not { } tickRate || recording.Frames is not { } frames ||
                recording.Tickrate.HasValue || recording.Ticks is not null ||
                recording.Subticks is not null || recording.Commands is not null)
                throw new InvalidDataException("Recording JSON mixes formats or is missing required frame data.");
            var replay = new ReplayData { TickRate = tickRate, Frames = frames };
            if (!ReplayFrameCodec.Validate(replay, out _))
                throw new InvalidDataException("Recording frame data is invalid.");
            return replay;
        }
        return LoadLegacy(recording);
    }

    // Preserve the original legacy checks and convert missing source state to nullable fields.
    private static ReplayData LoadLegacy(RecordingFile recording)
    {
        if (recording.Tickrate is not { } tickRate || recording.Ticks is not { } ticks ||
            recording.Subticks is not { } subs || recording.Commands is not { } commands)
            throw new InvalidDataException("Recording JSON is missing required data.");
        if (commands.Length != ticks.Length)
            throw new InvalidDataException("Recording commands must match the tick count.");
        const uint eventDrop = 1U << 0;
        const uint dropReleasePose = 1U << 3;
        foreach (var tick in ticks)
            if ((tick.EventFlags & eventDrop) != 0 && (tick.EventDropVectorFlags & dropReleasePose) == 0)
                throw new InvalidDataException("Recording lacks the required drop release pose.");
        const uint commandFieldWeaponSelectDef = 1U << 8;
        foreach (var command in commands)
            if ((command.Fields & commandFieldWeaponSelectDef) == 0)
                throw new InvalidDataException("Recording uses an unsupported command JSON format.");
        return ReplayFrameCodec.FromRecording(tickRate, ticks, subs, commands);
    }
}
