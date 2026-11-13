using Rocks.Build.Rules;

// 호스트·CPU·CRT와 무관하게 같은 비트를 내는 수학 함수(쿠킹·에셋 너겟 결과용). IEEE 기본 연산과 비트 연산만
// 쓰고 CRT 수학 함수를 부르지 않는다. 설계: Docs/Design/CoreDataFoundations.ko.md 2절.
public sealed class DeterministicMath : CoreModuleRules
{
    protected override string ModuleName => "DeterministicMath";

    protected override string[] RequiredModules => ["PlatformBase"];
}
