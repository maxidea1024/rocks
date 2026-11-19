using System.Text.RegularExpressions;
using Rocks.Build;
using Rocks.Build.Rules;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // include 지시문 한 줄의 철자다. 주석을 지운 한 줄에 맞춘다(문자열 리터럴 안의 철자는 줄 머리의 # 가 아니라 맞지 않는다).
    private static readonly Regex IncludeSpelling = Pattern("^[ \\t]*#[ \\t]*include[ \\t]*[<\"]([^<>\"]+)[>\"]");

    /// <summary>
    /// 소스와 헤더가 PCH 헤더를 직접 include하면 거부한다(Docs/Design/UnityBuildPch.ko.md 8.1·10절, 규칙 <c>pch-header-include</c>).
    /// 모듈 PCH 헤더(<c>PrivatePchHeader</c>)와 공유 PCH 헤더(<c>SharedPchHeader</c>)는 빌드가 강제 include한다. 직접 include하면
    /// PCH를 끈 빌드(<c>--pch off</c>)와 공개 헤더 단독 컴파일 검사에서도 PCH 목록 전체가 들어와, 빠뜨린 include를 그 검사들이 찾지
    /// 못한다. PCH 헤더가 다른 PCH 헤더를 include하는 것만 허용한다(위 제공 모듈의 공유 PCH 헤더가 아래 것을 담는다). 사용 예:
    /// <c>PolicyChecks.PchHeaderIncludes(root, ReleaseExclusion.DefaultRulesAssembly(root))</c>.
    /// </summary>
    /// <exception cref="FileNotFoundException">규칙 어셈블리가 없다(먼저 <c>dotnet build Programs/Rocks.Tools.csproj</c>).</exception>
    public static IReadOnlyList<CheckFailure> PchHeaderIncludes(string root, string rulesAssembly)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(rulesAssembly))
        {
            throw new FileNotFoundException($"rules assembly {rulesAssembly} is missing; build Programs/Rocks.Tools.csproj first", rulesAssembly);
        }

        var spellings = new HashSet<string>(StringComparer.Ordinal);
        var headers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach ((NativePlatform platform, NativeArchitecture architecture) in UnityPlatforms)
        {
            var context = new BuildContext(root, Path.Combine(root, "out", "policy-unity"), platform, architecture, NativeConfiguration.Debug);
            foreach (NativeModuleInput module in RuleLoader.LoadAll(rulesAssembly, context).Modules)
            {
                if (module.SharedPchHeader is string shared)
                {
                    string spelling = shared.Replace('\\', '/');
                    spellings.Add(spelling);
                    foreach (string directory in module.PublicIncludeDirectories)
                    {
                        headers.Add(Path.GetFullPath(Path.Combine(directory, spelling), root));
                    }
                }

                if (module.PrivatePchHeader is string header)
                {
                    headers.Add(Path.GetFullPath(header, root));
                }
            }
        }

        return PchHeaderIncludeFailures(root, NativeSources(root, OwnedSourceRoots), spellings, headers);
    }

    /// <summary>
    /// 주어진 파일들(절대 경로)에서 PCH 헤더를 가리키는 include를 찾는다. 규칙 평가와 떨어진 함수다(양성·음성 대조 시험이 직접 부른다).
    /// </summary>
    /// <param name="root">실패 위치를 적을 기준 디렉터리다.</param>
    /// <param name="files">검사할 소스와 헤더의 절대 경로다.</param>
    /// <param name="sharedSpellings">공유 PCH 헤더의 include 철자(<c>Core/CoreSharedPch.h</c>)다. 꺾쇠·따옴표 어느 쪽이든 맞춘다.</param>
    /// <param name="pchHeaders">PCH 헤더 파일의 절대 경로다(모듈 PCH와 공유 PCH). 따옴표 include의 상대 경로를 이 집합에 맞추고, 이
    /// 집합의 파일은 검사 대상에서 뺀다.</param>
    public static IReadOnlyList<CheckFailure> PchHeaderIncludeFailures(string root, IEnumerable<string> files, IReadOnlySet<string> sharedSpellings,
        IReadOnlySet<string> pchHeaders)
    {
        List<CheckFailure> failures = [];
        foreach (string path in files)
        {
            if (pchHeaders.Contains(path) || !File.Exists(path))
            {
                continue;
            }

            string relative = Relative(root, path);
            string directory = Path.GetDirectoryName(path)!;
            int line = 0;
            foreach (string text in WithoutComments(File.ReadAllText(path, StrictUtf8)).Split('\n'))
            {
                ++line;
                Match match = IncludeSpelling.Match(text);
                if (!match.Success)
                {
                    continue;
                }

                string spelling = match.Groups[1].Value.Replace('\\', '/');
                if (sharedSpellings.Contains(spelling) || pchHeaders.Contains(Path.GetFullPath(spelling, directory)))
                {
                    failures.Add(new(relative, line, "pch-header-include",
                        $"'{spelling}' is a precompiled header list that the build force-includes; include the headers this file uses instead " +
                        "(sources must compile without the PCH)"));
                }
            }
        }

        return failures;
    }
}
