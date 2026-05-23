// BotWeaponLock native Metamod:Source plugin entry point.

#include <ISmmPlugin.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include <eiface.h>
#include <icvar.h>
#include <convar.h>
#include <interfaces/interfaces.h>

#include "hooks.h"
#include "dispatch.h"
#include "lock_state.h"
#include "commands.h"

class BotWeaponLockPlugin : public ISmmPlugin
{
public:
    bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
    bool Unload(char *error, size_t maxlen) override;

    bool Pause(char * /*error*/, size_t /*maxlen*/) override { return true; }
    bool Unpause(char * /*error*/, size_t /*maxlen*/) override { return true; }
    void AllPluginsLoaded() override {}

    const char *GetAuthor() override { return "XBribo"; }
    const char *GetName() override { return "BotWeaponLock"; }
    const char *GetDescription() override { return "Lock a bot's weapon slot, blocking AI weapon switches."; }
    const char *GetURL() override { return ""; }
    const char *GetLicense() override { return "GPL3"; }
    const char *GetVersion() override { return "0.1.3"; }
    const char *GetDate() override { return __DATE__; }
    const char *GetLogTag() override { return "BWL"; }
};

BotWeaponLockPlugin g_botWeaponLockPlugin;
PLUGIN_EXPOSE(BotWeaponLockPlugin, g_botWeaponLockPlugin);

static HMODULE GetSelfModule()
{
    HMODULE mod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetSelfModule),
        &mod);
    return mod;
}

// .../addons/BotWeaponLock/bin/win64/BotWeaponLock.dll -> .../addons/BotWeaponLock/gamedata.json
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

bool BotWeaponLockPlugin::Load(PluginId id, ISmmAPI *ismm,
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
    BotWeaponLock::Dispatch::g_pEngine = static_cast<IVEngineServer2 *>(
        ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!BotWeaponLock::Dispatch::g_pEngine)
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
    BotWeaponLock::Commands::g_pEngine = BotWeaponLock::Dispatch::g_pEngine;

    // Same interface doubles as the "real server.dll" anchor for sig-scan.
    // (serverIface already grabbed above.)

    std::string gamedataPath = ComputeGamedataPath();
    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");
        return false;
    }

    if (!BotWeaponLock::Hooks::Install(gamedataPath, serverIface,
                                       error, maxlen))
        return false;

    OutputDebugStringA("[BotWeaponLock] plugin loaded successfully\n");
    return true;
}

bool BotWeaponLockPlugin::Unload(char * /*error*/, size_t /*maxlen*/)
{
    BotWeaponLock::Hooks::Remove();
    BotWeaponLock::LockState::ClearAll();
    BotWeaponLock::Dispatch::g_pEngine = nullptr;
    BotWeaponLock::Commands::g_pEngine = nullptr;
    ConVar_Unregister();
    g_pCVar = nullptr;
    OutputDebugStringA("[BotWeaponLock] plugin unloaded\n");
    return true;
}
