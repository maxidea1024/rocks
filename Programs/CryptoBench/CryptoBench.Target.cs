using Rocks.Build.Rules;

// 암호 처리량 측정 타깃이다. 수치는 Release로 측정한다:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CryptoBench /p:Configuration=Release
public sealed class CryptoBenchTarget : TargetRules
{
    public override string Name => "CryptoBench";

    public override IReadOnlyList<string> Modules => ["CryptoBench"];
}
