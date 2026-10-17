#pragma once
#include "core/cs2_sdk/src/core/memory_module.h"
#include <nlohmann/json.hpp>
#include <string>

namespace cs2bc::gamedata {
// Computes <plugin>/gamedata.json from <plugin>/bin/<platform>/<binary>.
// Does not check whether the file exists.
std::string ComputePath();

// Loads gamedata and resolves non-Schema offsets.
bool Load(nlohmann::json& gamedata, cs2_sdk::modules::ModuleInfo& serverModule, char* error, size_t maxlen);
} // namespace cs2bc::gamedata
