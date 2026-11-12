#pragma once

#include <Compression/Compression.h>
#include <Compression/CompressionDictionary.h>
#include <Task/WorkerScheduler.h>

// WorkerScheduler 위의 비동기 압축·해제다. 동기 API와 같은 구현·같은 분할 단위를 쓰므로
// 결과 byte가 같다.
//
// 사용 예(파일 읽기 작업이 채운 버퍼를 이어서 해제):
//   // task는 source·output보다 나중에 선언한다(먼저 소멸해야 한다).
//   CompressionTask task;
//   auto readDone = Span<const WorkerTaskHandle>(&readTask, 1);
//   auto submitted = TrySubmitDecompress(scheduler, task, CompressionFormat::Zstd, packed.View(),
//                                        64 << 20, unpacked, readDone);
//   ...
//   auto bytes = task.TryWaitResult(); // 성공 시 unpacked 뒤에 붙인 byte 수
//
// 계약:
// - 할당기: body와 코덱 작업 메모리는 Append 변형에서는 output.GetAllocator(), Span
//   변형에서는 scratch에서 할당한다. 이 할당기는 worker에서 output 성장·코덱 scratch·capture
//   해제를 수행하므로 소유 스레드와 동시에 써도 안전해야 한다(System·Binned·Budget·Pool·
//   External·Debug 허용, Arena·Stack 금지). 사전 작업은 capture가 사전 handle 복사본을 가지므로
//   마지막 handle이 worker에서 놓일 수 있다(사전 할당기도 같은 조건, CompressionDictionary.h).
// - 수명: source·output(또는 destination·scratch)은 작업이 terminal이 될 때까지 유효해야
//   하고 다른 코드가 건드리지 않아야 한다. source의 주소·길이는 제출 시점에 고정되며 앞선
//   의존 작업은 내용만 채울 수 있다. output은 제출 시점에 읽지 않고 body가 시작할 때 원래
//   길이를 기록하므로, 의존으로 직렬화한 여러 작업이 같은 output에 이어 붙일 수 있다.
//   source와 output 저장소의 겹침은 body가 검사해 InvalidArgument로 보고한다.
// - 취소: 협력적이다. body 시작 전과 코덱 분할 단위(1MiB)마다 확인한다. Deflate·Zlib·Gzip과
//   zstd 압축은 입력 소비나 출력 생성 1MiB마다, zstd 해제는 입력 1MiB마다 확인한다(압축률이
//   아주 높은 입력은 한 번에 큰 출력을 만들 수 있다). Lz4Frame은 압축이 입력 1MiB마다, 해제가 입력
//   소비나 출력 생성 1MiB마다 확인한다. Lz4(raw block)는 한 번의 코덱 호출을 끊지 않는다
//   (HC 12로 2GiB 가까운 입력은 100초 이상 걸릴 수 있어 Shutdown(Cancel)도 그만큼 기다린다).
//   취소를 관측하면 output 길이를 되돌리고 결과는 Cancelled다.
// - 용량: scheduler의 maxInFlight를 넘으면 제출이 CapacityExceeded다. 제출 속도는 호출자가
//   조절한다.
namespace rocks {

// 비동기 작업 한 건의 결과 보관소다. 결과 slot을 객체 안에 두므로 이동할 수 없다(Array
// 원소가 될 수 없으니 C 배열이나 UniquePtr로 둔다).
// - 소멸자는 진행 중인 작업에 취소를 요청하고 끝날 때까지 기다린다. 대기 중인 작업의 취소는
//   worker가 처리하므로, 모든 worker가 막혀 있으면 소멸자도 기다린다. 같은 scheduler의 worker
//   (다른 작업의 body·capture 정리 포함)에서 진행 중인 task를 소멸하면 기다릴 수 없으므로
//   Fatal이다. 그래서 task를 다른 작업의 capture가 소유하게 하지 않는다.
// - 같은 객체에 대한 제출·결과 조회·소멸은 한 스레드에서 한다. 다른 스레드는 Handle().Clone()
//   으로 얻은 handle로 Wait·Status를 한다(재제출이 handle을 바꾸기 때문이다).
// - terminal 이후 같은 객체로 다시 제출할 수 있다.
class CompressionTask {
public:
    CompressionTask() noexcept = default;
    ~CompressionTask() noexcept;
    CompressionTask(const CompressionTask&) = delete;
    CompressionTask& operator=(const CompressionTask&) = delete;

    // 의존 연결(Span<const WorkerTaskHandle>(&task.Handle(), 1))·Wait·RequestCancel·Clone용이다.
    // 제출 전에는 invalid handle이다.
    const WorkerTaskHandle& Handle() const noexcept {
        return handle_;
    }

    // 결과를 돌려준다. 기다리지 않는다.
    // - 제출한 적 없으면 StaleHandle, 아직 terminal이 아니면 Busy다.
    // - 실행하지 않은 작업(실행 전 취소·Shutdown(Cancel)·의존 실패)은 scheduler 상태를
    // 옮긴다(Cancelled·
    //   DependencyFailed).
    // - 실행한 작업은 결과 slot을 우선한다: 성공이면 쓴 byte 수, 실패면 동기 API와 같은 오류, 실행
    // 중 취소면
    //   Cancelled. 성공 뒤 늦게 취소가 걸려 handle 상태가 Cancelled여도 결과는 성공(데이터
    //   유효)이며, 이 handle에 의존한 후속 작업은 DependencyFailed가 된다.
    Result<usize> TryGetResult() const noexcept;

    // terminal까지 기다린 뒤 TryGetResult를 돌려준다. 시간 초과는 Busy다. 같은 scheduler의
    // worker에서 부르면 WrongThread다(WorkerTaskHandle::Wait 계약).
    Result<usize> TryWaitResult(uint32 timeoutMs = platform::InfiniteTimeout) const noexcept;

private:
    enum class Outcome : uint8 {
        NotRun,
        Completed,
        Failed,
        Cancelled
    };

    friend struct CompressionTaskAccess;

    WorkerTaskHandle handle_;
    Outcome outcome_ = Outcome::NotRun;
    ErrorCode error_ = ErrorCode::None;
    usize bytes_ = 0;
};

// TryCompressAppend를 worker에서 실행한다. 제출 실패 시 task·output은 바뀌지 않는다.
// 오류: 동기 검증 오류(겹침·Lz4 한계), 진행 중인 task Busy, 할당 실패, scheduler 거부(ShuttingDown·
// CapacityExceeded·WrongContext·InvalidArgument·OutOfMemory)를 그대로 돌려준다.
Result<void> TrySubmitCompress(WorkerScheduler& scheduler, CompressionTask& task,
                               const CompressionSettings& settings, Span<const uint8> source,
                               Array<uint8>& output,
                               Span<const WorkerTaskHandle> dependencies = {}) noexcept;

// TryDecompressAppend를 worker에서 실행한다. 계약은 TrySubmitCompress와 같다.
Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 CompressionFormat format, Span<const uint8> source,
                                 usize maxOutputBytes, Array<uint8>& output,
                                 Span<const WorkerTaskHandle> dependencies = {}) noexcept;

// 크기를 아는 TryDecompress를 worker에서 실행한다. destination과 scratch는 terminal까지 유효해야
// 한다.
Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 CompressionFormat format, Span<const uint8> source,
                                 Span<uint8> destination, Allocator& scratch,
                                 Span<const WorkerTaskHandle> dependencies = {}) noexcept;

// 사전 변형이다. 각각 사전을 쓰는 TryCompressAppend·TryDecompressAppend·TryDecompress를 worker에서
// 실행한다. 작업이 사전 handle 복사본을 가지므로 제출 뒤 호출자의 handle을 놓아도 된다. 빈 handle은
// StaleHandle이다.
Result<void> TrySubmitCompress(WorkerScheduler& scheduler, CompressionTask& task,
                               const CompressDictionary& dictionary, Span<const uint8> source,
                               Array<uint8>& output,
                               Span<const WorkerTaskHandle> dependencies = {}) noexcept;
Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 const DecompressDictionary& dictionary, Span<const uint8> source,
                                 usize maxOutputBytes, Array<uint8>& output,
                                 Span<const WorkerTaskHandle> dependencies = {}) noexcept;
Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task,
                                 const DecompressDictionary& dictionary, Span<const uint8> source,
                                 Span<uint8> destination, Allocator& scratch,
                                 Span<const WorkerTaskHandle> dependencies = {}) noexcept;

} // namespace rocks
