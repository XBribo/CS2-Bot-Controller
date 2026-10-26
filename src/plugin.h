// Metamod plugin lifecycle and metadata declarations.
#pragma once

#include <ISmmPlugin.h>

namespace cs2bc {

class BotControllerPlugin : public ISmmPlugin
{
  public:
    // Loads features with rollback on failure.
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    // Drains hooks, then releases plugin state.
    bool Unload(char* error, size_t maxlen) override;
    // Handles pause requests.
    bool Pause(char* error, size_t maxlen) override;
    // Handles resume requests.
    bool Unpause(char* error, size_t maxlen) override;
    // Handles plugin-load completion.
    void AllPluginsLoaded() override;

    // Plugin author.
    const char* GetAuthor() override;
    // Plugin name.
    const char* GetName() override;
    // Plugin description.
    const char* GetDescription() override;
    // Project URL.
    const char* GetURL() override;
    // Plugin license.
    const char* GetLicense() override;
    // Build version.
    const char* GetVersion() override;
    // Build timestamp.
    const char* GetDate() override;
    // Log tag.
    const char* GetLogTag() override;

  private:
    bool m_convarsRegistered = false;
};

extern BotControllerPlugin g_plugin;

} // namespace cs2bc

PLUGIN_GLOBALVARS();
