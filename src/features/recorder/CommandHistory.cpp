#include "CommandHistory.h"
#include "dispatch.h"

#include <engine2/ivengineserver2.h>
#include <engine2/gameshareddefs.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace cs2bc::command_history {
namespace {
constexpr size_t kMaxHistoryBytes = 65536;
constexpr int kMaxHistoryEntries = 128;

// Bot commands may omit the client clock; their native simulation uses server time.
bool CommandTick(const CSGOUserCmdPB& command, int& tick)
{
    if (command.has_base() && command.base().has_client_tick())
    {
        tick = command.base().client_tick();
        if (tick != 0) return tick > 0;
    }
    auto* globals = dispatch::g_engine ? dispatch::g_engine->GetServerGlobals() : nullptr;
    if (!globals) return false;
    tick = globals->tickcount;
    return tick >= 0;
}

// Fractions may include the endpoint; native time conversion normalizes it.
bool ValidFraction(float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; }

// Accepts absent interpolation, but not malformed ranges or fractions.
bool ValidInterpolation(const CSGOInterpolationInfoPB& value)
{
    return value.src_tick() >= -1 && value.dst_tick() >= -1 &&
           (value.src_tick() < 0 || value.dst_tick() < 0 || value.src_tick() <= value.dst_tick()) &&
           ValidFraction(value.frac());
}

// Keeps attack indices tied to complete, finite history entries.
bool ValidHistory(const CSGOUserCmdPB& history)
{
    const int count = history.input_history_size();
    const auto validIndex = [count](int index) { return index == -1 || (index >= 0 && index < count); };
    if (!history.has_base() || !history.base().has_client_tick() || history.base().client_tick() < 0 ||
        count > kMaxHistoryEntries || !validIndex(history.attack1_start_history_index()) ||
        !validIndex(history.attack2_start_history_index()))
        return false;
    for (const auto& entry : history.input_history())
    {
        if (!std::isfinite(entry.view_angles().x()) || !std::isfinite(entry.view_angles().y()) ||
            !std::isfinite(entry.view_angles().z()) || !entry.has_player_tick_count() || entry.player_tick_count() < 0 ||
            !entry.has_render_tick_count() || entry.render_tick_count() < 0 || !ValidFraction(entry.player_tick_fraction()) ||
            !ValidFraction(entry.render_tick_fraction()) ||
            !ValidInterpolation(entry.sv_interp0()) || !ValidInterpolation(entry.sv_interp1()) ||
            !ValidInterpolation(entry.player_interp()) || !ValidFraction(entry.cl_interp().frac()))
            return false;
        if (entry.has_shoot_position() &&
            (!std::isfinite(entry.shoot_position().x()) || !std::isfinite(entry.shoot_position().y()) ||
             !std::isfinite(entry.shoot_position().z())))
            return false;
    }
    return true;
}

// Retains -1 sentinels and rejects overflow instead of wrapping a clock.
bool ShiftTick(int source, int64_t delta, int& result)
{
    const int64_t shifted = source < 0 ? source : source + delta;
    if ((source >= 0 && shifted < 0) || shifted < -1 || shifted > std::numeric_limits<int>::max()) return false;
    result = static_cast<int>(shifted);
    return true;
}

// Moves both interpolation endpoints by the same whole-tick offset.
bool RebaseInterpolation(CSGOInterpolationInfoPB& value, int64_t delta)
{
    int tick;
    if (value.has_src_tick())
    {
        if (!ShiftTick(value.src_tick(), delta, tick)) return false;
        value.set_src_tick(tick);
    }
    if (value.has_dst_tick())
    {
        if (!ShiftTick(value.dst_tick(), delta, tick)) return false;
        value.set_dst_tick(tick);
    }
    return true;
}
} // namespace

bool Capture(const CSGOUserCmdPB& command, std::string& bytes)
{
    int tick;
    if (!CommandTick(command, tick)) return false;
    CSGOUserCmdPB history;
    history.mutable_base()->set_client_tick(tick);
    if (command.base().has_random_seed()) history.mutable_base()->set_random_seed(command.base().random_seed());
    history.mutable_input_history()->CopyFrom(command.input_history());
    history.set_attack1_start_history_index(command.attack1_start_history_index());
    history.set_attack2_start_history_index(command.attack2_start_history_index());
    return ValidHistory(history) && history.ByteSizeLong() <= kMaxHistoryBytes && history.SerializeToString(&bytes);
}

bool Parse(std::string_view bytes, CSGOUserCmdPB& history)
{
    return !bytes.empty() && bytes.size() <= kMaxHistoryBytes && history.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) &&
           ValidHistory(history);
}

void Clear(CSGOUserCmdPB& command)
{
    command.clear_input_history();
    command.set_attack1_start_history_index(-1);
    command.set_attack2_start_history_index(-1);
    command.clear_is_predicting_body_shot_fx();
    command.clear_is_predicting_head_shot_fx();
    command.clear_is_predicting_kill_ragdolls();
}

bool Apply(CSGOUserCmdPB& history, CSGOUserCmdPB& command)
{
    int tick;
    if (!CommandTick(command, tick)) return false;
    const int64_t delta = static_cast<int64_t>(tick) - history.base().client_tick();
    for (auto& entry : *history.mutable_input_history())
    {
        int shifted;
        if (!ShiftTick(entry.player_tick_count(), delta, shifted)) return false;
        entry.set_player_tick_count(shifted);
        if (!ShiftTick(entry.render_tick_count(), delta, shifted)) return false;
        entry.set_render_tick_count(shifted);
        if ((entry.has_sv_interp0() && !RebaseInterpolation(*entry.mutable_sv_interp0(), delta)) ||
            (entry.has_sv_interp1() && !RebaseInterpolation(*entry.mutable_sv_interp1(), delta)) ||
            (entry.has_player_interp() && !RebaseInterpolation(*entry.mutable_player_interp(), delta)))
            return false;
        // Client-side diagnostics refer to the original world, not this replay.
        entry.clear_frame_number();
        entry.clear_target_ent_index();
        entry.clear_target_head_pos_check();
        entry.clear_target_abs_pos_check();
        entry.clear_target_abs_ang_check();
    }
    Clear(command);
    command.mutable_input_history()->Swap(history.mutable_input_history());
    command.set_attack1_start_history_index(history.attack1_start_history_index());
    command.set_attack2_start_history_index(history.attack2_start_history_index());
    if (history.base().has_random_seed()) command.mutable_base()->set_random_seed(history.base().random_seed());
    else command.mutable_base()->clear_random_seed();
    return true;
}
} // namespace cs2bc::command_history
