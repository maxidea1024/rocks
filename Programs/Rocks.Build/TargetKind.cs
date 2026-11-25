using Rocks.Build.Rules;

namespace Rocks.Build;

/// <summary>
/// 타깃 종류와 Program 속성, 그리고 거기서 정해지는 타깃 매크로 넷이다(Docs/Design/BuildTargets 3.1·3.2절, BS2a). 매크로 값은 이 한 곳에서
/// 계산한다: 모든 compile(<c>ActionGraph.TargetCompileArguments</c>), 메타 컴파일러 AST 생성, <c>test</c>의 도구 검사가 <see cref="Defines"/>를
/// 받는다. 기본값(<see cref="Default"/>)은 Program 기본으로 Game과 같은 조합(0·0·1·1)이다. 사용 예:
/// <code>
/// TargetKind kind = new(TargetType.Program, CompileWithEditor: true);
/// // kind.Defines == ["ROCKS_WITH_EDITOR=1", "ROCKS_WITH_EDITOR_DATA=1", "ROCKS_WITH_SERVER_CODE=1", "ROCKS_WITH_CLIENT_CODE=1"]
/// </code>
/// </summary>
/// <param name="Type">타깃 종류다.</param>
/// <param name="CompileWithEditor">Program만: 에디터 모듈을 링크하는 도구(UE bCompileAgainstEditor).</param>
/// <param name="WithEditorData">Program만: 에디터 전용 데이터를 읽는 도구(BD19).</param>
public sealed record TargetKind(TargetType Type, bool CompileWithEditor = false, bool WithEditorData = false)
{
    /// <summary>타깃 매크로 이름의 앞부분이다. 규칙·명령줄은 이 접두의 매크로를 정의할 수 없다(빌드 예약).</summary>
    public const string MacroPrefix = "ROCKS_WITH_";

    /// <summary>에디터 코드 매크로 이름이다(UE WITH_EDITOR).</summary>
    public const string WithEditorMacro = "ROCKS_WITH_EDITOR";

    /// <summary>에디터 전용 데이터 매크로 이름이다(UE WITH_EDITORONLY_DATA).</summary>
    public const string WithEditorDataMacro = "ROCKS_WITH_EDITOR_DATA";

    /// <summary>서버 코드 매크로 이름이다(UE WITH_SERVER_CODE).</summary>
    public const string WithServerCodeMacro = "ROCKS_WITH_SERVER_CODE";

    /// <summary>클라이언트 코드 매크로 이름이다(UE WITH_CLIENT_CODE).</summary>
    public const string WithClientCodeMacro = "ROCKS_WITH_CLIENT_CODE";

    /// <summary>Program 기본(Game과 같은 매크로 조합)이다. 타깃 규칙 없이 만드는 빌드 요청(시험 fixture 등)의 기본값이다.</summary>
    public static TargetKind Default { get; } = new(TargetType.Program);

    /// <summary>에디터 코드가 들어가는가(Editor, Program+CompileWithEditor).</summary>
    public bool HasEditor => Type == TargetType.Editor || Type == TargetType.Program && CompileWithEditor;

    /// <summary>에디터 전용 데이터가 들어가는가. 에디터 코드는 에디터 데이터를 읽으므로 HasEditor면 참이다.</summary>
    public bool HasEditorData => HasEditor || Type == TargetType.Program && WithEditorData;

    /// <summary>서버 코드가 들어가는가(Client만 뺀다).</summary>
    public bool HasServerCode => Type != TargetType.Client;

    /// <summary>클라이언트 코드가 들어가는가(Server만 뺀다).</summary>
    public bool HasClientCode => Type != TargetType.Server;

    /// <summary>컴파일 정의 넷(<c>이름=0|1</c>)이다. 이름 순서는 고정이다.</summary>
    public IReadOnlyList<string> Defines =>
    [
        WithEditorMacro + "=" + Bit(HasEditor),
        WithEditorDataMacro + "=" + Bit(HasEditorData),
        WithServerCodeMacro + "=" + Bit(HasServerCode),
        WithClientCodeMacro + "=" + Bit(HasClientCode)
    ];

    private static string Bit(bool value) => value ? "1" : "0";
}
