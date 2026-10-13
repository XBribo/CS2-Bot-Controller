// Optional pre-command state shared by native recordings and external replay producers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cs2bc {
#pragma pack(push, 4)
struct ReplaySourceStateData
{
    uint64_t fields;
    int32_t weaponDefIndex;
    uint32_t weaponInstanceId; // Opaque source identity; never written into engine handles.
    uint32_t values[36];
};
#pragma pack(pop)
static_assert(sizeof(ReplaySourceStateData) == 160);

namespace replay_source_state {
constexpr uint64_t kWeaponFields = uint64_t{ 0x1ff } << 27;
constexpr uint64_t kClockFields = (uint64_t{ 1 } << 3) | (uint64_t{ 1 } << 24) | (uint64_t{ 1 } << 26) |
                                  (uint64_t{ 1 } << 27) | (uint64_t{ 1 } << 29) | (uint64_t{ 1 } << 33) | (uint64_t{ 1 } << 35);
// Movement, pawn, aim-punch service, identified weapon, weapon service.
using Targets = std::array<void*, 5>;
using ReadMemory = bool (*)(const void*, int, void*, size_t);
using WriteMemory = bool (*)(void*, int, const void*, size_t);
struct Write
{
    void* base{};
    int offset{};
    uint32_t bits{};
    size_t size{};
};
using Writes = std::array<Write, 36>;

// Resolve optional fields only from Schema; absent offsets are never guessed.
void ResolveOffsets(int (*find)(const char*, const char*));
// Capture complete groups from the actual command-pre targets.
void Capture(const Targets& targets, int weaponDef, uint32_t weaponInstance, int sourceTick, ReadMemory read, ReplaySourceStateData& out);
// Validate selected scalars, groups, weapon identity and clock prerequisites.
bool Validate(const ReplaySourceStateData& data, int sourceTick, float sourceRate);
// Stage all targets and rebased values before making any engine-state changes.
bool Prepare(const Targets& targets, const ReplaySourceStateData& data, int sourceTick, float sourceRate,
             int liveTick, float interval, Writes& writes);
// Apply staged writes through the same guarded engine memory API as movement state.
bool Apply(const Writes& writes, WriteMemory write);
} // namespace replay_source_state
} // namespace cs2bc
