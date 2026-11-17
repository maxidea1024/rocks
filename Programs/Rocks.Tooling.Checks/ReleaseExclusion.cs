using Rocks.Build;
using Rocks.Build.Rules;

namespace Rocks.Tooling.Checks;

/// <summary>
/// Release 배제 위반 하나다. Rule은 <c>release-developer-module</c>·<c>release-shader-compiler</c>·<c>release-windows-opengl</c>이고
/// Item은 위반 대상(Developer 모듈 이름, 링크 입력 이름, OpenGLGraphics)이다.
/// </summary>
public sealed record ReleaseViolation(string Rule, NativePlatform Platform, string Target, string Module, string Item)
{
    public override string ToString() => $"{Rule}: target {Target} ({Platform} Release) module {Module}: {Item}";
}

/// <summary>
/// 알려진 Release 배제 예외다. 같은 (Rule, Platform, Module, Item) 위반을 실패 대신 알림으로 보고한다. Until은 예외가 없어지는 단계,
/// Reason은 근거다. 예외가 어떤 위반과도 맞지 않으면 그 자체가 실패다(없어진 예외를 지우게 한다).
/// </summary>
public sealed record ReleaseException(string Rule, NativePlatform Platform, string Module, string Item, string Until, string Reason);

/// <summary>Release 배제 검사 결과다. Notices는 알려진 예외에 걸린 위반이다(조용히 넘기지 않고 출력한다).</summary>
public sealed record ReleaseExclusionResult(IReadOnlyList<CheckFailure> Failures, IReadOnlyList<string> Notices);

/// <summary>
/// Release 배제 정책이다(Docs/Design/ShaderSystem.ko.md 9절). Game 타깃(<see cref="TargetType.Game"/>, UE Shipping 대상)을 Release 구성의
/// 각 플랫폼(Windows x64·macOS arm64·Android arm64)으로 규칙 평가해 다음을 거부한다.
/// <list type="bullet">
/// <item><c>release-developer-module</c>: 모듈 도달 집합에 Developer 모듈(<c>&lt;계층&gt;/Source/Developer/</c> 아래 규칙)이 있다.</item>
/// <item><c>release-shader-compiler</c>: 도달 모듈의 시스템 라이브러리·prebuilt 라이브러리·링크 인자에 d3dcompiler·dxcompiler가 있다.</item>
/// <item><c>release-windows-opengl</c>: Windows 대상이 OpenGLGraphics에 닿는다(GLES31_PC는 개발용 미리보기, UE와 같다). Android는 GLES
/// backend로 링크한다.</item>
/// </list>
/// Program 타깃(도구·시험·워커)은 대상이 아니다(UE도 Program 타깃은 Shipping에서도 Developer 모듈을 쓴다).
/// </summary>
public static class ReleaseExclusion
{
    /// <summary>
    /// 알려진 예외다. 둘 다 SH2(쿠킹된 셰이더를 받는 Graphics API, OpenGLGraphics의 GLES 미리보기 전환)에서 없어진다.
    /// </summary>
    public static readonly IReadOnlyList<ReleaseException> KnownExceptions =
    [
        new("release-shader-compiler", NativePlatform.Windows, "D3D12Graphics", "d3dcompiler", "SH2",
            "D3D12Graphics compiles its shaders at run time with D3DCompile (ShaderSystem 4: d3dcompiler link and runtime D3DCompile are removed in SH2)"),
        new("release-windows-opengl", NativePlatform.Windows, "OpenGLGraphics", "OpenGLGraphics", "SH2",
            "Sandbox links the desktop OpenGL backend for its D3D12/OpenGL self-test (ShaderSystem 9: OpenGLGraphics becomes the GLES preview backend, kept out of Release Windows executables, in SH2)")
    ];

    // 검사하는 대상 플랫폼과 아키텍처다(개발 호스트 두 곳과 모바일).
    private static readonly (NativePlatform Platform, NativeArchitecture Architecture)[] Platforms =
    [
        (NativePlatform.Windows, NativeArchitecture.X64), (NativePlatform.MacOS, NativeArchitecture.Arm64),
        (NativePlatform.Android, NativeArchitecture.Arm64)
    ];

    // 셰이더 컴파일러 라이브러리 이름 조각이다(대소문자 무시).
    private static readonly string[] ShaderCompilerLibraries = ["d3dcompiler", "dxcompiler"];

    private const string OpenGLModule = "OpenGLGraphics";

    /// <summary>
    /// 저장소 규칙 어셈블리(<c>out/dotnet/bin/Rocks.Engine.Rules/Debug/net10.0/Rocks.Engine.Rules.dll</c>)의 기본 경로다.
    /// </summary>
    public static string DefaultRulesAssembly(string root) =>
        Path.Combine(Path.GetFullPath(root), "out", "dotnet", "bin", "Rocks.Engine.Rules", "Debug", "net10.0", "Rocks.Engine.Rules.dll");

    /// <summary>
    /// 저장소 규칙을 세 플랫폼의 Release로 평가해 검사한다. 사용 예: <c>ReleaseExclusion.Check(root, ReleaseExclusion.DefaultRulesAssembly(root))</c>.
    /// </summary>
    /// <exception cref="FileNotFoundException">규칙 어셈블리가 없다(먼저 <c>dotnet build Programs/Rocks.Tools.csproj</c>).</exception>
    /// <exception cref="ArgumentException">규칙 평가 실패(<see cref="RuleLoader.LoadAll"/>).</exception>
    public static ReleaseExclusionResult Check(string root, string rulesAssembly)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(rulesAssembly))
        {
            throw new FileNotFoundException($"rules assembly {rulesAssembly} is missing; build Programs/Rocks.Tools.csproj first", rulesAssembly);
        }

        IReadOnlyDictionary<string, string> directories = RuleDirectories(root);
        var violations = new List<ReleaseViolation>();
        foreach ((NativePlatform platform, NativeArchitecture architecture) in Platforms)
        {
            var context = new BuildContext(root, Path.Combine(root, "out", "policy-release"), platform, architecture, NativeConfiguration.Release);
            LoadedRuleSet rules = RuleLoader.LoadAll(rulesAssembly, context);
            var developer = rules.Modules.Where(module => IsDeveloper(module.ModuleDirectory ?? directories.GetValueOrDefault(module.Name)))
                .Select(module => module.Name).ToHashSet(StringComparer.Ordinal);
            var targets = rules.Targets.Where(target => target.Type == TargetType.Game)
                .Select(target => (target.Name, target.Modules)).ToArray();
            violations.AddRange(Analyze(rules.Modules, developer, targets, platform));
        }

        return Apply(violations, KnownExceptions);
    }

    /// <summary>
    /// Game 타깃(이름·고른 모듈)마다 도달 집합을 만들어 위반을 모은다. 규칙 평가와 떨어진 순수 함수다(음성 대조 시험이 직접 부른다).
    /// </summary>
    /// <exception cref="ArgumentException">타깃이나 모듈이 없는 모듈에 의존한다.</exception>
    public static IReadOnlyList<ReleaseViolation> Analyze(IReadOnlyList<NativeModuleInput> modules, IReadOnlySet<string> developerModules,
        IReadOnlyList<(string Name, IReadOnlyList<string> Modules)> gameTargets, NativePlatform platform)
    {
        var byName = modules.ToDictionary(module => module.Name, StringComparer.Ordinal);
        var violations = new List<ReleaseViolation>();
        foreach ((string target, IReadOnlyList<string> selected) in gameTargets)
        {
            var reached = new SortedSet<string>(StringComparer.Ordinal);
            var pending = new Stack<string>(selected);
            while (pending.Count != 0)
            {
                string name = pending.Pop();
                if (!reached.Add(name))
                {
                    continue;
                }

                if (!byName.TryGetValue(name, out NativeModuleInput? module))
                {
                    throw new ArgumentException($"target {target}: unknown module {name}");
                }

                foreach (string dependency in module.Dependencies)
                {
                    pending.Push(dependency);
                }
            }

            foreach (string name in reached)
            {
                NativeModuleInput module = byName[name];
                if (developerModules.Contains(name))
                {
                    violations.Add(new("release-developer-module", platform, target, name, name));
                }

                IEnumerable<string> links = module.SystemLibraries.Concat(module.PrebuiltLibraries.Select(Path.GetFileName))
                    .Concat(module.AdditionalLinkArguments).OfType<string>();
                foreach (string link in links)
                {
                    foreach (string compiler in ShaderCompilerLibraries)
                    {
                        if (link.Contains(compiler, StringComparison.OrdinalIgnoreCase))
                        {
                            violations.Add(new("release-shader-compiler", platform, target, name, compiler));
                        }
                    }
                }

                if (platform == NativePlatform.Windows && name == OpenGLModule)
                {
                    violations.Add(new("release-windows-opengl", platform, target, name, OpenGLModule));
                }
            }
        }

        return violations;
    }

    /// <summary>
    /// 예외에 맞는 위반은 알림으로, 나머지는 실패로 나눈다. 어떤 위반과도 맞지 않은 예외는 <c>release-exception-stale</c> 실패다.
    /// </summary>
    public static ReleaseExclusionResult Apply(IReadOnlyList<ReleaseViolation> violations, IReadOnlyList<ReleaseException> exceptions)
    {
        var failures = new List<CheckFailure>();
        var notices = new List<string>();
        var used = new HashSet<ReleaseException>();
        foreach (ReleaseViolation violation in violations)
        {
            ReleaseException? known = exceptions.FirstOrDefault(exception => exception.Rule == violation.Rule &&
                exception.Platform == violation.Platform && exception.Module == violation.Module && exception.Item == violation.Item);
            if (known is null)
            {
                failures.Add(new($"target {violation.Target}", 0, violation.Rule, violation.ToString()));
                continue;
            }

            used.Add(known);
            notices.Add($"known Release exception until {known.Until}: {violation} ({known.Reason})");
        }

        foreach (ReleaseException exception in exceptions.Where(exception => !used.Contains(exception)))
        {
            failures.Add(new($"module {exception.Module}", 0, "release-exception-stale",
                $"known Release exception ({exception.Rule} {exception.Platform} {exception.Item}, until {exception.Until}) no longer applies; remove it"));
        }

        return new(failures, notices);
    }

    // 모듈 이름 → 규칙 파일 폴더(저장소 상대)다. 규칙 파일 이름이 모듈 이름인 것만 안다(<Module>.Build.cs).
    private static IReadOnlyDictionary<string, string> RuleDirectories(string root)
    {
        var directories = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (string folder in new[] { "Core/Source", "Engine/Source", "Engine/Plugins" })
        {
            string full = Path.Combine(root, folder);
            if (!Directory.Exists(full))
            {
                continue;
            }

            foreach (string file in Directory.EnumerateFiles(full, "*.Build.cs", SearchOption.AllDirectories))
            {
                string name = Path.GetFileName(file)[..^".Build.cs".Length];
                directories.TryAdd(name, Path.GetRelativePath(root, Path.GetDirectoryName(file)!).Replace('\\', '/'));
            }
        }

        return directories;
    }

    // <계층>/Source/Developer/<모듈> 또는 플러그인의 .../Source/Developer/<모듈>이다.
    private static bool IsDeveloper(string? directory)
    {
        if (directory is null)
        {
            return false;
        }

        string[] parts = directory.Replace('\\', '/').Split('/');
        for (int index = 0; index + 1 < parts.Length; ++index)
        {
            if (parts[index] == "Source" && parts[index + 1] == "Developer")
            {
                return true;
            }
        }

        return false;
    }
}
