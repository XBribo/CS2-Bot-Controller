// BotWeaponLock console commands.

#include "commands.h"
#include "dispatch.h"
#include "hooks.h"
#include "lock_state.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BotWeaponLock
{
    namespace Commands
    {
        IVEngineServer2 *g_pEngine = nullptr;

        void PrintToCaller(const CCommandContext &context, const char *fmt, ...)
        {
            char buf[1024];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);

            const CPlayerSlot slot = context.GetPlayerSlot();
            if (g_pEngine && slot.IsValid())
                g_pEngine->ClientPrintf(slot, buf);
            else
                Msg("%s", buf);
        }

        static LockTarget ParseTarget(const char *s)
        {
            if (!s) return LockTarget::None;
            if (std::strcmp(s, "slot1") == 0) return LockTarget::Slot1;
            if (std::strcmp(s, "slot2") == 0) return LockTarget::Slot2;
            if (std::strcmp(s, "slot3") == 0) return LockTarget::Slot3;
            if (std::strcmp(s, "slot4") == 0) return LockTarget::Slot4;
            if (std::strcmp(s, "slot5") == 0) return LockTarget::Slot5;
            return LockTarget::None;
        }

        static const char *TargetName(LockTarget t)
        {
            switch (t)
            {
            case LockTarget::Slot1: return "slot1";
            case LockTarget::Slot2: return "slot2";
            case LockTarget::Slot3: return "slot3";
            case LockTarget::Slot4: return "slot4";
            case LockTarget::Slot5: return "slot5";
            default: return "none";
            }
        }
    }
}

CON_COMMAND_F(blw_lock,
              "blw_lock <slot> <slot1|slot2|slot3|slot4|slot5>  Lock the bot at <slot> to a weapon slot.",
              FCVAR_NONE)
{
    if (args.ArgC() < 3)
    {
        BotWeaponLock::Commands::PrintToCaller(context,
            "usage: blw_lock <slot> <slot1|slot2|slot3|slot4|slot5>\n");
        return;
    }
    const int slot = std::atoi(args.Arg(1));
    const auto tgt = BotWeaponLock::Commands::ParseTarget(args.Arg(2));
    if (tgt == BotWeaponLock::LockTarget::None)
    {
        BotWeaponLock::Commands::PrintToCaller(context,
            "[BWL] error: target must be slot1..slot5\n");
        return;
    }
    int rc = BotWeaponLock::Dispatch::Lock(slot, tgt);
    if (rc == 0)
        BotWeaponLock::Commands::PrintToCaller(context,
            "[BWL] locked slot %d to %s\n", slot, BotWeaponLock::Commands::TargetName(tgt));
    else if (rc == -1)
        BotWeaponLock::Commands::PrintToCaller(context,
            "[BWL] error: plugin not initialized (rc=%d)\n", rc);
    else
        BotWeaponLock::Commands::PrintToCaller(context,
            "[BWL] error: invalid args (rc=%d)\n", rc);
}

CON_COMMAND_F(blw_unlock,
              "blw_unlock <slot>  Unlock the bot at <slot>.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        BotWeaponLock::Commands::PrintToCaller(context, "usage: blw_unlock <slot>\n");
        return;
    }
    const int slot = std::atoi(args.Arg(1));
    int rc = BotWeaponLock::Dispatch::Unlock(slot);
    if (rc == 0)
        BotWeaponLock::Commands::PrintToCaller(context,
            "[BWL] unlocked slot %d\n", slot);
    else
        BotWeaponLock::Commands::PrintToCaller(context,
            "[BWL] error: invalid slot (rc=%d)\n", rc);
}

CON_COMMAND_F(blw_unlock_all,
              "blw_unlock_all  Unlock every slot.",
              FCVAR_NONE)
{
    BotWeaponLock::Dispatch::UnlockAll();
    BotWeaponLock::Commands::PrintToCaller(context, "[BWL] unlocked all slots\n");
}

CON_COMMAND_F(blw_status,
              "blw_status  Print hook status and locked slots.",
              FCVAR_NONE)
{
    BotWeaponLock::Commands::PrintToCaller(context,
        "[BWL] hooks: %s | EquipBestWeapon=%p EquipPistol=%p SelectItem=%p GetSlot=%p\n",
        BotWeaponLock::Hooks::Status(),
        BotWeaponLock::Hooks::EquipBestWeaponAddress(),
        BotWeaponLock::Hooks::EquipPistolAddress(),
        BotWeaponLock::Hooks::SelectItemAddress(),
        BotWeaponLock::Hooks::GetSlotAddress());

    int total = BotWeaponLock::LockState::CountLocked();
    BotWeaponLock::Commands::PrintToCaller(context,
        "[BWL] locked count: %d\n", total);

    if (total > 0)
    {
        for (int s = 0; s < BotWeaponLock::LockState::kMaxSlots; ++s)
        {
            auto t = BotWeaponLock::LockState::Get(s);
            if (t != BotWeaponLock::LockTarget::None)
            {
                BotWeaponLock::Commands::PrintToCaller(context,
                    "[BWL]   slot %2d -> %s\n", s,
                    BotWeaponLock::Commands::TargetName(t));
            }
        }
    }
}
