using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>
    /// 플랫폼 진입점을 직접 정의해도 되는 파일이다. Launch의 진입점 매크로 정의와, Rocks를 내장한 호스트를 흉내 내는 시험 실행 파일
    /// (호스트가 자기 진입점에서 RunEmbeddedApplication을 부른다)뿐이다. 새 예외는 이유와 함께 여기에 더한다.
    /// </summary>
    public static readonly IReadOnlyList<string> ProcessEntryPointExceptions =
    [
        "Core/Source/Runtime/Launch/Public/Launch/EntryPoint.h",
        "Programs/LaunchTests/Private/Win64/EmbeddedHost.cpp",
        "Programs/LaunchTests/Private/Posix/EmbeddedHost.cpp",
    ];

    // 진입점 이름이다(TCHAR 매크로 형태 _tmain·_tWinMain 포함).
    private const string EntryNames = "main|wmain|WinMain|wWinMain|_tmain|_tWinMain";

    // 반환형·속성·호출 규약 앞부분이다: [[nodiscard]]·extern "C"·int·INT·auto 뒤에 WINAPI·CALLBACK·PASCAL·APIENTRY·
    // __stdcall·__cdecl이 올 수 있다.
    private const string EntryPrefix =
        "(?:\\[\\[[^\\]]*\\]\\]\\s*)*(?:extern\\s*(?:\"[^\"]*\")?\\s*)?(?:static\\s+)?(?:int|INT|auto)\\s+" +
        "(?:(?:__stdcall|__cdecl|WINAPI|WINAPIV|CALLBACK|PASCAL|APIENTRY)\\s+)?";

    // 한 줄에 반환형과 이름이 함께 있는 정의·선언이다(주석·문자열은 지운 뒤 본다).
    private static readonly Regex ProcessEntryDefinition = Pattern("^\\s*" + EntryPrefix + "(" + EntryNames + ")\\s*\\(");

    // 반환형이 윗줄에 있는 형태(윗줄 끝이 반환형·호출 규약이고 이 줄이 이름으로 시작)다.
    private static readonly Regex ProcessEntryReturnLine = Pattern(
        "^\\s*(?:\\[\\[[^\\]]*\\]\\]\\s*)*(?:extern\\s*(?:\"[^\"]*\")?\\s*)?(?:static\\s+)?(?:int|INT|auto)" +
        "(?:\\s+(?:__stdcall|__cdecl|WINAPI|WINAPIV|CALLBACK|PASCAL|APIENTRY))?\\s*$");

    private static readonly Regex ProcessEntryNameLine = Pattern("^\\s*(?:(?:__stdcall|__cdecl|WINAPI|WINAPIV|CALLBACK|PASCAL|APIENTRY)\\s+)?(" +
        EntryNames + ")\\s*\\(");

    // 진입점을 담을 수 있는 소스 확장자다(NativeSources의 .h·.c·.cpp·.mm보다 넓게 본다).
    private static readonly string[] EntrySourceExtensions = [".h", ".hpp", ".hh", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx", ".mm"];

    /// <summary>
    /// 엔진이 소유한 native 실행 프로그램은 공통 실행 계층(Launch)의 진입점 매크로(ROCKS_IMPLEMENT_APPLICATION)로 시작한다
    /// (rule id process-entry-point, Docs/Design/CrashHandlingAndLaunch.ko.md 16절·25절 LC4·LC5). main·wmain·WinMain·wWinMain·
    /// _tmain·_tWinMain을 직접 정의하면 크래시 수집·명령줄 캡처·종료 코드 정리를 건너뛰므로 거부한다. 반환형이 윗줄에 있는 형태와
    /// 속성·호출 규약·auto 반환형도 본다. 진입점을 만드는 다른 매크로(전처리기 전개)는 lexical 검사로 잡지 못한다. 예외는
    /// <see cref="ProcessEntryPointExceptions"/>뿐이다. 외부 라이브러리 원본은 제외한다. C# 도구는 대상이 아니다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ProcessEntryPoints(string root)
    {
        IReadOnlyList<string> upstream = UpstreamDirectories(root);
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (!EntrySourceExtensions.Contains(Path.GetExtension(path).ToLowerInvariant()) ||
                upstream.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)) ||
                ProcessEntryPointExceptions.Contains(relative, StringComparer.Ordinal))
            {
                continue;
            }

            var (code, _) = StripCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            for (int index = 0; index < code.Length; ++index)
            {
                Match match = ProcessEntryDefinition.Match(code[index]);
                if (!match.Success && index > 0 && ProcessEntryReturnLine.IsMatch(code[index - 1]))
                {
                    match = ProcessEntryNameLine.Match(code[index]);
                }

                if (match.Success)
                {
                    failures.Add(new(relative, index + 1, "process-entry-point",
                        $"'{match.Groups[1].Value}' is defined directly; use ROCKS_IMPLEMENT_APPLICATION from <Launch/EntryPoint.h>"));
                }
            }
        }

        return failures;
    }
}
