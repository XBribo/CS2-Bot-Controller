// CCSBot Update/Upkeep/Jump detours

#include "BotController.h"
#include "BotControllerState.h"
#include "ccsbot_slot.h"
#include "sig_scan.h"
#include "MotionRecorder.h"
#include "version_targets.h"
#include "hook.h"
#include "platform.h"

#include <tier0/dbg.h>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace tg = BotController::targets;

using Update_t = void(BC_FASTCALL *)(void *bot);
using Upkeep_t = void(BC_FASTCALL *)(void *bot);
using Jump_t = char(BC_FASTCALL *)(void *bot, char mustJump);
using UpdateLookAngles_t = void(BC_FASTCALL *)(void *bot);
using SetEyeAngles_t = void(BC_FASTCALL *)(void *pawn, float *angle);

namespace BotController
{
    namespace BotControllerHooks
    {
        static Update_t g_origUpdate = nullptr;
        static void *g_addrUpdate = nullptr;
        static Upkeep_t g_origUpkeep = nullptr;
        static void *g_addrUpkeep = nullptr;
        static Jump_t g_origJump = nullptr;
        static void *g_addrJump = nullptr;
        static UpdateLookAngles_t g_origUpdateLookAngles = nullptr;
        static void *g_addrUpdateLookAngles = nullptr;
        static SetEyeAngles_t g_origSetEyeAngles = nullptr;
        static void *g_addrSetEyeAngles = nullptr;
#if defined(_WIN32)
        static void **g_ppEntityIdentityChunks = nullptr;
#endif
        static bool g_installed = false;
        static std::string g_status = "not_attempted";

        // slot -> last CCSBot* seen in Update (for profile reads by slot)
        static void *g_slotToBot[64] = {nullptr};
        static std::mutex g_slotToBotMu;

        static Hook g_hookUpdate;
        static Hook g_hookUpkeep;
        static Hook g_hookJump;
        static Hook g_hookUpdateLookAngles;
        static Hook g_hookSetEyeAngles;

        // Normalizes an angle to the engine's expected [-180, 180) range.
        static float NormalizeDeg(float angle)
        {
            angle = std::fmod(angle + 180.0f, 360.0f);
            if (angle < 0.0f)
                angle += 360.0f;
            return angle - 180.0f;
        }

#if defined(_WIN32)
        // Resolves the entity identity chunk pointer referenced by SetEyeAngles.
        static void ResolveSetEyeAnglesEntityChunks(void *setEyeAngles)
        {
            g_ppEntityIdentityChunks = nullptr;
            if (!setEyeAngles)
                return;

            constexpr size_t kSearchBytes = 0x120;
            uint8_t code[kSearchBytes] = {};
            if (!TryReadMemory(setEyeAngles, 0, code, sizeof(code)))
                return;

            auto *functionBase = reinterpret_cast<uint8_t *>(setEyeAngles);
            for (size_t i = 0; i + 10 <= kSearchBytes; ++i)
            {
                if (code[i] != 0x4C || code[i + 1] != 0x8B || code[i + 2] != 0x05 ||
                    code[i + 7] != 0x4D || code[i + 8] != 0x85 || code[i + 9] != 0xC0)
                    continue;

                int32_t relative = 0;
                std::memcpy(&relative, code + i + 3, sizeof(relative));
                g_ppEntityIdentityChunks =
                    reinterpret_cast<void **>(functionBase + i + 7 + relative);
                return;
            }
        }

        // Resolves the live controller owning a replay pawn through entity chunks.
        static void *ReplayControllerForPawn(void *pawn)
        {
            if (!pawn || !g_ppEntityIdentityChunks)
                return nullptr;

            uint32_t handle = 0;
            if (!SafeRead(pawn, tg::kPawn_Controller, handle) ||
                handle == 0xFFFFFFFFu || handle == 0xFFFFFFFEu)
                return nullptr;

            void *chunks = nullptr;
            if (!TryReadMemory(g_ppEntityIdentityChunks, 0, &chunks, sizeof(chunks)) || !chunks)
                return nullptr;

            const uint32_t entityIndex = handle & 0x7FFFu;
            void *chunk = nullptr;
            if (!TryReadMemory(chunks,
                               static_cast<int>((entityIndex >> 9) * sizeof(void *)),
                               &chunk, sizeof(chunk)) ||
                !chunk)
                return nullptr;

            constexpr int kIdentitySize = 0x70;
            auto *identity = reinterpret_cast<uint8_t *>(chunk) +
                             static_cast<size_t>(entityIndex & 0x1FFu) * kIdentitySize;
            uint32_t liveHandle = 0;
            void *controller = nullptr;
            if (!SafeRead(identity, 0x10, liveHandle) || liveHandle != handle ||
                !SafeRead(identity, 0x00, controller))
                return nullptr;
            return controller;
        }
#endif

        // Calls SetEyeAngles while temporarily bypassing the new fake-client early-out.
        static bool ApplyReplayEyeAnglesInternal(void *pawn, float pitch, float yaw)
        {
            if (!pawn || !g_origSetEyeAngles)
                return false;

            float angle[3] = {pitch, NormalizeDeg(yaw), 0.0f};
#if defined(_WIN32)
            void *controller = ReplayControllerForPawn(pawn);
            uint32_t controllerFlags = 0;
            bool restoreFakeClient = false;
            if (controller &&
                SafeRead(controller, tg::kEnt_Flags, controllerFlags) &&
                (controllerFlags & 0x100u) != 0)
            {
                const uint32_t publishedFlags = controllerFlags & ~0x100u;
                restoreFakeClient =
                    WriteField(controller, tg::kEnt_Flags, publishedFlags);
            }
#endif
            g_origSetEyeAngles(pawn, angle);
#if defined(_WIN32)
            if (restoreFakeClient)
                WriteField(controller, tg::kEnt_Flags, controllerFlags);
#endif
            return true;
        }

        // Skip the Bot tick under All lock OR while replaying
        static void BC_FASTCALL HookedUpdate(void *bot)
        {
            int slot = CCSBotToSlot(bot);
            if (slot >= 0 && slot < 64)
            {
                std::lock_guard<std::mutex> lk(g_slotToBotMu);
                g_slotToBot[slot] = bot;
            }
            if (slot >= 0 &&
                (BotControllerState::GetAll(slot) || MotionRecorder::IsReplaying(slot)))
            {
                const uint8_t ticked = 1;
                WriteField(bot, tg::kBot_AiTickedFlag, ticked);
                return;
            }
            g_origUpdate(bot);
        }

        // Skip the per-frame view tick under All or Aim lock.
        // EXCEPTION: while a slot is replaying, drive ONLY the view
        static void BC_FASTCALL HookedUpdateLookAngles(void *bot); // fwd decl

        static void BC_FASTCALL HookedUpkeep(void *bot)
        {
            int slot = CCSBotContextToSlot(bot);
            if (slot >= 0 && MotionRecorder::IsReplaying(slot))
                return;
            if (slot >= 0 &&
                (BotControllerState::GetAll(slot) || BotControllerState::GetAim(slot)))
            {
                return;
            }
            g_origUpkeep(bot);
        }

        // view replay
        static void BC_FASTCALL HookedUpdateLookAngles(void *bot)
        {
            int slot = CCSBotContextToSlot(bot);
            if (slot >= 0 &&
                (MotionRecorder::IsReplaying(slot) ||
                 BotControllerState::GetAll(slot) ||
                 BotControllerState::GetAim(slot)))
                return;
            g_origUpdateLookAngles(bot);
        }

        // Engine eye-angle
        static void BC_FASTCALL HookedSetEyeAngles(void *pawn, float *angle)
        {
            int slot = pawn ? ControllerSlotForPawn(pawn) : -1;
            if (slot >= 0 && MotionRecorder::IsReplaying(slot))
                return;
            g_origSetEyeAngles(pawn, angle);
        }

        // Skip Jump under Jump lock; return 0 mimics its own gate-fail.
        static char BC_FASTCALL HookedJump(void *bot, char mustJump)
        {
            int slot = CCSBotToSlot(bot);
            if (slot >= 0 && BotControllerState::GetJump(slot))
                return 0;
            return g_origJump(bot, mustJump);
        }

        // Resolve a sig from gamedata against the loaded server.dll.
        bool Install(const nlohmann::json &gd, const Sig::ModuleInfo &serverModule,
                     char *errorOut, size_t errorOutLen)
        {
            g_addrUpdate = Sig::ResolveSig(gd, serverModule, "CCSBot::Update",
                                           errorOut, errorOutLen);
            if (!g_addrUpdate)
            {
                g_status = "failed: Update sig";
                return false;
            }

            g_addrUpkeep = Sig::ResolveSig(gd, serverModule, "CCSBot::Upkeep",
                                           errorOut, errorOutLen);
            if (!g_addrUpkeep)
            {
                g_status = "failed: Upkeep sig";
                return false;
            }

            // Jump is optional; failure leaves all/aim working, only jump dies.
            char jumpErr[256] = {0};
            g_addrJump = Sig::ResolveSig(gd, serverModule, "CCSBot::Jump",
                                         jumpErr, sizeof(jumpErr));
            if (!g_addrJump)
            {
                char dbg[320];
                std::snprintf(dbg, sizeof(dbg),
                              "[BotController] WARN: CCSBot::Jump sig not resolved (%s); jump-lock disabled\n",
                              jumpErr);
                DebugOut(dbg);
            }

            // UpdateLookAngles is optional
            char ulaErr[256] = {0};
            g_addrUpdateLookAngles = Sig::ResolveSig(gd, serverModule,
                                                     "CCSBot::UpdateLookAngles",
                                                     ulaErr, sizeof(ulaErr));
            if (!g_addrUpdateLookAngles)
            {
                char dbg[320];
                std::snprintf(dbg, sizeof(dbg),
                              "[BotController] WARN: CCSBot::UpdateLookAngles sig not resolved (%s); replay view-drive disabled\n",
                              ulaErr);
                DebugOut(dbg);
            }

            // SetEyeAngles is optional; without it replay view falls back to
            // the (smoothing) UpdateLookAngles hook only.
            char seaErr[256] = {0};
            g_addrSetEyeAngles = Sig::ResolveSig(gd, serverModule,
                                                 "CCSPlayerPawn::SetEyeAngles",
                                                 seaErr, sizeof(seaErr));
            if (!g_addrSetEyeAngles)
            {
                char dbg[320];
                std::snprintf(dbg, sizeof(dbg),
                              "[BotController] WARN: CCSPlayerPawn::SetEyeAngles sig not resolved (%s); replay 1:1 view disabled\n",
                              seaErr);
                DebugOut(dbg);
            }
#if defined(_WIN32)
            else
            {
                ResolveSetEyeAnglesEntityChunks(g_addrSetEyeAngles);
            }
#endif

            // required: Update
            if (!g_hookUpdate.Create(g_addrUpdate,
                                     reinterpret_cast<void *>(&HookedUpdate),
                                     reinterpret_cast<void **>(&g_origUpdate)) ||
                !g_hookUpdate.Enable())
            {
                std::snprintf(errorOut, errorOutLen, "hook CCSBot::Update failed");
                g_hookUpdate.Remove();
                g_origUpdate = nullptr;
                g_status = "failed: hook Update";
                return false;
            }

            // required: Upkeep
            if (!g_hookUpkeep.Create(g_addrUpkeep,
                                     reinterpret_cast<void *>(&HookedUpkeep),
                                     reinterpret_cast<void **>(&g_origUpkeep)) ||
                !g_hookUpkeep.Enable())
            {
                std::snprintf(errorOut, errorOutLen, "hook CCSBot::Upkeep failed");
                g_hookUpkeep.Remove();
                g_origUpkeep = nullptr;
                g_hookUpdate.Remove();
                g_origUpdate = nullptr;
                g_status = "failed: hook Upkeep";
                return false;
            }

            // optional: Jump
            if (g_addrJump)
            {
                if (!g_hookJump.Create(g_addrJump,
                                       reinterpret_cast<void *>(&HookedJump),
                                       reinterpret_cast<void **>(&g_origJump)) ||
                    !g_hookJump.Enable())
                {
                    DebugOut("[BotController] WARN: hook CCSBot::Jump failed; jump-lock disabled\n");
                    g_hookJump.Remove();
                    g_origJump = nullptr;
                    g_addrJump = nullptr;
                }
            }

            // optional: UpdateLookAngles
            if (g_addrUpdateLookAngles)
            {
                if (!g_hookUpdateLookAngles.Create(g_addrUpdateLookAngles,
                                                   reinterpret_cast<void *>(&HookedUpdateLookAngles),
                                                   reinterpret_cast<void **>(&g_origUpdateLookAngles)) ||
                    !g_hookUpdateLookAngles.Enable())
                {
                    DebugOut("[BotController] WARN: hook UpdateLookAngles failed; replay view-drive disabled\n");
                    g_hookUpdateLookAngles.Remove();
                    g_origUpdateLookAngles = nullptr;
                    g_addrUpdateLookAngles = nullptr;
                }
            }

            // optional: SetEyeAngles
            if (g_addrSetEyeAngles)
            {
                if (!g_hookSetEyeAngles.Create(g_addrSetEyeAngles,
                                               reinterpret_cast<void *>(&HookedSetEyeAngles),
                                               reinterpret_cast<void **>(&g_origSetEyeAngles)) ||
                    !g_hookSetEyeAngles.Enable())
                {
                    DebugOut("[BotController] WARN: hook SetEyeAngles failed; replay 1:1 view disabled\n");
                    g_hookSetEyeAngles.Remove();
                    g_origSetEyeAngles = nullptr;
                    g_addrSetEyeAngles = nullptr;
                }
            }

            g_installed = true;
            g_status = "ok";

            char dbg[400];
            std::snprintf(dbg, sizeof(dbg),
                          "[BotController] Update@%p Upkeep@%p Jump@%p ULA@%p SEA@%p\n",
                          g_addrUpdate, g_addrUpkeep, g_addrJump,
                          g_addrUpdateLookAngles, g_addrSetEyeAngles);
            DebugOut(dbg);
            return true;
        }

        void Remove()
        {
            if (!g_installed)
                return;
            g_hookSetEyeAngles.Remove();
            g_origSetEyeAngles = nullptr;
#if defined(_WIN32)
            g_ppEntityIdentityChunks = nullptr;
#endif
            g_hookUpdateLookAngles.Remove();
            g_origUpdateLookAngles = nullptr;
            g_hookJump.Remove();
            g_origJump = nullptr;
            g_hookUpkeep.Remove();
            g_origUpkeep = nullptr;
            g_hookUpdate.Remove();
            g_origUpdate = nullptr;
            g_installed = false;
            g_status = "not_attempted";
            {
                std::lock_guard<std::mutex> lk(g_slotToBotMu);
                for (int i = 0; i < 64; ++i)
                    g_slotToBot[i] = nullptr;
            }
        }

        const char *Status() { return g_status.c_str(); }
        void *UpdateAddress() { return g_addrUpdate; }
        void *UpkeepAddress() { return g_addrUpkeep; }
        void *JumpAddress() { return g_addrJump; }
        void *UpdateLookAnglesAddress() { return g_addrUpdateLookAngles; }

        // Publishes a replay angle without depending on the bot upkeep path.
        bool ApplyReplayEyeAngles(void *pawn, float pitch, float yaw)
        {
            return ApplyReplayEyeAnglesInternal(pawn, pitch, yaw);
        }

        // Last CCSBot* seen in Update for this slot
        void *BotForSlot(int slot)
        {
            if (slot < 0 || slot >= 64)
                return nullptr;
            std::lock_guard<std::mutex> lk(g_slotToBotMu);
            return g_slotToBot[slot];
        }
    }
}
