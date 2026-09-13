// BotController runtime lifecycle implementation.

#include "runtime.h"
#include <atomic>
#include <cstdio>

#include <tier0/dbg.h>

#include "WeaponLocker.h"
#include "WeaponLockerState.h"

#include "BotController.h"
#include "BotControllerState.h"

#include "BuyController.h"
#include "BuyControllerState.h"

#include "InputInjector.h"
#include "MotionRecorder.h"

#include "ProjectileBirthAlign.h"

namespace cs2bc::runtime {

namespace {

// -----------------------------------------------------------------------------
// Prepared/static runtime data.
//
// These values remain in memory while the MetaMod plugin is loaded so that
// hooks can be removed and reinstalled without re-reading gamedata or
// re-discovering the server module.
// -----------------------------------------------------------------------------

nlohmann::json g_gamedata;
cs2bc::sig::ModuleInfo g_serverModule;

bool g_prepared = false;
std::atomic<bool> g_enabled{ false };
bool g_runtimeRequested = false;
bool g_metaPaused = false;

// Safely writes an error message when an error buffer was supplied.
void SetError(char* error, size_t maxlen, const char* message)
{
    if (!error || maxlen == 0) return;

    std::snprintf(error, maxlen, "%s", message ? message : "");

    error[maxlen - 1] = '\0';
}

// Clears an existing error buffer.
void ClearError(char* error, size_t maxlen)
{
    if (!error || maxlen == 0) return;

    error[0] = '\0';
}

} // namespace

// -----------------------------------------------------------------------------
// Prepare
// -----------------------------------------------------------------------------

bool Prepare(const nlohmann::json& gamedata, const cs2bc::sig::ModuleInfo& serverModule, char* error, size_t maxlen)
{
    ClearError(error, maxlen);

    // Do not replace the data underneath an active runtime.
    if (g_enabled.load(std::memory_order_acquire))
    {
        SetError(error, maxlen, "Cannot prepare BotController runtime while it is enabled");

        return false;
    }

    if (!serverModule)
    {
        SetError(error, maxlen, "Invalid server module");

        return false;
    }

    if (gamedata.is_null())
    {
        SetError(error, maxlen, "Invalid gamedata");

        return false;
    }

    // Copy these values intentionally.
    //
    // The copies have to remain valid after BotControllerPlugin::Load()
    // returns because Enable() may later be called from MetaMod Unpause()
    // or from the managed/C ABI runtime controller.
    g_gamedata = gamedata;
    g_serverModule = serverModule;

    g_prepared = true;

    return true;
}

// -----------------------------------------------------------------------------
// Disable
//
// IMPORTANT:
// Runtime state is deliberately discarded.
//
// This plugin deals with live CS2 entity/player pointers. Keeping that state
// across a period where bots may be removed and slots reused would be unsafe.
// -----------------------------------------------------------------------------

void Disable()
{
    // Mark disabled first.
    //
    // NativeHook::Remove() waits for active hook invocations before destroying
    // callback storage, so after the following calls return no runtime callback
    // remains owned by these modules.
    g_enabled.store(false, std::memory_order_release);

    // ---------------------------------------------------------------------
    // 1. Player simulation hooks
    //
    // These are the most frequently called hooks and should disappear first.
    // ---------------------------------------------------------------------

    cs2bc::input_injector::Remove();

    // ---------------------------------------------------------------------
    // 2. Recording/replay state
    //
    // MotionRecorder also owns the lazily-installed DropWeapon hook.
    // ---------------------------------------------------------------------

    cs2bc::motion_recorder::ClearAll();

    // Pending projectile modifications must not survive runtime shutdown.
    cs2bc::projectile_birth_align::Clear();

    // ---------------------------------------------------------------------
    // 3. Remaining runtime hooks
    //
    // Reverse order of normal installation.
    // ---------------------------------------------------------------------

    cs2bc::buy_controller_hooks::Remove();
    cs2bc::bot_controller_hooks::Remove();
    cs2bc::weapon_locker_hooks::Remove();

    // ---------------------------------------------------------------------
    // 4. Transient logical state
    // ---------------------------------------------------------------------

    cs2bc::buy_controller_state::ClearAll();

    cs2bc::weapon_locker_state::ClearAll();

    cs2bc::bot_controller_state::ClearAllAll();
    cs2bc::bot_controller_state::ClearAllAim();
}

// -----------------------------------------------------------------------------
// Enable
// -----------------------------------------------------------------------------

bool Enable(char* error, size_t maxlen)
{
    ClearError(error, maxlen);

    // Absolutely critical:
    //
    // Current module Install() implementations are not guaranteed to tolerate
    // being called twice while their hooks are already installed.
    //
    // Therefore the runtime coordinator must prevent double installation.
    if (g_enabled.load(std::memory_order_acquire))
    {
        return true;
    }

    if (!g_prepared)
    {
        SetError(error, maxlen, "BotController runtime has not been prepared");

        return false;
    }

    if (!g_serverModule)
    {
        SetError(error, maxlen, "BotController server module is unavailable");

        return false;
    }

    // Ensure no hooks or transient state are left behind after a previous
    // partial/failed Enable().
    Disable();

    // ---------------------------------------------------------------------
    // 1. WeaponLocker
    //
    // Required module.
    // ---------------------------------------------------------------------

    if (!cs2bc::weapon_locker_hooks::Install(g_gamedata, g_serverModule, error, maxlen))
    {
        Disable();

        if (!error || maxlen == 0 || error[0] == '\0')
        {
            SetError(error, maxlen, "WeaponLocker hook installation failed");
        }

        return false;
    }

    // ---------------------------------------------------------------------
    // 2. BotController
    //
    // Required module.
    // ---------------------------------------------------------------------

    if (!cs2bc::bot_controller_hooks::Install(g_gamedata, g_serverModule, error, maxlen))
    {
        Disable();

        if (!error || maxlen == 0 || error[0] == '\0')
        {
            SetError(error, maxlen, "BotController hook installation failed");
        }

        return false;
    }

    // ---------------------------------------------------------------------
    // 3. BuyController
    //
    // Optional, matching the behaviour of the original plugin.
    // ---------------------------------------------------------------------

    char buyError[256] = { 0 };

    if (!cs2bc::buy_controller_hooks::Install(g_gamedata, g_serverModule, buyError, sizeof(buyError)))
    {
        Warning("[BotController] BuyController::Install failed (%s); "
                "bot buy control disabled\n",
                buyError);
    }

    // ---------------------------------------------------------------------
    // 4. InputInjector
    //
    // Optional, again preserving the original behaviour.
    //
    // This module owns the expensive player-simulation hooks, so when
    // Disable() is called these hooks disappear completely.
    // ---------------------------------------------------------------------

    char injectorError[256] = { 0 };

    if (!cs2bc::input_injector::Install(g_gamedata, g_serverModule, injectorError, sizeof(injectorError)))
    {
        Warning("[BotController] InputInjector::Install failed (%s); "
                "record/replay movement will be unavailable\n",
                injectorError);
    }

    g_enabled.store(true, std::memory_order_release);

    Msg("[BotController] Runtime enabled\n");

    return true;
}

// -----------------------------------------------------------------------------
// SetEnabled
//
// This is especially useful later from exports.cpp:
//
//     BotController_SetRuntimeEnabled(1)
//         -> runtime::SetEnabled(true, ...)
//
//     BotController_SetRuntimeEnabled(0)
//         -> runtime::SetEnabled(false, ...)
// -----------------------------------------------------------------------------

bool SetEnabled(bool enabled, char* error, size_t maxlen)
{
    g_runtimeRequested = enabled;

    if (!enabled)
    {
        ClearError(error, maxlen);
        Disable();
        return true;
    }

    if (g_metaPaused)
    {
        // Request remembered, but MetaMod pause wins.
        ClearError(error, maxlen);
        return true;
    }

    return Enable(error, maxlen);
}

bool SetMetaPaused(bool paused, char* error, size_t maxlen)
{
    g_metaPaused = paused;

    if (paused)
    {
        ClearError(error, maxlen);
        Disable();
        return true;
    }

    if (!g_runtimeRequested)
    {
        ClearError(error, maxlen);
        return true;
    }

    return Enable(error, maxlen);
}

// -----------------------------------------------------------------------------
// Status
// -----------------------------------------------------------------------------

bool IsPrepared() { return g_prepared; }

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

// -----------------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------------

void Shutdown()
{
    Disable();

    // Forget all data used for reinstalling hooks.
    g_gamedata = nlohmann::json{};
    g_serverModule = cs2bc::sig::ModuleInfo{};

    g_prepared = false;
    g_enabled.store(false, std::memory_order_release);
    g_runtimeRequested = false;
    g_metaPaused = false;
}

} // namespace cs2bc::runtime
