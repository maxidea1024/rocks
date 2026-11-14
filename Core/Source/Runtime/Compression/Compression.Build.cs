using Rocks.Build;
using Rocks.Build.Rules;

// 압축 헬퍼(Godot Compression·UE FCompression 대응): LZ4(raw block·frame)·Zstd·Deflate·Zlib·Gzip 동기 API, Brotli 해제와
// WorkerScheduler 비동기 API.
// 외부 코덱(Core/Source/ThirdParty)의 헤더·정의는 이 모듈의 Private 구현에만 노출한다(Docs/Design/Compression.ko.md).
// 클래스 이름은 System.IO.Compression과 헷갈리지 않도록 CompressionModule이다(Task 모듈의 TaskRuntime과 같은 방식).
public sealed class CompressionModule : CoreModuleRules
{
    private static readonly string[] Codecs = ["Lz4", "Zstd", "Zlib", "Brotli"];

    protected override string ModuleName => "Compression";

    protected override string[] RequiredModules => ["Core", "Task", .. Codecs];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, Codecs);

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, Codecs);
}
