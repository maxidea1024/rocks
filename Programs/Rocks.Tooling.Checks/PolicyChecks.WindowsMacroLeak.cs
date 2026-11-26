using System.Text.RegularExpressions;
using Rocks.Build;
using Rocks.Build.Rules;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>Windows SDK 헤더를 감싸는 쌍의 시작 헤더 철자다(Docs/Design/UnityBuildPch.ko.md 6.1절 "매크로를 남기는 헤더").</summary>
    public const string WindowsHeadersBegin = "Platform/Win64/WindowsHeadersBegin.h";

    /// <summary>Windows SDK 헤더를 감싸는 쌍의 끝 헤더 철자다.</summary>
    public const string WindowsHeadersEnd = "Platform/Win64/WindowsHeadersEnd.h";

    private const string WindowsMacroLeakRule = "windows-macro-leak";

    // 생성 헤더의 이름 끝이다. 출력 디렉터리에 있어 저장소 헤더로 찾아지지 않는다. 선언만 담는다.
    private const string GeneratedHeaderSuffix = ".generated.h";

    // 셰이더 파라미터 구조체 생성 헤더의 이름 끝이다(Docs/Design/ShaderSystem 16.2절 SSD51 — 출력 디렉터리, 선언과 표만 담는다).
    private const string ShaderParameterHeaderSuffix = ".shaderparams.h";

    // include 지시문 한 줄에서 #include 뒤의 내용이다. 주석을 지우고 줄 이음을 이은 한 줄에 맞춘다.
    private static readonly Regex IncludeOperand = Pattern("^[ \\t]*#[ \\t]*include\\b[ \\t]*(.*)$");

    // 쌍 밖에 둘 수 있는 시스템 헤더다(C 표준, CRT, POSIX 이름의 CRT 헤더). 매크로를 남기지 않거나 표준이 정한 이름만 남긴다. 확장자와
    // 디렉터리가 없는 C++ 표준 헤더(<atomic>)와 intrinsic 헤더(*intrin.h)는 IsCleanSystemHeader가 이름 꼴로 받는다. 철자는 대소문자를
    // 구분하지 않는다. 매크로를 남기지 않는 새 시스템 헤더를 쓰기 시작하면 여기에 더한다.
    private static readonly HashSet<string> CleanSystemHeaders = new(StringComparer.OrdinalIgnoreCase)
    {
        "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h",
        "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h",
        "stdnoreturn.h", "string.h", "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h", "wctype.h",
        "conio.h", "crtdbg.h", "direct.h", "eh.h", "excpt.h", "fcntl.h", "io.h", "malloc.h", "memory.h", "process.h", "sal.h", "share.h",
        "sys/stat.h", "sys/timeb.h", "sys/types.h", "sys/utime.h", "vcruntime.h", "vcruntime_new.h",
        // 다른 CPU·컴파일러 갈래의 intrinsic 헤더다(전처리 조건의 두 갈래를 모두 읽는다).
        "arm_acle.h", "arm_neon.h", "cpuid.h"
    };

    // 외부 라이브러리 헤더 안에서 Windows SDK 헤더로 알아보는 이름이다. 저장소 소스의 include는 이 목록이 아니라 깨끗한 헤더 목록으로
    // 판정한다(모르는 헤더가 걸린다). 외부 라이브러리 헤더는 다른 플랫폼 갈래의 시스템 헤더(unistd.h 등)를 함께 담고 있어 그렇게 할 수
    // 없으므로, 여기서만 이름으로 고른다.
    private static readonly HashSet<string> WindowsSdkHeaderNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "windows.h", "winsock2.h", "winsock.h", "ws2tcpip.h", "winuser.h", "winbase.h", "windef.h", "minwindef.h", "winnt.h", "wincrypt.h",
        "winternl.h", "winioctl.h", "wingdi.h", "winreg.h", "winsvc.h", "wincon.h", "windowsx.h", "fileapi.h", "synchapi.h", "processthreadsapi.h",
        "objbase.h", "objidl.h", "unknwn.h", "ole2.h", "oleauto.h", "oaidl.h", "ocidl.h", "combaseapi.h", "propidl.h", "rpc.h", "rpcndr.h",
        "shlobj.h", "shlobj_core.h", "shellapi.h", "shlwapi.h", "knownfolders.h", "shobjidl.h", "commctrl.h",
        "dbghelp.h", "psapi.h", "tlhelp32.h", "bcrypt.h", "werapi.h", "wtsapi32.h", "processsnapshot.h", "xinput.h", "xaudio2.h", "mmsystem.h",
        "mmdeviceapi.h", "audioclient.h", "dwmapi.h", "dinput.h", "setupapi.h", "iphlpapi.h", "userenv.h", "aclapi.h", "sddl.h", "dcomp.h",
        "dxcore.h"
    };

    // 외부 라이브러리 헤더 가운데, Windows SDK include가 저장소가 정의하지 않는 매크로의 갈래에만 있는 것이다(저장소 루트 기준 경로). 검사는
    // 전처리 조건을 평가하지 않으므로 확인한 것을 여기에 적는다. 그 헤더가 직접 적은 SDK include만 봐주고, 그 헤더가 include하는 다른
    // 헤더는 그대로 읽는다. 라이브러리 버전을 올리면 경로가 바뀌어 다시 걸리므로 그때 갈래를 다시 확인한다.
    private static readonly HashSet<string> ThirdPartyHeadersWithUnusedWindowsBranch = new(StringComparer.Ordinal)
    {
        // ZLIB_WINAPI일 때만 <windows.h>를 연다. Zlib 모듈과 소비자는 그 매크로를 정의하지 않는다(ThirdParty.json).
        "Core/Source/ThirdParty/Zlib/zlib-1.4/zconf.h",
        // VK_USE_PLATFORM_WIN32_KHR일 때만 <windows.h>를 연다. 저장소는 정의하지 않고 vulkan_core.h와 vulkan_win32.h를 따로 include한다.
        "Engine/Source/ThirdParty/VulkanHeaders/vulkan-headers-1.4.363.0/include/vulkan/vulkan.h",
        // USE_WIN32_FILEIO일 때만 <windows.h>를 연다. LibTiff 모듈은 tif_win32.c 대신 tif_rocks.c를 쓰고 그 매크로를 정의하지 않는다
        // (헤더가 _WIN32에서 AVOID_WIN32_FILEIO를 정의한다 — Docs/Design/AssetSystem 12.10절).
        "Engine/Source/ThirdParty/LibTiff/tiff-4.7.2/libtiff/tiffio.h",
        // MINIAUDIO_IMPLEMENTATION일 때만 <windows.h>를 연다(구현 구간). 그 매크로는 Miniaudio 모듈의 MiniaudioGlue.c만 정의하고
        // 소비자(AudioMiniaudio)는 선언 구간만 본다(Docs/Design/AudioSystem 3.34.1절).
        "Engine/Source/ThirdParty/Miniaudio/miniaudio-0.11.26/miniaudio.h"
    };

    // 이 접두로 시작하는 이름도 같은 계열이다(Direct3D·DXGI·Direct2D·DirectWrite·WRL).
    private static readonly string[] WindowsSdkHeaderPrefixes = ["d3d", "dxgi", "d2d1", "dwrite", "wrl/"];

    /// <summary>
    /// 쌍 밖에 둘 수 있는 시스템 헤더 철자인지다: C++ 표준 헤더(확장자·디렉터리 없음), 목록의 C·CRT 헤더, intrinsic 헤더, 생성 헤더(리플렉션·셰이더 파라미터).
    /// 저장소 파일로 찾아지지 않은 include에만 묻는다.
    /// </summary>
    public static bool IsCleanSystemHeader(string spelling)
    {
        spelling = spelling.Replace('\\', '/');
        return (!spelling.Contains('.') && !spelling.Contains('/')) || CleanSystemHeaders.Contains(spelling) ||
            (!spelling.Contains('/') && spelling.EndsWith("intrin.h", StringComparison.OrdinalIgnoreCase)) ||
            spelling.EndsWith(GeneratedHeaderSuffix, StringComparison.Ordinal) ||
            spelling.EndsWith(ShaderParameterHeaderSuffix, StringComparison.Ordinal);
    }

    /// <summary>알려진 Windows SDK 헤더 이름인지다(외부 라이브러리 헤더 안의 include를 알아볼 때만 쓴다).</summary>
    public static bool IsWindowsSdkHeaderName(string spelling)
    {
        spelling = spelling.Replace('\\', '/');
        return WindowsSdkHeaderNames.Contains(spelling) ||
            WindowsSdkHeaderPrefixes.Any(prefix => spelling.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// Windows SDK 헤더를 쌍 밖에서 include하는 곳을 찾는다(Docs/Design/UnityBuildPch.ko.md 6.1절, 규칙 <c>windows-macro-leak</c>).
    /// Windows SDK 헤더는 <c>near</c>·<c>far</c>·<c>small</c>·<c>interface</c>와 A/W 함수 이름을 매크로로 남기고, 유니티 묶음에서는
    /// 그 매크로가 뒤 구성원을 깨뜨린다. 그런 헤더는 <see cref="WindowsHeadersBegin"/>과 <see cref="WindowsHeadersEnd"/> 사이에 둔다. 쌍이
    /// 매크로를 숨긴 뒤에는 같은 번역 단위의 뒤 SDK include도 쌍 안이어야 파싱되므로, 유니티를 끈 모듈과 <c>UnityExclude</c>의 소스도 본다.
    /// Windows 플랫폼의 모듈 규칙만 평가한다. 사용 예:
    /// <c>PolicyChecks.WindowsMacroLeaks(root, ReleaseExclusion.DefaultRulesAssembly(root))</c>.
    /// </summary>
    /// <exception cref="FileNotFoundException">규칙 어셈블리가 없다(먼저 <c>dotnet build Programs/Rocks.Tools.csproj</c>).</exception>
    public static IReadOnlyList<CheckFailure> WindowsMacroLeaks(string root, string rulesAssembly)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(rulesAssembly))
        {
            throw new FileNotFoundException($"rules assembly {rulesAssembly} is missing; build Programs/Rocks.Tools.csproj first", rulesAssembly);
        }

        var context = new BuildContext(root, Path.Combine(root, "out", "policy-unity"), NativePlatform.Windows, NativeArchitecture.X64,
            NativeConfiguration.Debug);
        return WindowsMacroLeaks(root, RuleLoader.LoadAll(rulesAssembly, context).Modules);
    }

    /// <summary>
    /// 모듈 목록(Windows 플랫폼으로 평가한 규칙 결과)을 검사한다. 규칙 평가와 떨어진 함수다(양성·음성 대조 시험이 직접 부른다). 모듈마다
    /// 저장소 소유 C++ 소스와 PCH 헤더에서 시작해 include로 닿는 저장소 헤더를 읽는다. include는 그 모듈의 컴파일 환경으로 찾는다: 따옴표
    /// include면 include한 파일의 디렉터리 먼저, 그다음 모듈의 private·public include 디렉터리와 보이는 의존 모듈의 public include
    /// 디렉터리 순서다. 외부 라이브러리 원본(경로에 <c>ThirdParty</c>가 있는 소스)은 시작점이 아니다. 같은 줄의 위반은 한 번만 낸다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> WindowsMacroLeaks(string root, IReadOnlyList<NativeModuleInput> modules)
    {
        root = Path.GetFullPath(root);
        var scan = new WindowsIncludeScan(root);
        var byName = modules.GroupBy(module => module.Name, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            string[] roots = module.Sources.Where(path => UnitySourceExtensions.Contains(Path.GetExtension(path)))
                .Concat(new[] { module.PrivatePchHeader, module.SharedPchHeader }.OfType<string>())
                .Select(path => Path.GetFullPath(path, root)).Where(path => IsOwnedHeader(root, path)).ToArray();
            if (roots.Length == 0)
            {
                continue;
            }

            IEnumerable<string> directories = module.PrivateIncludeDirectories.Concat(module.PublicIncludeDirectories)
                .Concat(ModuleVisibility.Of(module, byName).SelectMany(dependency => dependency.PublicIncludeDirectories));
            scan.Module(roots, directories.Select(path => Path.GetFullPath(path, root)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray());
        }

        return scan.Failures();
    }

    // 따라가는 저장소 헤더다: 루트 아래의 소유 소스(외부 라이브러리 원본과 빌드 출력은 뺀다).
    private static bool IsOwnedHeader(string root, string path)
    {
        string relative = Relative(root, path);
        return !relative.StartsWith("../", StringComparison.Ordinal) && !Path.IsPathRooted(relative) &&
            OwnedSourceRoots.Any(owned => relative.StartsWith(owned + "/", StringComparison.Ordinal)) &&
            !relative.Split('/').Contains("ThirdParty");
    }

    private enum IncludeKind
    {
        Angle,
        Quote,

        // 꺾쇠도 따옴표도 아닌 include다(#include HEADER_MACRO).
        NonLiteral
    }

    private readonly record struct IncludeLine(int Line, IncludeKind Kind, string Spelling);

    // 한 번의 검사 동안 파일 읽기와 include 해석을 모듈을 가로질러 캐시한다. 모듈 수백 개가 같은 Core 헤더를 다시 읽고 같은 철자를 같은
    // 디렉터리 목록에서 다시 찾기 때문이다.
    private sealed class WindowsIncludeScan(string root)
    {
        // 파일의 include 지시문이다(주석 제거, 줄 이음 처리 뒤).
        private readonly Dictionary<string, IncludeLine[]> includes = new(StringComparer.OrdinalIgnoreCase);

        // (디렉터리, 철자)가 가리키는 파일이다. 없으면 null이다.
        private readonly Dictionary<(string Directory, string Spelling), string?> files = [];

        // (include 디렉터리 목록 번호, 철자)의 해석 결과다. 디렉터리 목록은 모듈마다 다르다.
        private readonly Dictionary<(int Directories, string Spelling), string?> resolved = [];

        private readonly Dictionary<string, int> directoryLists = new(StringComparer.OrdinalIgnoreCase);

        // 저장소 소유 파일인지와, 저장소 안의 외부 라이브러리 파일인지다.
        private readonly Dictionary<string, bool> owned = new(StringComparer.OrdinalIgnoreCase);

        private readonly Dictionary<(string Path, int Line, string Message), CheckFailure> failures = [];

        public IReadOnlyList<CheckFailure> Failures() =>
            failures.Values.OrderBy(failure => failure.Path, StringComparer.Ordinal).ThenBy(failure => failure.Line).ToArray();

        public void Module(IReadOnlyList<string> roots, IReadOnlyList<string> includeDirectories)
        {
            string key = string.Join('|', includeDirectories);
            if (!directoryLists.TryGetValue(key, out int directories))
            {
                directories = directoryLists.Count;
                directoryLists.Add(key, directories);
            }

            var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            // 외부 라이브러리 헤더가 Windows SDK 헤더를 여는지다(이 모듈의 include 환경에서).
            var opensSdk = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            var pending = new Queue<string>();
            foreach (string file in roots.Order(StringComparer.Ordinal))
            {
                if (File.Exists(file) && visited.Add(file))
                {
                    pending.Enqueue(file);
                }
            }

            while (pending.TryDequeue(out string? file))
            {
                string relative = Relative(root, file);
                int openLine = 0;
                void Fail(int line, string message) => failures.TryAdd((relative, line, message), new(relative, line, WindowsMacroLeakRule, message));
                foreach (IncludeLine include in Includes(file))
                {
                    if (include.Kind == IncludeKind.NonLiteral)
                    {
                        if (openLine == 0)
                        {
                            Fail(include.Line, $"'#include {include.Spelling}' is not a literal include, so it cannot be checked; write the header " +
                                $"name, or include it between <{WindowsHeadersBegin}> and <{WindowsHeadersEnd}> if it is a Windows SDK header");
                        }

                        continue;
                    }

                    if (include.Spelling == WindowsHeadersBegin)
                    {
                        if (openLine != 0)
                        {
                            Fail(include.Line, $"{WindowsHeadersBegin} is already open since line {openLine}; close it with {WindowsHeadersEnd} first");
                        }

                        openLine = include.Line;
                        continue;
                    }

                    if (include.Spelling == WindowsHeadersEnd)
                    {
                        if (openLine == 0)
                        {
                            Fail(include.Line, $"{WindowsHeadersEnd} without {WindowsHeadersBegin}");
                        }

                        openLine = 0;
                        continue;
                    }

                    string? header = Resolve(file, include, includeDirectories, directories);
                    if (header is null)
                    {
                        if (openLine == 0 && !IsCleanSystemHeader(include.Spelling))
                        {
                            Fail(include.Line, $"<{include.Spelling}> is included outside the Windows header pair. A Windows SDK header leaves " +
                                "macros (near, far, small, interface, A/W function names) that break later sources of a unity batch, and after " +
                                $"{WindowsHeadersEnd} it no longer parses outside the pair. Include it between <{WindowsHeadersBegin}> and " +
                                $"<{WindowsHeadersEnd}>; if it is a system header that leaves no macros, add it to the clean header list in " +
                                "PolicyChecks.WindowsMacroLeak.cs");
                        }

                        continue;
                    }

                    if (!Owned(header))
                    {
                        if (openLine == 0 && OpensWindowsSdk(header, includeDirectories, directories, opensSdk, []))
                        {
                            Fail(include.Line, $"<{include.Spelling}> opens Windows SDK headers and is included outside the Windows header pair; " +
                                $"include it between <{WindowsHeadersBegin}> and <{WindowsHeadersEnd}>");
                        }

                        continue;
                    }

                    if (openLine != 0)
                    {
                        Fail(include.Line, $"only Windows SDK headers go between {WindowsHeadersBegin} and {WindowsHeadersEnd}; include " +
                            $"'{include.Spelling}' outside (it would be parsed with the leaked macros alive)");
                    }

                    if (visited.Add(header))
                    {
                        pending.Enqueue(header);
                    }
                }

                if (openLine != 0)
                {
                    Fail(openLine, $"{WindowsHeadersBegin} is not closed with {WindowsHeadersEnd}");
                }
            }
        }

        // 외부 라이브러리 헤더가 직접 또는 다른 외부 라이브러리 헤더를 거쳐 알려진 Windows SDK 헤더를 include하는가. 전처리 조건의 두
        // 갈래를 모두 읽는다. active는 지금 따라가는 사슬이다(순환 include를 끊는다).
        private bool OpensWindowsSdk(string header, IReadOnlyList<string> includeDirectories, int directories, Dictionary<string, bool> known,
            HashSet<string> active)
        {
            if (known.TryGetValue(header, out bool result))
            {
                return result;
            }

            if (!active.Add(header))
            {
                return false;
            }

            bool unusedBranch = ThirdPartyHeadersWithUnusedWindowsBranch.Contains(Relative(root, header));
            foreach (IncludeLine include in Includes(header))
            {
                if (include.Kind == IncludeKind.NonLiteral)
                {
                    continue;
                }

                string? next = Resolve(header, include, includeDirectories, directories);
                if (next is null ? !unusedBranch && IsWindowsSdkHeaderName(include.Spelling)
                    : !Owned(next) && OpensWindowsSdk(next, includeDirectories, directories, known, active))
                {
                    result = true;
                    break;
                }
            }

            active.Remove(header);
            // 순환을 끊느라 false로 본 결과는 사슬 중간에서는 확정이 아니다. 사슬이 다 풀린 뒤(시작 헤더)와 true만 기억한다.
            if (result || active.Count == 0)
            {
                known[header] = result;
            }

            return result;
        }

        private bool Owned(string path)
        {
            if (!owned.TryGetValue(path, out bool result))
            {
                result = IsOwnedHeader(root, path);
                owned.Add(path, result);
            }

            return result;
        }

        // 저장소 안의 파일로 찾는다(소유 헤더와 외부 라이브러리 헤더). 저장소 밖으로 해석되거나 찾지 못하면 null이다.
        private string? Resolve(string file, IncludeLine include, IReadOnlyList<string> includeDirectories, int directories)
        {
            if (include.Kind == IncludeKind.Quote && Probe(Path.GetDirectoryName(file)!, include.Spelling) is string beside)
            {
                return beside;
            }

            if (!resolved.TryGetValue((directories, include.Spelling), out string? result))
            {
                result = includeDirectories.Select(directory => Probe(directory, include.Spelling)).FirstOrDefault(path => path is not null);
                resolved.Add((directories, include.Spelling), result);
            }

            return result;
        }

        private string? Probe(string directory, string spelling)
        {
            if (!files.TryGetValue((directory, spelling), out string? result))
            {
                string path = Path.GetFullPath(Path.Combine(directory, spelling));
                string relative = Relative(root, path);
                result = File.Exists(path) && !relative.StartsWith("../", StringComparison.Ordinal) && !Path.IsPathRooted(relative) ? path : null;
                files.Add((directory, spelling), result);
            }

            return result;
        }

        private IncludeLine[] Includes(string file)
        {
            if (!includes.TryGetValue(file, out IncludeLine[]? result))
            {
                result = IncludeLines(File.ReadAllText(file, StrictUtf8)).ToArray();
                includes.Add(file, result);
            }

            return result;
        }
    }

    // 소스 텍스트의 include 지시문이다. 주석을 지우고, 역슬래시로 끝나는 줄을 다음 줄과 이은 뒤 본다(줄 번호는 이은 첫 줄이다).
    private static IEnumerable<IncludeLine> IncludeLines(string text)
    {
        string[] lines = WithoutComments(text).Split('\n');
        for (int index = 0; index < lines.Length; ++index)
        {
            int first = index;
            string line = lines[index].TrimEnd('\r');
            while (line.EndsWith('\\') && index + 1 < lines.Length)
            {
                line = line[..^1] + lines[++index].TrimEnd('\r');
            }

            Match match = IncludeOperand.Match(line);
            if (!match.Success)
            {
                continue;
            }

            string operand = match.Groups[1].Value.Trim();
            char close = operand.StartsWith('<') ? '>' : operand.StartsWith('"') ? '"' : '\0';
            int end = close == '\0' ? -1 : operand.IndexOf(close, 1);
            yield return end > 1
                ? new(first + 1, close == '>' ? IncludeKind.Angle : IncludeKind.Quote, operand[1..end].Replace('\\', '/'))
                : new(first + 1, IncludeKind.NonLiteral, operand);
        }
    }
}
