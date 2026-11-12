using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

/// <summary>읽기 전용 검사 결과다. 경로는 가능한 경우 저장소 상대 경로이며 Line=0은 파일 전체 오류다.</summary>
public sealed record CheckFailure(string Path, int Line, string Rule, string Message)
{
    public override string ToString() => $"{Path}:{Line}: {Rule}: {Message}";
}

/// <summary>엔진 소유 소스의 lexical 정책이다. 외부 SDK나 최종 바이너리 의존성을 증명하지 않는다.</summary>
public static class PolicyChecks
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static Regex Pattern(string value) => new(value,
        RegexOptions.CultureInvariant | RegexOptions.Compiled, TimeSpan.FromSeconds(1));

    private static readonly Regex BannedRuntime = Pattern(
        "#\\s*include\\s*[<\"](?:vector|string|array|deque|list|map|set|unordered_map|unordered_set|memory|functional|thread|mutex|future|iostream|fstream|sstream|algorithm|ranges|span|filesystem|utility|tuple|type_traits)[>\"]|\\bstd\\s*::");
    // 언어 규칙이 std 이름을 직접 요구하는 지점만 파일·이름 단위로 허용한다. 구조적 바인딩의 tuple
    // protocol은 std::tuple_size/std::tuple_element 특수화를 찾으므로 Tuple.h만 그 두 이름과 기본
    // 템플릿 선언용 <utility>를 쓸 수 있다. 같은 줄에 다른 std 사용이 있으면 여전히 위반이다.
    private static readonly Dictionary<string, Regex> RuntimeStdAllowances = new(StringComparer.Ordinal)
    {
        ["Core/Source/Runtime/Core/Public/Core/Tuple.h"] = Pattern(
            "#\\s*include\\s*<utility>|\\bstd\\s*::\\s*(?:tuple_size|tuple_element)\\b"),
    };
    private static readonly Regex BannedPlatform = Pattern(
        "#\\s*(?:if|ifdef|ifndef|elif)\\b[^\\n]*(?:_WIN32|_WIN64|__APPLE__|__linux__|ROCKS_PLATFORM_)" +
        "|#\\s*include\\s*[<\"](?:[Ww]indows|windowsx|synchapi|processthreadsapi|pthread|semaphore|unistd|mach/[^>\"]+)\\.h[>\"]" +
        "|\\b(?:CreateThread|WaitForSingleObject|pthread_create|pthread_join|pthread_mutex_lock|pthread_cond_wait|Sleep|nanosleep)\\s*\\(");
    private static readonly Regex BannedSuppression = Pattern(
        "^\\s*#\\s*pragma\\s+(?:warning\\s*\\(\\s*(?:disable|[0-3])\\b|(?:clang|GCC)\\s+diagnostic\\s+(?:ignored|warning)\\b)" +
        "|^\\s*#\\s*define\\s+(?:_CRT_SECURE_NO_WARNINGS|_SCL_SECURE_NO_WARNINGS)\\b");
    private static readonly Regex SuppressedFlag = Pattern(
        "^(?:/WX-|/W[0-3]|/w(?:d[0-9]+)?|(?:/clang:)?(?:-w|-Wno-.+))$");
    private static readonly Regex PythonInvocation = Pattern(
        "\\bpython(?:3(?:\\.[0-9]+)?)?(?:\\.exe)?\\b|setup-python\\b|[^\\s\"']+\\.py(?:[\\s\"']|$)");

    // 엔진 런타임·프로그램·플러그인 소스다. 테스트(모듈의 Tests/, 테스트 runner)와 테스트 지원 코드는
    // STL 금지 대상이 아니다(STL 호환 테스트가 STL을 직접 쓴다).
    private static readonly string[] RuntimeRoots = ["Core/Source/Runtime", "Engine/Source", "Engine/Plugins", "Programs"];

    // 엔진이 소유한 C++ 소스의 최상위 폴더다(서식·경고·플랫폼 경계 검사 범위).
    private static readonly string[] OwnedSourceRoots = ["Core", "Engine", "Programs", "Templates"];

    private static bool IsTestSource(string relative) =>
        relative.Split('/').Contains("Tests") ||
        relative.StartsWith("Programs/FoundationTests/", StringComparison.Ordinal);

    public static IReadOnlyList<CheckFailure> Runtime(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, RuntimeRoots))
        {
            string relative = Relative(root, path);
            if (IsTestSource(relative))
                continue;
            RuntimeStdAllowances.TryGetValue(relative, out Regex? allowance);
            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                string checkedText = allowance is null ? text : allowance.Replace(text, " ");
                if (BannedRuntime.IsMatch(checkedText))
                    failures.Add(new(relative, line, "runtime-stl", "forbidden runtime STL use"));
            }
        }
        return failures;
    }

    public static IReadOnlyList<CheckFailure> SourceWarnings(string root) =>
        Scan(root, OwnedSourceRoots, BannedSuppression,
            "warning-source", "owned source suppresses warnings");

    private static readonly Regex OptimizationOverride = Pattern(
        "\\bROCKS_(?:DISABLE|ENABLE)_OPTIMIZATION\\b|#\\s*pragma\\s+(?:clang\\s+)?optimize\\b|_Pragma\\s*\\(\\s*\"(?:clang\\s+)?optimize\\b" +
        "|\\[\\[\\s*clang\\s*::\\s*optnone\\s*\\]\\]|__attribute__\\s*\\(\\(\\s*optnone\\b");
    private static readonly Regex NeverOptimizedRule = Pattern("\\bCodeOptimization\\s*\\.\\s*Never\\b");
    // 강제 인라인은 쓰지 않는다(인라인 판단은 Clang 최적화·ThinLTO에 맡긴다).
    private static readonly Regex ForcedInline = Pattern(
        "__attribute__\\s*\\(\\(\\s*(?:__)?always_inline|\\[\\[\\s*(?:clang|gnu)\\s*::\\s*always_inline|__forceinline\\b");
    private const string OptimizationMacroHeader = "Core/Source/Runtime/Base/Public/Base/Compiler.h";

    /// <summary>
    /// 최적화 끄기는 디버깅 중 임시 설정이다. 제품 소스에 남으면 Development 성능이 조용히 떨어지고 Release가
    /// 최적화 없이 나가거나 깨지므로 다음을 거부한다.
    /// <list type="bullet">
    /// <item>C++: ROCKS_DISABLE_OPTIMIZATION, #pragma (clang) optimize, optnone 속성. 매크로를 정의하는
    /// Base/Compiler.h와 테스트 소스는 예외다.</item>
    /// <item>모듈 규칙(*.Build.cs): CodeOptimization.Never. Always는 의도한 성능 설정이므로 허용한다.</item>
    /// <item>강제 인라인(always_inline 속성, __forceinline): 테스트를 포함한 모든 소스(규칙 rule id
    /// forced-inline). 인라인 판단은 Clang 최적화와 ThinLTO에 맡긴다.</item>
    /// </list>
    /// </summary>
    public static IReadOnlyList<CheckFailure> OptimizationOverrides(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (relative == OptimizationMacroHeader)
                continue;
            ScanFile(path, relative, ForcedInline, "forced-inline",
                "forced inlining is not used; let Clang optimization and ThinLTO decide", failures);
            if (IsTestSource(relative))
                continue;
            ScanFile(path, relative, OptimizationOverride, "optimization-override",
                "temporary optimization override left in product source", failures);
        }

        foreach (string path in Files(root, OwnedSourceRoots).Where(path => path.EndsWith(".Build.cs", StringComparison.Ordinal)))
            ScanFile(path, Relative(root, path), NeverOptimizedRule, "optimization-override",
                "module rule disables optimization (CodeOptimization.Never is for local debugging only)", failures);
        return failures;
    }

    public static IReadOnlyList<CheckFailure> PlatformBoundary(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (IsNativeBackend(relative))
                continue;
            ScanFile(path, relative, BannedPlatform, "platform-boundary",
                "OS dependency outside native backend", failures);
        }
        return failures;
    }

    /// <summary>
    /// 명시 선택한 native backend만 허용한다. 유사 접두사 디렉터리는 예외가 아니다.
    /// <list type="bullet">
    /// <item>Core/Source/Runtime/PlatformBase/Private/{Win64,Mac,Posix,Apple,Android}/</item>
    /// <item>Core/Source/Developer/TestSupport/Private/{Win64,Mac,Posix,Apple,Android}/</item>
    /// <item>Programs/Commandlet/Private/Win64/</item>
    /// <item>Engine/Source/{Runtime,Developer}/&lt;Module&gt;/Private/{Win64,Mac,Posix,Apple,Android}/: 창·그래픽
    /// backend처럼 OS SDK를 직접 쓰는 엔진 모듈 구현. 공개 헤더(Public/)와 공통 구현은 여전히 금지다.</item>
    /// </list>
    /// </summary>
    public static bool IsNativeBackend(string relative)
    {
        string[] parts = relative.Replace('\\', '/').Split('/');
        if (parts.Length >= 5 && parts[0] == "Programs" && parts[1] == "Commandlet" && parts[2] == "Private")
            return parts[3] == "Win64";
        if (parts.Length >= 7 && parts[0] == "Engine" && parts[1] == "Source" &&
            parts[2] is "Runtime" or "Developer" && parts[4] == "Private")
            return parts[5] is "Win64" or "Mac" or "Posix" or "Apple" or "Android";
        if (parts.Length < 7 || parts[0] != "Core" || parts[1] != "Source" || parts[4] != "Private")
            return false;
        bool platform = parts[5] is "Win64" or "Mac" or "Posix" or "Apple" or "Android";
        return (parts[2] == "Runtime" && parts[3] == "PlatformBase" && platform) ||
               (parts[2] == "Developer" && parts[3] == "TestSupport" && platform);
    }

    /// <summary>shell 문자열을 실행하지 않는다. flags 검증에는 가능한 한 실제 ArgumentList를 전달한다.</summary>
    public static IReadOnlyList<CheckFailure> CompilerArguments(IReadOnlyList<string> arguments,
        string source = "<command>")
    {
        List<CheckFailure> failures = [];
        bool windows = arguments.Contains("/WX", StringComparer.Ordinal);
        if (!windows && !arguments.Contains("-Werror", StringComparer.Ordinal))
            Fail("missing warnings-as-errors");
        foreach (string flag in windows ? new[] { "/W4" } : ["-Wall", "-Wextra", "-Wpedantic"])
            if (!arguments.Contains(flag, StringComparer.Ordinal))
                Fail("missing required warning flag: " + flag);
        foreach (string argument in arguments)
        {
            if (SuppressedFlag.IsMatch(argument))
                Fail("warning suppression or reduced warning level: " + argument);
            if (argument.Contains("_CRT_SECURE_NO_WARNINGS", StringComparison.Ordinal) ||
                argument.Contains("_SCL_SECURE_NO_WARNINGS", StringComparison.Ordinal))
                Fail("global deprecation suppression: " + argument);
            if (argument.StartsWith('@'))
                Fail("unexpanded response file; audit the expanded compiler arguments");
        }
        return failures;
        void Fail(string message) => failures.Add(new(source, 0, "warning-command", message));
    }

    public static IReadOnlyList<CheckFailure> CompilerCommand(string command, string source = "<command>")
    {
        try { return CompilerArguments(SplitCommand(command), source); }
        catch (FormatException error) { return [new(source, 0, "warning-command", error.Message)]; }
    }

    public static IReadOnlyList<CheckFailure> CompilationDatabase(string path)
    {
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(path, StrictUtf8));
        List<CheckFailure> failures = [];
        if (document.RootElement.ValueKind != JsonValueKind.Array || document.RootElement.GetArrayLength() == 0)
            return [new(path, 0, "warning-database", "empty or invalid compilation database")];
        foreach (JsonElement entry in document.RootElement.EnumerateArray())
        {
            string file = entry.TryGetProperty("file", out var name) ? name.GetString() ?? path : path;
            if (entry.TryGetProperty("arguments", out JsonElement values) && values.ValueKind == JsonValueKind.Array)
                failures.AddRange(CompilerArguments(values.EnumerateArray().Select(value =>
                    value.GetString() ?? throw new FormatException("null compiler argument")).ToArray(), file));
            else if (entry.TryGetProperty("command", out JsonElement command))
                failures.AddRange(CompilerCommand(command.GetString() ?? "", file));
            else
                failures.Add(new(file, 0, "warning-database", "missing compiler command/arguments"));
        }
        return failures;
    }

    /// <summary>새 MSBuild/CLI/workflow/launcher만 검사한다. 보존된 .py와 시험 fixture는 실행 경로가 아니다.</summary>
    public static IReadOnlyList<CheckFailure> PythonFreeMainPath(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, ["Programs", "Build", ".github"]))
        {
            string relative = Relative(root, path);
            if (relative.StartsWith("Programs/Rocks.ToolTests/", StringComparison.Ordinal) ||
                relative.StartsWith("Programs/Rocks.Tooling.Checks/", StringComparison.Ordinal))
                continue;
            string extension = Path.GetExtension(path).ToLowerInvariant();
            if (extension is not (".cs" or ".csproj" or ".props" or ".targets" or ".proj" or ".ps1" or ".cmd" or ".sh" or ".yml" or ".yaml"))
                continue;
            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                string trimmed = text.TrimStart();
                if (trimmed.StartsWith("//", StringComparison.Ordinal) || trimmed.StartsWith('#'))
                    continue;
                if (PythonInvocation.IsMatch(text))
                    failures.Add(new(relative, line, "python-main-path", "Python dependency in an active tool entry point"));
            }
        }
        return failures;
    }

    public static IReadOnlyList<string> NativeSources(string root, IReadOnlyList<string>? folders = null) =>
        Files(root, folders ?? OwnedSourceRoots).Where(path =>
            Path.GetExtension(path).ToLowerInvariant() is ".h" or ".cpp" or ".mm").ToArray();

    internal static IEnumerable<string> Files(string root, IReadOnlyList<string> folders)
    {
        root = Path.GetFullPath(root);
        if (!Directory.Exists(root))
            throw new DirectoryNotFoundException(root);
        EnumerationOptions options = new() { RecurseSubdirectories = true,
            AttributesToSkip = FileAttributes.ReparsePoint, IgnoreInaccessible = false };
        return folders.Select(folder => Path.Combine(root, folder)).Where(Directory.Exists)
            .SelectMany(folder => Directory.EnumerateFiles(folder, "*", options))
            .Where(path => !Relative(root, path).Split('/').Any(part => part is "bin" or "obj" or "__pycache__"))
            .OrderBy(path => Relative(root, path), StringComparer.Ordinal).ToArray();
    }

    private static IReadOnlyList<CheckFailure> Scan(string root, IReadOnlyList<string> folders,
        Regex pattern, string rule, string message)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, folders))
            ScanFile(path, Relative(root, path), pattern, rule, message, failures);
        return failures;
    }

    private static void ScanFile(string path, string relative, Regex pattern, string rule,
        string message, List<CheckFailure> failures)
    {
        int line = 0;
        foreach (string text in File.ReadLines(path, StrictUtf8))
            if (pattern.IsMatch(text))
                failures.Add(new(relative, ++line, rule, message));
            else
                ++line;
    }

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');

    // compile_commands의 인용된 경로를 토큰화할 뿐 shell 확장·환경 변수·명령 실행을 하지 않는다.
    private static IReadOnlyList<string> SplitCommand(string command)
    {
        List<string> result = [];
        StringBuilder token = new();
        char quote = '\0';
        foreach (char character in command)
        {
            if (quote != '\0')
            {
                if (character == quote) quote = '\0'; else token.Append(character);
            }
            else if (character is '\'' or '"') quote = character;
            else if (char.IsWhiteSpace(character))
            {
                if (token.Length > 0) { result.Add(token.ToString()); token.Clear(); }
            }
            else token.Append(character);
        }
        if (quote != '\0') throw new FormatException("unterminated compiler command quote");
        if (token.Length > 0) result.Add(token.ToString());
        return result;
    }
}
