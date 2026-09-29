// Conversion between the public frame model and private ABI/disk DTOs.

using System.Numerics;

namespace BotControllerApi;

internal static class ReplayFrameCodec
{
    // Validate grouping before allocating and flatten only at the native boundary.
    internal static bool TryEncode(ReplayData replay, out ReplayFrameData[] frames, out SubtickMove[] subs)
    {
        frames = [];
        subs = [];
        if (!float.IsFinite(replay.TickRate) || replay.TickRate <= 0) return false;
        long total = 0;
        foreach (var frame in replay.Frames)
        {
            if (frame is null || frame.Subticks is null || frame.Subticks.Length > 36 ||
                frame.SourcePlayerTick is < 0 || HasHistory(frame.Post) ||
                (HasHistory(frame.Pre) && frame.SourcePlayerTick is null) ||
                (frame.Drop?.Target is not null && frame.Drop.BodyYaw is not null))
                return false;
            total += frame.Subticks.Length;
        }
        if (total > int.MaxValue) return false;
        frames = new ReplayFrameData[replay.Frames.Length];
        subs = new SubtickMove[(int)total];
        int offset = 0;
        for (int i = 0; i < frames.Length; ++i)
        {
            var frame = replay.Frames[i];
            frames[i] = new ReplayFrameData
            {
                Tick = EncodeTick(frame),
                Input = EncodeInput(frame.Input),
                History = EncodeHistory(frame.Pre, frame.SourcePlayerTick, replay.TickRate)
            };
            frame.Subticks.CopyTo(subs, offset);
            offset += frame.Subticks.Length;
        }
        return true;
    }

    // Keep the legacy recording file shape private, but expose one frame per tick.
    internal static ReplayData FromRecording(float tickRate, NativeReplayTick[] ticks, SubtickMove[] subs, NativeReplayInput[] commands)
    {
        if (!float.IsFinite(tickRate) || tickRate <= 0 || commands.Length != ticks.Length)
            throw new InvalidDataException("Recording rate or command count is invalid.");
        var frames = new ReplayFrame[ticks.Length];
        int offset = 0;
        for (int i = 0; i < ticks.Length; ++i)
        {
            uint count = ticks[i].NumSubtick;
            if (count > 36 || count > subs.Length - offset)
                throw new InvalidDataException("Recording subticks do not match its frames.");
            frames[i] = Decode(new ReplayFrameData
            {
                Tick = ticks[i], Input = commands[i],
                History = new NativeReplayHistory { SourcePlayerTick = -1 }
            }, subs.AsSpan(offset, (int)count).ToArray());
            offset += (int)count;
        }
        if (offset != subs.Length)
            throw new InvalidDataException("Recording has unclaimed subticks.");
        return new ReplayData { TickRate = tickRate, Frames = frames };
    }

    // Decode presence independently of zero, including the recorded clock and drop event.
    internal static ReplayFrame Decode(ReplayFrameData data, SubtickMove[] subticks)
    {
        var pre = DecodeSnapshot(data.Tick.Pre);
        var history = data.History;
        if ((history.Fields & 1) != 0) pre.JumpPressedTime = history.JumpPressedTime;
        if ((history.Fields & 2) != 0) pre.LastDuckTime = history.LastDuckTime;
        if ((history.Fields & 4) != 0) pre.LastActualJumpPress = new(history.LastActualJumpPressTick, history.LastActualJumpPressFrac);
        if ((history.Fields & 8) != 0) pre.LastUsableJumpPress = new(history.LastUsableJumpPressTick, history.LastUsableJumpPressFrac);
        if ((history.Fields & 16) != 0) pre.LastLanded = new(history.LastLandedTick, history.LastLandedFrac);
        if ((history.Fields & 32) != 0) pre.LastLandedVelocity = new Vector3(history.LastLandedVelocityX, history.LastLandedVelocityY, history.LastLandedVelocityZ);
        return new ReplayFrame
        {
            SourcePlayerTick = history.SourcePlayerTick >= 0 ? history.SourcePlayerTick : null,
            Pre = pre, Post = DecodeSnapshot(data.Tick.Post), Input = DecodeInput(data.Input),
            WeaponDefIndex = data.Tick.WeaponDefIndex, Subticks = subticks, Drop = DecodeDrop(data.Tick)
        };
    }

    // The post boundary is output reference only; movement history is pre-command input.
    private static bool HasHistory(MovementSnapshot snapshot)
        => snapshot.JumpPressedTime.HasValue || snapshot.LastDuckTime.HasValue ||
           snapshot.LastActualJumpPress.HasValue || snapshot.LastUsableJumpPress.HasValue ||
           snapshot.LastLanded.HasValue || snapshot.LastLandedVelocity.HasValue;

    // Preserve the accepted disk/native prefix without publishing padding or field masks.
    private static MovementSnapshot DecodeSnapshot(NativeMovementSnapshot value) => new()
    {
        OriginX = value.OriginX,
        OriginY = value.OriginY,
        OriginZ = value.OriginZ,
        VelX = value.VelX,
        VelY = value.VelY,
        VelZ = value.VelZ,
        Pitch = value.Pitch,
        Yaw = value.Yaw,
        Roll = value.Roll,
        EntityFlags = value.EntityFlags,
        MoveType = value.MoveType,
        Buttons = value.Buttons,
        Buttons1 = value.Buttons1,
        Buttons2 = value.Buttons2,
        DuckAmount = value.DuckAmount,
        DuckSpeed = value.DuckSpeed,
        LadderNormalX = value.LadderNormalX,
        LadderNormalY = value.LadderNormalY,
        LadderNormalZ = value.LadderNormalZ,
        Ducked = value.Ducked,
        Ducking = value.Ducking,
        DesiresDuck = value.DesiresDuck,
        ActualMoveType = value.ActualMoveType,
    };

    // Marshal only the existing movement boundary fields.
    private static NativeMovementSnapshot EncodeSnapshot(MovementSnapshot value) => new()
    {
        OriginX = value.OriginX,
        OriginY = value.OriginY,
        OriginZ = value.OriginZ,
        VelX = value.VelX,
        VelY = value.VelY,
        VelZ = value.VelZ,
        Pitch = value.Pitch,
        Yaw = value.Yaw,
        Roll = value.Roll,
        EntityFlags = value.EntityFlags,
        MoveType = value.MoveType,
        Buttons = value.Buttons,
        Buttons1 = value.Buttons1,
        Buttons2 = value.Buttons2,
        DuckAmount = value.DuckAmount,
        DuckSpeed = value.DuckSpeed,
        LadderNormalX = value.LadderNormalX,
        LadderNormalY = value.LadderNormalY,
        LadderNormalZ = value.LadderNormalZ,
        Ducked = value.Ducked,
        Ducking = value.Ducking,
        DesiresDuck = value.DesiresDuck,
        ActualMoveType = value.ActualMoveType,
    };

    // Encode one tick's boundaries and optional native drop release pose.
    private static NativeReplayTick EncodeTick(ReplayFrame frame)
    {
        var tick = new NativeReplayTick
        {
            Pre = EncodeSnapshot(frame.Pre), Post = EncodeSnapshot(frame.Post),
            WeaponDefIndex = frame.WeaponDefIndex, NumSubtick = (uint)frame.Subticks.Length
        };
        if (frame.Drop is not { } drop) return tick;
        tick.EventFlags = 1;
        tick.EventWeaponDefIndex = drop.WeaponDefIndex;
        tick.EventDropVectorFlags = 8;
        if (drop.Target is { } target)
        {
            tick.EventDropVectorFlags |= 1;
            tick.EventDropTargetX = target.X; tick.EventDropTargetY = target.Y; tick.EventDropTargetZ = target.Z;
        }
        if (drop.Velocity is { } velocity)
        {
            tick.EventDropVectorFlags |= 2;
            tick.EventDropVelocityX = velocity.X; tick.EventDropVelocityY = velocity.Y; tick.EventDropVelocityZ = velocity.Z;
        }
        if (drop.BodyYaw is { } yaw)
        {
            tick.EventDropVectorFlags |= 4;
            tick.EventDropTargetX = yaw;
        }
        tick.EventDropReleaseX = drop.ReleasePosition.X; tick.EventDropReleaseY = drop.ReleasePosition.Y; tick.EventDropReleaseZ = drop.ReleasePosition.Z;
        tick.EventDropReleaseQuatX = drop.ReleaseRotation.X; tick.EventDropReleaseQuatY = drop.ReleaseRotation.Y;
        tick.EventDropReleaseQuatZ = drop.ReleaseRotation.Z; tick.EventDropReleaseQuatW = drop.ReleaseRotation.W;
        return tick;
    }

    // Decode drop presence instead of interpreting an all-zero tail as an event.
    private static ReplayDrop? DecodeDrop(NativeReplayTick tick) => (tick.EventFlags & 1) == 0 ? null : new()
    {
        WeaponDefIndex = tick.EventWeaponDefIndex,
        Target = (tick.EventDropVectorFlags & 1) != 0 ? new Vector3(tick.EventDropTargetX, tick.EventDropTargetY, tick.EventDropTargetZ) : null,
        Velocity = (tick.EventDropVectorFlags & 2) != 0 ? new Vector3(tick.EventDropVelocityX, tick.EventDropVelocityY, tick.EventDropVelocityZ) : null,
        BodyYaw = (tick.EventDropVectorFlags & 4) != 0 ? tick.EventDropTargetX : null,
        ReleasePosition = new(tick.EventDropReleaseX, tick.EventDropReleaseY, tick.EventDropReleaseZ),
        ReleaseRotation = new(tick.EventDropReleaseQuatX, tick.EventDropReleaseQuatY, tick.EventDropReleaseQuatZ, tick.EventDropReleaseQuatW)
    };

    // Each nullable group maps to exactly one existing command presence bit.
    private static NativeReplayInput EncodeInput(ReplayInput input)
    {
        var command = new NativeReplayInput { Fields = 1U << 8 };
        if (input.ForwardMove is { } forward) { command.Fields |= 1; command.ForwardMove = forward; }
        if (input.LeftMove is { } left) { command.Fields |= 2; command.LeftMove = left; }
        if (input.UpMove is { } up) { command.Fields |= 4; command.UpMove = up; }
        if (input.ViewAngles is { } view)
        {
            command.Fields |= 8; command.Pitch = view.X; command.Yaw = view.Y; command.Roll = view.Z;
        }
        if (input.Buttons is { } buttons)
        {
            command.Fields |= 16; command.Buttons = buttons.Held;
            command.Buttons1 = buttons.Changed; command.Buttons2 = buttons.PressedAndReleased;
        }
        if (input.Mouse is { } mouse)
        {
            command.Fields |= 32; command.MouseDx = mouse.Dx; command.MouseDy = mouse.Dy;
        }
        if (input.WeaponSelectDefIndex is { } weapon) { command.Fields |= 64; command.WeaponSelect = weapon; }
        if (input.LeftHandDesired is { } hand) { command.Fields |= 128; command.LeftHandDesired = hand ? (byte)1 : (byte)0; }
        return command;
    }

    // Never expose source entity indices as live-server weapon definitions.
    private static ReplayInput DecodeInput(NativeReplayInput command)
    {
        if ((command.Fields & 64) != 0 && command.WeaponSelect > 0 && (command.Fields & 256) == 0)
            throw new InvalidDataException("Recording weapon selection is not an item definition.");
        return new ReplayInput
        {
            ForwardMove = (command.Fields & 1) != 0 ? command.ForwardMove : null,
            LeftMove = (command.Fields & 2) != 0 ? command.LeftMove : null,
            UpMove = (command.Fields & 4) != 0 ? command.UpMove : null,
            ViewAngles = (command.Fields & 8) != 0 ? new Vector3(command.Pitch, command.Yaw, command.Roll) : null,
            Buttons = (command.Fields & 16) != 0 ? new ReplayButtons(command.Buttons, command.Buttons1, command.Buttons2) : null,
            Mouse = (command.Fields & 32) != 0 ? new ReplayMouse(command.MouseDx, command.MouseDy) : null,
            WeaponSelectDefIndex = (command.Fields & 64) != 0 ? command.WeaponSelect : null,
            LeftHandDesired = (command.Fields & 128) != 0 ? command.LeftHandDesired != 0 : null
        };
    }

    // Build the private mask from complete public values; root tickrate owns the clock rate.
    private static NativeReplayHistory EncodeHistory(MovementSnapshot pre, int? sourceTick, float tickRate)
    {
        var history = new NativeReplayHistory { SourcePlayerTick = sourceTick ?? -1, SourceTickrate = tickRate };
        if (pre.JumpPressedTime is { } jump) { history.Fields |= 1; history.JumpPressedTime = jump; }
        if (pre.LastDuckTime is { } duck) { history.Fields |= 2; history.LastDuckTime = duck; }
        if (pre.LastActualJumpPress is { } actual)
        {
            history.Fields |= 4; history.LastActualJumpPressTick = actual.Tick; history.LastActualJumpPressFrac = actual.Fraction;
        }
        if (pre.LastUsableJumpPress is { } usable)
        {
            history.Fields |= 8; history.LastUsableJumpPressTick = usable.Tick; history.LastUsableJumpPressFrac = usable.Fraction;
        }
        if (pre.LastLanded is { } landed)
        {
            history.Fields |= 16; history.LastLandedTick = landed.Tick; history.LastLandedFrac = landed.Fraction;
        }
        if (pre.LastLandedVelocity is { } velocity)
        {
            history.Fields |= 32; history.LastLandedVelocityX = velocity.X; history.LastLandedVelocityY = velocity.Y; history.LastLandedVelocityZ = velocity.Z;
        }
        return history;
    }
}
