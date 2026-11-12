#pragma once

#include <Core/Result.h>

namespace rocks {

// 동적 코드의 소유권을 retain/release 콜백으로 보존한다. 빈 lease는 정적 링크 코드에 유효하다.
// context와 콜백 코드는 마지막 release가 끝날 때까지 유효해야 하며 콜백은 실패를 반환하지 않는다.
// 이 래퍼 자체는 스레드 안전하지 않다. 다른 lease 복사본을 여러 스레드에서 쓴다면
// 공유 context의 카운터와 해제 절차는 콜백 구현이 동기화해야 한다.
class ModuleLease {
public:
    using Retain = void (*)(void*) noexcept;
    using Release = void (*)(void*) noexcept;

    ModuleLease() noexcept = default;

    // 인자가 잘못되면 콜백 없이 실패하고, 성공하면 retain을 정확히 한 번 호출한다.
    static Result<ModuleLease> Acquire(void* context, Retain retain, Release release) noexcept {
        if (context == nullptr || retain == nullptr || release == nullptr) {
            return Result<ModuleLease>::Failure(ErrorCode::InvalidArgument);
        }
        ModuleLease lease;
        lease.context_ = context;
        lease.retain_ = retain;
        lease.release_ = release;
        retain(context);
        return Result<ModuleLease>::Success(static_cast<ModuleLease&&>(lease));
    }

    // 복사는 보존 횟수를 늘리고 이동은 기존 보존 횟수를 그대로 넘긴다.
    ModuleLease(const ModuleLease& other) noexcept
        : context_(other.context_), retain_(other.retain_), release_(other.release_) {
        if (context_ != nullptr) {
            retain_(context_);
        }
    }

    ModuleLease(ModuleLease&& other) noexcept
        : context_(other.context_), retain_(other.retain_), release_(other.release_) {
        other.context_ = nullptr;
        other.retain_ = nullptr;
        other.release_ = nullptr;
    }

    ModuleLease& operator=(const ModuleLease& other) noexcept {
        if (this != &other) {
            ModuleLease replacement(other);
            *this = static_cast<ModuleLease&&>(replacement);
        }
        return *this;
    }

    ModuleLease& operator=(ModuleLease&& other) noexcept {
        if (this != &other) {
            // 이전 release가 이 인스턴스에 재진입해도 새 소유권을 잃지 않도록
            // 교체를 완료한 뒤 previous의 소멸자로 이전 소유권을 반환한다.
            ModuleLease previous(static_cast<ModuleLease&&>(*this));
            context_ = other.context_;
            retain_ = other.retain_;
            release_ = other.release_;
            other.context_ = nullptr;
            other.retain_ = nullptr;
            other.release_ = nullptr;
        }
        return *this;
    }

    ~ModuleLease() noexcept {
        Reset();
    }

    void Reset() noexcept {
        void* context = context_;
        Release release = release_;
        // 콜백 전에 비워 재진입 Reset을 무해하게 만들고 새로 설치된 lease를 보존한다.
        context_ = nullptr;
        retain_ = nullptr;
        release_ = nullptr;
        if (context != nullptr) {
            release(context);
        }
    }

    explicit operator bool() const noexcept {
        return context_ != nullptr;
    }

private:
    void* context_ = nullptr;
    Retain retain_ = nullptr;
    Release release_ = nullptr;
};
} // namespace rocks
