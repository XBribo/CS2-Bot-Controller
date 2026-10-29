#include "core/gamedata.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "offsets.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include <cstdio>
#include <string>
namespace cs2bc::gamedata {
bool Load(nlohmann::json& gamedata, cs2_sdk::modules::ModuleInfo& serverModule, char* error, size_t maxlen)
{
    const std::string gamedataPath = cs2_sdk::gameconfig::ComputePath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    if (!cs2_sdk::gameconfig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        std::snprintf(error, maxlen, "Failed to load gamedata: %s", gamedataPath.c_str());
        return false;
    }

    cs2_sdk::modules::Initialize();
    if (!cs2_sdk::modules::server || !cs2_sdk::modules::server->Code())
    {
        std::snprintf(error, maxlen, "Could not open CS2 server module at '%s'",
                      cs2_sdk::modules::server ? cs2_sdk::modules::server->Path() : "");
        return false;
    }
    serverModule = cs2_sdk::modules::server->Code();

    return cs2bc::offsets::LoadFromGamedata(gamedata, error, maxlen);
}
} // namespace cs2bc::gamedata
