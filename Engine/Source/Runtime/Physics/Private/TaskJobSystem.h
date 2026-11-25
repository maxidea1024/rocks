#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Platform/Sync.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
// clang-format on

namespace rocks {

class WorkerScheduler;

namespace physics {

// Jolt job을 Task WorkerScheduler에서 실행하는 job 시스템이다(Docs/Design/PhysicsModule 6.7절,
// 7.1.1절 PMD20). barrier는 JobSystemWithBarrier가 맡고, 이 클래스는 job 생성·해제·제출만 한다.
//
// - job은 미리 잡은 고정 목록(maxJobs — Jolt cMaxPhysicsJobs)에서 만든다. Jolt가 한 PhysicsSystem의
//   동시 job 수를 그 아래로 지킨다(넘으면 Fatal).
// - QueueJob은 job 참조를 하나 더 쥐고 scheduler에 Execute → Release를 제출한다. scheduler가 없으면
//   제출 thread에서 바로 Execute한다(Jolt JobSystemSingleThreaded와 같다).
// - 제출이 실패하면(포화·할당 실패·종료 중) 바로 실행하지 않고 미룬 job 목록에 둔다. Jolt job
//   함수는 자기 body 접근 권한 범위(BodyAccess::Grant) 안에서 다음 job의 의존을 풀어 QueueJob을
//   부르므로, 그 자리에서 실행하면 권한 범위가 겹쳐 Jolt assert가 난다(PhysicsModule 7.1.1절). 미룬
//   job은 Jolt barrier를 기다리는 thread가 실행하고(WaitForJobs는 barrier의 실행 가능한 job을 대기
//   thread에서 실행한다 — step의 job은 모두 barrier에 있다), worker도 자기 job을 끝낸 뒤 비운다.
// - worker는 job을 결정적 부동소수점 환경(platform::RunWithDeterministicFloatingPoint)에서
// 실행한다.
//   Jolt는 모든 실행 thread의 FP 상태가 같아야 한다(어댑터 경계 10.4절 — 다르면 broadphase bounds가
//   어긋난다). 장면 step도 같은 환경에서 돈다(JoltPhysicsScene::Step).
// - Jolt WaitForJobs가 끝나도 제출한 Task가 아직 Release하지 않았을 수 있으므로 남은 Task 수를
//   세고, 소멸자는 그 수가 0이 될 때까지 기다린다(조건 변수 — sleep 없음).
//
// 스레드: CreateJob·QueueJob은 Jolt가 여러 thread에서 부른다. 생성·파괴는 장면 소유 thread다.
class TaskJobSystem final : public JPH::JobSystemWithBarrier {
public:
    // scheduler는 빌린다(nullptr이면 제출 thread 실행 — GetMaxConcurrency 1). scheduler는 이
    // 객체보다 오래 살아야 한다.
    TaskJobSystem(WorkerScheduler* scheduler, JPH::uint maxJobs, JPH::uint maxBarriers);
    ~TaskJobSystem() override;

    TaskJobSystem(const TaskJobSystem&) = delete;
    TaskJobSystem& operator=(const TaskJobSystem&) = delete;

    int GetMaxConcurrency() const override;
    JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                             JPH::uint32 dependencies = 0) override;
    void CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& function,
                    JPH::JobHandle* outJobs, JPH::uint32 count,
                    JPH::uint32 dependencies = 0) override;

    // 제출에 실패해 미룬 job 수다(누적 — 시험·진단).
    uint64 DeferredJobs() const noexcept;

    // worker가 job을 실행하기 전에 FP 환경을 결정적 환경으로 바꿔야 했던 횟수다(누적 — 진단: worker
    // 환경을 누가 바꿔 놓았는지).
    uint64 FloatingPointCorrections() const noexcept;

    // 제출해 아직 Release하지 않은 Task 수다(시험·진단).
    uint32 OutstandingTasks() const noexcept;

    // 남은 Task가 모두 끝날 때까지 기다리고 미룬 job 목록을 비운다(step 뒤·장면 파괴 전 — 소멸자도
    // 부른다). step 밖에서만 부른다.
    void WaitForOutstandingTasks() noexcept;

protected:
    friend struct QueuedJob;

    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    void TaskFinished() noexcept;

    // 미룬 job을 하나씩 꺼내 실행하고 참조를 놓는다(이미 실행된 job은 Execute가 아무것도 하지
    // 않는다). job 실행 밖(권한 범위 밖)에서만 부른다.
    void DrainDeferred() noexcept;

    WorkerScheduler* scheduler_ = nullptr;
    // 생성 때 고정한 동시 실행 수다(Jolt가 한 Update 안에서 여러 번 읽는다 — step 도중 scheduler
    // 상태가 바뀌어도 job 분할이 어긋나지 않게).
    int concurrency_ = 1;
    JPH::FixedSizeFreeList<Job> jobs_;
    mutable platform::Mutex outstandingMutex_;
    platform::ConditionVariable outstandingIdle_;
    // outstandingMutex_가 보호한다.
    uint32 outstanding_ = 0;
    // 미룬 job(참조를 하나씩 쥔다)이다. maxJobs만큼 미리 잡는다(살아 있는 job 수보다 많을 수 없다).
    Array<Job*> deferred_;
    uint64 deferredTotal_ = 0;
    // 원자 연산으로만 읽고 쓴다.
    uint64 floatingPointCorrections_ = 0;
};

} // namespace physics

} // namespace rocks
