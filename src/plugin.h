// Metamod plugin lifecycle and metadata declarations.
#pragma once

#include <ISmmPlugin.h>

namespace cs2bc {

class BotControllerPlugin : public ISmmPlugin
{
  public:
    // Initializes the plugin core and enables runtime hooks.
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;

    // Disables runtime hooks and fully releases plugin core state.
    bool Unload(char* error, size_t maxlen) override;

    // Disables runtime hooks while keeping the plugin core loaded.
    bool Pause(char* error, size_t maxlen) override;

    // Re-enables runtime hooks after a pause.
    bool Unpause(char* error, size_t maxlen) override;

    // Handles completion of Metamod plugin loading.
    void AllPluginsLoaded() override;

    // Plugin metadata.
    const char* GetAuthor() override;
    const char* GetName() override;
    const char* GetDescription() override;
    const char* GetURL() override;
    const char* GetLicense() override;
    const char* GetVersion() override;
    const char* GetDate() override;
    const char* GetLogTag() override;
};

extern BotControllerPlugin g_plugin;

} // namespace cs2bc

PLUGIN_GLOBALVARS();
