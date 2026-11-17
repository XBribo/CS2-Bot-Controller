// BotController API capability.

using CounterStrikeSharp.API.Core.Capabilities;

namespace BotControllerApi
{
    public static class BotControllerCapability
    {
        // Stable cross-plugin capability key.
        public static readonly PluginCapability<IBotControllerApi> Cap =
            new("botcontroller:api");
    }
}
