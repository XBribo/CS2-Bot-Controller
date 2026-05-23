// BotLocker console commands.

#include "commands.h"
#include "dispatch.h"
#include "WeaponLocker.h"
#include "BotLocker.h"
#include "WeaponLockerState.h"
#include "BotLockerState.h"

#include <tier0/dbg.h>
#include <convar.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BotLocker
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
        BotLocker::Commands::PrintToCaller(context,
            "usage: blw_lock <slot> <slot1|slot2|slot3|slot4|slot5>\n");
        return;
    }
    const int slot = std::atoi(args.Arg(1));
    const auto tgt = BotLocker::Commands::ParseTarget(args.Arg(2));
    if (tgt == BotLocker::LockTarget::None)
    {
        BotLocker::Commands::PrintToCaller(context,
            "[BWL] error: target must be slot1..slot5\n");
        return;
    }
    int rc = BotLocker::Dispatch::Lock(slot, tgt);
    if (rc == 0)
        BotLocker::Commands::PrintToCaller(context,
            "[BWL] locked slot %d to %s\n", slot, BotLocker::Commands::TargetName(tgt));
    else if (rc == -1)
        BotLocker::Commands::PrintToCaller(context,
            "[BWL] error: plugin not initialized (rc=%d)\n", rc);
    else
        BotLocker::Commands::PrintToCaller(context,
            "[BWL] error: invalid args (rc=%d)\n", rc);
}

CON_COMMAND_F(blw_unlock,
              "blw_unlock <slot>  Unlock the bot at <slot>.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        BotLocker::Commands::PrintToCaller(context, "usage: blw_unlock <slot>\n");
        return;
    }
    const int slot = std::atoi(args.Arg(1));
    int rc = BotLocker::Dispatch::Unlock(slot);
    if (rc == 0)
        BotLocker::Commands::PrintToCaller(context,
            "[BWL] unlocked slot %d\n", slot);
    else
        BotLocker::Commands::PrintToCaller(context,
            "[BWL] error: invalid slot (rc=%d)\n", rc);
}

CON_COMMAND_F(blw_unlock_all,
              "blw_unlock_all  Unlock every slot.",
              FCVAR_NONE)
{
    BotLocker::Dispatch::UnlockAll();
    BotLocker::Commands::PrintToCaller(context, "[BWL] unlocked all slots\n");
}

CON_COMMAND_F(blw_status,
              "blw_status  Print hook status and locked slots.",
              FCVAR_NONE)
{
    BotLocker::Commands::PrintToCaller(context,
        "[BWL] hooks: %s | EquipBestWeapon=%p EquipPistol=%p SelectItem=%p GetSlot=%p\n",
        BotLocker::WeaponLockerHooks::Status(),
        BotLocker::WeaponLockerHooks::EquipBestWeaponAddress(),
        BotLocker::WeaponLockerHooks::EquipPistolAddress(),
        BotLocker::WeaponLockerHooks::SelectItemAddress(),
        BotLocker::WeaponLockerHooks::GetSlotAddress());

    int total = BotLocker::WeaponLockerState::CountLocked();
    BotLocker::Commands::PrintToCaller(context,
        "[BWL] locked count: %d\n", total);

    if (total > 0)
    {
        for (int s = 0; s < BotLocker::WeaponLockerState::kMaxSlots; ++s)
        {
            auto t = BotLocker::WeaponLockerState::Get(s);
            if (t != BotLocker::LockTarget::None)
            {
                BotLocker::Commands::PrintToCaller(context,
                    "[BWL]   slot %2d -> %s\n", s,
                    BotLocker::Commands::TargetName(t));
            }
        }
    }
}

CON_COMMAND_F(bl_lock_bot,
              "bl_lock_bot <slot>  Freeze the bot at <slot>'s AI tick.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        BotLocker::Commands::PrintToCaller(context, "usage: bl_lock_bot <slot>\n");
        return;
    }
    const int slot = std::atoi(args.Arg(1));
    int rc = BotLocker::Dispatch::LockBot(slot);
    if (rc == 0)
        BotLocker::Commands::PrintToCaller(context, "[BL] locked bot slot %d\n", slot);
    else
        BotLocker::Commands::PrintToCaller(context,
            "[BL] error: invalid slot (rc=%d)\n", rc);
}

CON_COMMAND_F(bl_unlock_bot,
              "bl_unlock_bot <slot>  Unfreeze the bot at <slot>.",
              FCVAR_NONE)
{
    if (args.ArgC() < 2)
    {
        BotLocker::Commands::PrintToCaller(context, "usage: bl_unlock_bot <slot>\n");
        return;
    }
    const int slot = std::atoi(args.Arg(1));
    int rc = BotLocker::Dispatch::UnlockBot(slot);
    if (rc == 0)
        BotLocker::Commands::PrintToCaller(context, "[BL] unlocked bot slot %d\n", slot);
    else
        BotLocker::Commands::PrintToCaller(context,
            "[BL] error: invalid slot (rc=%d)\n", rc);
}

CON_COMMAND_F(bl_unlock_all_bots,
              "bl_unlock_all_bots  Unfreeze every bot.",
              FCVAR_NONE)
{
    BotLocker::Dispatch::UnlockAllBots();
    BotLocker::Commands::PrintToCaller(context, "[BL] unlocked all bots\n");
}

CON_COMMAND_F(bl_status,
              "bl_status  Print CCSBot hook status and frozen bots.",
              FCVAR_NONE)
{
    BotLocker::Commands::PrintToCaller(context,
        "[BL] hooks: %s | Update=%p | Upkeep=%p\n",
        BotLocker::BotLockerHooks::Status(),
        BotLocker::BotLockerHooks::UpdateAddress(),
        BotLocker::BotLockerHooks::UpkeepAddress());

    int total = BotLocker::BotLockerState::CountLocked();
    BotLocker::Commands::PrintToCaller(context,
        "[BL] frozen bot count: %d\n", total);
    if (total > 0)
    {
        for (int s = 0; s < BotLocker::BotLockerState::kMaxSlots; ++s)
            if (BotLocker::BotLockerState::Get(s))
                BotLocker::Commands::PrintToCaller(context, "[BL]   slot %2d\n", s);
    }
}
