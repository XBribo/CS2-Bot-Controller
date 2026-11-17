namespace BotControllerApi;

public static class BotControllerProvider
{
    private static IBotControllerApi? _api;
    private static bool _registered;

    public static void Attach(IBotControllerApi api, Action<Func<IBotControllerApi>> register)
    {
        if (!_registered)
        {
            register(() => _api!);
            _registered = true;
        }
        _api = api;
    }

    public static void Detach(IBotControllerApi? api)
    {
        if (ReferenceEquals(_api, api)) _api = null;
    }
}
