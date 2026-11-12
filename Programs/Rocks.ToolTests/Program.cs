using System.Text;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        Console.InputEncoding = new UTF8Encoding(false);
        try
        {
            if (args.Length == 0) throw new ArgumentException("Expected self, native, meta, policy, format or scaffold");
            if (args[0] == "child") return await ChildAsync(args[1..]);
            if (args[0] == "scaffold") return Scaffold(args[1..]);
            Options options = Options.Parse(args[1..]);
            string root = options.Root;
            if (!Directory.Exists(root)) throw new DirectoryNotFoundException(root);
            TestSuite suite = new();
            switch (args[0])
            {
                case "self": SelfTests.Register(suite, options); break;
                case "native":
                    NativeTests.Register(suite, options, await NativeEnvironment.CreateAsync(options.Optional("compiler")));
                    break;
                case "meta":
                    MetaTests.Register(suite, options, await NativeEnvironment.CreateAsync(options.Required("compiler")));
                    break;
                case "policy":
                    var failures = PolicyChecks.Runtime(root).Concat(PolicyChecks.PlatformBoundary(root))
                        .Concat(PolicyChecks.SourceWarnings(root)).Concat(PolicyChecks.OptimizationOverrides(root))
                        .Concat(PolicyChecks.PythonFreeMainPath(root)).ToList();
                    if (options.Optional("compile-commands") is string database) failures.AddRange(PolicyChecks.CompilationDatabase(database));
                    foreach (var failure in failures) Console.Error.WriteLine(failure);
                    return failures.Count == 0 ? 0 : 1;
                case "format":
                    await FormatCheck.CheckAsync(root, options.Optional("formatter") ??
                        Environment.GetEnvironmentVariable("ROCKS_CLANG_FORMAT") ?? "clang-format");
                    return 0;
                default: throw new ArgumentException("Unknown command: " + args[0]);
            }
            return await suite.RunAsync(options.Json, options.Seed);
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        { Console.Error.WriteLine(error.Message); return 2; }
    }

    private static int Scaffold(string[] args)
    {
        if (args.Length == 0) throw new ArgumentException("Expected scaffold plan/apply/validate");
        Dictionary<string, string> values = new(StringComparer.Ordinal);
        for (int index = 1; index < args.Length; index += 2)
            if (index + 1 >= args.Length || !args[index].StartsWith("--", StringComparison.Ordinal) ||
                !values.TryAdd(args[index][2..], args[index + 1])) throw new ArgumentException("Invalid scaffold option");
        string Get(string key) => values.TryGetValue(key, out string? value) ? value : throw new ArgumentException("Missing --" + key);
        switch (args[0])
        {
            case "plan":
                if (values.Keys.Any(key => key is not ("root" or "name" or "output" or "plan"))) throw new ArgumentException("Unknown plan option");
                ScaffoldService.PlanModule(Get("root"), Get("name"), Get("output"), Get("plan")); break;
            case "apply":
                if (values.Count != 1) throw new ArgumentException("Expected only --plan");
                ScaffoldService.Apply(Get("plan")); break;
            case "validate":
                if (values.Count != 1) throw new ArgumentException("Expected only --plan");
                ScaffoldService.ValidateOutput(Get("plan")); break;
            default: throw new ArgumentException("Unknown scaffold operation");
        }
        return 0;
    }

    // 도구 자체 실행/timeout/pipe 음성대조 전용 child다. shell과 Python을 사용하지 않는다.
    private static async Task<int> ChildAsync(string[] args)
    {
        int? hard = await ProcessHardTimeoutReviewTests.TryRunChildAsync(args);
        if (hard.HasValue) return hard.Value;
        int? reviewed = await ProcessReviewTests.TryRunChildAsync(args);
        if (reviewed.HasValue) return reviewed.Value;
        if (args.SequenceEqual(new[] { "--hang" })) { Console.WriteLine("ready"); await Task.Delay(Timeout.InfiniteTimeSpan); }
        if (args.SequenceEqual(new[] { "--echo-input" })) { Console.Write(await Console.In.ReadToEndAsync()); return 0; }
        if (args.SequenceEqual(new[] { "--large-output" }))
        {
            string text = new('x', 256 * 1024);
            Console.Out.Write("출력🎮" + text); Console.Error.Write("오류🎮" + text); return 17;
        }
        if (args.Length == 2 && args[0] == "--exit-code") return int.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture);
        throw new ArgumentException("Unknown internal child control");
    }
}
