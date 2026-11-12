using Rocks.Build.Rules;

// Archive 계약·버전 규칙(Object 의존 없음).
public sealed class Serialization : CoreModuleRules
{
    protected override string ModuleName => "Serialization";

    protected override string[] RequiredModules => ["Core"];
}
