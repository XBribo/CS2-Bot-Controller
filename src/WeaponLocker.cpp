// Detours for CCSBot::EquipBestWeapon, EquipPistol, and
// CCSPlayer_WeaponServices::SelectItem (vtable[27] @ 0x180ABC950).

#include "WeaponLocker.h"
#include "sig_scan.h"
#include "WeaponLockerState.h"
#include "ccsbot_slot.h"
#include "dispatch.h"

#include <Windows.h>
#include <MinHook.h>

#include <tier0/dbg.h>
#include <eiface.h>
#include <playerslot.h>

#include <cstdio>
#include <cstdarg>
#include <vector>
#include <mutex>
#include <unordered_map>

using EquipBestWeapon_t = void(__fastcall *)(void *self, char mustEquip);
using EquipPistol_t     = void(__fastcall *)(void *self, char mustEquip);
using SelectItem_t      = char(__fastcall *)(void *ws, void *weapon, int flag);
using GetSlot_t         = void *(__fastcall *)(void *ws, int slot, unsigned int mask);

// pawn (CCSPlayerPawn*) -> WeaponServices* lives at +0xA00.
static constexpr int kOffsetWeaponServicesInPawn = 0xA00;
// CCSBot* -> pawn* lives at +0x18.
static constexpr int kOffsetPawnInBot            = 0x18;

namespace BotLocker
{
    namespace WeaponLockerHooks
    {
        static EquipBestWeapon_t g_origEquipBestWeapon = nullptr;
        static EquipPistol_t     g_origEquipPistol     = nullptr;
        static SelectItem_t      g_origSelectItem      = nullptr;
        static GetSlot_t         g_pGetSlot            = nullptr;

        static void             *g_addrEquipBestWeapon = nullptr;
        static void             *g_addrEquipPistol     = nullptr;
        static void             *g_addrSelectItem      = nullptr;
        static void             *g_addrGetSlot         = nullptr;

        static std::string       g_status              = "not_attempted";
        static bool              g_installed           = false;

        // WeaponServices* -> (bot slot, pawn). slot is set by the bot-level
        // hooks (they own the slot info); pawn lets HookedSelectItem re-check
        // the current controller and skip the block when a human took over.
        struct WsBinding { int slot; void *pawn; };
        static std::unordered_map<void *, WsBinding> g_wsToBinding;
        // Inverse: slot -> WeaponServices*. Lets dispatch find the ws for a
        // given bot and invoke a forced switch.
        static void                            *g_slotToWs[64] = {nullptr};
        static std::mutex                       g_wsToSlotMu;

        // Broadcast a debug line to server console + listen-server host
        // (slot 0). User on a listen server sees it in their game console.
        static void DebugLine(const char *fmt, ...)
        {
            char buf[256];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            Msg("%s", buf);
            if (Dispatch::g_pEngine)
                Dispatch::g_pEngine->ClientPrintf(CPlayerSlot(0), buf);
        }

        // ---- ws -> slot cache helpers ----

        static void RememberWsForBot(void *bot, int slot)
        {
            if (!bot || slot < 0 || slot >= 64) return;
            void *pawn = *reinterpret_cast<void **>(
                reinterpret_cast<char *>(bot) + kOffsetPawnInBot);
            if (!pawn) return;
            void *ws = *reinterpret_cast<void **>(
                reinterpret_cast<char *>(pawn) + kOffsetWeaponServicesInPawn);
            if (!ws) return;
            std::lock_guard<std::mutex> lk(g_wsToSlotMu);
            g_wsToBinding[ws] = {slot, pawn};
            g_slotToWs[slot]  = ws;
        }

        static WsBinding LookupBindingForWs(void *ws)
        {
            if (!ws) return {-1, nullptr};
            std::lock_guard<std::mutex> lk(g_wsToSlotMu);
            auto it = g_wsToBinding.find(ws);
            return it == g_wsToBinding.end() ? WsBinding{-1, nullptr} : it->second;
        }

        // LockTarget -> engine weapon-slot index (WeaponServices::GetSlot N).
        // SlotN value 1..5 maps directly to engine slot N-1 (0..4).
        static int LockTargetToEngineSlot(LockTarget t)
        {
            int v = static_cast<int>(t);
            if (v < 1 || v > 5) return -1;
            return v - 1;
        }

        // ---- edge-triggered logging ----

        static int g_lastLoggedLock[64] = {0};

        static void MaybeLogEdge(const char *which, void *bot, const SlotResolution &sr, LockTarget lt)
        {
            if (sr.slot < 0 || sr.slot >= 64) return;
            int curr = static_cast<int>(lt);
            if (g_lastLoggedLock[sr.slot] == curr) return;
            g_lastLoggedLock[sr.slot] = curr;
            DebugLine("[BWL][hook] %s slot=%d lock=%d bot=%p\n",
                      which, sr.slot, curr, bot);
        }

        void ResetLogEdgeForSlot(int slot)
        {
            if (slot < 0 || slot >= 64) return;
            g_lastLoggedLock[slot] = -1;
        }

        // ---- detours ----

        static void __fastcall HookedEquipBestWeapon(void *bot, char mustEquip)
        {
            auto sr = ResolveSlot(bot);
            if (sr.slot >= 0) RememberWsForBot(bot, sr.slot);
            LockTarget lt = (sr.slot >= 0) ? WeaponLockerState::Get(sr.slot) : LockTarget::None;
            MaybeLogEdge("EquipBestWeapon", bot, sr, lt);
            if (lt != LockTarget::None) return;
            g_origEquipBestWeapon(bot, mustEquip);
        }

        static void __fastcall HookedEquipPistol(void *bot, char mustEquip)
        {
            auto sr = ResolveSlot(bot);
            if (sr.slot >= 0) RememberWsForBot(bot, sr.slot);
            LockTarget lt = (sr.slot >= 0) ? WeaponLockerState::Get(sr.slot) : LockTarget::None;
            MaybeLogEdge("EquipPistol", bot, sr, lt);
            if (lt != LockTarget::None) return;
            g_origEquipPistol(bot, mustEquip);
        }

        // Block-log dedup: log only when the (slot, weapon-being-blocked)
        // pair changes, so repeated blocks of the same switch don't spam.
        static void *g_lastBlockedWeapon[64] = {nullptr};

        static char __fastcall HookedSelectItem(void *ws, void *weapon, int flag)
        {
            WsBinding bind = LookupBindingForWs(ws);
            if (bind.slot < 0)
                return g_origSelectItem(ws, weapon, flag);

            // Human took over this pawn -> current m_hController != bot slot
            // we cached; don't block player's weapon switches.
            int curSlot = ControllerSlotForPawn(bind.pawn);
            if (curSlot != bind.slot)
                return g_origSelectItem(ws, weapon, flag);

            LockTarget lt = WeaponLockerState::Get(bind.slot);
            if (lt == LockTarget::None)
                return g_origSelectItem(ws, weapon, flag);

            int engineSlot = LockTargetToEngineSlot(lt);
            if (engineSlot < 0 || !g_pGetSlot)
                return g_origSelectItem(ws, weapon, flag);

            void *targetWeapon = g_pGetSlot(ws, engineSlot, 0xFFFFFFFFu);
            // No weapon in the locked slot -> can't enforce, let it through.
            if (!targetWeapon)
                return g_origSelectItem(ws, weapon, flag);

            // Switch is to the lock target -> allow.
            if (weapon == targetWeapon)
                return g_origSelectItem(ws, weapon, flag);

            // Switch is to something else -> block.
            int slot = bind.slot;
            if (slot >= 0 && slot < 64 && g_lastBlockedWeapon[slot] != weapon)
            {
                g_lastBlockedWeapon[slot] = weapon;
                DebugLine("[BWL][block] SelectItem slot=%d lock=%d weapon=%p target=%p\n",
                          slot, static_cast<int>(lt), weapon, targetWeapon);
            }
            return 0;
        }

        // ---- install / remove ----

        static void *ResolveSig(const std::string &gamedataText,
                                HMODULE serverModule,
                                const char *name,
                                char *errorOut, size_t errorOutLen)
        {
            std::string sig = Sig::FindWindowsSig(gamedataText, name);
            if (sig.empty())
            {
                std::snprintf(errorOut, errorOutLen,
                              "gamedata missing '%s.signatures.windows'", name);
                return nullptr;
            }
            std::vector<uint8_t> bytes;
            std::vector<bool>    wild;
            if (!Sig::ParseSigString(sig, bytes, wild))
            {
                std::snprintf(errorOut, errorOutLen,
                              "failed to parse '%s' sig: '%s'", name, sig.c_str());
                return nullptr;
            }
            void *addr = Sig::FindPatternIn(serverModule, bytes, wild);
            if (!addr)
            {
                std::snprintf(errorOut, errorOutLen,
                              "sig '%s' not found in server.dll", name);
                return nullptr;
            }
            return addr;
        }

        bool Install(const std::string &gamedataPath,
                     void *serverIface,
                     char *errorOut, size_t errorOutLen)
        {
            HMODULE serverModule = Sig::ModuleFromInterfacePtr(serverIface);
            if (!serverModule)
            {
                std::snprintf(errorOut, errorOutLen,
                              "ModuleFromInterfacePtr returned null");
                g_status = "failed: no server module";
                return false;
            }

            std::string gd = Sig::ReadFile(gamedataPath);
            if (gd.empty())
            {
                std::snprintf(errorOut, errorOutLen,
                              "failed to read gamedata: %s", gamedataPath.c_str());
                g_status = "failed: gamedata missing";
                return false;
            }

            g_addrEquipBestWeapon = ResolveSig(gd, serverModule,
                                               "CCSBot::EquipBestWeapon",
                                               errorOut, errorOutLen);
            if (!g_addrEquipBestWeapon) { g_status = "failed: EquipBestWeapon sig"; return false; }

            g_addrEquipPistol = ResolveSig(gd, serverModule,
                                           "CCSBot::EquipPistol",
                                           errorOut, errorOutLen);
            if (!g_addrEquipPistol)     { g_status = "failed: EquipPistol sig"; return false; }

            g_addrSelectItem = ResolveSig(gd, serverModule,
                                          "CCSPlayer_WeaponServices::SelectItem",
                                          errorOut, errorOutLen);
            if (!g_addrSelectItem)      { g_status = "failed: SelectItem sig"; return false; }

            g_addrGetSlot = ResolveSig(gd, serverModule,
                                       "CCSPlayer_WeaponServices::GetSlot",
                                       errorOut, errorOutLen);
            if (!g_addrGetSlot)         { g_status = "failed: GetSlot sig"; return false; }
            g_pGetSlot = reinterpret_cast<GetSlot_t>(g_addrGetSlot);

            if (MH_Initialize() != MH_OK)
            {
                std::snprintf(errorOut, errorOutLen, "MH_Initialize failed");
                g_status = "failed: MH_Initialize";
                return false;
            }

            auto failCleanup = [&](const char *what) -> bool {
                std::snprintf(errorOut, errorOutLen, "%s failed", what);
                MH_Uninitialize();
                g_origEquipBestWeapon = nullptr;
                g_origEquipPistol     = nullptr;
                g_origSelectItem      = nullptr;
                return false;
            };

            if (MH_CreateHook(g_addrEquipBestWeapon,
                              reinterpret_cast<void *>(&HookedEquipBestWeapon),
                              reinterpret_cast<void **>(&g_origEquipBestWeapon)) != MH_OK)
            { g_status = "failed: MH_CreateHook EquipBestWeapon"; return failCleanup("MH_CreateHook EquipBestWeapon"); }

            if (MH_CreateHook(g_addrEquipPistol,
                              reinterpret_cast<void *>(&HookedEquipPistol),
                              reinterpret_cast<void **>(&g_origEquipPistol)) != MH_OK)
            { g_status = "failed: MH_CreateHook EquipPistol"; return failCleanup("MH_CreateHook EquipPistol"); }

            if (MH_CreateHook(g_addrSelectItem,
                              reinterpret_cast<void *>(&HookedSelectItem),
                              reinterpret_cast<void **>(&g_origSelectItem)) != MH_OK)
            { g_status = "failed: MH_CreateHook SelectItem"; return failCleanup("MH_CreateHook SelectItem"); }

            if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
            {
                std::snprintf(errorOut, errorOutLen, "MH_EnableHook failed");
                MH_Uninitialize();
                g_status = "failed: MH_EnableHook";
                return false;
            }

            g_installed = true;
            g_status = "ok";

            char dbg[384];
            std::snprintf(dbg, sizeof(dbg),
                          "[BotWeaponLock] hooks installed: EquipBestWeapon=%p EquipPistol=%p SelectItem=%p GetSlot=%p\n",
                          g_addrEquipBestWeapon, g_addrEquipPistol,
                          g_addrSelectItem, g_addrGetSlot);
            OutputDebugStringA(dbg);
            return true;
        }

        void Remove()
        {
            if (!g_installed) return;
            MH_DisableHook(MH_ALL_HOOKS);
            MH_RemoveHook(g_addrEquipBestWeapon);
            MH_RemoveHook(g_addrEquipPistol);
            MH_RemoveHook(g_addrSelectItem);
            MH_Uninitialize();
            g_installed = false;
            g_status = "not_attempted";
            {
                std::lock_guard<std::mutex> lk(g_wsToSlotMu);
                g_wsToBinding.clear();
                for (int i = 0; i < 64; ++i) g_slotToWs[i] = nullptr;
            }
            for (int i = 0; i < 64; ++i)
            {
                g_lastLoggedLock[i] = 0;
                g_lastBlockedWeapon[i] = nullptr;
            }
        }

        const char *Status()             { return g_status.c_str(); }
        void *EquipBestWeaponAddress()   { return g_addrEquipBestWeapon; }
        void *EquipPistolAddress()       { return g_addrEquipPistol; }
        void *SelectItemAddress()        { return g_addrSelectItem; }
        void *GetSlotAddress()           { return g_addrGetSlot; }

        int SwitchToLockTarget(int slot)
        {
            if (!g_installed || !g_origSelectItem || !g_pGetSlot) return 3;
            if (slot < 0 || slot >= 64) return 3;

            LockTarget lt = WeaponLockerState::Get(slot);
            if (lt == LockTarget::None) return 3;
            int engineSlot = LockTargetToEngineSlot(lt);
            if (engineSlot < 0) return 3;

            void *ws = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_wsToSlotMu);
                ws = g_slotToWs[slot];
            }
            if (!ws) return 1;  // bot hasn't ticked yet; lock will still
                                // take effect once AI runs.

            void *target = g_pGetSlot(ws, engineSlot, 0xFFFFFFFFu);
            if (!target) return 2;

            // Route through the original (un-hooked) function so we don't
            // ping-pong through HookedSelectItem.
            g_origSelectItem(ws, target, 0);
            DebugLine("[BWL][switch] slot=%d lock=%d ws=%p target=%p\n",
                      slot, static_cast<int>(lt), ws, target);
            return 0;
        }
    }
}
