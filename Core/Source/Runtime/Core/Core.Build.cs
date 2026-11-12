using Rocks.Build.Rules;

// 컨테이너·할당기·문자열·수학·진단·시간(UE Core 대응). 창·입력 의존 금지.
public sealed class Core : CoreModuleRules
{
    protected override string ModuleName => "Core";
    protected override string[] RequiredModules => ["PlatformBase", "DeterministicMath"];
}
