using System.Text;

namespace Rocks.MetaCompiler;

internal static class Program
{
    public static async Task<int> Main(string[] arguments)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        Console.InputEncoding = new UTF8Encoding(false);
        string input = "<command-line>";
        try
        {
            if (arguments.Length != 0 && arguments[0] == "ids") return IdentityCommands.Run(arguments[1..]);
            CompilerOptions options = CompilerOptions.Parse(arguments);
            input = Path.GetFullPath(options.Input);
            await CompilerDriver.RunAsync(options);
            return 0;
        }
        catch (MetadataException error)
        {
            SourceSite site = error.Site ?? new(input, 1);
            Console.Error.WriteLine($"{site.File}:{site.Line}: error {error.Code}: {error.Message}");
            Console.Error.WriteLine("hint: " + error.Hint);
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or TimeoutException or System.ComponentModel.Win32Exception or InvalidOperationException or System.Text.Json.JsonException)
        {
            Console.Error.WriteLine($"{input}:1: error RMC1000: {error.Message}");
            Console.Error.WriteLine("hint: Check input/output permissions, explicit options and the native toolchain environment; no output is accepted on failure.");
        }
        return 1;
    }
}
