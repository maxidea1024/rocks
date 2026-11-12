using Rocks.Build;
using Rocks.Build.Rules;

public sealed class SampleGameModule : ModuleRules
{
    // 리플렉션 헤더 Source/GameObject.h를 include 철자 "GameObject.h"로 연다. 생성 헤더는 <GameObject.generated.h>다.
    // identity 명세는 ModuleDirectory(프로젝트 루트)의 SampleGame.rocksids.json이다.
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "SampleGame",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Source"),
        PublicIncludeDirectories = [context.PathInRoot("Source")],
        Dependencies = ["ObjectRuntime"],
        Metadata = ["GameObject.h"],
        ModuleDirectory = "."
    };
}
