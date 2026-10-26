#pragma once

#include "cs_usercmd.pb.h"

#include <string>
#include <string_view>

namespace cs2bc::command_history {
// Stores the simulation anchor and attack history, not live movement fields.
bool Capture(const CSGOUserCmdPB& command, int simulationTick, std::string& bytes);
// Validates serialized history before it enters a replay buffer.
bool Parse(std::string_view bytes, CSGOUserCmdPB& history);
// Rebases a frame-local history without changing relative entry times.
bool Apply(CSGOUserCmdPB& history, CSGOUserCmdPB& command, int simulationTick);
// Removes unrelated client history when none is supplied.
void Clear(CSGOUserCmdPB& command);
} // namespace cs2bc::command_history
