#pragma once

#include <Core/Result.h>

namespace rocks::physics {

// Jolt 프로세스 공유 자원의 참조다(Docs/Design/PhysicsModule 7.1.1절 PMD21, 어댑터 경계 10.4절).
// 참조 수가 0 → 1이 될 때 할당·Trace·AssertFailed hook을 걸고 VerifyJoltVersionID → Factory →
// RegisterTypes를 하며, 1 → 0이 될 때 UnregisterTypes → Factory 삭제를 한다.
// 장면(JoltPhysicsScene)이 하나씩 쥔다. Jolt 객체는 참조를 쥔 동안에만 만든다.
//
// 사용 예:
//   auto runtime = JoltRuntimeLease::Acquire();
//   if (!runtime) { return Result<void>::Failure(runtime.Error()); }
//   // ... Jolt 객체 생성·사용·파괴 ...
//   // runtime이 파괴되면 참조를 놓는다(마지막이면 Jolt 등록 해제).
//
// 스레드: Acquire·해제는 잠금으로 직렬화한다(어느 스레드에서나).
class JoltRuntimeLease {
public:
    JoltRuntimeLease() noexcept = default;
    JoltRuntimeLease(JoltRuntimeLease&& other) noexcept;
    JoltRuntimeLease& operator=(JoltRuntimeLease&& other) noexcept;
    ~JoltRuntimeLease();

    JoltRuntimeLease(const JoltRuntimeLease&) = delete;
    JoltRuntimeLease& operator=(const JoltRuntimeLease&) = delete;

    // 참조를 하나 얻는다. 오류: 라이브러리와 이 소비자의 Jolt 정의가 달라 버전 ID가 맞지 않음
    // (InvalidArgument — RegisterTypes는 불일치에서 진단 없이 abort하므로 먼저 확인한다).
    static Result<JoltRuntimeLease> Acquire() noexcept;

    bool IsHeld() const noexcept {
        return held_;
    }

    // 참조를 놓는다(쥐지 않았으면 아무것도 하지 않는다).
    void Release() noexcept;

private:
    bool held_ = false;
};

// 지금 쥔 참조 수다(시험·진단용).
uint32 JoltRuntimeReferenceCount() noexcept;

// hook으로 받아 아직 돌려주지 않은 Jolt 블록 수다(시험이 잔여 0을 확인한다).
uint64 JoltLiveAllocations() noexcept;

} // namespace rocks::physics
