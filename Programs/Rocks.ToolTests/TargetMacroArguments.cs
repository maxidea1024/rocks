namespace Rocks.ToolTests;

/// <summary>
/// 시험이 Core 헤더를 직접 컴파일할 때 붙이는 타깃 매크로 넷이다(Docs/Design/BuildTargets 3.2·3.4절). meta 검사 인자(<c>--clang-arg</c>)에는
/// 타깃 매크로가 없다: 메타 컴파일러가 조합을 스스로 정하고(BS2c) 넘기면 RMC1000으로 거부하므로, 메타 컴파일러를 거치지 않는 직접 컴파일만 이
/// 값을 더한다. 기본은 메타 컴파일러의 주 조합인 에디터 조합이다. 사용 예:
/// <code>
/// arguments.AddRange(options.ClangArguments);
/// arguments.AddRange(TargetMacroArguments.Of(editor: true));
/// </code>
/// </summary>
internal static class TargetMacroArguments
{
    /// <summary>editor면 에디터 조합(에디터 코드·데이터 1), 아니면 게임 조합(둘 다 0)이다. 서버·클라이언트 코드는 둘 다 1이다.</summary>
    public static IEnumerable<string> Of(bool editor = true)
    {
        string bit = editor ? "1" : "0";
        return ["-DROCKS_WITH_EDITOR=" + bit, "-DROCKS_WITH_EDITOR_DATA=" + bit, "-DROCKS_WITH_SERVER_CODE=1", "-DROCKS_WITH_CLIENT_CODE=1"];
    }
}
