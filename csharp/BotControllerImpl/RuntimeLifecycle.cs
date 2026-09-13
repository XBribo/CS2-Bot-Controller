using CounterStrikeSharp.API.Core;

using BotControllerApi;

namespace BotControllerImpl;

public partial class BotControllerPlugin
{
    private bool _nativeCompatible;
    private bool _managedRuntimeEnabled;

    internal bool RuntimeEnabled =>
        _nativeCompatible &&
        _managedRuntimeEnabled &&
        BotController.RuntimeEnabled;

    // Coordinates the managed listeners with the native hook lifecycle.
    internal bool SetRuntimeEnabled(bool enabled)
    {
        if (!_nativeCompatible || !BotController.RuntimePrepared)
            return false;

        if (enabled)
        {
            if (_managedRuntimeEnabled && BotController.RuntimeEnabled)
                return true;

            if (!BotController.SetRuntimeEnabled(true) || !BotController.RuntimeEnabled)
                return false;

            try
            {
                EnableManagedRuntime();
                return true;
            }
            catch
            {
                DisableManagedRuntime();
                BotController.SetRuntimeEnabled(false);
                return false;
            }
        }

        DisableManagedRuntime();

        bool disabled = BotController.SetRuntimeEnabled(false);
        return disabled && !BotController.RuntimeEnabled;
    }

    private void EnableManagedRuntime()
    {
        if (_managedRuntimeEnabled)
            return;

        RegisterListener<Listeners.OnTick>(_driver.Tick);
        RegisterListener<Listeners.OnTick>(ProcessPendingProjectileCandidates);
        RegisterListener<Listeners.OnEntitySpawned>(OnProjectileEntitySpawned);

        RegisterEventHandler<EventSmokegrenadeDetonate>(OnSmokegrenadeDetonate);
        RegisterEventHandler<EventFlashbangDetonate>(OnFlashbangDetonate);
        RegisterEventHandler<EventHegrenadeDetonate>(OnHegrenadeDetonate);
        RegisterEventHandler<EventMolotovDetonate>(OnMolotovDetonate);
        RegisterEventHandler<EventDecoyDetonate>(OnDecoyDetonate);

        _managedRuntimeEnabled = true;
    }

    private void DisableManagedRuntime()
    {
        _managedRuntimeEnabled = false;

        RemoveListener<Listeners.OnEntitySpawned>(OnProjectileEntitySpawned);
        RemoveListener<Listeners.OnTick>(ProcessPendingProjectileCandidates);
        RemoveListener<Listeners.OnTick>(_driver.Tick);

        DeregisterEventHandler<EventSmokegrenadeDetonate>(OnSmokegrenadeDetonate);
        DeregisterEventHandler<EventFlashbangDetonate>(OnFlashbangDetonate);
        DeregisterEventHandler<EventHegrenadeDetonate>(OnHegrenadeDetonate);
        DeregisterEventHandler<EventMolotovDetonate>(OnMolotovDetonate);
        DeregisterEventHandler<EventDecoyDetonate>(OnDecoyDetonate);

        _driver.Clear();
        _recordingFiles.Clear();
        ClearAllProjectileState();
    }
}
