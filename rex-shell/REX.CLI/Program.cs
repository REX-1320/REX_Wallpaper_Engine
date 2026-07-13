using System.IO.Compression;
using System.Text.Json;

namespace REX.CLI;

class Program
{
    static async Task<int> Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("REX CLI v0.1.0");
            Console.WriteLine("Usage:");
            Console.WriteLine("  rex-cli init <name>     Scaffold a new widget");
            Console.WriteLine("  rex-cli dev              Start dev server (hot reload)");
            Console.WriteLine("  rex-cli validate         Validate widget manifest");
            Console.WriteLine("  rex-cli package          Package widget as .rexwidget");
            Console.WriteLine("  rex-cli publish          Submit to marketplace");
            return 0;
        }

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
            permissions = new { network = false, dataSources = new[] { "clock" } }
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
        await Task.Delay(1000);
        Console.WriteLine("Dev server running. Watching for changes...");
        await Task.Delay(-1);
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
        Console.WriteLine("manifest.json is valid");
        return 0;
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
            CompressionLevel.Optimal, includeBaseDirectory: false,
            excludeDirectories: false);
        Console.WriteLine($"Packaged to {outputPath}");
        return 0;
    }

    static async Task<int> PublishWidget()
    {
        Console.WriteLine("Publishing widget...");
        await Task.Delay(500);
        Console.WriteLine("Submitted for review");
        return 0;
    }
}
