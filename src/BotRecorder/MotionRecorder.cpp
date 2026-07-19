// Motion recording & replay implementation

#include "MotionRecorder.h"
#include "BotController.h"
#include "InputInjector.h"
#include "WeaponLocker.h"
#include "ccsbot_slot.h"
#include "version_targets.h"
#include "platform.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace tg = BotController::targets;

namespace BotController
{
    namespace MotionRecorder
    {
        struct RecordState
        {
            std::atomic<bool> recording{false};
            std::vector<ReplayTick> ticks;
            std::vector<SubtickMove> subs;
            // Subtick moves seen on PlayerRunCommand, awaiting the matching
            // ProcessMovement post that commits them to a tick.
            std::vector<SubtickMove> pendingSubs;
            MovementSnapshot pendingPre{};
            bool havePre{false};
            std::atomic<void *> liveWs{nullptr};
            std::atomic<int> currentDef{-1};
            std::mutex mu; // guards ticks/subs/pending/pre
        };

        struct ReplayState
        {
            std::atomic<bool> playing{false};
            std::atomic<bool> loop{false};
            std::vector<ReplayTick> ticks;
            std::vector<SubtickMove> subs;
            std::vector<ReplayCommandFrameData> commands;
            std::vector<ReplayMovementExtra> movementExtras;
            std::vector<uint32_t> subOffset; // prefix sum, size ticks.size()+1
            std::atomic<int> cursor{0};
            std::atomic<int> lastAppliedDef{-1};
            std::mutex mu; // guards ticks/subs/subOffset
        };

        static std::array<RecordState, kMaxSlots> g_rec;
        static std::array<ReplayState, kMaxSlots> g_rep;

        /* ! Replay-rate probe */
        static int64_t g_lastCommitQpc[kMaxSlots] = {0};
        /* ? Velocity-vs-displacement probe */
        static float g_lastPostX[kMaxSlots] = {0};
        static float g_lastPostY[kMaxSlots] = {0};
        static bool g_haveLastPost[kMaxSlots] = {false};
        /* ? Record-side probe */
        static int64_t g_recLastQpc[kMaxSlots] = {0};
        static float g_recLastNodeX[kMaxSlots] = {0};
        static float g_recLastNodeY[kMaxSlots] = {0};
        static bool g_recHaveLast[kMaxSlots] = {false};

        static bool ValidSlot(int s) { return s >= 0 && s < kMaxSlots; }

        static int64_t NowMicros()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       clock::now().time_since_epoch())
                .count();
        }

        // Reads a three-float engine vector through one guarded memory operation.
        static bool ReadVector3(void *base, int offset, float &x, float &y, float &z)
        {
            float values[3] = {};
            if (!TryReadMemory(base, offset, values, sizeof(values)))
                return false;
            x = values[0];
            y = values[1];
            z = values[2];
            return true;
        }

        // Writes a three-float engine vector through one guarded memory operation.
        static bool WriteVector3(void *base, int offset, float x, float y, float z)
        {
            const float values[3] = {x, y, z};
            return TryWriteMemory(base, offset, values, sizeof(values));
        }

        // Resolves the current scene node through the July 2026 body component layout.
        static void *ResolveSceneNode(void *entity)
        {
            void *body = nullptr;
            if (!SafeRead(entity, tg::kEnt_BodyComponent, body) || !body)
                return nullptr;

            void *node = nullptr;
            return SafeRead(body, tg::kBody_SceneNode, node) ? node : nullptr;
        }

        // Read a MovementSnapshot from live engine state (services -> pawn).
        static bool ReadSnapshot(int slot, void *services, MovementSnapshot &out)
        {
            if (!services)
                return false;
            void *pawn = InputInjector::ResolveReplayPawn(slot, services);
            if (!pawn)
                return false;

            void *node = ResolveSceneNode(pawn);
            return node &&
                   ReadVector3(pawn, tg::kEnt_AbsVelocity,
                               out.velX, out.velY, out.velZ) &&
                   SafeRead(pawn, tg::kEnt_Flags, out.entityFlags) &&
                   SafeRead(pawn, tg::kEnt_MoveType, out.moveType) &&
                   SafeRead(pawn, tg::kEnt_ActualMoveType, out.actualMoveType) &&
                   SafeRead(services, tg::kServices_Buttons, out.buttons) &&
                   SafeRead(services, tg::kServices_Buttons1, out.buttons1) &&
                   SafeRead(services, tg::kServices_Buttons2, out.buttons2) &&
                   SafeRead(services, tg::kServices_DuckAmount, out.duckAmount) &&
                   SafeRead(services, tg::kServices_DuckSpeed, out.duckSpeed) &&
                   ReadVector3(services, tg::kServices_LadderNormal,
                               out.ladderNormalX, out.ladderNormalY, out.ladderNormalZ) &&
                   SafeRead(services, tg::kServices_Ducked, out.ducked) &&
                   SafeRead(services, tg::kServices_Ducking, out.ducking) &&
                   SafeRead(services, tg::kServices_DesiresDuck, out.desiresDuck) &&
                   ReadVector3(pawn, tg::kPawn_ViewAngle,
                               out.pitch, out.yaw, out.roll) &&
                   ReadVector3(node, tg::kNode_AbsOrigin,
                               out.originX, out.originY, out.originZ);
        }

        // ---- recording ----

        bool StartRecord(int slot)
        {
            if (!ValidSlot(slot))
                return false;
            RecordState &r = g_rec[slot];
            {
                std::lock_guard<std::mutex> lk(r.mu);
                r.ticks.clear();
                r.subs.clear();
                r.pendingSubs.clear();
                r.havePre = false;
                r.ticks.reserve(4096); // ~64s @ 64 tick
                r.subs.reserve(4096);
            }
            r.currentDef.store(-1, std::memory_order_relaxed);
            r.liveWs.store(nullptr, std::memory_order_relaxed);
            r.recording.store(true, std::memory_order_release);
            return true;
        }

        bool StopRecord(int slot)
        {
            if (!ValidSlot(slot))
                return false;
            g_rec[slot].recording.store(false, std::memory_order_release);
            return true;
        }

        bool IsRecording(int slot)
        {
            return ValidSlot(slot) &&
                   g_rec[slot].recording.load(std::memory_order_acquire);
        }

        int RecordedTickCount(int slot)
        {
            if (!ValidSlot(slot))
                return -1;
            RecordState &r = g_rec[slot];
            std::lock_guard<std::mutex> lk(r.mu);
            return static_cast<int>(r.ticks.size());
        }

        int RecordedSubtickCount(int slot)
        {
            if (!ValidSlot(slot))
                return -1;
            RecordState &r = g_rec[slot];
            std::lock_guard<std::mutex> lk(r.mu);
            return static_cast<int>(r.subs.size());
        }

        void SetLiveWs(int slot, void *ws)
        {
            if (ValidSlot(slot))
                g_rec[slot].liveWs.store(ws, std::memory_order_relaxed);
        }

        void *LiveWs(int slot)
        {
            return ValidSlot(slot)
                       ? g_rec[slot].liveWs.load(std::memory_order_relaxed)
                       : nullptr;
        }

        void SetCurrentDef(int slot, int defIndex)
        {
            if (ValidSlot(slot))
                g_rec[slot].currentDef.store(defIndex, std::memory_order_relaxed);
        }

        void OnCapturePre(int slot, void *services, void *cmd)
        {
            (void)cmd;
            if (!ValidSlot(slot) || !services)
                return;
            RecordState &r = g_rec[slot];
            if (!r.recording.load(std::memory_order_acquire))
                return;
            MovementSnapshot pre{};
            if (!ReadSnapshot(slot, services, pre))
                return;
            std::lock_guard<std::mutex> lk(r.mu);
            r.pendingPre = pre;
            r.havePre = true;
        }

        void OnCaptureSubticks(int slot, const SubtickMove *moves, int count)
        {
            if (!ValidSlot(slot) || count < 0)
                return;
            RecordState &r = g_rec[slot];
            if (!r.recording.load(std::memory_order_acquire))
                return;
            if (count > kMaxSubtickPerTick)
                count = kMaxSubtickPerTick;
            std::lock_guard<std::mutex> lk(r.mu);
            r.pendingSubs.clear();
            for (int i = 0; i < count; ++i)
                r.pendingSubs.push_back(moves[i]);

            /* ? Record-side subtick diagnostic */
            for (int i = 0; i < count; ++i)
            {
                if (moves[i].button & 1u)
                {
                    char dbg[160];
                    std::snprintf(dbg, sizeof(dbg),
                                  "[BL][recSt] slot=%d i=%d btn=%u pressed=%.2f when=%.3f\n",
                                  slot, i, moves[i].button, moves[i].pressed, moves[i].when);
                    DebugOut(dbg);
                }
            }
        }

        void OnCapturePost(int slot, void *services, void *cmd)
        {
            // cmd is actually the CMoveData* (hook passes moveData here)
            if (!ValidSlot(slot) || !services)
                return;
            RecordState &r = g_rec[slot];
            if (!r.recording.load(std::memory_order_acquire))
                return;

            MovementSnapshot post{};
            if (!ReadSnapshot(slot, services, post))
                return;

            if (cmd)
            {
                ReadVector3(cmd, tg::kMove_AbsOrigin,
                            post.originX, post.originY, post.originZ);
            }

            // Active weapon def for this tick.
            void *ws = r.liveWs.load(std::memory_order_relaxed);
            int def = WeaponLockerHooks::ActiveWeaponDef(ws);
            if (def < 0)
                def = r.currentDef.load(std::memory_order_relaxed);

            int tickIdx;
            uint32_t nSub;
            {
                std::lock_guard<std::mutex> lk(r.mu);
                ReplayTick t{};
                t.pre = r.havePre ? r.pendingPre : post;
                t.post = post;
                t.weaponDefIndex = def;
                nSub = static_cast<uint32_t>(r.pendingSubs.size());
                t.numSubtick = nSub;
                for (const auto &sm : r.pendingSubs)
                    r.subs.push_back(sm);
                r.ticks.push_back(t);
                r.pendingSubs.clear();
                r.havePre = false;
                tickIdx = static_cast<int>(r.ticks.size()) - 1;
            }

            /* ? Record-side diagnostics */
            int64_t now = NowMicros();
            long long dtUs = g_recHaveLast[slot]
                                 ? (now - g_recLastQpc[slot])
                                 : -1;
            float velR = std::sqrt(post.velX * post.velX + post.velY * post.velY);
            float nodeD = -1.0f;
            if (g_recHaveLast[slot])
            {
                float dx = post.originX - g_recLastNodeX[slot];
                float dy = post.originY - g_recLastNodeY[slot];
                nodeD = std::sqrt(dx * dx + dy * dy) * 64.0f;
            }
            // MoveData origin this tick (cmd == moveData)
            float mvX = 0, mvY = 0;
            if (cmd)
            {
                float mvZ = 0.0f;
                ReadVector3(cmd, tg::kMove_AbsOrigin, mvX, mvY, mvZ);
            }
            g_recLastQpc[slot] = now;
            g_recLastNodeX[slot] = post.originX;
            g_recLastNodeY[slot] = post.originY;
            g_recHaveLast[slot] = true;

            char dbg[256];
            std::snprintf(dbg, sizeof(dbg),
                          "[BL][rec] t=%d dt_us=%lld mt=%u nSub=%u def=%d velR=%.1f nodeD=%.1f "
                          "node=(%.1f,%.1f) mv=(%.1f,%.1f)\n",
                          tickIdx, dtUs, (unsigned)post.moveType, nSub, def, velR, nodeD,
                          post.originX, post.originY, mvX, mvY);
            DebugOut(dbg);
        }

        int CopyTicks(int slot, ReplayTick *out, int maxTicks)
        {
            if (!ValidSlot(slot) || !out || maxTicks <= 0)
                return 0;
            RecordState &r = g_rec[slot];
            std::lock_guard<std::mutex> lk(r.mu);
            int n = static_cast<int>(r.ticks.size());
            if (n > maxTicks)
                n = maxTicks;
            for (int i = 0; i < n; ++i)
                out[i] = r.ticks[i];
            return n;
        }

        int CopySubticks(int slot, SubtickMove *out, int maxSubticks)
        {
            if (!ValidSlot(slot) || !out || maxSubticks <= 0)
                return 0;
            RecordState &r = g_rec[slot];
            std::lock_guard<std::mutex> lk(r.mu);
            int n = static_cast<int>(r.subs.size());
            if (n > maxSubticks)
                n = maxSubticks;
            for (int i = 0; i < n; ++i)
                out[i] = r.subs[i];
            return n;
        }

        // ---- replay ----

        // Load legacy buffers by supplying empty extended buffers
        bool LoadReplay(int slot, const ReplayTick *ticks, int tickCount,
                        const SubtickMove *subs, int subCount) noexcept
        {
            return LoadReplayExtended(slot, ticks, tickCount, subs, subCount,
                                      nullptr, 0, nullptr, 0);
        }

        // Validate, stage, and atomically replace all replay buffers
        bool LoadReplayExtended(int slot, const ReplayTick *ticks, int tickCount,
                                const SubtickMove *subs, int subCount,
                                const ReplayCommandFrameData *commands,
                                int commandCount,
                                const ReplayMovementExtra *movementExtras,
                                int movementExtraCount) noexcept
        {
            try
            {
                if (!ValidSlot(slot) || !ticks || tickCount < 0 || subCount < 0 ||
                    (subCount > 0 && !subs) ||
                    (commandCount != 0 && commandCount != tickCount) ||
                    (commandCount > 0 && !commands) ||
                    (movementExtraCount != 0 && movementExtraCount != tickCount) ||
                    (movementExtraCount > 0 && !movementExtras))
                {
                    return false;
                }

                ReplayState &p = g_rep[slot];
                if (p.playing.load(std::memory_order_acquire))
                    return false;

                std::vector<ReplayTick> stagedTicks;
                std::vector<SubtickMove> stagedSubs;
                std::vector<ReplayCommandFrameData> stagedCommands;
                std::vector<ReplayMovementExtra> stagedMovementExtras;
                std::vector<uint32_t> stagedOffsets(
                    static_cast<size_t>(tickCount) + 1, 0);

                uint64_t totalSubticks = 0;
                for (int i = 0; i < tickCount; ++i)
                {
                    if (ticks[i].numSubtick > kMaxSubtickPerTick)
                        return false;
                    stagedOffsets[static_cast<size_t>(i)] =
                        static_cast<uint32_t>(totalSubticks);
                    totalSubticks += ticks[i].numSubtick;
                    if (totalSubticks > static_cast<uint64_t>(subCount))
                        return false;
                }
                if (totalSubticks != static_cast<uint64_t>(subCount))
                    return false;
                stagedOffsets[static_cast<size_t>(tickCount)] =
                    static_cast<uint32_t>(totalSubticks);

                if (tickCount > 0)
                    stagedTicks.assign(ticks, ticks + tickCount);
                if (subCount > 0)
                    stagedSubs.assign(subs, subs + subCount);
                if (commandCount > 0)
                    stagedCommands.assign(commands, commands + commandCount);
                if (movementExtraCount > 0)
                {
                    stagedMovementExtras.assign(
                        movementExtras, movementExtras + movementExtraCount);
                }

                std::lock_guard<std::mutex> lk(p.mu);
                if (p.playing.load(std::memory_order_acquire))
                    return false;

                p.ticks.swap(stagedTicks);
                p.subs.swap(stagedSubs);
                p.commands.swap(stagedCommands);
                p.movementExtras.swap(stagedMovementExtras);
                p.subOffset.swap(stagedOffsets);
                p.cursor.store(0, std::memory_order_relaxed);
                p.lastAppliedDef.store(-1, std::memory_order_relaxed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool StartReplay(int slot, bool loop)
        {
            if (!ValidSlot(slot))
                return false;
            ReplayState &p = g_rep[slot];
            {
                std::lock_guard<std::mutex> lk(p.mu);
                if (p.ticks.empty())
                    return false;
            }
            p.cursor.store(0, std::memory_order_relaxed);
            p.lastAppliedDef.store(-1, std::memory_order_relaxed);
            p.loop.store(loop, std::memory_order_relaxed);
            p.playing.store(true, std::memory_order_release);
            return true;
        }

        bool StopReplay(int slot)
        {
            if (!ValidSlot(slot))
                return false;
            g_rep[slot].playing.store(false, std::memory_order_release);
            InputInjector::ClearReplayPawn(slot);
            return true;
        }

        bool IsReplaying(int slot)
        {
            return ValidSlot(slot) &&
                   g_rep[slot].playing.load(std::memory_order_acquire);
        }

        int ReplayCursor(int slot)
        {
            if (!ValidSlot(slot))
                return -1;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return -1;
            return p.cursor.load(std::memory_order_relaxed);
        }

        int ReplayTotal(int slot)
        {
            if (!ValidSlot(slot))
                return 0;
            ReplayState &p = g_rep[slot];
            std::lock_guard<std::mutex> lk(p.mu);
            return static_cast<int>(p.ticks.size());
        }

        // cursor points at the NEXT tick; the one just applied is cursor-1.
        bool CurrentReplayTick(int slot, ReplayTick &out)
        {
            if (!ValidSlot(slot))
                return false;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return false;
            std::lock_guard<std::mutex> lk(p.mu);
            int total = static_cast<int>(p.ticks.size());
            int idx = p.cursor.load(std::memory_order_relaxed) - 1;
            if (idx < 0)
                idx = 0;
            if (idx >= total)
                return false;
            out = p.ticks[idx];
            return true;
        }

        bool ReplayCommandViewSnapshot(int slot, MovementSnapshot &out)
        {
            if (!ValidSlot(slot))
                return false;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return false;
            std::lock_guard<std::mutex> lk(p.mu);
            int total = static_cast<int>(p.ticks.size());
            int cur = p.cursor.load(std::memory_order_relaxed);
            if (cur < 0 || cur >= total)
                return false;
            out = p.ticks[cur].pre;
            return true;
        }

        int CurrentReplaySubticks(int slot, SubtickMove *out, int maxOut)
        {
            if (!ValidSlot(slot) || !out || maxOut <= 0)
                return -1;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return -1;
            std::lock_guard<std::mutex> lk(p.mu);
            int total = static_cast<int>(p.ticks.size());
            int idx = p.cursor.load(std::memory_order_relaxed);
            if (idx < 0 || idx >= total)
                return -1;
            uint32_t begin = p.subOffset[idx];
            uint32_t end = p.subOffset[idx + 1];
            int n = static_cast<int>(end - begin);
            if (n > maxOut)
                n = maxOut;
            for (int i = 0; i < n; ++i)
                out[i] = p.subs[begin + i];
            return n;
        }

        bool CurrentReplayInputButtons(int slot, uint64_t &b0, uint64_t &b1,
                                       uint64_t &b2)
        {
            if (!ValidSlot(slot))
                return false;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return false;
            std::lock_guard<std::mutex> lk(p.mu);
            int total = static_cast<int>(p.ticks.size());
            int cur = p.cursor.load(std::memory_order_relaxed);
            if (cur < 0 || cur >= total)
                return false;
            const MovementSnapshot &pre = p.ticks[cur].pre;
            b0 = pre.buttons;
            b1 = pre.buttons1;
            b2 = pre.buttons2;
            if (b1 == 0 && b2 == 0)
            {
                uint64_t heldPrev = (cur > 0) ? p.ticks[cur - 1].pre.buttons : 0;
                b1 = b0 & ~heldPrev;
                b2 = heldPrev & ~b0;
            }
            return true;
        }

        bool SwitchBotWeaponByDef(int slot, int defIndex)
        {
            if (!ValidSlot(slot) || defIndex < 0)
                return false;
            if (!WeaponLockerHooks::WeaponHooksReady())
                return false;
            void *ws = WeaponLockerHooks::WsForSlot(slot);
            if (!ws)
                return false;
            void *weapon = WeaponLockerHooks::FindWeaponByDef(ws, defIndex);
            if (!weapon)
                return false;
            return WeaponLockerHooks::SelectWeaponRaw(ws, weapon);
        }

        // Def index of the bot's current active weapon
        int BotActiveWeaponDef(int slot)
        {
            if (!ValidSlot(slot) || !WeaponLockerHooks::WeaponHooksReady())
                return -1;
            void *ws = WeaponLockerHooks::WsForSlot(slot);
            if (!ws)
                return -1;
            return WeaponLockerHooks::ActiveWeaponDef(ws);
        }

        // Entity index for cmd.weaponselect this replay tick
        int CurrentReplayWeaponDef(int slot)
        {
            if (!ValidSlot(slot))
                return -1;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return -1;
            std::lock_guard<std::mutex> lk(p.mu);
            int total = static_cast<int>(p.ticks.size());
            int cur = p.cursor.load(std::memory_order_relaxed);
            if (cur < 0 || cur >= total)
                return -1;
            return p.ticks[cur].weaponDefIndex;
        }

        int CurrentReplayWeaponSelect(int slot)
        {
            if (!ValidSlot(slot) || !WeaponLockerHooks::WeaponHooksReady())
                return -1;

            // Recorded def for the tick about to be simulated
            int recordedDef = CurrentReplayWeaponDef(slot);
            if (recordedDef < 0)
                return -1;

            void *ws = WeaponLockerHooks::WsForSlot(slot);
            if (!ws)
                return -1;

            // Already holding the recorded weapon -> no switch
            if (WeaponLockerHooks::ActiveWeaponDef(ws) == recordedDef)
            {
                g_rep[slot].lastAppliedDef.store(recordedDef, std::memory_order_relaxed);
                return -1;
            }

            void *weapon = WeaponLockerHooks::FindWeaponByDef(ws, recordedDef);
            if (!weapon)
                return -1;
            WeaponLockerHooks::SelectWeaponRaw(ws, weapon);
            g_rep[slot].lastAppliedDef.store(recordedDef, std::memory_order_relaxed);
            return WeaponLockerHooks::WeaponEntIndex(weapon);
        }

        // Write replay velocity onto the pawn. View replay is driven by SetEyeAngles.
        static void WriteVelocityToPawn(int slot, void *services,
                                        const MovementSnapshot &s)
        {
            void *pawn = InputInjector::ResolveReplayPawn(slot, services);
            if (!pawn)
                return;
            WriteVector3(pawn, tg::kEnt_AbsVelocity, s.velX, s.velY, s.velZ);
        }

        // Writes replay origin through the current body-component scene node.
        static void WriteSceneNodeOrigin(int slot, void *services,
                                         const MovementSnapshot &s,
                                         float zBias = 0.0f)
        {
            void *pawn = InputInjector::ResolveReplayPawn(slot, services);
            if (!pawn)
                return;

            void *node = ResolveSceneNode(pawn);
            if (!node)
                return;

            WriteVector3(node, tg::kNode_AbsOrigin,
                         s.originX, s.originY, s.originZ + zBias);
        }

        // Write origin + velocity into CMoveData.
        static void WriteMoveData(void *moveData, const MovementSnapshot &s)
        {
            WriteVector3(moveData, tg::kMove_AbsOrigin,
                         s.originX, s.originY, s.originZ);
            WriteVector3(moveData, tg::kMove_Velocity, s.velX, s.velY, s.velZ);
        }

        // Restores duck and ladder state through guarded field writes.
        static void WriteMovementServiceState(void *services, const MovementSnapshot &s)
        {
            WriteField(services, tg::kServices_DuckAmount, s.duckAmount);
            WriteField(services, tg::kServices_DuckSpeed, s.duckSpeed);
            WriteVector3(services, tg::kServices_LadderNormal,
                         s.ladderNormalX, s.ladderNormalY, s.ladderNormalZ);
            WriteField(services, tg::kServices_Ducked, s.ducked);
            WriteField(services, tg::kServices_Ducking, s.ducking);
            WriteField(services, tg::kServices_DesiresDuck, s.desiresDuck);
        }

        // ProcessMovement (pre): seed CMoveData + pawn + moveType with pre state.
        void OnReplayPre(int slot, void *services, void *moveData)
        {
            if (!ValidSlot(slot) || !services || !moveData)
                return;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return;
            ReplayTick t{};
            {
                std::lock_guard<std::mutex> lk(p.mu);
                int total = static_cast<int>(p.ticks.size());
                int cur = p.cursor.load(std::memory_order_relaxed);
                if (cur >= total)
                    return; // commit handler will stop/loop
                t = p.ticks[cur];
            }
            WriteMoveData(moveData, t.pre);
            WriteVelocityToPawn(slot, services, t.pre);
            WriteMovementServiceState(services, t.pre);
            // Feed recorded buttons so the engine's Duck()/ladder logic runs
            WriteField(services, tg::kServices_Buttons, t.pre.buttons);
            WriteField(services, tg::kServices_Buttons1, t.pre.buttons1);
            WriteField(services, tg::kServices_Buttons2, t.pre.buttons2);
            void *pawn = InputInjector::ResolveReplayPawn(slot, services);
            if (pawn)
            {
                WriteField(pawn, tg::kEnt_MoveType, t.pre.moveType);
                WriteSceneNodeOrigin(slot, services, t.pre);
                BotControllerHooks::ApplyReplayEyeAngles(
                    pawn, t.pre.pitch, t.pre.yaw);
            }
        }

        // FinishMove (pre): write post snapshot into CMoveData + scene-node origin.
        void OnReplayFinishMove(int slot, void *services, void *moveData)
        {
            if (!ValidSlot(slot) || !services || !moveData)
                return;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return;
            ReplayTick t{};
            {
                std::lock_guard<std::mutex> lk(p.mu);
                int total = static_cast<int>(p.ticks.size());
                int cur = p.cursor.load(std::memory_order_relaxed);
                if (cur >= total)
                    return;
                t = p.ticks[cur];
            }
            WriteMoveData(moveData, t.post);
#if defined(_WIN32)
            WriteSceneNodeOrigin(slot, services, t.post, 1000.0f);
#else
            WriteSceneNodeOrigin(slot, services, t.post);
#endif
        }

        void OnReplayCommit(int slot, void *services)
        {
            if (!ValidSlot(slot) || !services)
                return;
            ReplayState &p = g_rep[slot];
            if (!p.playing.load(std::memory_order_acquire))
                return;

            ReplayTick t{};
            int cur, total;
            {
                std::lock_guard<std::mutex> lk(p.mu);
                total = static_cast<int>(p.ticks.size());
                cur = p.cursor.load(std::memory_order_relaxed);
                if (cur >= total)
                {
                    if (p.loop.load(std::memory_order_relaxed) && total > 0)
                    {
                        p.cursor.store(0, std::memory_order_relaxed);
                        p.lastAppliedDef.store(-1, std::memory_order_relaxed);
                        return;
                    }
                    p.playing.store(false, std::memory_order_release);
                    InputInjector::ClearReplayPawn(slot);
                    return;
                }
                t = p.ticks[cur];
            }

            void *pawn = InputInjector::ResolveReplayPawn(slot, services);
            if (pawn)
            {
                WriteField(pawn, tg::kEnt_MoveType, t.post.moveType);
                WriteField(pawn, tg::kEnt_ActualMoveType, t.post.actualMoveType);
                // Merge ground + ducking bits from the recording, keep the rest live.
                uint32_t live = 0;
                uint32_t mask = tg::kFL_OnGround | tg::kFL_Ducking;
                if (SafeRead(pawn, tg::kEnt_Flags, live))
                {
                    live = (live & ~mask) | (t.post.entityFlags & mask);
                    WriteField(pawn, tg::kEnt_Flags, live);
                }
                BotControllerHooks::ApplyReplayEyeAngles(
                    pawn, t.post.pitch, t.post.yaw);
            }

            WriteVelocityToPawn(slot, services, t.post);
            WriteSceneNodeOrigin(slot, services, t.post);
            WriteMovementServiceState(services, t.post);

            p.cursor.store(cur + 1, std::memory_order_relaxed);

            /* ! Rate probe */
            int64_t now = NowMicros();
            int64_t prev = g_lastCommitQpc[slot];
            g_lastCommitQpc[slot] = now;
            long long dtUs = prev ? (now - prev) : -1;

            /* ? Speed probe */
            float velR = std::sqrt(t.post.velX * t.post.velX + t.post.velY * t.post.velY);
            float velD = -1.0f;
            if (g_haveLastPost[slot])
            {
                float dx = t.post.originX - g_lastPostX[slot];
                float dy = t.post.originY - g_lastPostY[slot];
                velD = std::sqrt(dx * dx + dy * dy) * 64.0f;
            }
            g_lastPostX[slot] = t.post.originX;
            g_lastPostY[slot] = t.post.originY;
            g_haveLastPost[slot] = true;

            char dbg[256];
            std::snprintf(dbg, sizeof(dbg),
                          "[BL][replay] t=%d/%d dt_us=%lld mt=%u grnd=%d velR=%.1f velD=%.1f "
                          "post=(%.1f,%.1f,%.1f)\n",
                          cur, total, dtUs, (unsigned)t.post.moveType, (int)(t.post.entityFlags & 1),
                          velR, velD, t.post.originX, t.post.originY, t.post.originZ);
            DebugOut(dbg);
        }

        void ClearAll()
        {
            for (int i = 0; i < kMaxSlots; ++i)
            {
                g_rec[i].recording.store(false, std::memory_order_release);
                g_rep[i].playing.store(false, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lk(g_rec[i].mu);
                    g_rec[i].ticks.clear();
                    g_rec[i].subs.clear();
                    g_rec[i].pendingSubs.clear();
                    g_rec[i].havePre = false;
                }
                {
                    std::lock_guard<std::mutex> lk(g_rep[i].mu);
                    g_rep[i].ticks.clear();
                    g_rep[i].subs.clear();
                    g_rep[i].commands.clear();
                    g_rep[i].movementExtras.clear();
                    g_rep[i].subOffset.clear();
                }
                g_rec[i].currentDef.store(-1, std::memory_order_relaxed);
                g_rec[i].liveWs.store(nullptr, std::memory_order_relaxed);
                g_rep[i].cursor.store(0, std::memory_order_relaxed);
                g_rep[i].lastAppliedDef.store(-1, std::memory_order_relaxed);
                InputInjector::ClearReplayPawn(i);
            }
        }
    }
}
