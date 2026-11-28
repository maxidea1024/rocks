#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Span.h>
#include <Core/Error.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/AsyncFile.h>

// 스트리밍 bulk(sidecar) 범위 읽기다(Docs/Design/AssetSystem 20.2 3·20.6 — PR12c, UE 5.8
// FBulkData::CreateStreamingRequest·IAsyncReadRequest 대응). AssetManager::OpenStreamedBulk가
// lease의 패키지에서 sidecar bulk 하나를 열어 StreamedBulk를 주고, StreamedBulk::Read가 범위 하나를
// 비동기로 읽는 BulkReadHandle을 준다. 읽은 byte는 덮는 sidecar 블록의 SHA-256으로 늘 검증한다(서명
// 사슬).
//
// 사용 예(오디오 청크 하나):
//   auto bulk = manager.OpenStreamedBulk(lease, clip->payload);
//   auto handle = bulk.Value().Read(chunkOffset, chunkBuffer.View(), BulkReadPriority::High,
//                                   OnChunkLoaded, &chunk);
//   // OnChunkLoaded(&chunk, result)가 worker 스레드에서 정확히 한 번 불린다.
//
// 스레드: StreamedBulk::Read와 손잡이 함수는 아무 스레드(AudioRT 제외 — 할당한다). callback은
// AssetManager의 worker(로더 worker)에서 돈다(worker 제출이 실패하면 원인을 로그에 남기고 그
// 자리에서 돈다 — AS 20.6 8).
namespace rocks {

// 진행 IO 수의 기본 상한이다(AssetManagerConfig::streamedMaxInFlight).
inline constexpr uint32 StreamedBulkMaxInFlight = 8;
// 한 요청이 읽는 블록 경계 범위의 byte 상한이다(AsyncReadFile 한 요청의 상한 4 GiB − 1).
inline constexpr uint64 StreamedBulkMaxReadBytes = NumericLimits<uint32>::Max();

// 범위 IO 구현의 열린 파일이다(구현이 정의한다 — AssetManager는 포인터만 쥔다).
class StreamedBulkFile;

// 읽기 하나의 결과다. error가 None이면 bytes만큼 읽었다(짧을 수 있다 — 호출자가 대조한다).
struct StreamedBulkIoResult {
    ErrorCode error = ErrorCode::None;
    uint64 bytes = 0;
};

// 읽기 요청 하나다. AssetManager가 채우고 Release까지 살려 둔다.
struct StreamedBulkIoRead {
    uint64 offset = 0;
    uint8* destination = nullptr;
    uint64 bytes = 0;
    // 완료 통지다. 아무 스레드에서 요청마다 정확히 한 번 부른다(Read가 성공한 요청만, Read가
    // 반환하기 전이어도 된다). 구현은 자기 lock을 쥔 채 부르지 않는다. AssetManager 쪽은 짧다(검증
    // 작업을 worker에 넣기만 한다 — 그 제출은 할당할 수 있다).
    void (*complete)(StreamedBulkIoRead& read,
                     const StreamedBulkIoResult& result) noexcept = nullptr;
    void* owner = nullptr;
    // 구현이 요청마다 쓰는 자리다.
    void* backendState = nullptr;
};

// 범위 IO 경계다(AS 20.6 1 — 로더의 CookedPackageIo와 같은 모양). 시험은 완료를 명시 gate로
// 내보내는 구현을 끼운다.
class StreamedBulkIo {
public:
    // path(NUL로 끝나는 UTF-8과 그 byte 수)를 열어 크기를 fileBytes에, 처음 header.Size() byte를
    // header에 읽는다(파일이 더 짧으면 읽은 만큼만 — 나머지는 0). 막을 수 있다(AssetManager는
    // worker에서만 부른다). 오류: 열기·크기·읽기 오류는 그 오류(없음은 NotFound), 할당 실패.
    virtual Result<StreamedBulkFile*> Open(const char* path, usize pathBytes, Span<uint8> header,
                                           uint64& fileBytes) noexcept = 0;

    // read 범위를 읽기 시작한다. 즉시 실패(자원 부족 등)는 그 오류이며 complete를 부르지 않는다.
    virtual Result<void> Read(StreamedBulkFile& file, StreamedBulkIoRead& read) noexcept = 0;

    // 진행 중 읽기의 취소를 요청한다(최선 노력 — complete는 Cancelled 또는 원래 결과로 한 번).
    // complete를 이 호출 안에서 부르지 않는다. complete 뒤·Release 전 호출은 무동작이다.
    // AssetManager는 자기 mutex 밖에서 부르고, 그동안 Release를 미룬다.
    virtual void Cancel(StreamedBulkIoRead& read) noexcept = 0;

    // complete가 불린 요청의 자원을 돌려준다(Read가 성공한 요청마다 한 번).
    virtual void Release(StreamedBulkIoRead& read) noexcept = 0;

    // 파일을 닫는다(진행 중 읽기가 없을 때만 — AssetManager가 계수로 보장한다).
    virtual void Close(StreamedBulkFile* file) noexcept = 0;

protected:
    ~StreamedBulkIo() = default;
};

// PlatformBase AsyncReadFile로 읽는 기본 구현이다(EngineAssets가 자기 AsyncIoContext로 만든다).
// Open은 열기·크기·머리 읽기(제출 뒤 Wait)를 한다. 읽기 결과의 Cancelled status는 Cancelled,
// 그 밖 실패는 FileErrorCode 대응이다.
//
// 수명: io와 allocator는 이 객체보다 오래 산다. allocator는 다른 스레드의 해제를 지원해야 한다.
class ROCKS_ASSETRUNTIME_API AsyncStreamedBulkIo final : public StreamedBulkIo {
public:
    AsyncStreamedBulkIo(Allocator& allocator, platform::AsyncIoContext& io) noexcept
        : allocator_(allocator), io_(io) {
    }

    Result<StreamedBulkFile*> Open(const char* path, usize pathBytes, Span<uint8> header,
                                   uint64& fileBytes) noexcept override;
    Result<void> Read(StreamedBulkFile& file, StreamedBulkIoRead& read) noexcept override;
    void Cancel(StreamedBulkIoRead& read) noexcept override;
    void Release(StreamedBulkIoRead& read) noexcept override;
    void Close(StreamedBulkFile* file) noexcept override;

private:
    Allocator& allocator_;
    platform::AsyncIoContext& io_;
};

// 읽기 우선순위다(UE EAsyncIOPriorityAndFlags의 축약 — 높은 것 먼저, 같으면 제출 순서).
enum class BulkReadPriority : uint8 {
    Low,
    Normal,
    High,
    Critical
};

// 읽기 손잡이의 상태다.
enum class BulkReadState : uint8 {
    Pending,
    Done,
    Failed,
    Cancelled
};

// 읽기 하나의 결과다(callback이 받는다). error: None(bytes = 요청 크기)·CorruptData(짧은 읽기·블록
// 해시 불일치·sidecar 머리·크기 다름)·Cancelled·IO 오류·OutOfMemory.
struct BulkReadResult {
    ErrorCode error = ErrorCode::None;
    uint64 bytes = 0;
};

using BulkReadCallback = void (*)(void* context, const BulkReadResult& result) noexcept;

namespace detail {
struct StreamedBulkSystem;
struct StreamedBulkFileRecord;
struct StreamedBulkReadOp;
} // namespace detail

// 읽기 하나의 손잡이다(move-only). Pending인 손잡이의 소멸은 platform::Fatal이다(막는 소멸을 두지
// 않는다 — Cancel 뒤 callback을 받고 놓는다). callback 전에 상태가 정해지므로 callback 안에서 자기
// 손잡이를 파괴할 수 있다. StreamedBulk보다 오래 살 수 있다.
class ROCKS_ASSETRUNTIME_API BulkReadHandle {
public:
    BulkReadHandle() noexcept = default;
    BulkReadHandle(BulkReadHandle&& other) noexcept;
    BulkReadHandle& operator=(BulkReadHandle&& other) noexcept;
    BulkReadHandle(const BulkReadHandle&) = delete;
    BulkReadHandle& operator=(const BulkReadHandle&) = delete;
    ~BulkReadHandle();

    bool IsValid() const noexcept {
        return op_ != nullptr;
    }

    // 상태다(빈 손잡이는 Done). callback 직전에 정해지므로 Pending이 아님을 본 것이 callback이
    // 돌아왔다는 뜻은 아니다(callback 끝은 callback 안에서 알린다).
    BulkReadState State() const noexcept;
    // 취소를 요청한다: 제출 전이면 큐에서 빼고 Cancelled, 제출 뒤면 IO 취소 요청(결과는 Cancelled
    // 또는 원래 결과), 검증 중·끝남·빈 손잡이는 무동작. callback은 어느 쪽이든 정확히 한 번이다.
    void Cancel() noexcept;
    // 제출 전이면 큐 순서를 바꾼다(UE 굶음 승격). 그 밖은 무동작.
    void SetPriority(BulkReadPriority priority) noexcept;

private:
    friend class StreamedBulk;

    explicit BulkReadHandle(detail::StreamedBulkReadOp* op) noexcept : op_(op) {
    }

    void Drop() noexcept;

    detail::StreamedBulkReadOp* op_ = nullptr;
};

// sidecar bulk 하나다(move-only, AssetManager::OpenStreamedBulk). 패키지가 아니라 계수된 sidecar
// 파일을 쥔다 — lease 반납·scope Revoke·패키지 내림 뒤에도 읽는다. AssetManager 파괴 전에 놓아야
// 한다(남으면 platform::Fatal).
class ROCKS_ASSETRUNTIME_API StreamedBulk {
public:
    StreamedBulk() noexcept = default;
    StreamedBulk(StreamedBulk&& other) noexcept;
    StreamedBulk& operator=(StreamedBulk&& other) noexcept;
    StreamedBulk(const StreamedBulk&) = delete;
    StreamedBulk& operator=(const StreamedBulk&) = delete;
    ~StreamedBulk();

    bool IsValid() const noexcept {
        return file_ != nullptr;
    }

    // bulk 크기(rawBytes)다.
    uint64 Bytes() const noexcept {
        return rawBytes_;
    }

    // [offset, offset + destination.Size())를 destination에 읽는다. destination은 callback이 돌아올
    // 때까지 살아 있어야 한다(Cancelled면 내용은 정하지 않는다). callback(context, result)은 worker
    // 스레드에서 정확히 한 번이다(0 byte도 — IO 없이 Done).
    //
    // 오류(즉시 — callback을 부르지 않는다): 빈 StreamedBulk·null callback·범위가 rawBytes를
    // 넘음·블록 경계로 넓힌 읽기 범위가 StreamedBulkMaxReadBytes 초과 InvalidArgument,
    // AssetManager Shutdown 뒤
    // ShuttingDown, 할당 실패 OutOfMemory.
    Result<BulkReadHandle> Read(uint64 offset, Span<uint8> destination, BulkReadPriority priority,
                                BulkReadCallback callback, void* context) noexcept;

private:
    friend class AssetManager;

    StreamedBulk(detail::StreamedBulkFileRecord* file, uint64 offset, uint64 rawBytes) noexcept
        : file_(file), offset_(offset), rawBytes_(rawBytes) {
    }

    void Drop() noexcept;

    detail::StreamedBulkFileRecord* file_ = nullptr;
    // sidecar 파일 안 bulk 시작 위치와 크기다.
    uint64 offset_ = 0;
    uint64 rawBytes_ = 0;
};

} // namespace rocks
