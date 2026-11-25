using Rocks.Build.Rules;

// UI 런타임을 실제 창에 연결하는 예제다.
public sealed class UiDemoTarget : TargetRules
{
    public override string Name => "UiDemo";
    public override TargetType Type => TargetType.Program;
    public override IReadOnlyList<string> Modules => ["UiDemo"];
    public override bool CheckPublicHeaders => true;
}
