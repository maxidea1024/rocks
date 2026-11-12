using Rocks.Build.Rules;

// 엔진 모듈 테스트 실행 파일 타깃이다. Foundation은 Object 계열까지, Core는 Object 없는 하위 모듈만 시험한다.
// 리플렉션 fixture와 그 identity 명세는 ReflectionTestFixtures 모듈이 소유한다(시험 실행 파일이 그 모듈에 의존한다).
public sealed class FoundationTarget : TargetRules
{
    public override string Name => "Foundation";

    public override IReadOnlyList<string> Modules => ["FoundationTests"];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "FoundationTests";
}

// 엔진 계층(World·Engine·에셋) 테스트 타깃이다. Foundation 테스트 전체를 함께 실행한다. GPU가 필요한 시험은 이
// 타깃에 넣지 않는다(Sandbox --self-test).
public sealed class EngineTestsTarget : TargetRules
{
    public override string Name => "EngineTests";

    public override IReadOnlyList<string> Modules => ["EngineTests"];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "EngineTests";
}

// 리플렉션 fixture 모듈(과 그 의존 모듈)만 담는 SDK 타깃이다. Rocks.Build.IntegrationTests가 SDK descriptor import —
// 외부 게임 모듈이 SDK 모듈의 리플렉션 타입을 상속·참조하고 그 모듈을 다시 생성하지 않음 — 을 시험할 때 내보낸다.
public sealed class ReflectionFixturesTarget : TargetRules
{
    public override string Name => "ReflectionFixtures";

    public override IReadOnlyList<string> Modules => ["ReflectionTestFixtures"];
}

public sealed class CoreTarget : TargetRules
{
    public override string Name => "Core";

    public override IReadOnlyList<string> Modules => ["CoreFoundationTests"];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "CoreFoundationTests";
}
