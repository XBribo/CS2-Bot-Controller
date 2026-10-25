#pragma once

#include "cs_usercmd.pb.h"

#include <string>
#include <string_view>

namespace cs2bc::command_history {
// Copies attack history without retaining the live command's movement fields.
bool Capture(const CSGOUserCmdPB& command, std::string& bytes);
// Validates serialized history before it enters a replay buffer.
bool Parse(std::string_view bytes, CSGOUserCmdPB& history);
// Rebases and consumes a frame-local history, preserving timestamp fractions.
bool Apply(CSGOUserCmdPB& history, CSGOUserCmdPB& command);
// Removes unrelated client history when none is supplied.
void Clear(CSGOUserCmdPB& command);
} // namespace cs2bc::command_history
