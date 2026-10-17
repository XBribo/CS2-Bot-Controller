#include "core/log.h"
#include "plugin.h"

#include <cstdio>

#include <icvar.h>
#include <convar.h>
#include <interfaces/interfaces.h>

#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)

#include "ISmmPluginExt.h"
#include "core/interfaces.h"
#include "core/gamedata.h"
#include "WeaponLocker.h"
#include "BotController.h"
#include "BuyController.h"
#include "BuyControllerState.h"
#include "InputInjector.h"
#include "MotionRecorder.h"
#include "WeaponLockerState.h"
#include "BotControllerState.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "core/cs2_sdk/src/core/schema.h"
#include "offsets.h"

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

PLUGIN_EXPOSE(cs2bc::BotControllerPlugin, cs2bc::g_plugin);

namespace cs2bc {

BotControllerPlugin g_plugin;

bool BotControllerPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    if (!log::Init(g_SMAPI->GetBaseDir(), "BotController", error, maxlen)) return false;
    const bool loaded = [&]() -> bool {
        if (!KHook::__exported__khook)
        {
            std::snprintf(error, maxlen, "Metamod with KHook support is required");
            return false;
        }

        if (!interfaces::Init(ismm, error, maxlen)) return false;

        char schemaError[256] = { 0 };
        if (!cs2_sdk::schema::Init(g_schemaSystem, schemaError, sizeof(schemaError)))
        {
            std::snprintf(error, maxlen, "Schema initialization failed: %s", schemaError);
            return false;
        }
        if (!cs2bc::offsets::LoadFromSchema(schemaError, sizeof(schemaError)))
        {
            cs2_sdk::schema::Reset();
            std::snprintf(error, maxlen, "Schema target resolution failed: %s", schemaError);
            return false;
        }
        ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);
        m_convarsRegistered = true;

        nlohmann::json gd;
        cs2_sdk::modules::ModuleInfo serverModule;
        if (!gamedata::Load(gd, serverModule, error, maxlen)) return false;

        if (!cs2bc::weapon_locker_hooks::Install(gd, serverModule, error, maxlen)) return false;

        if (!cs2bc::bot_controller_hooks::Install(gd, serverModule, error, maxlen)) return false;

        // BuyController is optional; missing sig only disables buy control
        char buyErr[256] = { 0 };
        if (!cs2bc::buy_controller_hooks::Install(gd, serverModule, buyErr, sizeof(buyErr)))
        {
            BC_LOG_WARN("BuyController::Install failed (%s); bot buy control disabled\n", buyErr);
        }

        // movement hooks for record/replay
        char injErr[256] = { 0 };
        if (!cs2bc::input_injector::Install(gd, serverModule, injErr, sizeof(injErr)))
        {
            BC_LOG_WARN("InputInjector::Install failed (%s); record/replay movement will be a no-op\n", injErr);
        }

        return true;
    }();
    if (!loaded)
    {
        BC_LOG_ERROR("Load failed: %s", error);
        Unload(nullptr, 0);
        return false;
    }
    BC_LOG_INFO("Loaded %s", GetVersion());
    BC_LOG_DEBUG("Built %s", GetDate());
    return true;
}

bool BotControllerPlugin::Pause(char*, size_t) { return true; }

bool BotControllerPlugin::Unpause(char*, size_t) { return true; }

void BotControllerPlugin::AllPluginsLoaded() {}

const char* BotControllerPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }
const char* BotControllerPlugin::GetName() { return "BotController"; }
const char* BotControllerPlugin::GetDescription() { return "Record & Replay and Control CS2 bots."; }
const char* BotControllerPlugin::GetURL() { return "https://github.com/XBribo/CS2-Bot-Controller"; }
const char* BotControllerPlugin::GetLicense() { return "AGPL-3.0"; }
const char* BotControllerPlugin::GetVersion() { return VERSION_STRING; }
const char* BotControllerPlugin::GetDate() { return BUILD_TIMESTAMP; }
const char* BotControllerPlugin::GetLogTag() { return "BC"; }

bool BotControllerPlugin::Unload(char* /*error*/, size_t /*maxlen*/)
{
    // Drain movement callbacks before releasing their recording and replay state.
    cs2bc::input_injector::Remove();
    cs2bc::motion_recorder::ClearAll();
    cs2bc::buy_controller_hooks::Remove();
    cs2bc::buy_controller_state::ClearAll();
    cs2bc::bot_controller_hooks::Remove();
    cs2bc::weapon_locker_hooks::Remove();
    cs2bc::weapon_locker_state::ClearAll();
    cs2bc::bot_controller_state::ClearAllAll();
    cs2bc::bot_controller_state::ClearAllAim();
    interfaces::Reset();
    cs2_sdk::schema::Reset();
    if (m_convarsRegistered) ConVar_Unregister();
    m_convarsRegistered = false;
    g_pCVar = nullptr;
    BC_LOG_INFO("Unloaded");
    log::Close();
    return true;
}

} // namespace cs2bc
