using Rocks.Build;
using Rocks.Build.Rules;

// 글꼴 배치에 쓰이는 CPU 데이터와 아틀라스 저장소다.
public sealed class TextLayout : EngineModuleRules
{
    protected override string ModuleName => "TextLayout";
    protected override string[] PublicDependencies => ["Core"];
}
