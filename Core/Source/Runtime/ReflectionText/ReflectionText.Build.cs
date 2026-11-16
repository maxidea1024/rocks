using Rocks.Build.Rules;

// 리플렉션 값의 텍스트·JSON 표기와 대입 묶음(리플렉션 B9i — Docs/Design/ReflectionExtensions.ko.md 10절).
// ObjectRuntime 밖에 두어 JSON·명령행 표기가 ObjectRuntime의 모든 사용자에게 끌려가지 않게 한다.
public sealed class ReflectionText : CoreModuleRules
{
    protected override string ModuleName => "ReflectionText";

    protected override string[] RequiredModules => ["ObjectRuntime", "Core"];
}
