using Rocks.Build;
using Rocks.Build.Rules;

// Compression helpers (Godot Compression and UE FCompression counterpart): synchronous LZ4 (raw block and frame), Zstd, Deflate, Zlib and Gzip APIs,
// Brotli decompression, and the sequential block container API. The asynchronous and parallel APIs on WorkerScheduler (Task) are in the AsyncCompression
// module. Compression sits below Task (Task -> TraceCapture -> Compression, Docs/Design/TraceSystem.ko.md section 3).
// External codec (Core/Source/ThirdParty) headers and definitions are exposed only to this module's Private implementation (Docs/Design/Compression.ko.md).
// The class is named CompressionModule so it is not confused with System.IO.Compression (same approach as TaskRuntime in the Task module).
public sealed class CompressionModule : CoreModuleRules
{
    private static readonly string[] Codecs = ["Lz4", "Zstd", "Zlib", "Brotli"];

    protected override string ModuleName => "Compression";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", .. Codecs];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, Codecs);

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, Codecs);
}
