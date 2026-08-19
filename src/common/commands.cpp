// BotController console commands: bc_lock / bc_unlock / bc_unlock_all / bc_status.

#include "commands.h"
#include "dispatch.h"
#include "WeaponLocker.h"
#include "BotController.h"
#include "InputInjector.h"
#include "WeaponLockerState.h"
#include "BotControllerState.h"
#include "MotionRecorder.h"
#include "BuyControllerState.h"
#include "BuyController.h"
#include "BotProfile.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BotController {
namespace Commands {
IVEngineServer2* g_pEngine = nullptr;

// ClientPrintf to the calling player, or server log if from console.
void PrintToCaller(const CCommandContext& context, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const CPlayerSlot slot = context.GetPlayerSlot();
    if (g_pEngine && slot.IsValid()) g_pEngine->ClientPrintf(slot, buf);
    else
        Msg("%s", buf);
}

// Parse kind string into LockKind.
static bool ParseKind(const char* s, LockKind& out)
{
    if (!s) return false;
    if (std::strcmp(s, "all") == 0)
    {
        out = LockKind::All;
        return true;
    }
    if (std::strcmp(s, "aim") == 0)
    {
        out = LockKind::Aim;
        return true;
    }
    if (std::strcmp(s, "weapon") == 0)
    {
        out = LockKind::Weapon;
        return true;
    }
    return false;
}

// Parse "slotN" into LockTarget.
static LockTarget ParseTarget(const char* s)
{
    if (!s) return LockTarget::None;
    if (std::strcmp(s, "slot1") == 0) return LockTarget::Slot1;
    if (std::strcmp(s, "slot2") == 0) return LockTarget::Slot2;
    if (std::strcmp(s, "slot3") == 0) return LockTarget::Slot3;
    if (std::strcmp(s, "slot4") == 0) return LockTarget::Slot4;
    if (std::strcmp(s, "slot5") == 0) return LockTarget::Slot5;
    return LockTarget::None;
}

static const char* TargetName(LockTarget t)
{
    switch (t)
    {
        case LockTarget::Slot1:
            return "slot1";
        case LockTarget::Slot2:
            return "slot2";
        case LockTarget::Slot3:
            return "slot3";
        case LockTarget::Slot4:
            return "slot4";
        case LockTarget::Slot5:
            return "slot5";
        default:
            return "none";
    }
}

static const char* KindName(LockKind k)
{
    switch (k)
    {
        case LockKind::All:
            return "all";
        case LockKind::Aim:
            return "aim";
        case LockKind::Weapon:
            return "weapon";
    }
    return "?";
}
} // namespace Commands
} // namespace BotController

CON_COMMAND_F(bc_lock,
              "bc_lock <all|aim|weapon> <slot> [slot1..slot5]  "
              "Lock a bot. weapon mode requires the weapon slot.",
              FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 3)
    {
        Commands::PrintToCaller(context, "usage: bc_lock <all|aim|weapon> <slot> [slot1..slot5]\n");
        return;
    }

    LockKind kind;
    if (!Commands::ParseKind(args.Arg(1), kind))
    {
        Commands::PrintToCaller(context, "[BC] error: kind must be all|aim|weapon\n");
        return;
    }

    const int slot = std::atoi(args.Arg(2));
    int arg = 0;

    if (kind == LockKind::Weapon)
    {
        if (args.ArgC() < 4)
        {
            Commands::PrintToCaller(context, "usage: bc_lock weapon <slot> <slot1..slot5>\n");
            return;
        }
        const auto tgt = Commands::ParseTarget(args.Arg(3));
        if (tgt == LockTarget::None)
        {
            Commands::PrintToCaller(context, "[BC] error: weapon target must be slot1..slot5\n");
            return;
        }
        arg = static_cast<int>(tgt);
    }

    int rc = Dispatch::Lock(slot, kind, arg);
    if (rc == 0)
    {
        if (kind == LockKind::Weapon)
            Commands::PrintToCaller(context, "[BC] locked slot %d weapon -> %s\n", slot,
                                    Commands::TargetName(static_cast<LockTarget>(arg)));
        else
            Commands::PrintToCaller(context, "[BC] locked slot %d (%s)\n", slot, Commands::KindName(kind));
    }
    else
    {
        Commands::PrintToCaller(context, "[BC] error: lock failed (rc=%d)\n", rc);
    }
}

CON_COMMAND_F(bc_unlock, "bc_unlock <all|aim|weapon> <slot>  Release one lock on a bot.", FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 3)
    {
        Commands::PrintToCaller(context, "usage: bc_unlock <all|aim|weapon> <slot>\n");
        return;
    }

    LockKind kind;
    if (!Commands::ParseKind(args.Arg(1), kind))
    {
        Commands::PrintToCaller(context, "[BC] error: kind must be all|aim|weapon\n");
        return;
    }

    const int slot = std::atoi(args.Arg(2));
    int rc = Dispatch::Unlock(slot, kind);
    if (rc == 0) Commands::PrintToCaller(context, "[BC] unlocked slot %d (%s)\n", slot, Commands::KindName(kind));
    else
        Commands::PrintToCaller(context, "[BC] error: unlock failed (rc=%d)\n", rc);
}

CON_COMMAND_F(bc_unlock_all, "bc_unlock_all <all|aim|weapon>  Release every lock of that kind.", FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 2)
    {
        Commands::PrintToCaller(context, "usage: bc_unlock_all <all|aim|weapon>\n");
        return;
    }

    LockKind kind;
    if (!Commands::ParseKind(args.Arg(1), kind))
    {
        Commands::PrintToCaller(context, "[BC] error: kind must be all|aim|weapon\n");
        return;
    }

    int rc = Dispatch::UnlockAll(kind);
    if (rc == 0) Commands::PrintToCaller(context, "[BC] unlocked all (%s)\n", Commands::KindName(kind));
    else
        Commands::PrintToCaller(context, "[BC] error: unlock_all failed (rc=%d)\n", rc);
}

CON_COMMAND_F(bc_status, "bc_status  Print a concise BotController status summary.", FCVAR_NONE)
{
    using namespace BotController;

    Commands::PrintToCaller(context, "[BC] hooks: weapon=%s bot=%s input=%s drop=%s buy=%s\n",
                            WeaponLockerHooks::Status(), BotControllerHooks::Status(), InputInjector::Status(),
                            MotionRecorder::DropHookReady() ? "ok" : "failed", BuyControllerHooks::Status());

    bool printedReplayHeader = false;
    for (int s = 0; s < MotionRecorder::kMaxSlots; ++s)
    {
        int cursor = MotionRecorder::ReplayCursor(s);
        int total = MotionRecorder::ReplayTotal(s);
        if (cursor >= 0 || total > 0)
        {
            if (!printedReplayHeader)
            {
                Commands::PrintToCaller(context, "[BC] replay slots:\n");
                printedReplayHeader = true;
            }
            Commands::PrintToCaller(context, "[BC]   replay slot %2d cursor=%d total=%d\n", s, cursor, total);
        }
    }
    if (!printedReplayHeader) Commands::PrintToCaller(context, "[BC] replay: none\n");

    Commands::PrintToCaller(context, "[BC] state: locks(all=%d aim=%d weapon=%d) buyPlans=%d\n",
                            BotControllerState::CountAll(), BotControllerState::CountAim(),
                            WeaponLockerState::CountLocked(), BuyControllerState::CountPlans());
    Commands::PrintToCaller(context, "[BC] drop: captured=%llu attempts=%llu commands=%llu\n",
                            (unsigned long long)MotionRecorder::DropCaptureCount(),
                            (unsigned long long)MotionRecorder::DropReplayAttemptCount(),
                            (unsigned long long)MotionRecorder::DropReplayNativeCallCount());
}

CON_COMMAND_F(bc_buy, "bc_buy <slot> <alias> [alias...]  Force a bot's buy plan for each round.", FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 3)
    {
        Commands::PrintToCaller(context, "usage: bc_buy <slot> <alias> [alias...]\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1));
    if (slot < 0 || slot >= BuyControllerState::kMaxSlots)
    {
        Commands::PrintToCaller(context, "[BC] error: slot out of range\n");
        return;
    }

    std::vector<std::string> items;
    for (int i = 2; i < args.ArgC(); ++i)
        items.emplace_back(args.Arg(i));

    BuyControllerState::Set(slot, items, false);
    Commands::PrintToCaller(context, "[BC] buy plan set slot %d (%d items)\n", slot, static_cast<int>(items.size()));
}

CON_COMMAND_F(bc_buy_skip, "bc_buy_skip <slot>  Force a bot to buy nothing each round.", FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 2)
    {
        Commands::PrintToCaller(context, "usage: bc_buy_skip <slot>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1));
    if (slot < 0 || slot >= BuyControllerState::kMaxSlots)
    {
        Commands::PrintToCaller(context, "[BC] error: slot out of range\n");
        return;
    }

    BuyControllerState::Set(slot, {}, true);
    Commands::PrintToCaller(context, "[BC] buy plan set slot %d -> skip\n", slot);
}

CON_COMMAND_F(bc_unbuy, "bc_unbuy <slot>  Remove a bot's buy plan (back to vanilla).", FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 2)
    {
        Commands::PrintToCaller(context, "usage: bc_unbuy <slot>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1));
    if (slot < 0 || slot >= BuyControllerState::kMaxSlots)
    {
        Commands::PrintToCaller(context, "[BC] error: slot out of range\n");
        return;
    }

    BuyControllerState::Clear(slot);
    Commands::PrintToCaller(context, "[BC] buy plan cleared slot %d\n", slot);
}

CON_COMMAND_F(bc_unbuy_all, "bc_unbuy_all  Remove every bot buy plan.", FCVAR_NONE)
{
    using namespace BotController;
    BuyControllerState::ClearAll();
    Commands::PrintToCaller(context, "[BC] all buy plans cleared\n");
}

CON_COMMAND_F(bc_profile, "bc_profile <slot>  Print a bot's BotProfile (skill/aim/weapon prefs).", FCVAR_NONE)
{
    using namespace BotController;

    if (args.ArgC() < 2)
    {
        Commands::PrintToCaller(context, "usage: bc_profile <slot>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1));
    BotProfileData d;
    if (!BotProfile::ReadProfile(slot, d))
    {
        Commands::PrintToCaller(context, "[BC] error: no live bot on slot %d (it must have ticked once)\n", slot);
        return;
    }

    Commands::PrintToCaller(context, "[BC] profile slot %d: skill=%.2f aggression=%.2f teamwork=%.2f\n", slot, d.skill, d.aggression,
                            d.teamwork);
    Commands::PrintToCaller(context, "[BC]   reaction=%.3f attackDelay=%.3f cost=%d difficulty=0x%X\n", d.reactionTime, d.attackDelay,
                            d.cost, d.difficulty);
    Commands::PrintToCaller(context, "[BC]   lookAtk accel=%.1f stiff=%.1f damp=%.1f\n", d.lookAccelAtk, d.lookStiffAtk, d.lookDampAtk);

    // Weapon preference: item def indices in priority order
    char line[256];
    int n = std::snprintf(line, sizeof(line), "[BC]   weaponPref(%d):", d.weaponPrefCount);
    for (int i = 0; i < d.weaponPrefCount && n < (int)sizeof(line) - 8; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %u", d.weaponPref[i]);
    std::snprintf(line + n, sizeof(line) - n, "\n");
    Commands::PrintToCaller(context, "%s", line);
}
