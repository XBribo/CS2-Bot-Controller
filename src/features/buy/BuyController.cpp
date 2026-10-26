#include "core/cs2_sdk/src/core/gameconfig.h"
// Detour for BuyState::OnUpdate

#include "BuyController.h"
#include "BuyControllerState.h"
#include "MotionRecorder.h"
#include "ccsbot_slot.h"
#include "nlohmann/json.hpp"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "offsets.h"
#include "dispatch.h"
#include "core/cs2_sdk/src/utils/hooks.h"

#include <engine2/isource2server.h>
#include <playerslot.h>
#include <convar.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace tg = cs2bc::offsets;

namespace cs2bc {
namespace buy_controller_hooks {

namespace {
void* g_addrOnUpdate = nullptr;
cs2_sdk::hooks::NativeHook<void, void*, void*> g_hookOnUpdate;
bool g_installed = false;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

// Previous m_isInitialDelay for entry detection.
uint8_t g_lastInitDelay[64] = { 0 };

// Run "buy <alias>" server-side for a bot slot
void IssueBuy(int slot, const char* alias)
{
    if (!dispatch::g_gameClients || slot < 0 || slot >= 64) return;
    char line[128];
    std::snprintf(line, sizeof(line), "buy %s", alias);
    CCommand cmd;
    if (!cmd.Tokenize(line)) return;
    dispatch::g_gameClients->ClientCommand(CPlayerSlot(slot), cmd);
}

// Executes the buy plan and finishes native buying.
void ApplyPlan(void* self, int slot)
{
    BuyPlan plan;
    if (!buy_controller_state::Copy(slot, plan)) return;

    if (!plan.skip)
        for (const auto& alias : plan.items)
            IssueBuy(slot, alias.c_str());

    // Finish native buying.
    const uint8_t done = 1;
    WriteField(self, tg::g_buyDoneBuying, done);
}

// Applies the plan before native buying.
KHook::Return<void> HookedOnUpdate(void* self, void* me) noexcept
{
    int slot = CCSBotToSlot(me);
    if (slot >= 0 && slot < 64 && motion_recorder::IsReplaying(slot)) return { KHook::Action::Supersede };
    if (slot < 0 || slot >= 64 || !buy_controller_state::HasPlan(slot))
    {
        return { KHook::Action::Ignore };
    }

    uint8_t init = 0;
    if (!SafeRead(self, tg::g_buyInitialDelay, init))
    {
        return { KHook::Action::Ignore };
    }
    // Rising m_isInitialDelay marks BuyState entry.
    if (init && !g_lastInitDelay[slot]) ApplyPlan(self, slot);
    g_lastInitDelay[slot] = init;

    return { KHook::Action::Ignore };
}

} // namespace

bool Install(const nlohmann::json& gd, const cs2_sdk::modules::ModuleInfo& serverModule, char* errorOut, size_t errorOutLen)
{
    g_addrOnUpdate = cs2_sdk::gameconfig::ResolveSig(gd, serverModule, "BuyState::OnUpdate", errorOut, errorOutLen);
    if (!g_addrOnUpdate)
    {
        g_status = "failed: OnUpdate sig";
        return false;
    }

    if (!g_hookOnUpdate.Install(g_addrOnUpdate, &HookedOnUpdate))
    {
        std::snprintf(errorOut, errorOutLen, "hook BuyState::OnUpdate failed");
        g_hookOnUpdate.Remove();
        g_status = "failed: hook OnUpdate";
        return false;
    }

    g_installed = true;
    g_status = "ok";
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_hookOnUpdate.Remove();
    g_installed = false;
    g_status = "not_attempted";
    for (unsigned char& i : g_lastInitDelay)
        i = 0;
}

const char* Status() { return g_status.c_str(); }
void* OnUpdateAddress() { return g_addrOnUpdate; }
} // namespace buy_controller_hooks
} // namespace cs2bc
