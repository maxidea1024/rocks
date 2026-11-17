using Rocks.Build.Rules;

// Core 수학 마이크로벤치마크 타깃이다. 수치는 Release로 측정한다:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=MathBench /p:Configuration=Release
public sealed class MathBenchTarget : TargetRules
{
    public override string Name => "MathBench";

    // 개발 도구·측정 실행 파일이다(Release 배제 정책 대상이 아니다).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["MathBench"];
}
