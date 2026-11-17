using Rocks.Build;
using Rocks.Build.Rules;

// 압축 헬퍼(Godot Compression·UE FCompression 대응): LZ4(raw block·frame)·Zstd·Deflate·Zlib·Gzip 동기 API, Brotli 해제,
// 블록 컨테이너의 순차 API. WorkerScheduler(Task) 위의 비동기·병렬 API는 AsyncCompression 모듈이다. Compression은 Task보다
// 아래 계층이다(Task → TraceCapture → Compression, Docs/Design/TraceSystem.ko.md 3절).
// 외부 코덱(Core/Source/ThirdParty)의 헤더·정의는 이 모듈의 Private 구현에만 노출한다(Docs/Design/Compression.ko.md).
// 클래스 이름은 System.IO.Compression과 헷갈리지 않도록 CompressionModule이다(Task 모듈의 TaskRuntime과 같은 방식).
public sealed class CompressionModule : CoreModuleRules
{
    private static readonly string[] Codecs = ["Lz4", "Zstd", "Zlib", "Brotli"];

    protected override string ModuleName => "Compression";

    protected override string[] RequiredModules => ["Core", .. Codecs];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, Codecs);

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, Codecs);
}
