#pragma once

#include <AssetNugget/NuggetCache.h>
#include <Task/WorkerScheduler.h>

// 에셋 너겟 일괄·비동기 요청이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 12, "A3c2 구현 세부" —
// Rockpile 12.2절 형태). 항목마다 WorkerScheduler 작업 하나가 NuggetCache의 동기 Get·Put을 부르고,
// 모든 항목이 끝나면 완료 handle이 완료된다(결과는 항목 순서 그대로).
//
// 사용 예:
//   NuggetGetRequest request(allocator);           // 결과보다 오래 산다 — 이동·복사 금지
//   const NuggetGetItem items[] = {{NuggetBucketTexture, keyA}, {NuggetBucketTexture, keyB}};
//   if (auto started = request.Start(cache, items, scheduler); !started) { ... }
//   request.Handle().Wait();                       // 또는 다른 작업의 의존으로 잇는다
//   for (const NuggetCacheGetResult& result : request.Results().Value()) { ... }
//
// 계약:
// - Start가 입력을 복사한다(bucket·값 선택·inputs·meta). Put 항목의 값 버퍼는 Start가 옮겨 간다.
//   호출자 항목 배열은 Start 뒤 바로 놓아도 된다.
// - 취소(RequestCancel·NuggetCache::Shutdown)는 협력적이다: 아직 시작하지 않은 항목은 Cancelled,
//   진행 중인 항목은 끝낸다.
// - 소멸자는 진행 중이면 취소를 요청하고 완료를 기다린다. 같은 scheduler의 worker(다른 작업의
//   body·capture 정리 포함)에서 진행 중인 요청을 소멸하면 Fatal이다(기다리면 교착). 그래서 요청을
//   다른 작업의 capture가 소유하게 하지 않는다.
// - 캐시와 scheduler는 요청이 끝날 때까지 살아 있어야 한다(캐시는 진행 중 요청이 있으면 소멸할 때
//   Fatal). 같은 객체의 Start·결과 조회·소멸은 한 스레드에서 한다. 다른 스레드는 Handle().Clone()
//   으로 기다린다.
// - scheduler를 Shutdown(Cancel)으로 끝내면 완료 handle(외부 이벤트)이 항목보다 먼저 Cancelled가
//   된다(남은 항목은 worker가 capture를 정리하며 Cancelled로 끝낸다). 그 scheduler의 요청은
//   scheduler Shutdown이 돌아온 뒤에 결과를 보거나 소멸하거나 다시 Start한다. 보통은
//   NuggetCache::Shutdown을 먼저 부른다. 완료 handle을 직접 취소하지 않는다(취소는 RequestCancel —
//   직접 취소해도 같은 상황이다). 항목이 아직 진행 중 목록에 남아 있으면 다시 Start는 Busy다.
namespace rocks {

struct NuggetGetItem {
    StringView bucket{};
    Sha256Digest key{};
    Span<const uint32> values{};
    NuggetPolicy policy = NuggetPolicy::Default;
};

// Put 항목이다. values는 Start가 옮겨 간다(ValueId 오름차순).
struct NuggetPutItem {
    explicit NuggetPutItem(Allocator& allocator) noexcept : values(allocator) {
    }

    StringView bucket{};
    Sha256Digest key{};
    Array<NuggetValue> values;
    Span<const NuggetInput> inputs{};
    Span<const uint8> meta{};
    NuggetPolicy policy = NuggetPolicy::Default;
};

// 시험 hook이다: 취소가 요청될 때(RequestCancel — 진행 중 소멸 포함) 부른다(제품 코드는 걸지
// 않는다).
struct NuggetRequestHooks {
    void (*onCancel)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

// 두 요청의 공용 부분이다(직접 만들지 않는다).
class ROCKS_ASSETNUGGET_API NuggetRequest {
public:
    NuggetRequest(const NuggetRequest&) = delete;
    NuggetRequest& operator=(const NuggetRequest&) = delete;

    // 완료 handle이다(의존 연결·Wait·Clone용). Start 전에는 빈 handle이다.
    const WorkerTaskHandle& Handle() const noexcept {
        return done_.Handle();
    }

    // 아직 시작하지 않은 항목을 Cancelled로 끝내게 한다(진행 중 항목은 끝낸다). 여러 번 불러도
    // 된다.
    void RequestCancel() noexcept;

    // 취소가 요청됐는지다(RequestCancel·NuggetCache::Shutdown·진행 중 소멸).
    bool IsCancelRequested() const noexcept;

    // 시험 hook이다(Start 전에 건다).
    void SetHooks(const NuggetRequestHooks& hooks) noexcept {
        hooks_ = hooks;
    }

protected:
    // allocator는 결과 slot·입력 복사·항목 작업 capture에 쓴다(worker에서도 쓰므로 동시 할당을
    // 지원해야 한다 — System·Binned 등).
    explicit NuggetRequest(Allocator& allocator) noexcept : armed_(allocator) {
    }

    ~NuggetRequest() noexcept;

    // 진행 중이면(Start 뒤 완료 전) true다.
    bool IsRunning() const noexcept;
    // 다시 Start할 수 없으면(진행 중이거나 항목이 아직 진행 중 목록에 남음) true다.
    bool IsBusy() const noexcept;
    // 결과를 볼 수 있는지다: Start 전 StaleHandle, 완료 전 Busy.
    Result<void> CheckDone() const noexcept;
    // 진행 중이면 취소를 요청하고 완료를 기다린다(파생 소멸자가 결과 slot보다 먼저 부른다). 같은
    // scheduler worker에서 진행 중이면 Fatal.
    void WaitForCompletion() noexcept;
    // 제출을 준비한다(이벤트·등록). 종료한 캐시면 registered = false로 성공한다(항목은 Cancelled).
    Result<void> Begin(NuggetCache& cache, WorkerScheduler& scheduler, usize count, bool put,
                       bool& registered) noexcept;
    // index 항목 작업을 낸다. 실패하면 그 오류다(항목은 호출자가 끝낸다).
    Result<void> Submit(usize index) noexcept;
    // 항목 하나(또는 Start의 제출 표지)가 끝났다. 마지막이면 목록에서 빼고 완료한다.
    void FinishOne() noexcept;
    // 캐시 Get·Put이다(종료 중에도 실행한다 — drain).
    NuggetCacheGetResult CacheGet(const NuggetCacheGet& request) noexcept;
    NuggetCachePutResult CachePut(const NuggetCachePut& request) noexcept;
    // 캐시의 대기 Put byte를 예약·반환한다(상한을 넘으면 false).
    bool ReserveBytes(uint64 bytes) noexcept;
    void ReleaseBytes(uint64 bytes) noexcept;

    // 항목 실행·취소(파생이 결과 slot에 쓴다 — 끝나면 FinishOne은 부르지 않는다).
    virtual void RunItem(usize index) noexcept = 0;
    virtual void CancelItem(usize index) noexcept = 0;

    NuggetCache* cache_ = nullptr;

private:
    friend class NuggetCache;
    friend struct NuggetItemTicket;

    // worker에서 항목 하나를 실행한다(취소면 CancelItem).
    void Execute(usize index, bool cancelled) noexcept;

    WorkerScheduler* scheduler_ = nullptr;
    // 항목마다 제출이 성공해 무장했는지다(NuggetItemTicket — 원자적으로 읽고 쓴다).
    Array<uint32> armed_;
    WorkerTaskEvent done_;
    uint32 remaining_ = 0;
    uint32 cancelled_ = 0;
    bool put_ = false;
    // 캐시 진행 중 목록에 있는지다(캐시가 잠금 안에서 원자적으로 쓴다).
    uint32 registered_ = 0;
    NuggetRequestHooks hooks_{};
    NuggetRequest* newer_ = nullptr;
    NuggetRequest* older_ = nullptr;
};

class ROCKS_ASSETNUGGET_API NuggetGetRequest final : public NuggetRequest {
public:
    explicit NuggetGetRequest(Allocator& allocator) noexcept;
    ~NuggetGetRequest() noexcept;

    // items를 낸다. 오류: 진행 중 Busy, 결과 slot·입력 복사·완료 이벤트 실패는 그 오류(아무것도
    // 내지 않았다). 항목 작업 제출 실패는 그 항목과 뒤 항목의 Dropped(그 오류)이고 Start는
    // 성공한다.
    Result<void> Start(NuggetCache& cache, Span<const NuggetGetItem> items,
                       WorkerScheduler& scheduler) noexcept;

    // 항목 순서의 결과다. 오류: Start 전 StaleHandle, 완료 전 Busy.
    Result<Span<const NuggetCacheGetResult>> Results() const noexcept;

    // required 캐시에서 failed 항목이 있으면 DependencyFailed, 아니면 성공이다(Results와 같은 시점
    // 규칙).
    Result<void> Outcome() const noexcept;

private:
    struct Stored {
        explicit Stored(Allocator& allocator) noexcept : bucket(allocator), values(allocator) {
        }

        String bucket;
        Sha256Digest key{};
        Array<uint32> values;
        NuggetPolicy policy = NuggetPolicy::Default;
    };

    void RunItem(usize index) noexcept override;
    void CancelItem(usize index) noexcept override;

    Array<Stored> items_;
    Array<NuggetCacheGetResult> results_;
};

class ROCKS_ASSETNUGGET_API NuggetPutRequest final : public NuggetRequest {
public:
    explicit NuggetPutRequest(Allocator& allocator) noexcept;
    ~NuggetPutRequest() noexcept;

    // items를 낸다(값 버퍼를 옮겨 간다 — Start가 실패하면 항목에 남는다). 대기 Put byte 상한을 넘는
    // 항목은 Dropped(CapacityExceeded — 실패 아님). 그 밖 계약은 NuggetGetRequest::Start와 같다.
    Result<void> Start(NuggetCache& cache, Span<NuggetPutItem> items,
                       WorkerScheduler& scheduler) noexcept;

    Result<Span<const NuggetCachePutResult>> Results() const noexcept;
    Result<void> Outcome() const noexcept;

private:
    struct Stored {
        explicit Stored(Allocator& allocator) noexcept
            : bucket(allocator), values(allocator), inputs(allocator), meta(allocator) {
        }

        String bucket;
        Sha256Digest key{};
        Array<NuggetValue> values;
        Array<NuggetInput> inputs;
        Array<uint8> meta;
        NuggetPolicy policy = NuggetPolicy::Default;
        uint64 reserved = 0;
    };

    void RunItem(usize index) noexcept override;
    void CancelItem(usize index) noexcept override;

    Array<Stored> items_;
    Array<NuggetCachePutResult> results_;
};

} // namespace rocks
