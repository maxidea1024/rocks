using Rocks.Build;
using Rocks.Build.Rules;

// Asynchronous compression and decompression (AsyncCompression.h) and parallel block container compression and reading (ParallelCompressedBlocks.h)
// on top of WorkerScheduler (Task). Kept separate so Compression does not depend on Task
// (order Task -> TraceCapture -> Compression, Docs/Design/Compression.ko.md section 5).
// To use the same codecs and block encoding as the synchronous path, only this module's Private implementation includes Compression's internal headers
// (Compression/Private: CompressionInternal.h and CompressedBlocksInternal.h). No other module sees Compression internals.
public sealed class AsyncCompression : CoreModuleRules
{
    protected override string ModuleName => "AsyncCompression";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Task", "Compression"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        [context.PathInRoot("Core/Source/Runtime/Compression/Private")];
}
