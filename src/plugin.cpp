// BotLocker native Metamod:Source plugin entry point.

#include <ISmmPlugin.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include <eiface.h>
#include <icvar.h>
#include <convar.h>
#include <interfaces/interfaces.h>

#include "WeaponLocker.h"
#include "BotLocker.h"
#include "dispatch.h"
#include "WeaponLockerState.h"
#include "BotLockerState.h"
#include "commands.h"

class BotLockerPlugin : public ISmmPlugin
{
public:
    bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
    bool Unload(char *error, size_t maxlen) override;

    bool Pause(char * /*error*/, size_t /*maxlen*/) override { return true; }
    bool Unpause(char * /*error*/, size_t /*maxlen*/) override { return true; }
    void AllPluginsLoaded() override {}

    const char *GetAuthor() override { return "XBribo"; }
    const char *GetName() override { return "BotLocker"; }
    const char *GetDescription() override { return "Lock CS2 bots: freeze AI tick and/or pin to a weapon slot."; }
    const char *GetURL() override { return ""; }
    const char *GetLicense() override { return "GPLv3"; }
    const char *GetVersion() override { return "0.2.0"; }
    const char *GetDate() override { return __DATE__; }
    const char *GetLogTag() override { return "BL"; }
};

BotLockerPlugin g_botLockerPlugin;
PLUGIN_EXPOSE(BotLockerPlugin, g_botLockerPlugin);

static HMODULE GetSelfModule()
{
    HMODULE mod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetSelfModule),
        &mod);
    return mod;
}

// .../addons/BotLocker/bin/win64/BotLocker.dll -> .../addons/BotLocker/gamedata.json
static std::string ComputeGamedataPath()
{
    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA(GetSelfModule(), path, MAX_PATH) == 0)
        return "";

    for (int i = 0; i < 3; ++i)
    {
        char *slash = std::strrchr(path, '\\');
        if (!slash) return "";
        *slash = '\0';
    }
    std::string result(path);
    result += "\\gamedata.json";
    return result;
}

bool BotLockerPlugin::Load(PluginId id, ISmmAPI *ismm,
                           char *error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    g_pCVar = static_cast<ICvar *>(
        ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(error, maxlen,
                      "Failed to get ICvar (%s) via engine factory",
                      CVAR_INTERFACE_VERSION);
        return false;
    }
    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);

    // The dispatch path needs IVEngineServer2::ClientCommand to inject a
    // slotN command into the engine's concommand pipeline.
    BotLocker::Dispatch::g_pEngine = static_cast<IVEngineServer2 *>(
        ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!BotLocker::Dispatch::g_pEngine)
    {
        std::snprintf(error, maxlen,
                      "Failed to get IVEngineServer2 (%s)",
                      INTERFACEVERSION_VENGINESERVER);
        return false;
    }

    // Need ISource2GameClients only as the anchor for sig-scan: its vtable
    // lives in the real server.dll past Metamod's shim.
    void *serverIface =
        ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMECLIENTS, nullptr);
    if (!serverIface)
    {
        std::snprintf(error, maxlen,
                      "Failed to get ISource2GameClients (%s)",
                      INTERFACEVERSION_SERVERGAMECLIENTS);
        return false;
    }

    // Engine interface used by console command output (ClientPrintf).
    BotLocker::Commands::g_pEngine = BotLocker::Dispatch::g_pEngine;

    // Same interface doubles as the "real server.dll" anchor for sig-scan.
    // (serverIface already grabbed above.)

    std::string gamedataPath = ComputeGamedataPath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    if (!BotLocker::WeaponLockerHooks::Install(gamedataPath, serverIface,
                                               error, maxlen))
        return false;

    if (!BotLocker::BotLockerHooks::Install(gamedataPath, serverIface,
                                            error, maxlen))
    {
        BotLocker::WeaponLockerHooks::Remove();
        return false;
    }

    OutputDebugStringA("[BotLocker] plugin loaded successfully\n");
    return true;
}

bool BotLockerPlugin::Unload(char * /*error*/, size_t /*maxlen*/)
{
    BotLocker::BotLockerHooks::Remove();
    BotLocker::WeaponLockerHooks::Remove();
    BotLocker::WeaponLockerState::ClearAll();
    BotLocker::BotLockerState::ClearAll();
    BotLocker::Dispatch::g_pEngine = nullptr;
    BotLocker::Commands::g_pEngine = nullptr;
    ConVar_Unregister();
    g_pCVar = nullptr;
    OutputDebugStringA("[BotLocker] plugin unloaded\n");
    return true;
}
