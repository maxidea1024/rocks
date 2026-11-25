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
            if (arguments.Length != 0 && arguments[0] == "ids")
            {
                return IdentityCommands.Run(arguments[1..]);
            }

            CompilerOptions options = CompilerOptions.Parse(ExpandResponseFile(arguments));
            input = options.Inputs[0].Path;
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

    // 인자가 "@<경로>" 하나면 그 파일의 줄(한 줄에 인자 하나, UTF-8, 빈 줄 무시)을 인자로 쓴다. 빌드 도구가 헤더가 많은
    // 모듈의 인자를 이렇게 넘긴다(Windows 명령줄 상한 — Docs/Design/ReflectionGeneratedSources.ko.md 6.3절).
    private static string[] ExpandResponseFile(string[] arguments)
    {
        if (arguments.Length != 1 || !arguments[0].StartsWith('@'))
        {
            return arguments;
        }

        return File.ReadAllLines(arguments[0][1..], new UTF8Encoding(false))
            .Where(line => line.Length != 0)
            .ToArray();
    }
}
