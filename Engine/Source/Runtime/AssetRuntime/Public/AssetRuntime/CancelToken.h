#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

// 취소 표시다(Docs/Design/AssetSystem.ko.md 11.1절 AssetRequest::cancel, 11.6절 7). 요청을 낸 쪽이
// CancelSource를 쥐고 Cancel을 부르면, 그 Token을 받은 쪽(로더)이 다음 단계 경계에서 표시를 보고
// 그 요청만 Cancelled로 끝낸다. UE 5.8 로딩에는 개별 취소 표시가
// 없다(FStreamableHandle::CancelHandle은 관심만 놓는다) — Rocks 고유 이름이다.
//
// 사용 예:
//   auto source = CancelSource::Create(allocator);
//   if (!source) { return Result<void>::Failure(source.Error()); }
//   request.cancel = source.Value().Token();
//   auto handle = loader.Request(request);
//   ...
//   source.Value().Cancel();   // 다음 Pump가 그 요청을 Cancelled로 끝낸다
//
// 스레드: Cancel·IsCancellationRequested·Token 복사·소멸은 아무 스레드에서나 부를 수 있다(공유
// 상태는 원자 계수와 원자 표시다). 한 객체를 여러 스레드가 동시에 대입하는 것은 안 된다(일반 값과
// 같다). 수명: 공유 상태는 마지막 Source·Token이 사라질 때 만든 allocator로 돌려준다 — allocator는
// 그때까지 살아 있어야 하고 다른 스레드의 해제를 지원해야 한다.
namespace rocks {

namespace detail {
struct CancelState;
} // namespace detail

// 취소 표시를 읽는 쪽이다. 복사할 수 있고(할당 없음) 기본값은 취소될 수 없는 token이다.
class ROCKS_ASSETRUNTIME_API CancelToken {
public:
    CancelToken() noexcept = default;
    CancelToken(const CancelToken& other) noexcept;
    CancelToken(CancelToken&& other) noexcept;
    CancelToken& operator=(const CancelToken& other) noexcept;
    CancelToken& operator=(CancelToken&& other) noexcept;
    ~CancelToken() noexcept;

    // 취소가 요청됐는지다(기본 token은 언제나 false).
    bool IsCancellationRequested() const noexcept;

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
class ROCKS_ASSETRUNTIME_API CancelSource {
public:
    CancelSource() noexcept = default;
    CancelSource(CancelSource&& other) noexcept;
    CancelSource& operator=(CancelSource&& other) noexcept;
    ~CancelSource() noexcept;
    CancelSource(const CancelSource&) = delete;
    CancelSource& operator=(const CancelSource&) = delete;

    // 공유 상태를 allocator로 만든다. 오류: 할당 실패는 그 오류다.
    static Result<CancelSource> Create(Allocator& allocator) noexcept;

    // 이 source의 token이다(빈 source면 취소될 수 없는 token).
    CancelToken Token() const noexcept;

    // 취소를 요청한다. 여러 번 불러도 같다. 빈 source면 아무것도 하지 않는다.
    void Cancel() noexcept;

    bool IsCancellationRequested() const noexcept;

    bool IsValid() const noexcept {
        return state_ != nullptr;
    }

private:
    detail::CancelState* state_ = nullptr;
};

} // namespace rocks
