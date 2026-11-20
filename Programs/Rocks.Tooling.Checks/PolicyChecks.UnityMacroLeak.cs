using System.Text.RegularExpressions;
using Rocks.Build;
using Rocks.Build.Rules;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>매크로를 남기는 헤더를 감싸는 쌍의 시작 헤더 철자다(Docs/Design/UnityBuildPch.ko.md 6.1절).</summary>
    public const string WindowsHeadersBegin = "Platform/Win64/WindowsHeadersBegin.h";

    /// <summary>매크로를 남기는 헤더를 감싸는 쌍의 끝 헤더 철자다.</summary>
    public const string WindowsHeadersEnd = "Platform/Win64/WindowsHeadersEnd.h";

    // include 지시문 한 줄의 종류(꺾쇠·따옴표)와 철자다. 주석을 지운 한 줄에 맞춘다.
    private static readonly Regex IncludeKindAndSpelling = Pattern("^[ \\t]*#[ \\t]*include[ \\t]*([<\"])([^<>\"]+)[>\"]");

    // 평범한 이름의 매크로를 번역 단위 끝까지 남기는 헤더다: Windows SDK·COM·셸·진단 계열. 철자는 대소문자를 구분하지 않는다. 새
    // Windows 헤더를 쓰기 시작하면 여기에 더한다. vulkan/vulkan.h는 VK_USE_PLATFORM_WIN32_KHR을 정의했을 때만 windows.h를 여는데,
    // 저장소는 그 매크로를 정의하지 않고 vulkan_core.h와 플랫폼 헤더를 따로 include하므로 목록에 없다.
    private static readonly HashSet<string> LeakyHeaderNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "windows.h", "winsock2.h", "ws2tcpip.h", "winuser.h", "winbase.h", "windef.h", "winnt.h", "wincrypt.h", "winternl.h", "winioctl.h",
        "wingdi.h", "winreg.h", "winsvc.h", "wincon.h", "windowsx.h",
        "objbase.h", "objidl.h", "unknwn.h", "ole2.h", "oleauto.h", "oaidl.h", "ocidl.h", "combaseapi.h", "propidl.h",
        "shlobj.h", "shlobj_core.h", "shellapi.h", "shlwapi.h", "knownfolders.h", "shobjidl.h",
        "dbghelp.h", "psapi.h", "tlhelp32.h", "bcrypt.h", "werapi.h", "wtsapi32.h", "processsnapshot.h", "xinput.h", "mmsystem.h",
        "mmdeviceapi.h", "audioclient.h", "dwmapi.h", "dinput.h", "setupapi.h", "iphlpapi.h", "userenv.h", "aclapi.h", "sddl.h",
        "dcomp.h", "dxc/dxcapi.h"
    };

    // 이 접두로 시작하는 헤더도 같은 계열이다(Direct3D·DXGI·Direct2D·DirectWrite·WRL).
    private static readonly string[] LeakyHeaderPrefixes = ["d3d", "dxgi", "d2d1", "dwrite", "wrl/"];

    // 생성 헤더의 이름 끝이다. 출력 디렉터리에 있어 저장소 헤더로 찾아지지 않는다.
    private const string GeneratedHeaderSuffix = ".generated.h";

    /// <summary>
    /// include 철자가 매크로를 남기는 헤더(Windows SDK 계열)인지다. 철자만 본다. 이름이 같은 접두로 시작하는 저장소 헤더
    /// (<c>D3D12Caps.h</c>)를 가려내는 것은 부르는 쪽 몫이다(<see cref="UnityModuleMacroLeaks"/>는 저장소 헤더로 찾아지면 묻지 않는다).
    /// </summary>
    public static bool IsMacroLeakingHeader(string spelling)
    {
        spelling = spelling.Replace('\\', '/');
        return LeakyHeaderNames.Contains(spelling) ||
            LeakyHeaderPrefixes.Any(prefix => spelling.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// 유니티를 켠 모듈에서 매크로를 남기는 헤더를 감싸지 않고 include하는 곳을 찾는다(Docs/Design/UnityBuildPch.ko.md 6.1절, 규칙
    /// <c>unity-macro-leak</c>). Windows SDK 헤더는 <c>near</c>·<c>far</c>·<c>small</c>·<c>interface</c>와 A/W 함수 이름을 매크로로
    /// 남기고, 묶음에서는 그 매크로가 뒤 구성원을 깨뜨린다. 그런 헤더는 <see cref="WindowsHeadersBegin"/>과
    /// <see cref="WindowsHeadersEnd"/> 사이에 둔다. 묶이는 소스와 그 소스가 include하는 저장소 헤더, 모듈의 PCH 헤더(모든 번역
    /// 단위 앞에 들어간다)를 본다. <c>UnityExclude</c>의
    /// 소스와 첫 <c>#include</c> 앞에서 <c>#define</c>하는 소스는 묶이지 않으므로 대상이 아니다. 사용 예:
    /// <c>PolicyChecks.UnityMacroLeaks(root, ReleaseExclusion.DefaultRulesAssembly(root))</c>.
    /// </summary>
    /// <exception cref="FileNotFoundException">규칙 어셈블리가 없다(먼저 <c>dotnet build Programs/Rocks.Tools.csproj</c>).</exception>
    public static IReadOnlyList<CheckFailure> UnityMacroLeaks(string root, string rulesAssembly)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(rulesAssembly))
        {
            throw new FileNotFoundException($"rules assembly {rulesAssembly} is missing; build Programs/Rocks.Tools.csproj first", rulesAssembly);
        }

        var failures = new List<CheckFailure>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach ((NativePlatform platform, NativeArchitecture architecture) in UnityPlatforms)
        {
            var context = new BuildContext(root, Path.Combine(root, "out", "policy-unity"), platform, architecture, NativeConfiguration.Debug);
            IReadOnlyList<NativeModuleInput> modules = RuleLoader.LoadAll(rulesAssembly, context).Modules;
            var byName = modules.GroupBy(module => module.Name, StringComparer.OrdinalIgnoreCase)
                .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);
            foreach (NativeModuleInput module in modules.Where(UnityRules.ModuleEnabled))
            {
                var excluded = module.UnityExclude.Select(path => Path.GetFullPath(path, root)).ToHashSet(StringComparer.OrdinalIgnoreCase);
                string[] sources = module.Sources.Select(path => Path.GetFullPath(path, root)).Where(path => !excluded.Contains(path)).ToArray();
                // 이 모듈의 번역 단위가 보는 include 디렉터리: 자기 private·public과, 의존 모듈의 공개 환경(그 공개 의존까지)이다.
                var directories = new List<string>(module.PrivateIncludeDirectories.Concat(module.PublicIncludeDirectories));
                var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { module.Name };
                var pending = new Queue<string>(module.PublicDependencies.Concat(module.PrivateDependencies));
                while (pending.TryDequeue(out string? name))
                {
                    if (visited.Add(name) && byName.TryGetValue(name, out NativeModuleInput? dependency))
                    {
                        directories.AddRange(dependency.PublicIncludeDirectories);
                        foreach (string next in dependency.PublicDependencies)
                        {
                            pending.Enqueue(next);
                        }
                    }
                }

                string[] pchHeaders = new[] { module.PrivatePchHeader, module.SharedPchHeader }.OfType<string>()
                    .Select(path => Path.GetFullPath(path, root)).ToArray();
                foreach (CheckFailure failure in UnityModuleMacroLeaks(root, module.Name, sources,
                    directories.Select(path => Path.GetFullPath(path, root)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray(), pchHeaders))
                {
                    if (seen.Add(failure.Path + ":" + failure.Line + ":" + failure.Message[(failure.Message.IndexOf(':') + 1)..]))
                    {
                        failures.Add(failure);
                    }
                }
            }
        }

        return failures;
    }

    /// <summary>
    /// 모듈 하나의 묶이는 소스(절대 경로)와 그 소스가 include하는 저장소 헤더를 검사한다. 규칙 평가와 떨어진 함수다(양성·음성 대조
    /// 시험이 직접 부른다). 저장소 헤더는 따옴표 include면 include한 파일의 디렉터리에서 먼저, 그다음 <paramref name="includeDirectories"/>
    /// 순서로 찾는다. 전처리 조건의 두 갈래를 모두 읽는다. <paramref name="pchHeaders"/>(절대 경로)는 모듈의 PCH 헤더로, 소스가
    /// include하지 않아도 읽는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> UnityModuleMacroLeaks(string root, string module, IReadOnlyList<string> sources,
        IReadOnlyList<string> includeDirectories, IReadOnlyList<string>? pchHeaders = null)
    {
        root = Path.GetFullPath(root);
        var failures = new List<CheckFailure>();
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pending = new Queue<string>();
        foreach (string source in sources.Order(StringComparer.Ordinal))
        {
            if (!UnitySourceExtensions.Contains(Path.GetExtension(source)) || !File.Exists(source) ||
                UnityRules.DefinesBeforeFirstInclude(File.ReadAllText(source, StrictUtf8)))
            {
                continue;
            }

            if (visited.Add(source))
            {
                pending.Enqueue(source);
            }
        }

        foreach (string header in pchHeaders ?? [])
        {
            if (File.Exists(header) && visited.Add(header))
            {
                pending.Enqueue(header);
            }
        }

        while (pending.TryDequeue(out string? file))
        {
            string relative = Relative(root, file);
            string directory = Path.GetDirectoryName(file)!;
            int line = 0;
            int openLine = 0;
            foreach (string text in WithoutComments(File.ReadAllText(file, StrictUtf8)).Split('\n'))
            {
                ++line;
                Match match = IncludeKindAndSpelling.Match(text);
                if (!match.Success)
                {
                    continue;
                }

                string spelling = match.Groups[2].Value.Replace('\\', '/');
                if (spelling == WindowsHeadersBegin)
                {
                    if (openLine != 0)
                    {
                        failures.Add(new(relative, line, "unity-macro-leak",
                            $"module {module}: {WindowsHeadersBegin} is already open since line {openLine}; close it with {WindowsHeadersEnd} first"));
                    }

                    openLine = line;
                    continue;
                }

                if (spelling == WindowsHeadersEnd)
                {
                    if (openLine == 0)
                    {
                        failures.Add(new(relative, line, "unity-macro-leak", $"module {module}: {WindowsHeadersEnd} without {WindowsHeadersBegin}"));
                    }

                    openLine = 0;
                    continue;
                }

                // 저장소 헤더인지를 먼저 본다. D3D12Caps.h나 DxgiGraphics/DxgiAdapter.h처럼 이름이 SDK 헤더 접두와 겹치는 저장소
                // 헤더는 누출 헤더가 아니라 따라가서 읽을 헤더다. 생성 헤더(.generated.h)는 선언만 담으므로 보지 않는다.
                string? header = (match.Groups[1].Value == "\"" ? new[] { directory } : []).Concat(includeDirectories)
                    .Select(candidate => Path.GetFullPath(Path.Combine(candidate, spelling))).FirstOrDefault(File.Exists);
                if (header is null || !IsOwnedHeader(root, header))
                {
                    if (openLine == 0 && !spelling.EndsWith(GeneratedHeaderSuffix, StringComparison.Ordinal) && IsMacroLeakingHeader(spelling))
                    {
                        failures.Add(new(relative, line, "unity-macro-leak",
                            $"module {module}: <{spelling}> leaves macros (near, far, small, interface, A/W function names) that break later " +
                            $"sources of a unity batch; include it between <{WindowsHeadersBegin}> and <{WindowsHeadersEnd}>, or list the " +
                            "source in UnityExclude"));
                    }

                    continue;
                }

                if (openLine != 0)
                {
                    failures.Add(new(relative, line, "unity-macro-leak",
                        $"module {module}: only Windows SDK headers go between {WindowsHeadersBegin} and {WindowsHeadersEnd}; include " +
                        $"'{spelling}' outside (it would be parsed with the leaked macros alive)"));
                }

                if (visited.Add(header))
                {
                    pending.Enqueue(header);
                }
            }

            if (openLine != 0)
            {
                failures.Add(new(relative, openLine, "unity-macro-leak", $"module {module}: {WindowsHeadersBegin} is not closed with {WindowsHeadersEnd}"));
            }
        }

        return failures;
    }

    // 따라가는 저장소 헤더다: 루트 아래의 소유 소스(외부 라이브러리 원본과 빌드 출력은 뺀다).
    private static bool IsOwnedHeader(string root, string path)
    {
        string relative = Relative(root, path);
        return !relative.StartsWith("../", StringComparison.Ordinal) && !Path.IsPathRooted(relative) &&
            OwnedSourceRoots.Any(owned => relative.StartsWith(owned + "/", StringComparison.Ordinal)) &&
            !relative.Split('/').Contains("ThirdParty");
    }
}
