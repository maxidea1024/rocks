using Rocks.Build.Rules;

public sealed class CommandletTarget : TargetRules
{
    public override string Name => "Commandlet";

    // 개발 도구·시험 실행 파일이다(Release 배제 정책 대상이 아니다).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["RocksCommandlet"];

    public override bool CheckPublicHeaders => true;
}
