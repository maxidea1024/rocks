using Rocks.Build.Rules;

// 원시 타입·Guid POD·ABI 정의만 담는 최하위 헤더 모듈이다(할당 없음).
public sealed class Base : CoreModuleRules
{
    protected override string ModuleName => "Base";

    protected override string[] RequiredModules => [];

    protected override bool HasSources => false;
}
