// Read a bot's BotProfile by slot. CCSBot* -> +kBot_Profile -> members.

#include "BotProfile.h"
#include "BotController.h"
#include "ccsbot_slot.h"
#include "version_targets.h"

#include <cstring>

namespace tg = BotController::targets;

namespace BotController
{
    namespace BotProfile
    {
        // Read profile members off a live bot for this slot
        bool ReadProfile(int slot, BotProfileData &out)
        {
            void *bot = BotControllerHooks::BotForSlot(slot);
            if (!bot)
                return false;
            // Guard against a stale pointer: it must still resolve to this slot
            if (CCSBotToSlot(bot) != slot)
                return false;

            void *prof = nullptr;
            if (!ReadField(bot, tg::kBot_Profile, prof))
                return false;
            if (!prof)
                return false;

            std::memset(&out, 0, sizeof(out));
            if (!ReadField(prof, tg::kProf_Aggression, out.aggression) ||
                !ReadField(prof, tg::kProf_Skill, out.skill) ||
                !ReadField(prof, tg::kProf_Teamwork, out.teamwork) ||
                !ReadField(prof, tg::kProf_ReactionTime, out.reactionTime) ||
                !ReadField(prof, tg::kProf_AttackDelay, out.attackDelay) ||
                !ReadField(prof, tg::kProf_LookAccelAtk, out.lookAccelAtk) ||
                !ReadField(prof, tg::kProf_LookStiffAtk, out.lookStiffAtk) ||
                !ReadField(prof, tg::kProf_LookDampAtk, out.lookDampAtk) ||
                !ReadField(prof, tg::kProf_Cost, out.cost) ||
                !ReadField(prof, tg::kProf_Difficulty, out.difficulty))
                return false;

            int count = 0;
            if (!ReadField(prof, tg::kProf_WeaponPrefCount, count))
                return false;
            if (count < 0)
                count = 0;
            if (count > 16)
                count = 16;
            out.weaponPrefCount = count;
            for (int i = 0; i < count; ++i)
                if (!ReadField(prof, tg::kProf_WeaponPref + i * 2,
                               out.weaponPref[i]))
                    return false;
            return true;
        }
    }
}
