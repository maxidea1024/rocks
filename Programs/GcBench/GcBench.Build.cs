using Rocks.Build;
using Rocks.Build.Rules;

// ObjectRegistry GC 측정 실행 파일이다(Docs/Design/GarbageCollectionScale.ko.md GC-S0). 제품 런타임에 들어가지 않는다.
// `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=GcBench -p:Configuration=Development`로 빌드한다.
// 자동 실행이 대화상자에서 멈추지 않도록 시험 지원 코드의 호스트 실패 처리(HostFailure)를 함께 넣는다.
public sealed class GcBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        const string support = "Core/Source/Developer/TestSupport";
        var sources = context.Sources("Programs/GcBench/Private").ToList();
        sources.Add(context.PathInRoot(context.Platform == NativePlatform.Windows
            ? $"{support}/Private/Win64/HostFailure.cpp"
            : $"{support}/Private/Posix/HostFailure.cpp"));
        return new()
        {
            Name = "GcBench",
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            PrivateIncludeDirectories = [context.PathInRoot($"{support}/Public")],
            Dependencies = ["ObjectRuntime", "GcSoak"]
        };
    }
}
