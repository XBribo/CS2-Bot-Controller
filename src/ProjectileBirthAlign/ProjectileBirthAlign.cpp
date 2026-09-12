#include "ProjectileBirthAlign.h"

#include "ccsbot_slot.h"
#include "runtime.h"
#include "version_targets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace cs2bc::projectile_birth_align {

namespace {

constexpr int kMaxPending = 64;
constexpr int kMaxAttempts = 4;

// Result of one attempt to apply a pending projectile correction.
enum class ApplyResult
{
    Applied,

    // Entity still exists but some field is not ready/readable yet.
    // Retry for a few simulation callbacks.
    Retry,

    // The original projectile entity has disappeared or its address has been
    // reused for another entity. Never touch it again.
    Stale
};

struct Pending
{
    uint64_t entityPtr;

    // Snapshot of CEntityIdentity::m_EHandle at Queue() time.
    //
    // The pointer alone is not sufficient because Source may recycle entity
    // storage. Before writing we verify that the same entity still occupies
    // this address.
    uint32_t entityHandle;

    std::array<float, 3> position;
    std::array<float, 3> velocity;

    int attemptsRemaining;
};

std::mutex g_mutex;

std::vector<Pending> g_pending;

std::atomic<int> g_pendingCount{ 0 };

int g_initialPositionOffset = -1;
int g_initialVelocityOffset = -1;

// Diagnostics.
//
// Accessed while g_mutex is held.
int g_queued = 0;
int g_applied = 0;
int g_expired = 0;
int g_failed = 0;

// -----------------------------------------------------------------------------
// Validation helpers
// -----------------------------------------------------------------------------

bool IsFiniteVector(const std::array<float, 3>& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

// Reads an entity's current handle.
//
// This gives us an identity token which can be compared later before writing
// through a cached native pointer.
bool ReadEntityHandle(void* entity, uint32_t& outHandle)
{
    outHandle = 0;

    if (!entity) return false;

    void* identity = nullptr;

    if (!GuardedRead(entity, targets::g_entIdentity, identity) || !identity)
    {
        return false;
    }

    uint32_t handle = 0;

    if (!GuardedRead(identity, targets::g_entIdentityEHandle, handle))
    {
        return false;
    }

    if (handle == 0U || handle == 0xFFFFFFFFU || handle == 0xFFFFFFFEU)
    {
        return false;
    }

    outHandle = handle;

    return true;
}

// -----------------------------------------------------------------------------
// Resolves a projectile's scene node through its body component.
// -----------------------------------------------------------------------------

void* ResolveSceneNode(void* entity)
{
    if (!entity) return nullptr;

    void* body = nullptr;

    if (!GuardedRead(entity, targets::g_entBodyComponent, body) || !body)
    {
        return nullptr;
    }

    void* node = nullptr;

    return GuardedRead(body, targets::g_bodySceneNode, node) ? node : nullptr;
}

// -----------------------------------------------------------------------------
// Applies one queued projectile correction.
//
// IMPORTANT:
//
// Before writing anything, verify that the entity handle still matches the
// handle captured by Queue(). This prevents a delayed pending operation from
// writing into an unrelated entity if Source recycled the entity address.
// -----------------------------------------------------------------------------

ApplyResult Apply(const Pending& pending)
{
    if (g_initialPositionOffset < 0 || g_initialVelocityOffset < 0)
    {
        return ApplyResult::Retry;
    }

    void* entity = reinterpret_cast<void*>(static_cast<uintptr_t>(pending.entityPtr)); // NOLINT(performance-no-int-to-ptr)

    if (!entity) return ApplyResult::Stale;

    // ---------------------------------------------------------------------
    // Verify entity identity before performing ANY writes.
    // ---------------------------------------------------------------------

    uint32_t currentHandle = 0;

    if (!ReadEntityHandle(entity, currentHandle))
    {
        // The entity may still be in a short initialization/destruction
        // transition, so allow the normal retry budget to handle this.
        return ApplyResult::Retry;
    }

    if (currentHandle != pending.entityHandle)
    {
        // Address has been recycled for another entity.
        return ApplyResult::Stale;
    }

    const size_t vectorSize = sizeof(float) * pending.position.size();

    // ---------------------------------------------------------------------
    // Native projectile birth state
    // ---------------------------------------------------------------------

    if (!TryWriteMemoryGuarded(entity, g_initialPositionOffset, pending.position.data(), vectorSize))
    {
        return ApplyResult::Retry;
    }

    if (!TryWriteMemoryGuarded(entity, g_initialVelocityOffset, pending.velocity.data(), vectorSize))
    {
        return ApplyResult::Retry;
    }

    if (!TryWriteMemoryGuarded(entity, targets::g_entAbsVelocity, pending.velocity.data(), vectorSize))
    {
        return ApplyResult::Retry;
    }

    // ---------------------------------------------------------------------
    // Scene-node origin
    //
    // This is supplementary. The native projectile fields above establish
    // the trajectory. AbsOrigin is updated when the scene node is available.
    // ---------------------------------------------------------------------

    void* node = ResolveSceneNode(entity);

    if (node)
    {
        TryWriteMemoryGuarded(node, targets::g_nodeAbsOrigin, pending.position.data(), vectorSize);
    }

    return ApplyResult::Applied;
}

} // namespace

// -----------------------------------------------------------------------------
// ConfigureOffsets
//
// Core configuration. This intentionally remains available while runtime is
// disabled because the offsets belong to the loaded server build, not to an
// individual runtime activation cycle.
// -----------------------------------------------------------------------------

int ConfigureOffsets(int initialPositionOffset, int initialVelocityOffset)
{
    if (initialPositionOffset < 0 || initialVelocityOffset < 0)
    {
        return -2;
    }

    std::scoped_lock lock(g_mutex);

    g_initialPositionOffset = initialPositionOffset;

    g_initialVelocityOffset = initialVelocityOffset;

    return 0;
}

// -----------------------------------------------------------------------------
// Queue
//
// Queues one replay projectile's recorded birth position and velocity.
//
// Return:
//   0  success
//  -2  bad pointer / values / entity identity
//  -3  offsets not configured
//  -4  BotController runtime disabled
//
// Bot ownership itself is verified by the managed replay layer before this
// function is called. Native ProjectileBirthAlign only receives a projectile
// pointer, not its owner/controller.
// -----------------------------------------------------------------------------

int Queue(uint64_t entityPtr, float posX, float posY, float posZ, float velX, float velY, float velZ)
{
    // Never accumulate delayed writes while BotController runtime is OFF.
    if (!cs2bc::runtime::IsEnabled()) return -4;

    if (entityPtr == 0) return -2;

    const std::array<float, 3> position = { posX, posY, posZ };

    const std::array<float, 3> velocity = { velX, velY, velZ };

    // Do not allow NaN / Inf values into native engine state.
    if (!IsFiniteVector(position) || !IsFiniteVector(velocity))
    {
        return -2;
    }

    void* entity = reinterpret_cast<void*>(static_cast<uintptr_t>(entityPtr)); // NOLINT(performance-no-int-to-ptr)

    // Snapshot the projectile's entity identity.
    //
    // If this cannot be read now, do not retain an unverified raw pointer.
    uint32_t entityHandle = 0;

    if (!ReadEntityHandle(entity, entityHandle))
    {
        return -2;
    }

    std::scoped_lock lock(g_mutex);

    if (g_initialPositionOffset < 0 || g_initialVelocityOffset < 0)
    {
        return -3;
    }

    // ---------------------------------------------------------------------
    // Deduplicate
    //
    // The managed candidate may retry while the native correction is already
    // pending. Do not create multiple writes for the same projectile.
    //
    // Refresh the existing entry instead.
    // ---------------------------------------------------------------------

    auto existing = std::find_if(g_pending.begin(), g_pending.end(), [entityPtr, entityHandle](const Pending& pending) {
        return pending.entityPtr == entityPtr && pending.entityHandle == entityHandle;
    });

    if (existing != g_pending.end())
    {
        existing->position = position;

        existing->velocity = velocity;

        existing->attemptsRemaining = kMaxAttempts;

        ++g_queued;

        g_pendingCount.store(static_cast<int>(g_pending.size()), std::memory_order_release);

        return 0;
    }

    // ---------------------------------------------------------------------
    // Keep queue bounded.
    // ---------------------------------------------------------------------

    if (static_cast<int>(g_pending.size()) >= kMaxPending)
    {
        g_pending.erase(g_pending.begin());

        ++g_expired;
    }

    g_pending.push_back({ .entityPtr = entityPtr,
                          .entityHandle = entityHandle,

                          .position = { posX, posY, posZ },

                          .velocity = { velX, velY, velZ },

                          .attemptsRemaining = kMaxAttempts });

    g_pendingCount.store(static_cast<int>(g_pending.size()), std::memory_order_release);

    ++g_queued;

    return 0;
}

// -----------------------------------------------------------------------------
// Clear
//
// Clears only pending runtime work.
//
// The configured offsets remain intact because they belong to the plugin core
// and are reused when runtime is enabled again.
// -----------------------------------------------------------------------------

int Clear()
{
    std::scoped_lock lock(g_mutex);

    const int cleared = static_cast<int>(g_pending.size());

    g_pending.clear();

    g_pendingCount.store(0, std::memory_order_release);

    return cleared;
}

// -----------------------------------------------------------------------------
// GetStatus
// -----------------------------------------------------------------------------

int GetStatus(Status* out, int size)
{
    if (!out || size < 0 || static_cast<size_t>(size) < sizeof(Status))
    {
        return -1;
    }

    std::scoped_lock lock(g_mutex);

    Status status{};

    status.size = static_cast<int32_t>(sizeof(Status));

    status.configured = g_initialPositionOffset >= 0 && g_initialVelocityOffset >= 0 ? 1 : 0;

    status.pending = static_cast<int32_t>(g_pending.size());

    status.queued = g_queued;

    status.applied = g_applied;

    status.expired = g_expired;

    status.failed = g_failed;

    status.initialPositionOffset = g_initialPositionOffset;

    status.initialVelocityOffset = g_initialVelocityOffset;

    std::memcpy(out, &status, sizeof(status));

    return 0;
}

// -----------------------------------------------------------------------------
// ProcessPending
//
// Called from native simulation hooks.
//
// Runtime OFF is deliberately an immediate no-op. Normally runtime::Disable()
// has already called Clear(), but this guard protects against unexpected call
// ordering.
// -----------------------------------------------------------------------------

void ProcessPending()
{
    // Very cheap hot-path exit.
    if (g_pendingCount.load(std::memory_order_acquire) == 0)
    {
        return;
    }

    // Do not perform native projectile writes while runtime is disabled.
    if (!cs2bc::runtime::IsEnabled())
    {
        Clear();
        return;
    }

    std::scoped_lock lock(g_mutex);

    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        const ApplyResult result = Apply(*it);

        if (result == ApplyResult::Applied)
        {
            ++g_applied;

            it = g_pending.erase(it);

            continue;
        }

        if (result == ApplyResult::Stale)
        {
            // Projectile disappeared or this address was recycled.
            //
            // Retrying would risk touching an unrelated entity.
            ++g_failed;

            it = g_pending.erase(it);

            continue;
        }

        // Temporary failure: retry for a small number of simulation calls.
        --it->attemptsRemaining;

        if (it->attemptsRemaining <= 0)
        {
            ++g_failed;

            it = g_pending.erase(it);
        }
        else
        {
            ++it;
        }
    }

    g_pendingCount.store(static_cast<int>(g_pending.size()), std::memory_order_release);
}

} // namespace cs2bc::projectile_birth_align
