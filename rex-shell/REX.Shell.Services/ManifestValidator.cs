using System.Text.Json;

namespace REX.Shell.Services;

public class ManifestValidator
{
    public WidgetManifest Validate(string json, string sourcePath)
    {
        var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;

        var errors = new List<string>();

        var manifest = new WidgetManifest();

        if (!TryGetString(root, "schemaVersion", out var sv) || !int.TryParse(sv, out var schemaVersion))
            errors.Add("Missing or invalid required field: schemaVersion");
        else
            manifest = manifest with { SchemaVersion = schemaVersion };

        if (!TryGetString(root, "id", out var id) || string.IsNullOrEmpty(id))
            errors.Add("Missing or empty required field: id");
        else
            manifest = manifest with { Id = id };

        if (!TryGetString(root, "name", out var name) || string.IsNullOrEmpty(name))
            errors.Add("Missing or empty required field: name");
        else
            manifest = manifest with { Name = name };

        if (!TryGetString(root, "version", out var version) || string.IsNullOrEmpty(version))
            errors.Add("Missing or empty required field: version");
        else
            manifest = manifest with { Version = version };

        if (!TryGetString(root, "author", out var author))
            author = "unknown";
        manifest = manifest with { Author = author };

        if (!TryGetString(root, "entry", out var entry) || string.IsNullOrEmpty(entry))
            errors.Add("Missing or empty required field: entry");
        else
            manifest = manifest with { Entry = entry };

        if (root.TryGetProperty("defaultSize", out var ds))
        {
            manifest = manifest with
            {
                DefaultSize = new WidgetSize
                {
                    Width = ds.GetProperty("width").GetInt32(),
                    Height = ds.GetProperty("height").GetInt32()
                }
            };
        }

        if (root.TryGetProperty("minSize", out var ms))
        {
            manifest = manifest with
            {
                MinSize = new WidgetSize
                {
                    Width = ms.GetProperty("width").GetInt32(),
                    Height = ms.GetProperty("height").GetInt32()
                }
            };
        }

        if (root.TryGetProperty("permissions", out var perms))
        {
            var permissions = new WidgetPermissions();
            if (perms.TryGetProperty("network", out var net))
                permissions = permissions with { Network = net.GetBoolean() };
            if (perms.TryGetProperty("dataSources", out var dsList))
                permissions = permissions with { DataSources = dsList.EnumerateArray().Select(e => e.GetString()!).ToList() };
            if (perms.TryGetProperty("storage", out var stor))
                permissions = permissions with { Storage = stor.GetBoolean() };
            manifest = manifest with { Permissions = permissions };
        }

        if (root.TryGetProperty("dataBindings", out var bindings))
        {
            manifest = manifest with
            {
                DataBindings = bindings.EnumerateArray().Select(e => e.GetString()!).ToList()
            };
        }

        if (errors.Count > 0)
            throw new ManifestValidationException(string.Join("; ", errors), errors, sourcePath);

        return manifest;
    }

    private static bool TryGetString(JsonElement el, string key, out string value)
    {
        if (el.TryGetProperty(key, out var prop) && prop.ValueKind == JsonValueKind.String)
        {
            value = prop.GetString()!;
            return true;
        }
        value = "";
        return false;
    }
}

public class ManifestValidationException : Exception
{
    public List<string> Errors { get; }
    public string SourcePath { get; }

    public ManifestValidationException(string message, List<string> errors, string sourcePath)
        : base(message)
    {
        Errors = errors;
        SourcePath = sourcePath;
    }
}
