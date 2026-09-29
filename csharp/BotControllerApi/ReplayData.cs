// One public frame model for recordings and external replay producers.

using System.Numerics;

namespace BotControllerApi;

public sealed class ReplayData
{
    public required float TickRate { get; init; }
    public required ReplayFrame[] Frames { get; init; }
}

public sealed class ReplayFrame
{
    // Recorded pre-command player tickbase, not the demo's global tick.
    public int? SourcePlayerTick { get; init; }
    public MovementSnapshot Pre { get; init; }
    public ReplayInput Input { get; init; }
    public MovementSnapshot Post { get; init; }
    public int WeaponDefIndex { get; init; } = -1;
    public SubtickMove[] Subticks { get; init; } = [];
    public ReplayDrop? Drop { get; init; }
}

// Optional history is restored from Pre; Post describes the recorded output.
public struct MovementSnapshot
{
    public float OriginX, OriginY, OriginZ;
    public float VelX, VelY, VelZ;
    public float Pitch, Yaw, Roll;
    public uint EntityFlags;
    public byte MoveType;
    public ulong Buttons, Buttons1, Buttons2;
    public float DuckAmount, DuckSpeed;
    public float LadderNormalX, LadderNormalY, LadderNormalZ;
    public byte Ducked, Ducking, DesiresDuck, ActualMoveType;
    // Null means absent; zero and non-positive time sentinels remain real values.
    public float? JumpPressedTime;
    public float? LastDuckTime;
    public ReplayTimestamp? LastActualJumpPress;
    public ReplayTimestamp? LastUsableJumpPress;
    public ReplayTimestamp? LastLanded;
    public Vector3? LastLandedVelocity;
}

public readonly record struct ReplayTimestamp(int Tick, float Fraction);

// The three CS2 button planes must be supplied or omitted together.
public readonly record struct ReplayButtons(ulong Held, ulong Changed, ulong PressedAndReleased);
public readonly record struct ReplayMouse(int Dx, int Dy);

// Null fields use existing native fallback; explicitly supplied zero never does.
public struct ReplayInput
{
    public float? ForwardMove, LeftMove, UpMove;
    public Vector3? ViewAngles;
    public ReplayButtons? Buttons;
    public ReplayMouse? Mouse;
    // Item definition, never an entity index from the source server.
    public int? WeaponSelectDefIndex;
    public bool? LeftHandDesired;
}

public sealed class ReplayDrop
{
    public int WeaponDefIndex { get; init; } = -1;
    public Vector3? Target { get; init; }
    public Vector3? Velocity { get; init; }
    public float? BodyYaw { get; init; }
    public Vector3 ReleasePosition { get; init; }
    public Quaternion ReleaseRotation { get; init; }
}
