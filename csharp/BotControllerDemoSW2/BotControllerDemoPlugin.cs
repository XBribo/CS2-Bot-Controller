// Demo consumer plugin — verifies BotController cross-plugin API via IInterfaceManager.
// Command: !bc_test  →  lock bot slot, then unlock after 3 seconds.

using Microsoft.Extensions.Logging;
using BotControllerApi;
using SwiftlyS2.Shared;
using SwiftlyS2.Shared.Commands;
using SwiftlyS2.Shared.Players;
using SwiftlyS2.Shared.Plugins;

namespace BotControllerDemoSW2;

[PluginMetadata(
    Id = "botcontroller.demo",
    Version = "1.0.0",
    Name = "BotController Demo",
    Author = "XBribo",
    Description = "Verifies BotController cross-plugin API via IInterfaceManager."
)]
public class BotControllerDemoPlugin(ISwiftlyCore core) : BasePlugin(core)
{
    private IBotControllerApi? _api;

    public override void UseSharedInterface(IInterfaceManager interfaceManager)
    {
        if (interfaceManager.TryGetSharedInterface<IBotControllerApi>(
                "botcontroller:api", out var api))
        {
            _api = api;
            Core.Logger.LogInformation("[Demo] BotController API obtained via IInterfaceManager.");
        }
        else
        {
            Core.Logger.LogWarning("[Demo] BotController API not available — is BotControllerImplSW2 loaded?");
        }
    }

    public override void Load(bool hotReload) { }
    public override void Unload() { }

    private static string Tag(string msg) => $"{Helper.ChatColors.Green}[Demo]{Helper.ChatColors.Default} {msg}".Colored();

    [Command("bc_test")]
    public void OnTest(ICommandContext ctx)
    {
        if (_api is null)
        {
            ctx.Reply(Tag("BotController API not available."));
            return;
        }

        if (ctx.Args.Length < 1 || !int.TryParse(ctx.Args[0], out int slot))
        {
            ctx.Reply(Tag("Usage: !bc_test <botSlot>"));
            return;
        }

        // Step 1: Lock the bot
        bool locked = _api.Lock(slot, LockKind.All);
        ctx.Reply(locked
            ? Tag($"Locked bot slot {slot} (All). Unlocking in 3s...")
            : Tag($"Failed to lock bot slot {slot}."));

        if (!locked) return;

        // Step 2: Unlock after 3 seconds
        _ = Task.Run(async () =>
        {
            await Task.Delay(3000);
            Core.Scheduler.NextTick(() =>
            {
                _api.Unlock(slot, LockKind.All);
                Core.Logger.LogInformation("[Demo] Unlocked bot slot {Slot}.", slot);
            });
        });
    }
}
