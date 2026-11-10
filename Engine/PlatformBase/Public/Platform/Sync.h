#pragma once

namespace rocks::platform {

// 할당 없는 native mutex wrapper이며 같은 thread의 재귀 Lock은 지원하지 않는다.
// 잠근 thread가 Unlock해야 하며, 모든 사용자와 대기자가 끝난 뒤에만 파괴한다.
// 잘못된 사용의 검출 여부나 대기 순서의 공정성은 플랫폼 공통 보장이 아니다.
class Mutex {
public:
    Mutex() noexcept;
    ~Mutex() noexcept;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void Lock() noexcept;
    void Unlock() noexcept;

private:
    // OS 타입을 공개 ABI에 노출하지 않는다. 각 backend가 크기·정렬을 static_assert한다.
    alignas(16) unsigned char storage_[64]{};
};

// Mutex 자체는 소유하지 않는다. guard보다 Mutex가 오래 살아야 하며 같은 thread에서
// guard를 파괴해야 한다. 보호 구간에서 사용자 callback을 호출할지는 상위 코드가 결정한다.
class LockGuard {
public:
    explicit LockGuard(Mutex& mutex) noexcept : mutex_(mutex) {
        mutex_.Lock();
    }

    ~LockGuard() noexcept {
        mutex_.Unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& mutex_;
};

} // namespace rocks::platform
