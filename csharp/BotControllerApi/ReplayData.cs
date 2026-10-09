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
    public int? SourcePlayerTick { get; init; }
    public MovementSnapshot Pre { get; init; }
    public ReplayInput Input { get; init; }
    public MovementSnapshot Post { get; init; }
    public int WeaponDefIndex { get; init; } = -1;
    public SubtickMove[] Subticks { get; init; } = [];
    public ReplayDrop? Drop { get; init; }
}

// Optional source state is restored from Pre; Post describes the recorded output.
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
    // Supplied modifiers are seeded at replay boundaries; null leaves engine state alone.
    public float? Stamina, VelocityModifier, GravityScale, Friction;
    public bool? GravityDisabled;
    // External velocity is one complete vector, not the player's own VelXYZ.
    public Vector3? BaseVelocity;
    // Additional command inputs; nullable groups never invent missing engine state.
    public float? DuckRoot, DuckView;
    public bool? DuckOverride;
    public ReplayTimestamp? LastJump;
    public float? LastJumpVelocityZ;
    public bool? UsingGroundTopology;
    public float? GroundTopologySmoothing, FrictionStashedSpeed;
    public bool? UseFrictionStashedSpeed;
    public float? FrictionStashedUntilFraction, FallVelocity;
    public int? LadderSurface, ShotsFired;
    public bool? Scoped;
    public Vector3? PredictableAngle, PredictableAngleVelocity, UnpredictableAngle;
    public ReplayTimestamp? PredictableAngleTime;
    public int? UnpredictableAngleTick;
    public ReplayWeaponState? Weapon;
}

public readonly record struct ReplayTimestamp(int Tick, float Fraction);

// Pre-command weapon state belongs to this item definition, not a source entity handle.
// Ammunition and reload state remain owned by the live engine.
public struct ReplayWeaponState
{
    public int DefIndex;
    // Opaque recording-local identity, used only to distinguish instances of the same item.
    public uint? InstanceId;
    public ReplayTimestamp? NextPrimaryAttack, NextSecondaryAttack;
    public float? RecoilIndex, AccuracyPenalty, LastShotTime, NextAttack;
    public int? BurstShotsRemaining;
}

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
