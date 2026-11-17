using Rocks.Build.Rules;

// 셰이더 도구 스파이크 타깃이다(SH0). 도구를 bootstrap한 기계에서 실행한다:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=ShaderToolSpike /p:Configuration=Debug
//   ShaderToolSpike --out <디렉터리> [--sm 6_0]
public sealed class ShaderToolSpikeTarget : TargetRules
{
    public override string Name => "ShaderToolSpike";

    // 개발 도구다(Release 배제 정책 대상이 아니다).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["ShaderToolSpike"];
}
