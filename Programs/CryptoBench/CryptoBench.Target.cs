using Rocks.Build.Rules;

// Crypto throughput benchmark target. Measure with Release:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CryptoBench /p:Configuration=Release
public sealed class CryptoBenchTarget : TargetRules
{
    public override string Name => "CryptoBench";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["CryptoBench"];
}
