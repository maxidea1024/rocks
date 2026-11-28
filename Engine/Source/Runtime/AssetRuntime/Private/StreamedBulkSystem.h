#pragma once

#include <AssetRuntime/AssetLoader.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/StreamedBulk.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Platform/Sync.h>

// 스트리밍 bulk 범위 읽기의 내부 상태다(Docs/Design/AssetSystem 20.6 — AssetManager가 하나를
// 쥔다). 모든 표·계수·단계는 mutex가 지키고, IO 호출·callback은 mutex 밖에서 한다. IO 취소는
// 요청의 ioPins를 올린 뒤 mutex 밖에서 부르고, 검증 작업은 ioPins가 0이 될 때까지 Release를
// 미룬다.
namespace rocks::detail {

// sidecar 파일의 열기 단계다.
enum class StreamedFilePhase : uint8 {
    Closed,
    Opening,
    Open,
    Failed
};

// 요청의 단계다.
enum class StreamedReadPhase : uint8 {
    Queued,
    Submitted,
    Verifying,
    Finishing,
    Finished
};

// sidecar 파일 하나다(경로별 하나 — StreamedBulk 수 + 살아 있는 요청 수 + 열기 작업이 계수한다).
struct StreamedBulkFileRecord {
    explicit StreamedBulkFileRecord(Allocator& allocator) noexcept
        : path(allocator), blockHashes(allocator) {
    }

    StreamedBulkSystem* system = nullptr;
    String path;
    uint64 fileBytes = 0;
    uint32 hashBlockBytes = 0;
    SidecarHeaderExpectation header{};
    Array<Sha256Digest> blockHashes;
    uint32 refs = 0;
    StreamedFilePhase phase = StreamedFilePhase::Closed;
    ErrorCode failure = ErrorCode::None;
    StreamedBulkFile* file = nullptr;
};

// 읽기 요청 하나다(손잡이와 시스템이 계수한다 — 둘 다 놓으면 지운다).
struct StreamedBulkReadOp {
    explicit StreamedBulkReadOp(Allocator& allocator) noexcept : scratch(allocator) {
    }

    StreamedBulkSystem* system = nullptr;
    StreamedBulkFileRecord* file = nullptr;
    // sidecar 파일 안 요청 범위의 시작과 destination이다.
    uint64 fileOffset = 0;
    Span<uint8> destination{};
    BulkReadPriority priority = BulkReadPriority::Normal;
    uint64 sequence = 0;
    BulkReadCallback callback = nullptr;
    void* context = nullptr;
    // BulkReadState 값이다(손잡이가 아무 스레드에서 읽는다 — 원자 값).
    uint8 state = static_cast<uint8>(BulkReadState::Pending);
    uint32 refs = 0;
    StreamedReadPhase phase = StreamedReadPhase::Queued;
    bool cancelRequested = false;
    // io.Read가 성공했고 검증 작업이 아직 가져가지 않았는지다(Cancel이 IO 취소를 부를 수 있는지).
    bool ioStarted = false;
    // mutex 밖에서 진행 중인 io.Cancel 수다(0이 될 때까지 검증 작업이 Release를 미룬다).
    uint32 ioPins = 0;
    // 블록 경계로 넓힌 읽기 범위다(scratch[0]이 rangeStart 위치).
    uint64 rangeStart = 0;
    Array<uint8> scratch;
    StreamedBulkIoRead ioRead;
    StreamedBulkIoResult ioResult;
    // Finishing 단계의 결과다.
    ErrorCode finishError = ErrorCode::None;
};

struct StreamedBulkSystem {
    StreamedBulkSystem(Allocator& allocatorIn, StreamedBulkIo& ioIn, AssetLoaderWorker& workerIn,
                       uint32 maxInFlightIn) noexcept
        : allocator(allocatorIn), io(ioIn), worker(workerIn), maxInFlight(maxInFlightIn),
          files(allocatorIn), queue(allocatorIn), submitted(allocatorIn) {
    }

    // 살아 있는 파일·요청이 있으면 platform::Fatal이다(AS 20.6 10 — 손잡이를 놓지 않고 파괴).
    ~StreamedBulkSystem();

    // path의 파일 기록을 찾거나 만들고 계수를 1 올린다(Game 스레드 — IO 없음). 오류: ShuttingDown,
    // 할당 실패.
    Result<StreamedBulkFileRecord*> Acquire(StringView path,
                                            const PackageSidecarState& sidecar) noexcept;
    // 파일 기록 계수를 1 내린다(0이면 닫고 지운다 — 아무 스레드).
    void ReleaseFile(StreamedBulkFileRecord* file) noexcept;

    // 요청을 큐에 넣는다(StreamedBulk::Read). 오류: ShuttingDown, 할당 실패.
    Result<StreamedBulkReadOp*> Submit(StreamedBulkFileRecord* file, uint64 fileOffset,
                                       Span<uint8> destination, BulkReadPriority priority,
                                       BulkReadCallback callback, void* context) noexcept;
    // 손잡이 쪽 연산이다.
    void Cancel(StreamedBulkReadOp* op) noexcept;
    void SetPriority(StreamedBulkReadOp* op, BulkReadPriority priority) noexcept;
    // 요청 계수를 1 내린다(0이면 지운다).
    void ReleaseOp(StreamedBulkReadOp* op) noexcept;

    // 새 요청을 막고 큐를 Cancelled로 끝내고 진행 IO를 취소한 뒤 모든 callback이 돌아올 때까지
    // 기다린다(AS 20.6 10). 두 번째 호출은 무동작이다.
    void Shutdown() noexcept;

    // 큐에서 꺼낼 수 있는 요청을 제출하고 열기·실패 작업을 건다.
    void Dispatch() noexcept;
    // mutex 안에서 ioPins를 올려 둔 요청의 IO 취소를 부르고 핀을 놓는다(mutex 밖에서 부른다).
    void CancelPinned(StreamedBulkReadOp* op) noexcept;

    Allocator& allocator;
    StreamedBulkIo& io;
    AssetLoaderWorker& worker;
    uint32 maxInFlight;
    platform::Mutex mutex;
    platform::ConditionVariable idle;
    // 요청의 ioPins가 0이 될 때 깨운다(검증 작업이 기다린다).
    platform::ConditionVariable unpinned;
    Array<StreamedBulkFileRecord*> files;
    Array<StreamedBulkReadOp*> queue;
    // 제출해 IO가 진행 중인 요청이다(Shutdown이 취소를 요청한다).
    Array<StreamedBulkReadOp*> submitted;
    uint32 inFlight = 0;
    uint64 sequence = 0;
    // callback이 아직 돌아오지 않은 요청 수 + 진행 중 열기 작업 수와 살아 있는 요청 객체 수다.
    uint64 outstanding = 0;
    uint64 liveOps = 0;
    bool shuttingDown = false;
};

} // namespace rocks::detail
