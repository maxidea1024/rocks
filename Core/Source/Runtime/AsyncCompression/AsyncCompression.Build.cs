using Rocks.Build;
using Rocks.Build.Rules;

// WorkerScheduler(Task) 위의 비동기 압축·해제(AsyncCompression.h)와 블록 컨테이너의 병렬 압축·읽기(ParallelCompressedBlocks.h)다.
// Compression이 Task에 의존하지 않도록 따로 둔다(Task → TraceCapture → Compression 순서, Docs/Design/Compression.ko.md 5절).
// 동기 경로와 같은 코덱·블록 인코딩을 쓰려고 Compression의 내부 헤더(Compression/Private: CompressionInternal.h·
// CompressedBlocksInternal.h)를 이 모듈의 Private 구현에서만 include한다. 그 밖의 모듈은 Compression 내부를 보지 않는다.
public sealed class AsyncCompression : CoreModuleRules
{
    protected override string ModuleName => "AsyncCompression";

    protected override string[] RequiredModules => ["Core", "Task", "Compression"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        [context.PathInRoot("Core/Source/Runtime/Compression/Private")];
}
