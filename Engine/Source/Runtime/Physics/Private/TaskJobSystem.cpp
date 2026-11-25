// Jolt job → Task WorkerScheduler(Docs/Design/PhysicsModule 6.7절, 7.1.1절).
#include "TaskJobSystem.h"

#include <Core/Function.h>
#include <Core/Memory/SystemAllocator.h>
#include <Base/PlacementNew.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Task/WorkerScheduler.h>

namespace rocks::physics {

namespace {

// 제출 함수(Task body)와 미룬 목록의 저장소다. 프로세스 수명이다(소멸하지 않는 정적 저장소):
// Function 정리는 capture 소멸(QueuedJob — 남은 Task 수를 줄여 장면 파괴를 풀어 준다) 뒤에 저장소를
// 해제하므로, 저장소가 job 시스템 멤버면 장면이 먼저 사라진 뒤 worker가 해제할 수 있다(7.1.1절).
SystemAllocator& TaskAllocator() noexcept {
    alignas(SystemAllocator) static unsigned char storage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator = ::new (static_cast<void*>(storage)) SystemAllocator();
    return *allocator;
}

} // namespace

// 제출한 Task가 쥔 job 참조다. 이 객체가 파괴될 때(body 실행 뒤 또는 scheduler가 body를 건너뛰고
// capture를 정리할 때 — 취소·종료) 참조를 놓고 남은 Task 수를 줄인다. 그래서 어느 경로로 끝나도
// 참조와 수가 남지 않는다.
struct QueuedJob {
    QueuedJob(TaskJobSystem* owner, TaskJobSystem::Job* queued) noexcept
        : system(owner), job(queued) {
    }

    QueuedJob(QueuedJob&& other) noexcept : system(other.system), job(other.job) {
        other.job = nullptr;
    }

    QueuedJob(const QueuedJob&) = delete;
    QueuedJob& operator=(const QueuedJob&) = delete;
    QueuedJob& operator=(QueuedJob&&) = delete;

    ~QueuedJob() {
        if (job != nullptr) {
            job->Release();
            system->TaskFinished();
        }
    }

    TaskJobSystem* system;
    TaskJobSystem::Job* job;
};

TaskJobSystem::TaskJobSystem(WorkerScheduler* scheduler, JPH::uint maxJobs, JPH::uint maxBarriers)
    : JPH::JobSystemWithBarrier(maxBarriers), scheduler_(scheduler), deferred_(TaskAllocator()) {
    jobs_.Init(maxJobs, maxJobs);
    // worker 수 + barrier에서 기다리며 job을 실행하는 호출 thread(Jolt JobSystemThreadPool과 같다).
    if (scheduler_ != nullptr) {
        concurrency_ = static_cast<int>(scheduler_->Stats().workerCount) + 1;
    }

    if (scheduler_ != nullptr && !deferred_.TryReserve(maxJobs)) {
        // Jolt는 할당 실패를 다루지 않는다(16.4절 — 진단 후 fatal).
        platform::Fatal("Jolt job system allocation failed (out of memory)");
    }
}

TaskJobSystem::~TaskJobSystem() {
    WaitForOutstandingTasks();
}

int TaskJobSystem::GetMaxConcurrency() const {
    return concurrency_;
}

JPH::JobHandle TaskJobSystem::CreateJob(const char* name, JPH::ColorArg color,
                                        const JobFunction& function, JPH::uint32 dependencies) {
    const JPH::uint32 index = jobs_.ConstructObject(name, color, this, function, dependencies);
    if (index == JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex) {
        // Jolt가 지키는 동시 job 상한(cMaxPhysicsJobs)을 넘었다 — 계약 위반이다.
        platform::Fatal("Jolt job pool exhausted (more concurrent jobs than cMaxPhysicsJobs)");
    }

    Job* job = &jobs_.Get(index);
    // handle이 참조를 하나 쥔다. 의존이 없으면 바로 제출한다.
    JPH::JobHandle handle(job);
    if (dependencies == 0) {
        QueueJob(job);
    }

    return handle;
}

void TaskJobSystem::CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& function,
                               JPH::JobHandle* outJobs, JPH::uint32 count,
                               JPH::uint32 dependencies) {
    for (JPH::uint32 index = 0; index < count; ++index) {
        outJobs[index] = CreateJob(name, color, function, dependencies);
    }
}

void TaskJobSystem::QueueJob(Job* job) {
    if (scheduler_ != nullptr) {
        // Jolt는 QueueJob 동안만 job을 살려 두므로 Task가 쥘 참조를 더한다.
        job->AddRef();
        {
            platform::LockGuard lock(outstandingMutex_);
            ++outstanding_;
        }

        QueuedJob queued(this, job);
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            TaskAllocator(), [lease = Move(queued)](WorkerTaskContext&) mutable noexcept {
                // 결정적 FP 환경에서 실행한다(클래스 설명). 자기 job을 끝낸 뒤는 권한 범위 밖이다 —
                // 미룬 job을 돕는다.
                const bool corrected = platform::RunWithDeterministicFloatingPoint(
                    [](void* context) noexcept {
                        auto& running = *static_cast<QueuedJob*>(context);
                        running.job->Execute();
                        running.system->DrainDeferred();
                    },
                    &lease);
                if (corrected) {
                    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(
                        &lease.system->floatingPointCorrections_, uint64{1});
                }
            });
        if (body) {
            auto submitted = scheduler_->Submit(Move(body.Value()));
            if (submitted) {
                return;
            }
        }

        // 제출하지 못했다(포화·할당 실패·종료 중). 거부된 body는 이 블록 끝에서 파괴되며
        // (Function 파괴 → QueuedJob 소멸) Task 몫의 참조와 수를 되돌린다 — TryCreate가 실패했으면
        // queued를 옮겨 받은 임시 lambda capture가 그 몫을 되돌린다. 그 자리에서 실행하지 않고
        // 미룬다(클래스 설명 — 권한 범위).
        job->AddRef();
        platform::LockGuard lock(outstandingMutex_);
        ++deferredTotal_;
        if (!deferred_.TryPushBack(job)) {
            platform::Fatal(
                "Jolt deferred job list overflow (more live jobs than cMaxPhysicsJobs)");
        }

        return;
    }

    job->Execute();
}

void TaskJobSystem::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint index = 0; index < count; ++index) {
        QueueJob(jobs[index]);
    }
}

void TaskJobSystem::FreeJob(Job* job) {
    jobs_.DestructObject(job);
}

void TaskJobSystem::TaskFinished() noexcept {
    platform::LockGuard lock(outstandingMutex_);
    if (--outstanding_ == 0) {
        outstandingIdle_.NotifyAll();
    }
}

void TaskJobSystem::DrainDeferred() noexcept {
    for (;;) {
        Job* job = nullptr;
        {
            platform::LockGuard lock(outstandingMutex_);
            auto popped = deferred_.TryPopBack();
            if (!popped) {
                return;
            }

            job = popped.Value();
        }

        job->Execute();
        job->Release();
    }
}

uint64 TaskJobSystem::FloatingPointCorrections() const noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&floatingPointCorrections_);
}

uint64 TaskJobSystem::DeferredJobs() const noexcept {
    platform::LockGuard lock(outstandingMutex_);
    return deferredTotal_;
}

uint32 TaskJobSystem::OutstandingTasks() const noexcept {
    platform::LockGuard lock(outstandingMutex_);
    return outstanding_;
}

void TaskJobSystem::WaitForOutstandingTasks() noexcept {
    {
        platform::LockGuard lock(outstandingMutex_);
        while (outstanding_ != 0) {
            (void)outstandingIdle_.Wait(outstandingMutex_);
        }
    }

    // step이 끝났으므로 미룬 job은 barrier가 이미 실행했다(Execute는 아무것도 하지 않는다) —
    // 남은 참조만 놓는다.
    DrainDeferred();
}

} // namespace rocks::physics
