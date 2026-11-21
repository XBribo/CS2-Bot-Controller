#include <cs2_sdk/gameconfig.h>

#include "BuyController.h"
#include "BuyControllerState.h"
#include "BotControllerState.h"
#include "MotionRecorder.h"
#include "ccsbot_slot.h"
#include "nlohmann/json.hpp"
#include <cs2_sdk/memory_module.h>
#include "offsets.h"
#include "dispatch.h"
#include "core/log.h"
#include <cs2_sdk/schema.h>
#include <engine2/ivengineserver2.h>
#include <engine2/gameshareddefs.h>
#include <tier0/threadtools.h>
#include <cs2_sdk/hooks.h>

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
bool g_buyOnceReady = false;
int g_buyZone = -1;
int g_lifeState = -1;
int g_team = -1;
std::string g_status = "not_attempted"; // NOLINT(bugprone-throwing-static-initialization)

void IssueBuys(int slot, const std::vector<std::string>& aliases)
{
    if (!dispatch::g_gameClients) return;
    for (const auto& alias : aliases)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "buy %s", alias.c_str());
        CCommand cmd;
        if (cmd.Tokenize(line)) dispatch::g_gameClients->ClientCommand(CPlayerSlot(slot), cmd);
    }
}

void ApplyPlan(void* self, int slot)
{
    BuyPlan plan;
    if (!buy_controller_state::Copy(slot, plan)) return;

    WriteField(self, tg::g_buyInitialDelay, uint8_t{0});
    WriteField(self, tg::g_buyDoneBuying, uint8_t{1});
    if (!plan.skip) IssueBuys(slot, plan.items);
}

KHook::Return<void> HookedOnUpdate(void* self, void* me) noexcept
{
    const int slot = CCSBotToSlot(me);
    if (slot < 0) return { KHook::Action::Ignore };
    if (motion_recorder::IsReplaying(slot)) return { KHook::Action::Supersede };

    uint8_t init = 0;
    if (!SafeRead(self, tg::g_buyInitialDelay, init)) return { KHook::Action::Ignore };
    if (init) ApplyPlan(self, slot);
    else if (buy_controller_state::SuppressesNative(slot))
        WriteField(self, tg::g_buyDoneBuying, uint8_t{1});

    return { KHook::Action::Ignore };
}

} // namespace

bool ReserveOnce(int slot, uint32_t pawn, const std::vector<std::string>& items)
{
    if (!ThreadInMainThread() || !g_buyOnceReady ||
        !dispatch::g_gameClients || !dispatch::g_engine || motion_recorder::IsReplaying(slot)) return false;
    auto* globals = dispatch::g_engine->GetServerGlobals();
    return globals && buy_controller_state::ReserveOnce(slot, pawn, items, globals->curtime);
}

void UpdatePending(void* bot)
{
    const auto mask = buy_controller_state::PendingSlots();
    if (!mask) return;
    const auto sr = ResolveSlot(bot);
    if (sr.slot < 0 || !(mask & (uint64_t{1} << sr.slot))) return;
    auto* globals = dispatch::g_engine->GetServerGlobals();
    if (!globals) return;
    bool zone = false;
    uint8_t life = 0, team = 0;
    const bool eligible = !motion_recorder::IsReplaying(sr.slot) && !bot_controller_state::GetAll(sr.slot) &&
        SafeRead(sr.pawn, g_lifeState, life) && life == 0 && SafeRead(sr.pawn, g_team, team) &&
        (team == 2 || team == 3) && SafeRead(sr.pawn, g_buyZone, zone);
    if (eligible && !zone)
    {
        static ConVarRefAbstract anywhere("mp_buy_anywhere");
        const int value = anywhere.IsValidRef() && anywhere.IsConVarDataAvailable() ? anywhere.GetInt() : 0;
        zone = value == 1 || value == team;
    }
    BuyPlan plan;
    using buy_controller_state::Claim;
    const auto claim = buy_controller_state::ClaimOnce(sr.slot, sr.pawnHandle, globals->curtime, eligible, zone, plan);
    if (claim == Claim::Dispatch)
        IssueBuys(sr.slot, plan.items);
    else if (claim != Claim::Wait)
        BC_LOG_WARN("buy once slot=%d tick=%d %s", sr.slot, globals->tickcount, claim == Claim::Expired ? "expired" : "invalidated");
}

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

    const cs2_sdk::schema::FieldBinding fields[] = {
        { &g_buyZone, "CCSPlayerPawn", "m_bInBuyZone" },
        { &g_lifeState, "CBaseEntity", "m_lifeState" },
        { &g_team, "CBaseEntity", "m_iTeamNum" },
    };
    g_status = "ok";
    g_buyOnceReady = cs2_sdk::schema::ResolveFields(fields);
    if (!g_buyOnceReady)
    {
        g_status = "degraded: buy-once disabled; missing";
        for (const auto& field : fields)
            if (*field.target < 0)
                g_status.append(" ").append(field.className).append("::").append(field.fieldName);
        BC_LOG_WARN("buy: %s", g_status.c_str());
    }
    g_installed = true;
    return true;
}

void Remove()
{
    if (!g_installed) return;
    g_hookOnUpdate.Remove();
    g_installed = false;
    g_buyOnceReady = false;
    g_status = "not_attempted";
}

const char* Status() { return g_status.c_str(); }
void* OnUpdateAddress() { return g_addrOnUpdate; }
} // namespace buy_controller_hooks
} // namespace cs2bc
