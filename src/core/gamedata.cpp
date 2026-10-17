#include "core/gamedata.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/cs2_sdk/src/utils/platform.h"
#include "offsets.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include <cstdio>
#include <filesystem>
#include <string>
namespace cs2bc::gamedata {
std::string ComputePath()
{
    std::filesystem::path path(cs2_sdk::platform::SelfModulePath());
    if (path.empty()) return "";
    for (int i = 0; i < 3; ++i)
    {
        path = path.parent_path();
        if (path.empty()) return "";
    }
    return (path / "gamedata.json").string();
}

bool Load(nlohmann::json& gamedata, cs2_sdk::modules::ModuleInfo& serverModule, char* error, size_t maxlen)
{
    const std::string gamedataPath = ComputePath();
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

    cs2bc::offsets::LoadFromGamedata(gamedata);

    return true;
}
} // namespace cs2bc::gamedata
