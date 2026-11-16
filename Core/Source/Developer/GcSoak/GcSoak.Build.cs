using Rocks.Build.Rules;

// 장기 GC smoke 시뮬레이터(도구 전용, Docs/Design/GarbageCollectionScale.ko.md 9절). 결정적 seed로 tick마다 객체 생성·간선
// 변경·루트·파괴·보유 계수와 증분·정지형 수집을 섞고 독립 모델과 대조한다. 짧은 실행은 FoundationTests(ObjectRuntime 시험)가,
// 긴 실행은 GcBench --soak이 부른다. 게임 런타임에는 들어가지 않는다.
public sealed class GcSoak : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "GcSoak";

    protected override string[] RequiredModules => ["ObjectRuntime"];
}
