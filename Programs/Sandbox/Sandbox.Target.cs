using Rocks.Build.Rules;

// 엔진 샘플 실행 파일 타깃이다. `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Sandbox`로 빌드한다.
public sealed class SandboxTarget : TargetRules
{
    public override string Name => "Sandbox";
    public override IReadOnlyList<string> Modules => ["Sandbox"];
    public override bool CheckPublicHeaders => true;
}
