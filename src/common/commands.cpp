// BotController console commands: bc_lock / bc_unlock / bc_unlock_all / bc_status.

#include "commands.h"
#include "dispatch.h"
#include "WeaponLocker.h" // NOLINT(misc-include-cleaner)
#include "BotController.h" // NOLINT(misc-include-cleaner)
#include "InputInjector.h" // NOLINT(misc-include-cleaner)
#include "WeaponLockerState.h"
#include "BotControllerState.h" // NOLINT(misc-include-cleaner)
#include "MotionRecorder.h" // NOLINT(misc-include-cleaner)
#include "BuyControllerState.h" // NOLINT(misc-include-cleaner)
#include "BuyController.h" // NOLINT(misc-include-cleaner)
#include "BotProfile.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector> // NOLINT(misc-include-cleaner)
#include <string> // NOLINT(misc-include-cleaner)

namespace cs2bc {
namespace commands {
IVEngineServer2* g_engine = nullptr;

// ClientPrintf to the calling player, or server log if from console.
void PrintToCaller(const CCommandContext& context, const char* fmt, ...) // NOLINT(modernize-avoid-variadic-functions)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const CPlayerSlot slot = context.GetPlayerSlot();
    if (g_engine && slot.IsValid()) g_engine->ClientPrintf(slot, buf);
    else
        Msg("%s", buf);
}

namespace {

// Parse kind string into LockKind.
bool ParseKind(const char* s, LockKind& out)
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
LockTarget ParseTarget(const char* s)
{
    if (!s) return LockTarget::None;
    if (std::strcmp(s, "slot1") == 0) return LockTarget::Slot1;
    if (std::strcmp(s, "slot2") == 0) return LockTarget::Slot2;
    if (std::strcmp(s, "slot3") == 0) return LockTarget::Slot3;
    if (std::strcmp(s, "slot4") == 0) return LockTarget::Slot4;
    if (std::strcmp(s, "slot5") == 0) return LockTarget::Slot5;
    return LockTarget::None;
}

const char* TargetName(LockTarget t)
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

const char* KindName(LockKind k)
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
} // namespace
} // namespace commands
} // namespace cs2bc

namespace {

namespace commands = cs2bc::commands;
namespace dispatch = cs2bc::dispatch;
namespace bot_controller_state = cs2bc::bot_controller_state;
namespace bot_profile = cs2bc::bot_profile;
namespace buy_controller_hooks = cs2bc::buy_controller_hooks;
namespace buy_controller_state = cs2bc::buy_controller_state;
namespace bot_controller_hooks = cs2bc::bot_controller_hooks;
namespace input_injector = cs2bc::input_injector;
namespace motion_recorder = cs2bc::motion_recorder;
namespace weapon_locker_hooks = cs2bc::weapon_locker_hooks;
namespace weapon_locker_state = cs2bc::weapon_locker_state;
using cs2bc::BotProfileData;
using cs2bc::LockKind;
using cs2bc::LockTarget;

CON_COMMAND_F(bc_lock, // NOLINT(bugprone-throwing-static-initialization)
              "bc_lock <all|aim|weapon> <slot> [slot1..slot5]  "
              "Lock a bot. weapon mode requires the weapon slot.",
              FCVAR_NONE)
{
    if (args.ArgC() < 3)
    {
        commands::PrintToCaller(context, "usage: bc_lock <all|aim|weapon> <slot> [slot1..slot5]\n");
        return;
    }

    LockKind kind;
    if (!commands::ParseKind(args.Arg(1), kind))
    {
        commands::PrintToCaller(context, "[BC] error: kind must be all|aim|weapon\n");
        return;
    }

    const int slot = std::atoi(args.Arg(2)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    int arg = 0;

    if (kind == LockKind::Weapon) // NOLINT(bugprone-branch-clone)
    {
        if (args.ArgC() < 4)
        {
            commands::PrintToCaller(context, "usage: bc_lock weapon <slot> <slot1..slot5>\n");
            return;
        }
        const auto tgt = commands::ParseTarget(args.Arg(3));
        if (tgt == LockTarget::None)
        {
            commands::PrintToCaller(context, "[BC] error: weapon target must be slot1..slot5\n");
            return;
        }
        arg = static_cast<int>(tgt);
    }

    int rc = dispatch::Lock(slot, kind, arg);
    if (rc == 0) // NOLINT(bugprone-branch-clone)
    {
        if (kind == LockKind::Weapon)
            commands::PrintToCaller(context, "[BC] locked slot %d weapon -> %s\n", slot,
                                    commands::TargetName(static_cast<LockTarget>(arg)));
        else
            commands::PrintToCaller(context, "[BC] locked slot %d (%s)\n", slot, commands::KindName(kind));
    }
    else
    {
        commands::PrintToCaller(context, "[BC] error: lock failed (rc=%d)\n", rc);
    }
}

CON_COMMAND_F(bc_unlock, // NOLINT(bugprone-throwing-static-initialization)
              "bc_unlock <all|aim|weapon> <slot>  Release one lock on a bot.",
              FCVAR_NONE)
{
    if (args.ArgC() < 3)
    {
        commands::PrintToCaller(context, "usage: bc_unlock <all|aim|weapon> <slot>\n");
        return;
    }

    LockKind kind;
    if (!commands::ParseKind(args.Arg(1), kind))
    {
        commands::PrintToCaller(context, "[BC] error: kind must be all|aim|weapon\n");
        return;
    }

    const int slot = std::atoi(args.Arg(2)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    int rc = dispatch::Unlock(slot, kind);
    if (rc == 0) commands::PrintToCaller(context, "[BC] unlocked slot %d (%s)\n", slot, commands::KindName(kind));
    else
        commands::PrintToCaller(context, "[BC] error: unlock failed (rc=%d)\n", rc);
}

CON_COMMAND_F(bc_unlock_all, // NOLINT(bugprone-throwing-static-initialization)
              "bc_unlock_all <all|aim|weapon>  Release every lock of that kind.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        commands::PrintToCaller(context, "usage: bc_unlock_all <all|aim|weapon>\n");
        return;
    }

    LockKind kind;
    if (!commands::ParseKind(args.Arg(1), kind))
    {
        commands::PrintToCaller(context, "[BC] error: kind must be all|aim|weapon\n");
        return;
    }

    int rc = dispatch::UnlockAll(kind);
    if (rc == 0) commands::PrintToCaller(context, "[BC] unlocked all (%s)\n", commands::KindName(kind));
    else
        commands::PrintToCaller(context, "[BC] error: unlock_all failed (rc=%d)\n", rc);
}

CON_COMMAND_F(bc_status, // NOLINT(bugprone-throwing-static-initialization,misc-unused-parameters)
              "bc_status  Print a concise BotController status summary.",
              FCVAR_NONE)
{
    commands::PrintToCaller(context, "[BC] hooks: weapon=%s bot=%s input=%s drop=%s buy=%s\n", weapon_locker_hooks::Status(),
                            bot_controller_hooks::Status(), input_injector::Status(), motion_recorder::DropHookReady() ? "ok" : "failed",
                            buy_controller_hooks::Status());

    bool printedReplayHeader = false;
    for (int s = 0; s < motion_recorder::kMaxSlots; ++s)
    {
        int cursor = motion_recorder::ReplayCursor(s);
        int total = motion_recorder::ReplayTotal(s);
        if (cursor >= 0 || total > 0)
        {
            if (!printedReplayHeader)
            {
                commands::PrintToCaller(context, "[BC] replay slots:\n");
                printedReplayHeader = true;
            }
            commands::PrintToCaller(context, "[BC]   replay slot %2d cursor=%d total=%d\n", s, cursor, total);
        }
    }
    if (!printedReplayHeader) commands::PrintToCaller(context, "[BC] replay: none\n");

    commands::PrintToCaller(context, "[BC] state: locks(all=%d aim=%d weapon=%d) buyPlans=%d\n", bot_controller_state::CountAll(),
                            bot_controller_state::CountAim(), weapon_locker_state::CountLocked(), buy_controller_state::CountPlans());
    commands::PrintToCaller(context, "[BC] drop: captured=%llu attempts=%llu commands=%llu\n", motion_recorder::DropCaptureCount(),
                            motion_recorder::DropReplayAttemptCount(), motion_recorder::DropReplayNativeCallCount());
}

CON_COMMAND_F(bc_buy, // NOLINT(bugprone-throwing-static-initialization)
              "bc_buy <slot> <alias> [alias...]  Force a bot's buy plan for each round.",
              FCVAR_NONE)
{
    if (args.ArgC() < 3)
    {
        commands::PrintToCaller(context, "usage: bc_buy <slot> <alias> [alias...]\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    if (!runtime::IsEnabled())
    {
        commands::PrintToCaller(context, "[BC] error: runtime disabled\n");

        return;
    }

    if (!bot_controller_hooks::IsLiveBotSlot(slot))
    {
        commands::PrintToCaller(context, "[BC] error: no live bot on slot %d\n", slot);

        return;
    }
    if (slot < 0 || slot >= buy_controller_state::kMaxSlots)
    {
        commands::PrintToCaller(context, "[BC] error: slot out of range\n");
        return;
    }

    std::vector<std::string> items;
    for (int i = 2; i < args.ArgC(); ++i)
        items.emplace_back(args.Arg(i));

    buy_controller_state::Set(slot, items, false);
    commands::PrintToCaller(context, "[BC] buy plan set slot %d (%d items)\n", slot, static_cast<int>(items.size()));
}

CON_COMMAND_F(bc_buy_skip, // NOLINT(bugprone-throwing-static-initialization)
              "bc_buy_skip <slot>  Force a bot to buy nothing each round.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        commands::PrintToCaller(context, "usage: bc_buy_skip <slot>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    if (!runtime::IsEnabled())
    {
        commands::PrintToCaller(context, "[BC] error: runtime disabled\n");

        return;
    }

    if (!bot_controller_hooks::IsLiveBotSlot(slot))
    {
        commands::PrintToCaller(context, "[BC] error: no live bot on slot %d\n", slot);

        return;
    }
    if (slot < 0 || slot >= buy_controller_state::kMaxSlots)
    {
        commands::PrintToCaller(context, "[BC] error: slot out of range\n");
        return;
    }

    buy_controller_state::Set(slot, {}, true);
    commands::PrintToCaller(context, "[BC] buy plan set slot %d -> skip\n", slot);
}

CON_COMMAND_F(bc_unbuy, // NOLINT(bugprone-throwing-static-initialization)
              "bc_unbuy <slot>  Remove a bot's buy plan (back to vanilla).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        commands::PrintToCaller(context, "usage: bc_unbuy <slot>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    if (slot < 0 || slot >= buy_controller_state::kMaxSlots)
    {
        commands::PrintToCaller(context, "[BC] error: slot out of range\n");
        return;
    }

    buy_controller_state::Clear(slot);
    commands::PrintToCaller(context, "[BC] buy plan cleared slot %d\n", slot);
}

CON_COMMAND_F(bc_unbuy_all, // NOLINT(bugprone-throwing-static-initialization,misc-unused-parameters)
              "bc_unbuy_all  Remove every bot buy plan.",
              FCVAR_NONE)
{
    buy_controller_state::ClearAll();
    commands::PrintToCaller(context, "[BC] all buy plans cleared\n");
}

CON_COMMAND_F(bc_profile, // NOLINT(bugprone-throwing-static-initialization)
              "bc_profile <slot>  Print a bot's BotProfile (skill/aim/weapon prefs).",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        commands::PrintToCaller(context, "usage: bc_profile <slot>\n");
        return;
    }

    const int slot = std::atoi(args.Arg(1)); // NOLINT(bugprone-unchecked-string-to-number-conversion)
    BotProfileData d;
    if (!bot_profile::ReadProfile(slot, d))
    {
        commands::PrintToCaller(context, "[BC] error: no live bot on slot %d (it must have ticked once)\n", slot);
        return;
    }

    commands::PrintToCaller(context, "[BC] profile slot %d: skill=%.2f aggression=%.2f teamwork=%.2f\n", slot, d.skill, d.aggression,
                            d.teamwork);
    commands::PrintToCaller(context, "[BC]   reaction=%.3f attackDelay=%.3f cost=%d difficulty=0x%X\n", d.reactionTime, d.attackDelay,
                            d.cost, d.difficulty);
    commands::PrintToCaller(context, "[BC]   lookAtk accel=%.1f stiff=%.1f damp=%.1f\n", d.lookAccelAtk, d.lookStiffAtk, d.lookDampAtk);

    // Weapon preference: item def indices in priority order
    char line[256];
    int n = std::snprintf(line, sizeof(line), "[BC]   weaponPref(%d):", d.weaponPrefCount);
    for (int i = 0; i < d.weaponPrefCount && n < static_cast<int>(sizeof(line)) - 8; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %u", d.weaponPref[i]);
    std::snprintf(line + n, sizeof(line) - n, "\n");
    commands::PrintToCaller(context, "%s", line);
}

} // namespace
