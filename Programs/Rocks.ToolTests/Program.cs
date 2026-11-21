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
            if (args.Length == 0)
            {
                throw new ArgumentException("Expected self, native, meta, log-release, policy, format or scaffold");
            }

            if (args[0] == "child")
            {
                return await ChildAsync(args[1..]);
            }

            if (args[0] == "scaffold")
            {
                return Scaffold(args[1..]);
            }

            Options options = Options.Parse(args[1..]);
            string root = options.Root;
            if (!Directory.Exists(root))
            {
                throw new DirectoryNotFoundException(root);
            }

            TestSuite suite = new();
            int parallelism = 1;
            switch (args[0])
            {
                case "self":
                    SelfTests.Register(suite, options);
                    break;
                case "native":
                    NativeTests.Register(suite, options, await NativeEnvironment.CreateAsync(options.Optional("compiler")));
                    break;
                case "objects":
                    // host에서 빌드 오브젝트만 검사한다(native 실행 파일이 필요 없다 — Android 교차 빌드용).
                    NativeTests.RegisterObjectChecks(suite, options, await NativeEnvironment.CreateAsync(options.Optional("compiler")),
                        options.Required("compile-commands"));
                    break;
                case "log-release":
                    // Release LogBench 실행 파일(--native-executable)의 로그 문자열 제외 검사다(LoggingSystem 7절).
                    LogReleaseTests.Register(suite, options);
                    break;
                case "meta":
                    // meta 시험은 저마다 임시 디렉터리에서 MetaCompiler·Clang 프로세스를 띄우므로 서로 독립이다.
                    // 직렬로는 CI 러너에서 단계 제한(10분)을 넘으므로 CPU 수만큼 함께 돌린다.
                    MetaTests.Register(suite, options, await NativeEnvironment.CreateAsync(options.Required("compiler")));
                    parallelism = options.Jobs(Math.Max(1, Environment.ProcessorCount));
                    break;
                case "policy":
                    var failures = PolicyChecks.Runtime(root).Concat(PolicyChecks.ReflectionAnnotations(root)).Concat(PolicyChecks.ModuleGeneratedHeaderIncludes(root)).Concat(PolicyChecks.PlatformBoundary(root)).Concat(PolicyChecks.PlacementNewHeaders(root)).Concat(PolicyChecks.PublicHeaderStaticStorage(root)).Concat(PolicyChecks.TargetMacros(root))
                        .Concat(PolicyChecks.TargetIntrinsics(root)).Concat(PolicyChecks.TestAccess(root))
                        .Concat(PolicyChecks.SourceWarnings(root)).Concat(PolicyChecks.OptimizationOverrides(root))
                        .Concat(PolicyChecks.CompilerBuiltins(root)).Concat(PolicyChecks.RawLimitLiterals(root)).Concat(PolicyChecks.InitializerListDeductions(root)).Concat(PolicyChecks.CVarRegistrations(root)).Concat(PolicyChecks.CompileTimeMessages(root)).Concat(PolicyChecks.EnglishSourceLiterals(root)).Concat(PolicyChecks.EnglishBuildFiles(root)).Concat(PolicyChecks.DeterministicMathSources(root)).Concat(PolicyChecks.CrtNumberConversions(root)).Concat(UnicodeTables.Check(root)).Concat(Json5UnicodeTablesGenerator.Check(root)).Concat(MathTables.Check(root)).Concat(NumberTables.Check(root))
                        .Concat(PolicyChecks.PythonFreeMainPath(root)).Concat(PolicyChecks.ThirdParty(root)).Concat(PolicyChecks.DerivedSources(root)).Concat(PolicyChecks.ForeignEngineBoundary(root)).Concat(PolicyChecks.PluginModuleRules(root))
                        .Concat(PolicyChecks.ShaderToolReferences(root)).Concat(PolicyChecks.ProcessEntryPoints(root)).ToList();
                    // Release 배제(셰이더 시스템 9절)는 규칙 어셈블리를 평가한다. 알려진 예외는 실패가 아니지만 조용히 넘기지 않고 출력한다.
                    string rulesAssembly = options.Optional("rules") ?? ReleaseExclusion.DefaultRulesAssembly(root);
                    ReleaseExclusionResult release = ReleaseExclusion.Check(root, rulesAssembly);
                    failures.AddRange(release.Failures);
                    // 유니티 묶음 충돌(Docs/Design/UnityBuildPch.ko.md 9.2절)도 규칙 어셈블리를 평가해 유니티를 켠 모듈만 본다.
                    failures.AddRange(PolicyChecks.UnityCollisions(root, rulesAssembly));
                    // PCH 헤더는 빌드가 강제 include한다 — 소스와 헤더가 직접 include하지 않는다(같은 문서 8.1·10절).
                    failures.AddRange(PolicyChecks.PchHeaderIncludes(root, rulesAssembly));
                    // Windows SDK 헤더는 Windows 플랫폼의 모든 모듈에서 감싸서 include한다(같은 문서 6.1절 "매크로를 남기는 헤더").
                    failures.AddRange(PolicyChecks.WindowsMacroLeaks(root, rulesAssembly));
                    foreach (string notice in release.Notices)
                    {
                        Console.WriteLine("note: " + notice);
                    }

                    if (options.Optional("compile-commands") is string database)
                    {
                        failures.AddRange(PolicyChecks.CompilationDatabase(database));
                    }

                    foreach (var failure in failures)
                    {
                        Console.Error.WriteLine(failure);
                    }

                    return failures.Count == 0 ? 0 : 1;
                case "unity-audit":
                    // 유니티 묶음 충돌을 모듈의 Unity 설정과 무관하게 모든 엔진 모듈(외부 라이브러리 제외)에 대해 센다
                    // (Docs/Design/UnityBuildPch.ko.md 9.3절 — 어느 모듈을 정리해 켤 수 있는지 본다). 검사가 아니라 조사라서 종료 코드는 0이다.
                    var audit = PolicyChecks.UnityCollisions(root, options.Optional("rules") ?? ReleaseExclusion.DefaultRulesAssembly(root),
                        module => !module.ThirdParty);
                    foreach (var failure in audit)
                    {
                        Console.WriteLine(failure);
                    }

                    Console.WriteLine($"{audit.Count} unity collision(s)");
                    return 0;
                case "third-party":
                    // 외부 라이브러리 업그레이드: manifest의 files·pragmaDirectives를 원본에서 다시 계산한다.
                    // 기본은 차이만 보고(종료 코드 1), --write면 고친다. 새 지시문은 3.6 규칙으로 검토한다.
                    bool write = options.Write;
                    var refreshed = ThirdPartyManifestRefresh.Run(root, write);
                    foreach (string path in refreshed)
                    {
                        Console.WriteLine((write ? "updated " : "stale ") + path);
                    }

                    return write || refreshed.Count == 0 ? 0 : 1;
                case "unicode-tables":
                    // UCD 원본에서 Core의 Unicode 표(UnicodeTables.OutputPath)를 다시 만든다. 기본은 차이만 보고하고
                    // (종료 코드 1), --write면 고친다.
                    bool writeTables = options.Write;
                    bool tablesChanged = UnicodeTables.Run(root, writeTables);
                    if (tablesChanged)
                    {
                        Console.WriteLine((writeTables ? "updated " : "stale ") + UnicodeTables.OutputPath);
                    }

                    return writeTables || !tablesChanged ? 0 : 1;
                case "json5-unicode-tables":
                    // UCD 원본에서 C# JSON5 reader의 General_Category 구간 표(Json5UnicodeTablesGenerator.OutputPath)를 다시 만든다(기본은
                    // 차이만 보고, --write면 고친다).
                    bool writeJson5 = options.Write;
                    bool json5Changed = Json5UnicodeTablesGenerator.Run(root, writeJson5);
                    if (json5Changed)
                    {
                        Console.WriteLine((writeJson5 ? "updated " : "stale ") + Json5UnicodeTablesGenerator.OutputPath);
                    }

                    return writeJson5 || !json5Changed ? 0 : 1;
                case "math-tables":
                    // 결정적 수학 모듈의 8비트 sRGB 표와 정확도 기준값 표를 정확한 정수 계산으로 다시 만든다(기본은
                    // 차이만 보고, --write면 고친다).
                    bool writeMath = options.Write;
                    var mathChanged = MathTables.Run(root, writeMath);
                    foreach (string path in mathChanged)
                    {
                        Console.WriteLine((writeMath ? "updated " : "stale ") + path);
                    }

                    return writeMath || mathChanged.Count == 0 ? 0 : 1;
                case "number-tables":
                    // Core 10진 ↔ 2진 변환의 5의 거듭제곱 표를 BigInteger로 다시 만든다(기본은 차이만 보고, --write면 고친다).
                    bool writeNumbers = options.Write;
                    bool numbersChanged = NumberTables.Run(root, writeNumbers);
                    if (numbersChanged)
                    {
                        Console.WriteLine((writeNumbers ? "updated " : "stale ") + NumberTables.OutputPath);
                    }

                    return writeNumbers || !numbersChanged ? 0 : 1;
                case "format":
                    // C++은 clang-format 21.1.6(--formatter, 없으면 ROCKS_CLANG_FORMAT), C#은 dotnet format(--dotnet, 작업 영역
                    // FormatCheck.CSharpWorkspace)으로 검사한다. 기본은 차이만 보고(종료 코드 2), --write면 고친다.
                    await FormatCheck.CheckAsync(root, options.Optional("formatter") ??
                        Environment.GetEnvironmentVariable("ROCKS_CLANG_FORMAT") ?? "clang-format",
                        options.Optional("dotnet") ?? "dotnet", options.Write);
                    return 0;
                default:
                    throw new ArgumentException("Unknown command: " + args[0]);
            }

            // --filter <a,b,...>: 이름에 쉼표로 나눈 조각 하나라도 들어 있는 시험만 돈다(반복 중 바뀐 부분만 확인할 때). 아무것도 남지
            // 않으면 실패다. --only는 정확한 이름 목록이다(TestSuite.RunAsync).
            if (options.Optional("filter") is string filter)
            {
                string[] parts = filter.Split(',', StringSplitOptions.RemoveEmptyEntries);
                suite.Retain(name => parts.Any(part => name.Contains(part, StringComparison.Ordinal)));
            }

            return await suite.RunAsync(options.Json, options.Seed, parallelism, options.Only);
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            Console.Error.WriteLine(error.Message);
            return 2;
        }
    }

    private static int Scaffold(string[] args)
    {
        if (args.Length == 0)
        {
            throw new ArgumentException("Expected scaffold plan/apply/validate");
        }

        Dictionary<string, string> values = new(StringComparer.Ordinal);
        for (int index = 1; index < args.Length; index += 2)
        {
            if (index + 1 >= args.Length || !args[index].StartsWith("--", StringComparison.Ordinal) ||
                !values.TryAdd(args[index][2..], args[index + 1]))
            {
                throw new ArgumentException("Invalid scaffold option");
            }
        }

        string Get(string key) => values.TryGetValue(key, out string? value) ? value : throw new ArgumentException("Missing --" + key);
        switch (args[0])
        {
            case "plan":
                if (values.Keys.Any(key => key is not ("root" or "name" or "output" or "plan")))
                {
                    throw new ArgumentException("Unknown plan option");
                }

                ScaffoldService.PlanModule(Get("root"), Get("name"), Get("output"), Get("plan"));
                break;
            case "apply":
                if (values.Count != 1)
                {
                    throw new ArgumentException("Expected only --plan");
                }

                ScaffoldService.Apply(Get("plan"));
                break;
            case "validate":
                if (values.Count != 1)
                {
                    throw new ArgumentException("Expected only --plan");
                }

                ScaffoldService.ValidateOutput(Get("plan"));
                break;
            default:
                throw new ArgumentException("Unknown scaffold operation");
        }

        return 0;
    }

    // 도구 자체 실행/timeout/pipe 음성대조 전용 child다. shell과 Python을 사용하지 않는다.
    private static async Task<int> ChildAsync(string[] args)
    {
        int? hard = await ProcessHardTimeoutReviewTests.TryRunChildAsync(args);
        if (hard.HasValue)
        {
            return hard.Value;
        }

        int? reviewed = await ProcessReviewTests.TryRunChildAsync(args);
        if (reviewed.HasValue)
        {
            return reviewed.Value;
        }

        if (args.SequenceEqual(new[] { "--hang" }))
        {
            Console.WriteLine("ready");
            await Task.Delay(Timeout.InfiniteTimeSpan);
        }

        if (args.SequenceEqual(new[] { "--echo-input" }))
        {
            Console.Write(await Console.In.ReadToEndAsync());
            return 0;
        }

        if (args.SequenceEqual(new[] { "--large-output" }))
        {
            string text = new('x', 256 * 1024);
            Console.Out.Write("출력🎮" + text);
            Console.Error.Write("오류🎮" + text);
            return 17;
        }

        if (args.Length == 2 && args[0] == "--exit-code")
        {
            return int.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture);
        }

        throw new ArgumentException("Unknown internal child control");
    }
}
