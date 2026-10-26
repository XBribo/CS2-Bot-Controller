#include "core/interfaces.h"
#include "core/log.h"
#include <ISmmPlugin.h>
#include "dispatch.h"
#include "commands.h"
#include "VoiceSender.h"
#include <icvar.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>
#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>
#include <schemasystem/schemasystem.h>
#include <cstdio>

ISchemaSystem* g_schemaSystem = nullptr;

namespace cs2bc::interfaces {
namespace {
void* g_serverInterface = nullptr;
}
// Acquires engine interfaces.
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

    // Server command interface.
    g_serverInterface = ismm->GetServerFactory()(INTERFACEVERSION_SERVERGAMECLIENTS, nullptr);
    if (!g_serverInterface)
    {
        std::snprintf(error, maxlen, "Failed to get ISource2GameClients (%s)", INTERFACEVERSION_SERVERGAMECLIENTS);
        return false;
    }

    GET_V_IFACE_ANY(GetEngineFactory, g_schemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);

    // Console output interface.
    cs2bc::commands::g_engine = cs2bc::dispatch::g_engine;

    // Bot buy command executor.
    cs2bc::dispatch::g_gameClients = static_cast<ISource2GameClients*>(g_serverInterface);

    // Voice message interface.
    auto* networkMessages = static_cast<INetworkMessages*>(ismm->GetEngineFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    if (!networkMessages)
    {
        networkMessages = static_cast<INetworkMessages*>(ismm->GetServerFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
    }
    cs2bc::voice_sender::SetInterfaces(cs2bc::dispatch::g_engine, networkMessages);
    if (!networkMessages)
    {
        BC_LOG_WARN("voice disabled: network messages unavailable");
    }

    return true;
}
// Releases interfaces after hooks are removed.
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
