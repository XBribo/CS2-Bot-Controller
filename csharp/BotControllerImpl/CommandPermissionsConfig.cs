using System.Text.Json;

#if BOT_CONTROLLER_SW2
namespace BotControllerImplSW2;
#else
namespace BotControllerImpl;
#endif

internal sealed record CommandPermissionsConfig(bool RequirePermissions = false,
    string Permission = "@botcontroller/admin")
{
    // Loads command permissions.
    internal static CommandPermissionsConfig Load(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (FileNotFoundException) { return new(); }
        catch (DirectoryNotFoundException) { return new(); }

        using var document = JsonDocument.Parse(text);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new JsonException("Expected config object.");
        if (!root.TryGetProperty("commands", out var commands)) return new();
        if (commands.ValueKind != JsonValueKind.Object) throw new JsonException("Expected commands object.");

        bool requirePermissions = false;
        if (commands.TryGetProperty("require_permissions", out var required))
        {
            if (required.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                throw new JsonException("Expected require_permissions boolean.");
            requirePermissions = required.GetBoolean();
        }

        string permission = "@botcontroller/admin";
        if (commands.TryGetProperty("permission", out var configured))
        {
            if (configured.ValueKind != JsonValueKind.String)
                throw new JsonException("Expected permission string.");
            permission = configured.GetString()!;
            int separator = permission.IndexOf('/');
            if (permission.Length < 4 || permission[0] != '@' || separator < 2 ||
                separator == permission.Length - 1 || permission.Any(char.IsWhiteSpace))
                throw new JsonException("Expected permission flag @domain/name.");
        }

        return new(requirePermissions, permission);
    }
}
