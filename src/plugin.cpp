// BotController native Metamod:Source plugin entry point.

#include "plugin.h"

#include <cstdio>
#include <string>

#include <eiface.h>
#include <icvar.h>
#include <convar.h>
#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>
#include <tier0/dbg.h>

#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)

#include "ISmmPluginExt.h"

#include "VoiceSender.h"
#include "dispatch.h"
#include "commands.h"
#include "sig_scan.h"
#include "schema_resolver.h"
#include "platform.h"
#include "ProjectileBirthAlign.h"
#include "version_targets.h"
#include "runtime.h"

#define VERSION_STRING  "v" SEMVER " @ " GITHUB_SHA
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

PLUGIN_EXPOSE(cs2bc::BotControllerPlugin, cs2bc::g_plugin);

namespace cs2bc {

BotControllerPlugin g_plugin;

namespace {

// Tracks initialization which belongs to the plugin core rather than
// the runtime hook layer.
//
// These flags allow Load() failure paths and Unload() to share exactly
// the same cleanup code.
bool g_schemaInitialized = false;
bool g_convarsRegistered = false;
bool g_coreReady = false;

// addons/<name>/bin/<platform>/<lib>
// -> up 3 dirs
// -> addons/<name>/gamedata.json
std::string ComputeGamedataPath()
{
    std::string p = cs2bc::SelfModulePath();

    if (p.empty()) return "";

    for (int i = 0; i < 3; ++i)
    {
        const size_t slash = p.find_last_of("/\\");

        if (slash == std::string::npos) return "";

        p.resize(slash);
    }

    return p + "/gamedata.json";
}

// -----------------------------------------------------------------------------
// ShutdownCore
//
// Full plugin-core cleanup.
//
// This is used both by MetaMod Unload() and by Load() failure paths.
//
// runtime::Shutdown() is deliberately called first so all callbacks/hooks are
// gone before engine interfaces and schema state become unavailable.
// -----------------------------------------------------------------------------

void ShutdownCore()
{
    g_coreReady = false;

    // Remove every runtime hook and forget prepared runtime data.
    cs2bc::runtime::Shutdown();

    // Clear interfaces used by runtime/API helper code.
    cs2bc::dispatch::g_gameClients = nullptr;
    cs2bc::dispatch::g_engine = nullptr;

    cs2bc::voice_sender::SetInterfaces(nullptr, nullptr);

    cs2bc::commands::g_engine = nullptr;

    // Schema state must only be reset if initialization succeeded.
    if (g_schemaInitialized)
    {
        cs2bc::schema::Reset();
        g_schemaInitialized = false;
    }

    // Likewise do not unregister ConVars unless they were registered.
    if (g_convarsRegistered)
    {
        ConVar_Unregister();
        g_convarsRegistered = false;
    }

    g_pCVar = nullptr;
}

} // namespace

// -----------------------------------------------------------------------------
// Load
// -----------------------------------------------------------------------------

bool BotControllerPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool /*late*/)
{
    PLUGIN_SAVEVARS();

    // Start from a known state.
    g_coreReady = false;

    // ---------------------------------------------------------------------
    // KHook support
    // ---------------------------------------------------------------------

    if (!KHook::__exported__khook)
    {
        std::snprintf(error, maxlen, "Metamod with KHook support is required");

        return false;
    }

    // ---------------------------------------------------------------------
    // ICvar
    // ---------------------------------------------------------------------

    g_pCVar = static_cast<ICvar*>(ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));

    if (!g_pCVar)
    {
        std::snprintf(error, maxlen, "Failed to get ICvar (%s) via engine factory", CVAR_INTERFACE_VERSION);

        ShutdownCore();
        return false;
    }

    // ---------------------------------------------------------------------
    // Schema
    // ---------------------------------------------------------------------

    char schemaError[256] = { 0 };

    if (!cs2bc::schema::Init(schemaError, sizeof(schemaError)))
    {
        std::snprintf(error, maxlen, "Schema initialization failed: %s", schemaError);

        ShutdownCore();
        return false;
    }

    g_schemaInitialized = true;

    if (!cs2bc::targets::LoadFromSchema(schemaError, sizeof(schemaError)))
    {
        std::snprintf(error, maxlen, "Schema target resolution failed: %s", schemaError);

        ShutdownCore();
        return false;
    }

    // Projectile alignment offsets are static core configuration.
    //
    // They remain configured while the runtime hook layer is disabled.
    if (cs2bc::projectile_birth_align::ConfigureOffsets(cs2bc::targets::g_projectileInitialPosition,
                                                        cs2bc::targets::g_projectileInitialVelocity) != 0)
    {
        Warning("[BotController] projectile birth alignment offsets unavailable\n");
    }

    // ---------------------------------------------------------------------
    // ConVars / console commands
    // ---------------------------------------------------------------------

    ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);

    g_convarsRegistered = true;

    // ---------------------------------------------------------------------
    // IVEngineServer2
    // ---------------------------------------------------------------------

    cs2bc::dispatch::g_engine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));

    if (!cs2bc::dispatch::g_engine)
    {
        std::snprintf(error, maxlen, "Failed to get IVEngineServer2 (%s)", INTERFACEVERSION_VENGINESERVER);

        ShutdownCore();
        return false;
    }

    // Console command output uses the same engine interface.
    cs2bc::commands::g_engine = cs2bc::dispatch::g_engine;

    // ---------------------------------------------------------------------
    // ISource2GameClients
    //
    // Used both by the controller and as an anchor for locating server.dll /
    // libserver.so for signature scanning.
    // ---------------------------------------------------------------------

    void* serverIface = ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMECLIENTS, nullptr);

    if (!serverIface)
    {
        std::snprintf(error, maxlen, "Failed to get ISource2GameClients (%s)", INTERFACEVERSION_SERVERGAMECLIENTS);

        ShutdownCore();
        return false;
    }

    cs2bc::dispatch::g_gameClients = static_cast<ISource2GameClients*>(serverIface);

    // ---------------------------------------------------------------------
    // INetworkMessages
    //
    // Optional. BotController can continue to work without voice sending.
    // ---------------------------------------------------------------------

    auto* networkMessages = static_cast<INetworkMessages*>(ismm->GetEngineFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));

    if (!networkMessages)
    {
        networkMessages = static_cast<INetworkMessages*>(ismm->GetServerFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    }

    cs2bc::voice_sender::SetInterfaces(cs2bc::dispatch::g_engine, networkMessages);

    if (!networkMessages)
    {
        Warning("[BotController] network messages interface unavailable; "
                "voice send disabled\n");
    }

    // ---------------------------------------------------------------------
    // gamedata.json
    // ---------------------------------------------------------------------

    const std::string gamedataPath = ComputeGamedataPath();

    if (gamedataPath.empty())
    {
        std::snprintf(error, maxlen, "Failed to compute gamedata.json path");

        ShutdownCore();
        return false;
    }

    nlohmann::json gamedata;

    if (!cs2bc::sig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        std::snprintf(error, maxlen, "Failed to load gamedata: %s", gamedataPath.c_str());

        ShutdownCore();
        return false;
    }

    // ---------------------------------------------------------------------
    // Locate server module
    // ---------------------------------------------------------------------

    cs2bc::sig::ModuleInfo serverModule = cs2bc::sig::ModuleFromInterfacePtr(serverIface);

    if (!serverModule)
    {
        std::snprintf(error, maxlen, "ModuleFromInterfacePtr returned null");

        ShutdownCore();
        return false;
    }

    // ---------------------------------------------------------------------
    // Resolve non-Schema configuration
    // ---------------------------------------------------------------------

    cs2bc::targets::LoadFromGamedata(gamedata);

    // ---------------------------------------------------------------------
    // Prepare runtime
    //
    // This copies gamedata + serverModule into the runtime controller.
    //
    // No hooks are installed by Prepare().
    // ---------------------------------------------------------------------

    if (!cs2bc::runtime::Prepare(gamedata, serverModule, error, maxlen))
    {
        ShutdownCore();
        return false;
    }

    g_coreReady = true;

    // ---------------------------------------------------------------------
    // Enable runtime
    //
    // For now we preserve the original BotController behaviour:
    // loading the MetaMod plugin immediately enables its hooks.
    //
    // Later the CSS provider can explicitly call runtime::SetEnabled(false)
    // when no bots are required.
    // ---------------------------------------------------------------------

    if (!cs2bc::runtime::Enable(error, maxlen))
    {
        ShutdownCore();
        return false;
    }

    Msg("[BotController] Core loaded; runtime enabled\n");

    return true;
}

// -----------------------------------------------------------------------------
// Pause
//
// MetaMod pause now performs a real runtime pause.
//
// The plugin DLL, interfaces, schema, gamedata, exported C ABI and commands
// remain loaded, but all BotController runtime hooks are removed.
// -----------------------------------------------------------------------------

bool BotControllerPlugin::Pause(char* error, size_t maxlen)
{
    if (!g_coreReady || !cs2bc::runtime::IsPrepared())
    {
        if (error && maxlen > 0)
        {
            std::snprintf(error, maxlen, "BotController core is not initialized");
        }

        return false;
    }

    // Idempotent.
    if (!cs2bc::runtime::IsEnabled()) return true;

    cs2bc::runtime::Disable();

    Msg("[BotController] Runtime paused\n");

    return true;
}

// -----------------------------------------------------------------------------
// Unpause
//
// Reinstalls the runtime hooks using the gamedata/module information retained
// by runtime::Prepare().
// -----------------------------------------------------------------------------

bool BotControllerPlugin::Unpause(char* error, size_t maxlen)
{
    if (!g_coreReady || !cs2bc::runtime::IsPrepared())
    {
        if (error && maxlen > 0)
        {
            std::snprintf(error, maxlen, "BotController core is not initialized");
        }

        return false;
    }

    // Idempotent.
    if (cs2bc::runtime::IsEnabled()) return true;

    if (!cs2bc::runtime::Enable(error, maxlen))
    {
        return false;
    }

    Msg("[BotController] Runtime resumed\n");

    return true;
}

// -----------------------------------------------------------------------------
// AllPluginsLoaded
// -----------------------------------------------------------------------------

void BotControllerPlugin::AllPluginsLoaded()
{
    // No additional cross-plugin initialization is currently required.
}

// -----------------------------------------------------------------------------
// Plugin metadata
// -----------------------------------------------------------------------------

const char* BotControllerPlugin::GetAuthor() { return "XBribo(๑•.•๑)"; }

const char* BotControllerPlugin::GetName() { return "BotController"; }

const char* BotControllerPlugin::GetDescription() { return "Record & Replay and Control CS2 bots."; }

const char* BotControllerPlugin::GetURL() { return ""; }

const char* BotControllerPlugin::GetLicense() { return "AGPL-3.0"; }

const char* BotControllerPlugin::GetVersion() { return VERSION_STRING; }

const char* BotControllerPlugin::GetDate() { return BUILD_TIMESTAMP; }

const char* BotControllerPlugin::GetLogTag() { return "BC"; }

// -----------------------------------------------------------------------------
// Unload
// -----------------------------------------------------------------------------

bool BotControllerPlugin::Unload(char* /*error*/, size_t /*maxlen*/)
{
    ShutdownCore();

    Msg("[BotController] Unloaded\n");

    return true;
}

} // namespace cs2bc
