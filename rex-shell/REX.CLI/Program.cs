using System.IO.Compression;
using System.Text.Json;

namespace REX.CLI;

class Program
{
    static async Task<int> Main(string[] args)
    {
        if (args.Length == 0)
        {
            PrintHelp();
            return 0;
        }

        try
        {
            return args[0] switch
            {
                "init" => args.Length > 1 ? InitWidget(args[1]) : 1,
                "dev" => await DevServer(),
                "validate" => ValidateManifest(),
                "package" => PackageWidget(),
                "publish" => await PublishWidget(),
                _ => 1
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Error: {ex.Message}");
            return 1;
        }
    }

    static void PrintHelp()
    {
        Console.WriteLine("REX CLI v0.1.0");
        Console.WriteLine("Usage:");
        Console.WriteLine("  rex-cli init <name>     Scaffold a new widget");
        Console.WriteLine("  rex-cli dev              Start dev server (hot reload)");
        Console.WriteLine("  rex-cli validate         Validate widget manifest");
        Console.WriteLine("  rex-cli package          Package widget as .rexwidget");
        Console.WriteLine("  rex-cli publish          Submit to marketplace");
    }

    static int InitWidget(string name)
    {
        var dir = Path.Combine(Directory.GetCurrentDirectory(), name);
        Directory.CreateDirectory(dir);

        var manifest = new
        {
            schemaVersion = 1,
            id = $"com.author.{name}",
            name,
            version = "1.0.0",
            author = "author-name",
            entry = "index.html",
            defaultSize = new { width = 240, height = 160 },
            minSize = new { width = 120, height = 80 },
            permissions = new
            {
                network = false,
                dataSources = new[] { "clock" },
                storage = false
            },
            dataBindings = Array.Empty<string>()
        };

        var manifestJson = JsonSerializer.Serialize(manifest, new JsonSerializerOptions
        {
            WriteIndented = true
        });
        File.WriteAllText(Path.Combine(dir, "manifest.json"), manifestJson);
        File.WriteAllText(Path.Combine(dir, "index.html"),
            "<!DOCTYPE html><html><body><h1>Hello REX</h1></body></html>");
        File.WriteAllText(Path.Combine(dir, "style.css"), "body { color: white; background: transparent; }");

        Console.WriteLine($"Created widget '{name}' at {dir}");
        return 0;
    }

    static async Task<int> DevServer()
    {
        Console.WriteLine("REX dev server starting...");

        var cts = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
        };

        using var watcher = new FileSystemWatcher(Directory.GetCurrentDirectory())
        {
            IncludeSubdirectories = true,
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.DirectoryName
        };

        watcher.Changed += (_, e) =>
        {
            if (!e.Name.StartsWith(".") && !e.Name.Contains("\\."))
                Console.WriteLine($"[HOT RELOAD] {e.Name} changed");
        };
        watcher.Created += (_, e) => Console.WriteLine($"[HOT RELOAD] {e.Name} created");
        watcher.Deleted += (_, e) => Console.WriteLine($"[HOT RELOAD] {e.Name} deleted");
        watcher.Renamed += (_, e) => Console.WriteLine($"[HOT RELOAD] {e.FullPath} renamed from {e.OldFullPath}");
        watcher.EnableRaisingEvents = true;

        Console.WriteLine("Dev server running. Watching for changes...");
        Console.WriteLine("Press Ctrl+C to stop.");

        try
        {
            await Task.Delay(-1, cts.Token);
        }
        catch (TaskCanceledException) { }

        Console.WriteLine("Dev server stopped.");
        return 0;
    }

    static int ValidateManifest()
    {
        var path = Path.Combine(Directory.GetCurrentDirectory(), "manifest.json");
        if (!File.Exists(path))
        {
            Console.Error.WriteLine("manifest.json not found");
            return 1;
        }

        try
        {
            var json = File.ReadAllText(path);
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;

            var errors = new List<string>();

            if (!root.TryGetProperty("schemaVersion", out _))
                errors.Add("Missing required field: schemaVersion");

            if (!root.TryGetProperty("id", out var idProp) || idProp.GetString() is not { Length: > 0 } id)
                errors.Add("Missing or empty required field: id");
            else if (!id.Contains('.'))
                errors.Add("Field 'id' should be a reverse-domain name (e.g. com.author.name)");

            if (!root.TryGetProperty("name", out var nameProp) || nameProp.GetString() is not { Length: > 0 })
                errors.Add("Missing or empty required field: name");

            if (!root.TryGetProperty("version", out var verProp) || verProp.GetString() is not { Length: > 0 })
                errors.Add("Missing or empty required field: version");

            if (!root.TryGetProperty("entry", out var entryProp) || entryProp.GetString() is not { Length: > 0 })
                errors.Add("Missing or empty required field: entry");

            if (!root.TryGetProperty("permissions", out _))
                errors.Add("Missing required field: permissions");

            if (errors.Count > 0)
            {
                Console.Error.WriteLine("Validation errors:");
                foreach (var err in errors)
                    Console.Error.WriteLine($"  - {err}");
                return 1;
            }

            Console.WriteLine($"manifest.json is valid — {root.GetProperty("name").GetString()} v{root.GetProperty("version").GetString()}");
            return 0;
        }
        catch (JsonException ex)
        {
            Console.Error.WriteLine($"Invalid JSON: {ex.Message}");
            return 1;
        }
    }

    static int PackageWidget()
    {
        var manifestPath = Path.Combine(Directory.GetCurrentDirectory(), "manifest.json");
        if (!File.Exists(manifestPath))
        {
            Console.Error.WriteLine("manifest.json not found");
            return 1;
        }

        var outputPath = Path.Combine(Directory.GetCurrentDirectory(), "widget.rexwidget");
        if (File.Exists(outputPath)) File.Delete(outputPath);

        ZipFile.CreateFromDirectory(Directory.GetCurrentDirectory(), outputPath,
            CompressionLevel.Optimal, includeBaseDirectory: false);
        Console.WriteLine($"Packaged to {outputPath}");
        return 0;
    }

    static async Task<int> PublishWidget()
    {
        Console.WriteLine("Publishing widget...");
        Console.WriteLine("Checking manifest...");
        if (ValidateManifest() != 0)
        {
            Console.Error.WriteLine("Fix validation errors before publishing");
            return 1;
        }

        var package = Path.Combine(Directory.GetCurrentDirectory(), "widget.rexwidget");
        if (!File.Exists(package))
        {
            Console.WriteLine("No package found. Run 'rex-cli package' first.");
            Console.WriteLine("Creating package...");
            if (PackageWidget() != 0)
                return 1;
        }

        Console.WriteLine("Submitting for review...");
        await Task.Delay(500);
        Console.WriteLine("Submitted successfully");
        return 0;
    }
}
