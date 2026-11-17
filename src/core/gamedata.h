#pragma once
#include "core/cs2_sdk/src/core/memory_module.h"
#include <nlohmann/json.hpp>

namespace cs2bc::gamedata {
// Loads gamedata and resolves non-Schema offsets.
bool Load(nlohmann::json& gamedata, cs2_sdk::modules::ModuleInfo& serverModule, char* error, size_t maxlen);
} // namespace cs2bc::gamedata
