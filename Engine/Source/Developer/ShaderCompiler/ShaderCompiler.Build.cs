using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 셰이더 컴파일 관리 모듈(메인 프로세스)이다(Docs/Design/ShaderSystem.ko.md 3·12.1절). 지금은 P6a의 형식 정의
/// (<c>Engine/Config/ShaderFormats/&lt;id&gt;.json</c>)와 엔진 컴파일 프로필(<c>Engine-&lt;호스트&gt;.json</c>) 읽기·검증·SHA-256만 있다.
/// 전처리기(결정 6)·job 키(P5)·워커 통신(SH1)이 이 모듈에 더해진다. 도구 표(ShaderToolTable)로 형식 정의의 도구 이름을
/// 검사하지만 도구 binary는 열지 않는다(정책 검사 shader-tool-binary-load).
/// </summary>
public sealed class ShaderCompiler : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderCompiler";

    protected override string[] RequiredModules => ["ShaderToolTable", "PlatformBase", "Core"];
}
