using System.Numerics;

namespace BotControllerApi;

internal static class ReplaySourceStateCodec
{
    internal const ulong WeaponFields = 0x1ffUL << 27;
    internal const ulong ClockFields = (1UL << 3) | (1UL << 24) | (1UL << 26) |
                                      (1UL << 27) | (1UL << 29) | (1UL << 33) | (1UL << 35);

    // Group complete nullable values, preserving explicit zeros and inactive time sentinels.
    internal static NativeReplaySourceState Encode(MovementSnapshot pre)
    {
        var data = new NativeReplaySourceState
        {
            WeaponDefIndex = pre.Weapon?.DefIndex ?? -1, WeaponInstanceId = pre.Weapon?.InstanceId ?? 0
        };
        SetFloat(ref data, 0, pre.DuckRoot); SetFloat(ref data, 1, pre.DuckView);
        SetBool(ref data, 2, pre.DuckOverride); SetTime(ref data, 3, pre.LastJump);
        SetFloat(ref data, 5, pre.LastJumpVelocityZ); SetBool(ref data, 6, pre.UsingGroundTopology);
        SetFloat(ref data, 7, pre.GroundTopologySmoothing); SetFloat(ref data, 8, pre.FrictionStashedSpeed);
        SetBool(ref data, 9, pre.UseFrictionStashedSpeed); SetFloat(ref data, 10, pre.FrictionStashedUntilFraction);
        SetInt(ref data, 11, pre.LadderSurface); SetFloat(ref data, 12, pre.FallVelocity);
        SetInt(ref data, 13, pre.ShotsFired); SetBool(ref data, 14, pre.Scoped);
        SetVector(ref data, 15, pre.PredictableAngle); SetVector(ref data, 18, pre.PredictableAngleVelocity);
        SetVector(ref data, 21, pre.UnpredictableAngle); SetTime(ref data, 24, pre.PredictableAngleTime);
        SetInt(ref data, 26, pre.UnpredictableAngleTick);
        if (pre.Weapon is { } weapon)
        {
            SetTime(ref data, 27, weapon.NextPrimaryAttack); SetTime(ref data, 29, weapon.NextSecondaryAttack);
            SetFloat(ref data, 31, weapon.RecoilIndex); SetFloat(ref data, 32, weapon.AccuracyPenalty);
            SetFloat(ref data, 33, weapon.LastShotTime); SetInt(ref data, 34, weapon.BurstShotsRemaining);
            SetFloat(ref data, 35, weapon.NextAttack);
        }
        return data;
    }

    // Decode only present groups; absent source-state leaves live-engine state untouched.
    internal static void Decode(in NativeReplaySourceState data, ref MovementSnapshot pre)
    {
        pre.DuckRoot = Float(data, 0); pre.DuckView = Float(data, 1); pre.DuckOverride = Bool(data, 2);
        pre.LastJump = Time(data, 3); pre.LastJumpVelocityZ = Float(data, 5);
        pre.UsingGroundTopology = Bool(data, 6); pre.GroundTopologySmoothing = Float(data, 7);
        pre.FrictionStashedSpeed = Float(data, 8); pre.UseFrictionStashedSpeed = Bool(data, 9);
        pre.FrictionStashedUntilFraction = Float(data, 10); pre.LadderSurface = Int(data, 11);
        pre.FallVelocity = Float(data, 12); pre.ShotsFired = Int(data, 13); pre.Scoped = Bool(data, 14);
        pre.PredictableAngle = Vector(data, 15); pre.PredictableAngleVelocity = Vector(data, 18);
        pre.UnpredictableAngle = Vector(data, 21); pre.PredictableAngleTime = Time(data, 24);
        pre.UnpredictableAngleTick = Int(data, 26);
        if ((data.Fields & WeaponFields) != 0)
            pre.Weapon = new ReplayWeaponState
            {
                DefIndex = data.WeaponDefIndex, InstanceId = data.WeaponInstanceId != 0 ? data.WeaponInstanceId : null,
                NextPrimaryAttack = Time(data, 27), NextSecondaryAttack = Time(data, 29),
                RecoilIndex = Float(data, 31), AccuracyPenalty = Float(data, 32), LastShotTime = Float(data, 33),
                BurstShotsRemaining = Int(data, 34), NextAttack = Float(data, 35)
            };
    }

    // Reject nonfinite values, partial groups, invalid fractions and missing weapon identity.
    internal static bool Validate(in NativeReplaySourceState data)
    {
        if ((data.Fields >> 36) != 0 || ((data.Fields & WeaponFields) != 0 && data.WeaponDefIndex <= 0)) return false;
        foreach (int start in (ReadOnlySpan<int>)[3, 24, 27, 29])
        {
            ulong mask = 3UL << start;
            if ((data.Fields & mask) != 0 && (data.Fields & mask) != mask) return false;
        }
        foreach (int start in (ReadOnlySpan<int>)[15, 18, 21])
        {
            ulong mask = 7UL << start;
            if ((data.Fields & mask) != 0 && (data.Fields & mask) != mask) return false;
        }
        for (int i = 0; i < 36; ++i)
        {
            if ((data.Fields & (1UL << i)) == 0) continue;
            if (i is 2 or 6 or 9 or 14) { if (data.Values[i] > 1) return false; }
            else if (i is not (3 or 11 or 13 or 24 or 26 or 27 or 29 or 34))
            {
                float value = BitConverter.UInt32BitsToSingle(data.Values[i]);
                if (!float.IsFinite(value) || ((i is 4 or 25 or 28 or 30) && (value < 0 || value >= 1))) return false;
            }
        }
        return true;
    }

    // Scalar helpers own the private bit representation, not the public model.
    private static void SetFloat(ref NativeReplaySourceState data, int index, float? value)
    {
        if (value is not { } supplied) return;
        data.Fields |= 1UL << index; data.Values[index] = BitConverter.SingleToUInt32Bits(supplied);
    }
    // Integer sentinels retain their signed bit pattern.
    private static void SetInt(ref NativeReplaySourceState data, int index, int? value)
    {
        if (value is not { } supplied) return;
        data.Fields |= 1UL << index; data.Values[index] = unchecked((uint)supplied);
    }
    // Native booleans occupy one byte in engine memory but four in this private transport.
    private static void SetBool(ref NativeReplaySourceState data, int index, bool? value)
    {
        if (value is not { } supplied) return;
        data.Fields |= 1UL << index; data.Values[index] = supplied ? 1U : 0U;
    }
    // Pair tick and fraction presence so malformed half-timestamps cannot be produced.
    private static void SetTime(ref NativeReplaySourceState data, int index, ReplayTimestamp? value)
    {
        if (value is not { } supplied) return;
        SetInt(ref data, index, supplied.Tick); SetFloat(ref data, index + 1, supplied.Fraction);
    }
    // Vectors are one public value and one complete native presence group.
    private static void SetVector(ref NativeReplaySourceState data, int index, Vector3? value)
    {
        if (value is not { } supplied) return;
        SetFloat(ref data, index, supplied.X); SetFloat(ref data, index + 1, supplied.Y); SetFloat(ref data, index + 2, supplied.Z);
    }
    // Read selected float bits without treating zero as absent.
    private static float? Float(in NativeReplaySourceState data, int index)
        => (data.Fields & (1UL << index)) != 0 ? BitConverter.UInt32BitsToSingle(data.Values[index]) : null;
    // Read selected signed integer bits, including nonpositive sentinels.
    private static int? Int(in NativeReplaySourceState data, int index)
        => (data.Fields & (1UL << index)) != 0 ? unchecked((int)data.Values[index]) : null;
    // Read boolean presence independently of its value.
    private static bool? Bool(in NativeReplaySourceState data, int index)
        => (data.Fields & (1UL << index)) != 0 ? data.Values[index] != 0 : null;
    // Decode an already validated complete timestamp.
    private static ReplayTimestamp? Time(in NativeReplaySourceState data, int index)
        => Int(data, index) is { } tick ? new(tick, Float(data, index + 1)!.Value) : null;
    // Decode an already validated complete vector.
    private static Vector3? Vector(in NativeReplaySourceState data, int index)
        => Float(data, index) is { } x ? new(x, Float(data, index + 1)!.Value, Float(data, index + 2)!.Value) : null;
}
