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

            // 셰이더 파라미터 구조체 생성 모드다(Docs/Design/ShaderSystem 16.2절 SSD49).
            if (arguments.Length != 0 && arguments[0] == "shader-parameters")
            {
                return await ShaderParameterGenerator.RunAsync(arguments[1..]);
            }

            CompilerOptions options = CompilerOptions.Parse(ExpandResponseFile(arguments));
            input = options.Inputs[0].Path;
            MetaRunRecorder recorder = MetaRunRecorder.Begin();
            try
            {
                await CompilerDriver.RunAsync(options);
                // 의존 기록은 성공한 실행만 낸다(Docs/Design/BuildReuse 2.4절 — 실패한 실행의 생성물은 게시되지 않는다).
                if (options.DependencyRecord is not null)
                {
                    recorder.WriteDependencyRecord(options.DependencyRecord);
                }
            }
            finally
            {
                if (options.PhaseReport is not null)
                {
                    recorder.WritePhaseReport(options.PhaseReport);
                }
            }

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
