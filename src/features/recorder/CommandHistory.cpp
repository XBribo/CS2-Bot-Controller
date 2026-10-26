#include "CommandHistory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cs2bc::command_history {
namespace {
constexpr size_t kMaxHistoryBytes = 65536;
constexpr int kMaxHistoryEntries = 128;

// Accepts fractions in [0, 1].
bool ValidFraction(float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; }

// Validates interpolation endpoints and fraction.
bool ValidInterpolation(const CSGOInterpolationInfoPB& value)
{
    return value.src_tick() >= -1 && value.dst_tick() >= -1 &&
           (value.src_tick() < 0 || value.dst_tick() < 0 || value.src_tick() <= value.dst_tick()) &&
           ValidFraction(value.frac());
}

// Validates history entries and attack indices.
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

// Shifts ticks and preserves -1 sentinels.
bool ShiftTick(int source, int64_t delta, int& result)
{
    const int64_t shifted = source < 0 ? source : source + delta;
    if ((source >= 0 && shifted < 0) || shifted < -1 || shifted > std::numeric_limits<int>::max()) return false;
    result = static_cast<int>(shifted);
    return true;
}

// Shifts and normalizes a tick/fraction timestamp.
bool ShiftTime(int sourceTick, float sourceFraction, double delta, int& tick, float& fraction)
{
    const double whole = std::floor(delta);
    const double shiftedFraction = sourceFraction + (delta - whole);
    int64_t carry = static_cast<int64_t>(std::floor(shiftedFraction));
    fraction = static_cast<float>(shiftedFraction - static_cast<double>(carry));
    if (fraction == 1.0F)
    {
        ++carry;
        fraction = 0.0F;
    }
    return ShiftTick(sourceTick, static_cast<int64_t>(whole) + carry, tick);
}

// Shifts interpolation time and preserves missing endpoints.
bool RebaseInterpolation(CSGOInterpolationInfoPB& value, double delta)
{
    int64_t whole = static_cast<int64_t>(std::floor(delta));
    int endAdvance = 0;
    if (value.src_tick() >= 0 && value.dst_tick() >= 0)
    {
        const int span = value.dst_tick() - value.src_tick();
        const double remainder = delta - static_cast<double>(whole);
        if (span == 0 && remainder > 0.0)
        {
            endAdvance = 1;
            value.set_frac(static_cast<float>(remainder));
        }
        else if (span > 0)
        {
            double fraction = value.frac() + remainder / span;
            if (fraction > 1.0)
            {
                ++whole;
                fraction -= 1.0 / span;
            }
            value.set_frac(static_cast<float>(fraction));
        }
    }
    int tick;
    if (value.has_src_tick())
    {
        if (!ShiftTick(value.src_tick(), whole, tick)) return false;
        value.set_src_tick(tick);
    }
    if (value.has_dst_tick())
    {
        if (!ShiftTick(value.dst_tick(), whole + endAdvance, tick)) return false;
        value.set_dst_tick(tick);
    }
    return true;
}
} // namespace

bool Capture(const CSGOUserCmdPB& command, int simulationTick, std::string& bytes)
{
    if (simulationTick < 0) return false;
    CSGOUserCmdPB history;
    history.mutable_base()->set_client_tick(simulationTick);
    // Command-number presence marks a simulation-clock capture.
    history.mutable_base()->set_legacy_command_number(command.base().legacy_command_number());
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

bool Apply(CSGOUserCmdPB& history, CSGOUserCmdPB& command, int simulationTick)
{
    if (simulationTick < 0) return false;
    double sourceEnd = 0.0;
    for (const auto& entry : history.input_history())
        sourceEnd = std::max(sourceEnd, entry.player_tick_count() + static_cast<double>(entry.player_tick_fraction()));
    // Older captures use the latest history time as the anchor.
    if (history.base().has_legacy_command_number())
        sourceEnd = std::max(sourceEnd, static_cast<double>(history.base().client_tick()));
    // A shared offset preserves relative times.
    const double delta = simulationTick - sourceEnd;
    for (auto& entry : *history.mutable_input_history())
    {
        int shifted;
        float fraction;
        if (!ShiftTime(entry.player_tick_count(), entry.player_tick_fraction(), delta, shifted, fraction)) return false;
        entry.set_player_tick_count(shifted);
        entry.set_player_tick_fraction(fraction);
        if (!ShiftTime(entry.render_tick_count(), entry.render_tick_fraction(), delta, shifted, fraction)) return false;
        entry.set_render_tick_count(shifted);
        entry.set_render_tick_fraction(fraction);
        if ((entry.has_sv_interp0() && !RebaseInterpolation(*entry.mutable_sv_interp0(), delta)) ||
            (entry.has_sv_interp1() && !RebaseInterpolation(*entry.mutable_sv_interp1(), delta)) ||
            (entry.has_player_interp() && !RebaseInterpolation(*entry.mutable_player_interp(), delta)))
            return false;
        // Clear source-world diagnostics.
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
