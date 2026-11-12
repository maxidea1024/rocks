#pragma once

#include <Core/Move.h>

namespace rocks {

// 범위를 벗어날 때 정리 함수를 한 번 실행한다(UE ON_SCOPE_EXIT / TScopeGuard 대응).
// 사용 예:
//   auto close = MakeScopeExit([&]() noexcept { file.Close(); });
//   ROCKS_ON_SCOPE_EXIT { allocator.Free(buffer); };
// 계약:
// - 정리 함수는 noexcept 호출 가능해야 하며 실패를 보고할 방법이 없으므로 실패하지 않는 정리에만
// 쓴다.
// - Dismiss()는 실행을 취소한다. 성공 경로에서 정리를 생략하는 rollback 패턴에 사용한다.
// - 복사·이동할 수 없다. 생성한 범위에서만 소멸하며 역순(생성의 반대) 실행은 C++ 소멸 순서를
// 따른다.
// - 참조 캡처는 guard보다 오래 살아야 한다. 같은 guard를 여러 스레드에서 다루지 않는다.
template <typename Function>
class ScopeExit {
public:
    // 함수 객체는 값으로 받아 소유한다. lvalue 람다는 복사, 임시 람다는 이동된다.
    explicit ScopeExit(Function function) noexcept : function_(Move(function)) {
        static_assert(noexcept(function_()), "ScopeExit cleanup must be noexcept");
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() noexcept {
        if (active_) {
            function_();
        }
    }

    void Dismiss() noexcept {
        active_ = false;
    }

private:
    Function function_;
    bool active_ = true;
};

// C++17 이후 prvalue 반환은 복사·이동 없이 호출 위치에 직접 생성된다.
template <typename Function>
ScopeExit<RemoveReferenceT<Function>> MakeScopeExit(Function&& function) noexcept {
    return ScopeExit<RemoveReferenceT<Function>>(Forward<Function>(function));
}

namespace detail {

struct ScopeExitMaker {
    template <typename Function>
    ScopeExit<Function> operator+(Function function) const noexcept {
        return ScopeExit<Function>(Move(function));
    }
};

} // namespace detail

} // namespace rocks

#define ROCKS_SCOPE_EXIT_CONCAT_INNER(left, right) left##right
#define ROCKS_SCOPE_EXIT_CONCAT(left, right) ROCKS_SCOPE_EXIT_CONCAT_INNER(left, right)

// 이름 없는 guard를 만든다. 뒤에 noexcept 람다 본문과 세미콜론을 쓴다: ROCKS_ON_SCOPE_EXIT { ... };
#define ROCKS_ON_SCOPE_EXIT                                                                        \
    const auto ROCKS_SCOPE_EXIT_CONCAT(rocksScopeExit_, __COUNTER__) =                             \
        ::rocks::detail::ScopeExitMaker{} + [&]() noexcept
