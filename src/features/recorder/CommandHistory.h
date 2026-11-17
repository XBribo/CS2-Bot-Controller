#pragma once

#include "cs_usercmd.pb.h"

#include <string>
#include <string_view>

namespace cs2bc::command_history {
// Captures the simulation clock and attack history.
bool Capture(const CSGOUserCmdPB& command, int simulationTick, std::string& bytes);
// Validates serialized input history.
bool Parse(std::string_view bytes, CSGOUserCmdPB& history);
// Rebases input history, preserving relative times.
bool Apply(CSGOUserCmdPB& history, CSGOUserCmdPB& command, int simulationTick);
// Clears input history and attack indices.
void Clear(CSGOUserCmdPB& command);
} // namespace cs2bc::command_history
