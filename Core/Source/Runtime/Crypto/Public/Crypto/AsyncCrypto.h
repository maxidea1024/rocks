#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Crypto/SegmentStream.h>
#include <Platform/Sync.h>
#include <Task/WorkerScheduler.h>

// WorkerScheduler 위의 비동기·병렬 세그먼트 스트림 암·복호다(Docs/Design/Crypto.ko.md 9.7절). 큰
// 버퍼 하나를 세그먼트 단위로 나눠 여러 worker 작업이 함께 처리한다. 봉인은
// SegmentStreamParallelWriter, 복호는 SegmentStreamRandomAccess의 const 함수를 쓰므로 결과 byte와
// 오류는 동기 API와 같다(세그먼트들은 헤더를 공유할 뿐 서로 독립이다).
//
// 사용 예(파일 읽기가 채운 버퍼를 이어서 복호):
//   // task는 ciphertext·plaintext보다 나중에 선언한다(먼저 소멸해야 한다).
//   crypto::SegmentCryptoTask task;
//   auto readDone = Span<const WorkerTaskHandle>(&readTask, 1);
//   auto submitted = crypto::TrySubmitSegmentDecrypt(scheduler, task, key, associatedData,
//                                                    ciphertext, plaintext, {}, readDone);
//   ...
//   auto bytes = task.TryWaitResult(); // 성공이면 plaintext 앞에 쓴 평문 byte 수
//
// 계약(AsyncCompression/AsyncCompression.h 규약과 같은 틀):
// - 작업 수: 한 제출은 worker 작업을 SegmentCryptoOptions::maxTasks개(기본 worker 수,
//   SegmentCryptoMaxTasks 이하, 세그먼트 수 이하)까지 만들고, 작업들은 다음 세그먼트를 원자적으로
//   가져가 처리한다. 첫 작업 제출이 거부되면 제출 전체가 실패하고(task·출력 불변), 그 뒤의 거부
//   (maxInFlight 등)는 작업 수만 줄인다(남은 작업이 모든 세그먼트를 처리한다).
// - 의존: dependencies는 모든 작업에 걸린다(예: AsyncFile 읽기 완료). 앞 작업이 실패·취소되면 이
//   작업들은 실행되지 않고 결과는 DependencyFailed(또는 Cancelled)다. 이 결과에 이어 붙일 작업은
//   Handles() 전체를 dependency로 준다(maxDependencies 안에 들도록 maxTasks를 정한다). 작업이
//   하나라도 성공하지 못하면 이어 붙인 작업은 실행되지 않는다.
// - 수명: 키는 제출 시점에 필요한 만큼 복사한다(암호화는 파생한 세그먼트 키, 복호는 마스터 키
// 사본).
//   그래서 제출이 돌아온 뒤 키를 버리거나 KeyRing에서 빼도 된다. 입력·출력 Span과 복호의
//   associatedData는 결과가 나올 때까지(TryWaitResult가 결과를 돌려줄 때까지) 유효해야 하고 다른
//   코드가 건드리지 않아야 한다. 주소·길이는 제출 시점에 고정되며 앞선 의존 작업은 내용만 채울 수
//   있다. 입력과 출력이 겹치면 InvalidArgument다(제자리 처리 없음).
// - 취소: 협력적이다. 작업 시작 전과 세그먼트 경계마다 확인한다. 세그먼트 하나(최대 16 MiB)를
//   처리하는 동안은 끊지 않는다. 한 작업이 취소를 보면 다른 작업도 다음 경계에서 멈춘다.
// - 실패·취소: 결과가 성공이 아니면 출력 Span 전체를 0으로 지운다(2절, 실행 전 취소·의존 실패도
//   지운다). 지우기는 마지막 작업이 끝날 때 하므로 TryWaitResult가 결과를 돌려준 뒤에는 끝나 있다.
//   한 세그먼트의 인증 실패는 전체 실패(AuthenticationFailed)다.
// - 할당: 작업 상태·body는 GetSecretAllocator()에서 할당한다(해제 때 지운다).
namespace rocks::crypto {

namespace detail {
struct SegmentCryptoAccess;
} // namespace detail

// 한 제출이 만드는 worker 작업 수의 상한이다.
inline constexpr uint32 SegmentCryptoMaxTasks = 8;

struct SegmentCryptoOptions {
    // 한 제출이 만드는 worker 작업 수의 상한이다. 0이면 scheduler의 worker 수다.
    // SegmentCryptoMaxTasks 보다 크면 그 값으로 자르고, 세그먼트 수보다 많이 만들지 않는다.
    uint32 maxTasks = 0;
};

// 비동기 세그먼트 작업 한 건의 결과 보관소다. 결과 slot과 작업 handle을 객체 안에 두므로 이동할 수
// 없다(Array 원소가 될 수 없으니 C 배열이나 UniquePtr로 둔다).
// - 소멸자는 진행 중인 작업에 취소를 요청하고 끝날 때까지 기다린다. 같은 scheduler의 worker에서
//   진행 중인 task를 소멸하면 기다릴 수 없으므로 Fatal이다.
// - 같은 객체에 대한 제출·결과 조회·취소·소멸은 한 스레드에서 한다. 다른 스레드는 Handles()의
//   Clone으로 Wait·Status를 한다.
// - 결과가 나온 뒤(모든 작업이 terminal) 같은 객체로 다시 제출할 수 있다.
class ROCKS_CRYPTO_API SegmentCryptoTask {
public:
    SegmentCryptoTask() noexcept = default;
    ~SegmentCryptoTask() noexcept;
    SegmentCryptoTask(const SegmentCryptoTask&) = delete;
    SegmentCryptoTask& operator=(const SegmentCryptoTask&) = delete;

    // 마지막 제출이 만든 worker 작업 handle들이다(이어 붙일 작업의 dependency·Wait·Status·Clone용).
    // 제출 전에는 비어 있다. 결과는 TryGetResult·TryWaitResult로 얻는다(handle 하나만 기다리면 다른
    // 작업이 아직 쓰는 중일 수 있다).
    [[nodiscard]] Span<const WorkerTaskHandle> Handles() const noexcept {
        return Span<const WorkerTaskHandle>(handles_, handleCount_);
    }

    // 모든 작업에 취소를 요청한다. 기다리지 않는다. 제출한 적 없으면 StaleHandle이다. 이미 결과가
    // 나왔으면 아무것도 바꾸지 않는다.
    Result<void> RequestCancel() const noexcept;

    // 결과를 돌려준다. 기다리지 않는다.
    // - 제출한 적 없으면 StaleHandle, 아직 끝나지 않은 작업이 있으면 Busy다.
    // - 성공이면 출력 앞에 쓴 byte 수(암호화는 헤더 포함 전체 암호문, 복호는 평문)다.
    // - 실패면 동기 API와 같은 오류, 취소면 Cancelled다. 실행되지 않은 작업 때문에 끝나지 못했으면
    //   scheduler 상태를 옮긴다(Cancelled·DependencyFailed).
    // - 모든 세그먼트를 처리한 뒤 늦게 취소가 걸려 handle 상태가 Cancelled여도 결과는 성공이다(이
    //   handle에 이어 붙인 작업은 DependencyFailed가 된다).
    [[nodiscard]] Result<usize> TryGetResult() const noexcept;

    // 모든 작업이 끝날 때까지 기다린 뒤 TryGetResult를 돌려준다. 시간 초과는 Busy다. 같은
    // scheduler의 worker에서 부르면 WrongThread다(WorkerTaskHandle::Wait 계약).
    [[nodiscard]] Result<usize>
    TryWaitResult(uint32 timeoutMs = platform::InfiniteTimeout) const noexcept;

private:
    enum class Outcome : uint8 {
        NotRun,
        Completed,
        Failed,
        Cancelled
    };

    friend struct detail::SegmentCryptoAccess;

    WorkerTaskHandle handles_[SegmentCryptoMaxTasks];
    uint32 handleCount_ = 0;
    Outcome outcome_ = Outcome::NotRun;
    ErrorCode error_ = ErrorCode::None;
    usize bytes_ = 0;
};

// plaintext 전체를 세그먼트 스트림으로 봉인해 ciphertext 앞에 쓴다(헤더 88 byte ‖ 세그먼트들).
// 결과 byte 수는 TryGetSegmentStreamCiphertextBytes(params, plaintext.Size())다. salt는 OS 난수다.
// 제출 오류(task·출력 불변): 진행 중인 task Busy → 빈 키 StaleHandle → 범위 밖 매개변수·입출력 겹침
// InvalidArgument → OS 난수 실패 DependencyFailed → 작업 상태 할당 실패 OutOfMemory → AES HW 없음
// Unsupported·평문 상한 초과 CapacityExceeded·비트맵 할당 실패 OutOfMemory
// (SegmentStreamParallelWriter::TryCreate와 같다) → ciphertext가 작으면 CapacityExceeded →
// scheduler 거부(ShuttingDown·CapacityExceeded·WrongContext·InvalidArgument).
[[nodiscard]] ROCKS_CRYPTO_API Result<void>
TrySubmitSegmentEncrypt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                        const SegmentStreamKey& key, const SegmentStreamParams& params,
                        Span<const uint8> associatedData, Span<const uint8> plaintext,
                        Span<uint8> ciphertext, const SegmentCryptoOptions& options = {},
                        Span<const WorkerTaskHandle> dependencies = {}) noexcept;

// 헤더부터 전체 암호문(ciphertext)을 검증·복호해 평문을 plaintext 앞에 쓴다. 헤더는 작업이 시작할
// 때 읽는다(앞선 의존 작업이 ciphertext를 채울 수 있다). 결과 오류는
// TryPeekSegmentHeader·TryGetSegmentLayout·SegmentStreamRandomAccess::TryOpen·TryOpenSegment와
// 같다(헤더·크기 형식 위반·다른 키 ID·commitment·세그먼트 검증 실패 AuthenticationFailed, 모르는
// 버전·알고리즘·HW 없는 AES Unsupported). plaintext가 평문보다 작으면 결과가 CapacityExceeded다.
// 제출 오류(task·출력 불변): 진행 중인 task Busy → 빈 키 StaleHandle → 입출력 겹침 InvalidArgument
// → 할당 실패 OutOfMemory → scheduler 거부.
[[nodiscard]] ROCKS_CRYPTO_API Result<void>
TrySubmitSegmentDecrypt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                        const SegmentStreamKey& key, Span<const uint8> associatedData,
                        Span<const uint8> ciphertext, Span<uint8> plaintext,
                        const SegmentCryptoOptions& options = {},
                        Span<const WorkerTaskHandle> dependencies = {}) noexcept;

} // namespace rocks::crypto
