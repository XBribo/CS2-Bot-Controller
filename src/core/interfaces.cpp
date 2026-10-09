#include "core/interfaces.h"
#include "core/log.h"
#include <ISmmPlugin.h>
#include "dispatch.h"
#include "commands.h"
#include "VoiceSender.h"
#include <icvar.h>
#include <eiface.h>
#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>
#include <schemasystem/schemasystem.h>
#include <cstdio>

ISchemaSystem* g_schemaSystem = nullptr;

namespace cs2bc::interfaces {
namespace {
void* g_serverInterface = nullptr;
}
// Acquires engine services without registering commands or installing hooks.
bool Init(SourceMM::ISmmAPI* ismm, char* error, size_t maxlen)
{
    g_pCVar = static_cast<ICvar*>(ismm->GetEngineFactory()(CVAR_INTERFACE_VERSION, nullptr));
    if (!g_pCVar)
    {
        std::snprintf(error, maxlen, "Failed to get ICvar (%s) via engine factory", CVAR_INTERFACE_VERSION);
        return false;
    }

    // IVEngineServer2::ClientCommand
    cs2bc::dispatch::g_engine = static_cast<IVEngineServer2*>(ismm->GetEngineFactory()(INTERFACEVERSION_VENGINESERVER, nullptr));
    if (!cs2bc::dispatch::g_engine)
    {
        std::snprintf(error, maxlen, "Failed to get IVEngineServer2 (%s)", INTERFACEVERSION_VENGINESERVER);
        return false;
    }

    // ISource2GameClients is used by dispatch and command paths.
    g_serverInterface = ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMECLIENTS, nullptr);
    if (!g_serverInterface)
    {
        std::snprintf(error, maxlen, "Failed to get ISource2GameClients (%s)", INTERFACEVERSION_SERVERGAMECLIENTS);
        return false;
    }

    GET_V_IFACE_ANY(GetEngineFactory, g_schemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);

    // Engine interface used by console command output (ClientPrintf).
    cs2bc::commands::g_engine = cs2bc::dispatch::g_engine;

    // Server-side command executor for issuing bot "buy" commands.
    cs2bc::dispatch::g_gameClients = static_cast<ISource2GameClients*>(g_serverInterface);

    // NetworkMessages lets the C ABI send recorded voice frames to clients.
    auto* networkMessages = static_cast<INetworkMessages*>(ismm->GetEngineFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    if (!networkMessages)
    {
        networkMessages = static_cast<INetworkMessages*>(ismm->GetServerFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    }
    cs2bc::voice_sender::SetInterfaces(cs2bc::dispatch::g_engine, networkMessages);
    if (!networkMessages)
    {
        BC_LOG_WARN("network messages interface unavailable; voice send disabled\n");
    }

    return true;
}
// CS2 simulates at 64 ticks per second, matching CounterStrikeSharp's interval.
float TickInterval() { return 1.0F / 64.0F; }

// Releases interface consumers after native callbacks have been removed.
void Reset()
{
    cs2bc::dispatch::g_engine = nullptr;
    cs2bc::dispatch::g_gameClients = nullptr;
    cs2bc::voice_sender::SetInterfaces(nullptr, nullptr);
    cs2bc::commands::g_engine = nullptr;
    g_serverInterface = nullptr;
    g_schemaSystem = nullptr;
}
} // namespace cs2bc::interfaces
