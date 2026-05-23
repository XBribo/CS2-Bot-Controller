// MinHook install/remove for CCSBot::Update and CCSBot::Upkeep. When
// BotLockerState says a bot slot is locked, both detours skip their body:
//   Update  (~10Hz): PathFollower / WeaponPreference / ReactionQueue / ...
//   Upkeep  (per-frame): UpdateLookAround + look jitter + UpdateLookAngles
// Without the Upkeep hook the bot's view would still drift each frame from
// jitter + spring-damper smoothing toward the last AI-set look target.

#pragma once

#include <string>

namespace BotLocker
{
    namespace BotLockerHooks
    {
        // Install. gamedataPath: absolute path to gamedata.json.
        // serverIface: any server-side interface pointer (used to locate
        // the real server.dll past Metamod's shim).
        bool Install(const std::string &gamedataPath,
                     void *serverIface,
                     char *errorOut, size_t errorOutLen);

        // Disable + remove. MinHook itself is owned by WeaponLockerHooks,
        // so this only removes the Update hook.
        void Remove();

        // Diagnostic: "ok" / "not_attempted" / "failed: <reason>".
        const char *Status();

        // Diagnostic: CCSBot::Update address (nullptr if not installed).
        void *UpdateAddress();

        // Diagnostic: CCSBot::Upkeep address (nullptr if not installed).
        void *UpkeepAddress();
    }
}
