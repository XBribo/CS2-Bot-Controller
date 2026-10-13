// Native source-state capture and replay use one descriptor table and group contract.
#include "ReplaySourceState.h"

#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace cs2bc::replay_source_state {
namespace {
enum Target { Movement, Pawn, Aim, Weapon, WeaponServices };
enum Kind { Float, Integer, Boolean, Tick, Fraction, Seconds };
struct Field
{
    Target target;
    Kind kind;
    const char* name;
    int component = 0;
};
constexpr std::array<Field, 36> kFields{{
    { Movement, Float, "m_flDuckRootOffset" },
    { Movement, Float, "m_flDuckViewOffset" },
    { Movement, Boolean, "m_bDuckOverride" },
    { Movement, Tick, "m_nLastJumpTick" },
    { Movement, Fraction, "m_flLastJumpFrac" },
    { Movement, Float, "m_flLastJumpVelocityZ" },
    { Movement, Boolean, "m_bUsingGroundTopologyOffset" },
    { Movement, Float, "m_flUsingGroundTopologyOffsetTransitionSmoothing" },
    { Movement, Float, "m_flFrictionStashedSpeed" },
    { Movement, Boolean, "m_bUseFrictionStashedSpeed" },
    { Movement, Float, "m_flUseFrictionStashedSpeedUntilFrac" },
    { Movement, Integer, "m_nLadderSurfacePropIndex" },
    { Movement, Float, "m_flFallVelocity" },
    { Pawn, Integer, "m_iShotsFired" },
    { Pawn, Boolean, "m_bIsScoped" },
    { Aim, Float, "m_predictableBaseAngle", 0 },
    { Aim, Float, "m_predictableBaseAngle", 4 },
    { Aim, Float, "m_predictableBaseAngle", 8 },
    { Aim, Float, "m_predictableBaseAngleVel", 0 },
    { Aim, Float, "m_predictableBaseAngleVel", 4 },
    { Aim, Float, "m_predictableBaseAngleVel", 8 },
    { Aim, Float, "m_unpredictableBaseAngle", 0 },
    { Aim, Float, "m_unpredictableBaseAngle", 4 },
    { Aim, Float, "m_unpredictableBaseAngle", 8 },
    { Aim, Tick, "m_predictableBaseTick" },
    { Aim, Fraction, "m_predictableBaseTickInterpAmount" },
    { Aim, Tick, "m_unpredictableBaseTick" },
    { Weapon, Tick, "m_nNextPrimaryAttackTick" },
    { Weapon, Fraction, "m_flNextPrimaryAttackTickRatio" },
    { Weapon, Tick, "m_nNextSecondaryAttackTick" },
    { Weapon, Fraction, "m_flNextSecondaryAttackTickRatio" },
    { Weapon, Float, "m_flRecoilIndex" },
    { Weapon, Float, "m_fAccuracyPenalty" },
    { Weapon, Seconds, "m_fLastShotTime" },
    { Weapon, Integer, "m_iBurstShotsRemaining" },
    { WeaponServices, Seconds, "m_flNextAttack" },
}};
std::array<int, 36> g_offsets = [] { std::array<int, 36> result{}; result.fill(-1); return result; }();

// Complete vectors and timestamps are indivisible presence groups.
uint64_t GroupMask(size_t index)
{
    for (size_t start : { 3U, 24U, 27U, 29U })
        if (index >= start && index < start + 2) return uint64_t{ 3 } << start;
    for (size_t start : { 15U, 18U, 21U })
        if (index >= start && index < start + 3) return uint64_t{ 7 } << start;
    return uint64_t{ 1 } << index;
}

// Selected floats must be finite; boolean and fraction encodings are bounded.
bool ValidValue(size_t index, uint32_t bits)
{
    const Kind kind = kFields[index].kind;
    if (kind == Boolean) return bits <= 1;
    if (kind == Integer || kind == Tick) return true;
    const float value = std::bit_cast<float>(bits);
    return std::isfinite(value) && (kind != Fraction || (value >= 0.0F && value < 1.0F));
}

// Consume class-name arrays within their initializer-list lifetime.
int FindField(int (*find)(const char*, const char*), std::initializer_list<const char*> classes, const char* field)
{
    for (const char* name : classes)
    {
        const int offset = find(name, field);
        if (offset >= 0) return offset;
    }
    return -1;
}
} // namespace

// Search each target's declaring hierarchy without depending on engine SDK types.
void ResolveOffsets(int (*find)(const char*, const char*))
{
    for (size_t i = 0; i < kFields.size(); ++i)
    {
        const auto& field = kFields[i];
        int offset = -1;
        switch (field.target)
        {
        case Movement: offset = FindField(find, { "CCSPlayer_MovementServices", "CPlayer_MovementServices_Humanoid", "CPlayer_MovementServices" }, field.name); break;
        case Pawn: offset = FindField(find, { "CCSPlayerPawn", "CCSPlayerPawnBase", "CBasePlayerPawn", "CBaseEntity" }, field.name); break;
        case Aim: offset = FindField(find, { "CCSPlayer_AimPunchServices" }, field.name); break;
        case Weapon: offset = FindField(find, { "CCSWeaponBaseGun", "CCSWeaponBase", "CBasePlayerWeapon" }, field.name); break;
        case WeaponServices: offset = FindField(find, { "CCSPlayer_WeaponServices", "CPlayer_WeaponServices" }, field.name); break;
        }
        g_offsets[i] = offset < 0 ? -1 : offset + field.component;
    }
}

// Missing targets or invalid reads omit whole groups, never manufacture zeros.
void Capture(const Targets& targets, int weaponDef, uint32_t weaponInstance, int sourceTick, ReadMemory read, ReplaySourceStateData& out)
{
    out = {};
    out.weaponDefIndex = weaponDef;
    out.weaponInstanceId = weaponInstance;
    for (size_t i = 0; i < kFields.size(); ++i)
    {
        const auto& field = kFields[i];
        if (!targets[field.target] || g_offsets[i] < 0 ||
            ((field.target == Weapon || field.target == WeaponServices) && (weaponDef <= 0 || !targets[Weapon])) ||
            ((kClockFields & (uint64_t{ 1 } << i)) != 0 && sourceTick < 0)) continue;
        const size_t size = field.kind == Boolean ? 1 : 4;
        if (read(targets[field.target], g_offsets[i], &out.values[i], size) && ValidValue(i, out.values[i]))
            out.fields |= uint64_t{ 1 } << i;
    }
    for (size_t i = 0; i < kFields.size(); ++i)
    {
        const uint64_t group = GroupMask(i);
        if ((out.fields & group) != group) out.fields &= ~group;
    }
}

// Reject malformed input before replacing loaded replay data or mutating live memory.
bool Validate(const ReplaySourceStateData& data, int sourceTick, float sourceRate)
{
    if ((data.fields >> 36) != 0 || ((data.fields & kWeaponFields) != 0 && data.weaponDefIndex <= 0)) return false;
    if ((data.fields & kClockFields) != 0 && (sourceTick < 0 || !std::isfinite(sourceRate) || sourceRate <= 0.0F)) return false;
    for (size_t i = 0; i < kFields.size(); ++i)
    {
        if ((data.fields & (uint64_t{ 1 } << i)) == 0) continue;
        const uint64_t group = GroupMask(i);
        if ((data.fields & group) != group || !ValidValue(i, data.values[i])) return false;
    }
    return true;
}

// Rebase positive clock values to the live command's tickbase; leave sentinels unchanged.
bool Prepare(const Targets& targets, const ReplaySourceStateData& data, int sourceTick, float sourceRate,
             int liveTick, float interval, Writes& writes)
{
    writes = {};
    if (!Validate(data, sourceTick, sourceRate)) return false;
    if ((data.fields & kClockFields) != 0 && (liveTick < 0 || !std::isfinite(interval) || interval <= 0.0F ||
        std::fabs(static_cast<double>(sourceRate) * interval - 1.0) > 0.0001)) return false;
    Writes staged{};
    for (size_t i = 0; i < kFields.size(); ++i)
    {
        if ((data.fields & (uint64_t{ 1 } << i)) == 0) continue;
        const auto& field = kFields[i];
        if (!targets[field.target] || g_offsets[i] < 0) return false;
        uint32_t bits = data.values[i];
        if (field.kind == Tick)
        {
            const int32_t tick = std::bit_cast<int32_t>(bits);
            if (tick > 0)
            {
                const int64_t mapped = static_cast<int64_t>(tick) - sourceTick + liveTick;
                if (mapped < std::numeric_limits<int32_t>::min() || mapped > std::numeric_limits<int32_t>::max()) return false;
                bits = std::bit_cast<uint32_t>(static_cast<int32_t>(mapped));
            }
        }
        else if (field.kind == Seconds)
        {
            const float value = std::bit_cast<float>(bits);
            if (value > 0.0F)
            {
                const double mapped = static_cast<double>(value) - static_cast<double>(sourceTick) / sourceRate + static_cast<double>(liveTick) * interval;
                if (!std::isfinite(mapped) || std::fabs(mapped) > std::numeric_limits<float>::max()) return false;
                bits = std::bit_cast<uint32_t>(static_cast<float>(mapped));
            }
        }
        staged[i] = { targets[field.target], g_offsets[i], bits, field.kind == Boolean ? size_t{ 1 } : size_t{ 4 } };
    }
    writes = staged;
    return true;
}

// Absent writes are skipped; failures stop replay rather than silently losing selected state.
bool Apply(const Writes& writes, WriteMemory write)
{
    for (const auto& item : writes)
        if (item.base && !write(item.base, item.offset, &item.bits, item.size)) return false;
    return true;
}
} // namespace cs2bc::replay_source_state
