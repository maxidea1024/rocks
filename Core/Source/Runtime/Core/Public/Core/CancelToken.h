#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

// 취소 표지다(Docs/Design/ProgressReporting 4장 — 처음 AssetSystem 11.1절 AssetRequest::cancel로
// 생겨 PG1에서 Core로 옮겼다). 요청을 낸 쪽이 CancelSource를 쥐고 Cancel을 부르면, 그 Token을 받은
// 쪽이 다음 단계 경계에서 표시를 보고 그 일만 Cancelled로 끝낸다(협조적 취소 — 강제 중단은 없다).
// UE 5.8 로딩에는 개별 취소 표시가 없다(FStreamableHandle::CancelHandle은 관심만 놓는다) — Rocks
// 고유 이름이다.
//
// 사용 예:
//   auto source = CancelSource::Create(allocator);
//   if (!source) { return Result<void>::Failure(source.Error()); }
//   request.cancel = source.Value().Token();
//   auto handle = loader.Request(request);
//   ...
//   source.Value().Cancel();   // 다음 Pump가 그 요청을 Cancelled로 끝낸다
//
//   // 작업 쪽: 단계 경계마다
//   if (auto checked = token.CheckCancelled(); !checked) { return checked; }
//
// 연결 취소: CreateLinked(allocator, parentToken)로 만든 source는 부모가 취소되면 함께 취소된
// 것으로 보인다(자식 쪽 Cancel은 부모에 닿지 않는다). 사슬 깊이는 MaxCancelChainDepth까지다.
//
// 스레드: Cancel·IsCancellationRequested·CheckCancelled·Token 복사·소멸은 아무 스레드에서나 부를
// 수 있다(공유 상태는 원자 계수와 원자 표시다). 한 객체를 여러 스레드가 동시에 대입하는 것은 안
// 된다(일반 값과 같다). 콜백 등록은 없다(임의 스레드 재진입을 부르므로 — 조회만 한다). 수명: 공유
// 상태는 마지막 Source·Token(연결된 자식이 붙잡은 부모 포함)이 사라질 때 만든 allocator로 돌려준다
// — allocator는 그때까지 살아 있어야 하고 다른 스레드의 해제를 지원해야 한다.
namespace rocks {

namespace detail {
struct CancelState;
} // namespace detail

// 연결 취소 사슬의 최대 깊이다(자기 자신 포함). 넘으면 CreateLinked가 CapacityExceeded다.
inline constexpr uint32 MaxCancelChainDepth = 8;

// 취소 표시를 읽는 쪽이다. 복사할 수 있고(할당 없음) 기본값은 취소될 수 없는 token이다.
class ROCKS_CORE_API CancelToken {
public:
    CancelToken() noexcept = default;
    CancelToken(const CancelToken& other) noexcept;
    CancelToken(CancelToken&& other) noexcept;
    CancelToken& operator=(const CancelToken& other) noexcept;
    CancelToken& operator=(CancelToken&& other) noexcept;
    ~CancelToken() noexcept;

    // 취소가 요청됐는지다. 자기 표시 또는 연결된 부모 사슬의 표시를 본다(기본 token은 언제나
    // false).
    bool IsCancellationRequested() const noexcept;

    // 취소가 요청됐으면 ErrorCode::Cancelled, 아니면 성공이다. 단계 경계에서 쓴다.
    Result<void> CheckCancelled() const noexcept;

    // 공유 상태가 있는지다(기본 token이면 false — 취소될 수 없다).
    bool CanBeCancelled() const noexcept {
        return state_ != nullptr;
    }

private:
    friend class CancelSource;

    explicit CancelToken(detail::CancelState* state) noexcept;
    void Release() noexcept;

    detail::CancelState* state_ = nullptr;
};

// 취소 표시를 쓰는 쪽이다. 이동만 할 수 있다. 소멸은 취소하지 않는다(표시는 그대로, 공유 상태
// 계수만 놓는다).
class ROCKS_CORE_API CancelSource {
public:
    CancelSource() noexcept = default;
    CancelSource(CancelSource&& other) noexcept;
    CancelSource& operator=(CancelSource&& other) noexcept;
    ~CancelSource() noexcept;
    CancelSource(const CancelSource&) = delete;
    CancelSource& operator=(const CancelSource&) = delete;

    // 공유 상태를 allocator로 만든다. 오류: 할당 실패는 그 오류다.
    static Result<CancelSource> Create(Allocator& allocator) noexcept;

    // parent에 연결된 source를 만든다: parent가 취소되면 이 source와 그 token도 취소된 것으로
    // 보인다. 자식 상태가 부모 상태를 참조 계수로 붙잡는다. parent가 기본(취소될 수 없는) token이면
    // 일반 source다. 오류: 사슬 깊이가 MaxCancelChainDepth를 넘으면 CapacityExceeded, 할당 실패는
    // 그 오류다.
    //
    // 사용 예(작업의 하위 작업):
    //   auto child = CancelSource::CreateLinked(allocator, parentSource.Token());
    static Result<CancelSource> CreateLinked(Allocator& allocator,
                                             const CancelToken& parent) noexcept;

    // 이 source의 token이다(빈 source면 취소될 수 없는 token).
    CancelToken Token() const noexcept;

    // 취소를 요청한다. 여러 번 불러도 같다. 빈 source면 아무것도 하지 않는다. 연결된 부모에는 닿지
    // 않는다.
    void Cancel() noexcept;

    // 자기 표시 또는 연결된 부모 사슬의 표시를 본다.
    bool IsCancellationRequested() const noexcept;

    bool IsValid() const noexcept {
        return state_ != nullptr;
    }

private:
    detail::CancelState* state_ = nullptr;
};

} // namespace rocks
