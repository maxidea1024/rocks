#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Engine/EngineLog.h>
#include <Engine/PendingWorkRegistry.h>
#include <Localization/Text.h>
#include <Platform/Clock.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressHub.h>

namespace rocks {

namespace {

// FinishAll이 보이는 막는 진행 작업의 제목이다(UE "Compiling Shaders" 진행 창 대응).
constexpr StringView PendingFinishTitle = "Finishing background work";
constexpr usize PendingNotFound = NumericLimits<usize>::Max();

} // namespace

PendingWorkRegistry::PendingWorkRegistry(Allocator& allocator) noexcept
    : allocator_(allocator), entries_(allocator), order_(allocator),
      ownerThread_(platform::CurrentThreadId()) {
}

PendingWorkRegistry::~PendingWorkRegistry() noexcept {
    if (!entries_.IsEmpty()) {
        Log::Warning(LogEngine,
                     "Pending work registry destroyed with {Count} registered sources (modules "
                     "must unregister before they unload)",
                     static_cast<uint64>(entries_.Size()));
    }
}

Result<void> PendingWorkRegistry::AdoptCurrentThread() noexcept {
    if (!entries_.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ownerThread_ = platform::CurrentThreadId();
    return Result<void>::Success();
}

bool PendingWorkRegistry::IsOwnerThread() const noexcept {
    return platform::CurrentThreadId() == ownerThread_;
}

usize PendingWorkRegistry::FindDependency(StringView name) const noexcept {
    for (usize index = 0; index < entries_.Size(); ++index) {
        const Entry& entry = entries_[index];
        if (!entry.excludedAsDependency && entry.source->Name() == name) {
            return index;
        }
    }

    return PendingNotFound;
}

PendingWorkRegistry::OrderResult PendingWorkRegistry::RebuildOrder() noexcept {
    // RunBefore 위상 정렬이다(같은 순위는 등록 순서 — 준비된 것 중 가장 앞 등록부터 놓는다).
    const usize count = entries_.Size();
    Array<usize> order(allocator_);
    Array<bool> placed(allocator_);
    if (!order.TryReserve(count) || !placed.TryResize(count)) {
        return OrderResult::OutOfMemory;
    }

    for (usize index = 0; index < count; ++index) {
        placed[index] = false;
    }

    // 놓지 않은 의존이 남아 있지 않은지다.
    const auto ready = [this, &placed](usize index) noexcept {
        for (const StringView dependency : entries_[index].source->RunBefore()) {
            const usize found = FindDependency(dependency);
            if (found != PendingNotFound && found != index && !placed[found]) {
                return false;
            }
        }

        return true;
    };

    while (order.Size() < count) {
        bool progressed = false;
        for (usize index = 0; index < count; ++index) {
            if (!placed[index] && ready(index)) {
                placed[index] = true;
                (void)order.TryPushBack(index);
                progressed = true;
                break;
            }
        }

        if (!progressed) {
            return OrderResult::Cycle;
        }
    }

    order_ = Move(order);
    return OrderResult::Sorted;
}

Result<void> PendingWorkRegistry::Register(PendingWorkSource& source) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (source.Name().IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (shutDown_) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    bool cycle = false;
    {
        platform::LockGuard lock(mutex_);
        for (const Entry& entry : entries_) {
            if (entry.source == &source || entry.source->Name() == source.Name()) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }
        }

        Entry added;
        added.source = &source;
        if (auto pushed = entries_.TryPushBack(added); !pushed) {
            return pushed;
        }

        OrderResult ordered = RebuildOrder();
        if (ordered == OrderResult::Cycle) {
            // 앞 목록은 순환이 없었으므로 새 source로 들어오는 간선을 버리면 순환이 끊긴다(UE와
            // 같다 — 새 source 자신의 RunBefore는 그대로).
            cycle = true;
            entries_.Last().excludedAsDependency = true;
            ordered = RebuildOrder();
        }

        if (ordered != OrderResult::Sorted) {
            // order_는 그대로이므로 새 칸만 빼면 등록 전 상태다.
            (void)entries_.TryPopBack();
            return Result<void>::Failure(ordered == OrderResult::OutOfMemory
                                             ? ErrorCode::OutOfMemory
                                             : ErrorCode::InvalidArgument);
        }
    }

    // 로그는 목록 잠금 밖에서 남긴다(sink가 RemainingCount를 불러도 교착하지 않게).
    if (cycle) {
        Log::Error(LogEngine,
                   "Pending work source {Name} makes a RunBefore cycle; other sources' RunBefore "
                   "entries naming it are ignored",
                   source.Name());
    }

    return Result<void>::Success();
}

Result<void> PendingWorkRegistry::Unregister(PendingWorkSource& source) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    platform::LockGuard lock(mutex_);
    for (usize index = 0; index < entries_.Size(); ++index) {
        if (entries_[index].source != &source) {
            continue;
        }

        (void)entries_.TryRemoveAt(index);
        // 할당 없이 순서를 고친다: 그 칸을 빼고 뒤 색인을 하나씩 당긴다(위상 순서는 그대로).
        for (usize position = 0; position < order_.Size();) {
            if (order_[position] == index) {
                (void)order_.TryRemoveAt(position);
                continue;
            }

            if (order_[position] > index) {
                --order_[position];
            }

            ++position;
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

uint64 PendingWorkRegistry::RemainingCount() const noexcept {
    platform::LockGuard lock(mutex_);
    uint64 total = 0;
    for (const Entry& entry : entries_) {
        total += entry.source->RemainingCount();
    }

    return total;
}

ErrorCode PendingWorkRegistry::FinishSource(PendingWorkSource& source,
                                            ProgressTaskId parent) noexcept {
    ProgressTaskHandle part;
    if (parent != NoProgressTask) {
        // 제목은 DisplayName, 유효하지 않으면 종류 이름이다.
        Text fallback;
        const Text* title = &source.DisplayName();
        if (!title->IsValid()) {
            if (auto made = Text::Invariant(allocator_, source.Name())) {
                fallback = Move(made.Value());
                title = &fallback;
            }
        }

        if (title->IsValid()) {
            ProgressTaskDesc desc;
            desc.title = title;
            desc.parent = parent;
            if (auto begun = ProgressHub::Get().Begin(desc)) {
                part = Move(begun.Value());
            }
        }
    }

    ErrorCode failed = ErrorCode::None;
    {
        // source의 ProgressScope가 하위 작업을 채우게 이 스레드를 붙인다(실패하면 진행 없이).
        auto binding = part.IsValid()
                           ? ProgressBinding::Bind(part.Id(), ProgressBindingRole::Primary)
                           : Result<ProgressBinding>::Failure(ErrorCode::NotReady);
        if (auto finished = source.FinishAll(); !finished) {
            failed = finished.Error();
        }
    }

    if (part.IsValid()) {
        (void)part.Finish(failed == ErrorCode::None ? ProgressOutcome::Succeeded
                                                    : ProgressOutcome::Failed);
    }

    return failed;
}

Result<void> PendingWorkRegistry::FinishAll() noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    inCall_ = true;
    // 막는 진행 작업은 처음으로 할 일이 있는 source에서 연다(할 일이 없으면 만들지 않는다). hub가
    // 꺼져 있으면 진행 없이 한다.
    ProgressTaskHandle task;
    bool taskTried = false;
    ErrorCode failed = ErrorCode::None;
    for (const usize index : order_) {
        PendingWorkSource& source = *entries_[index].source;
        if (!taskTried && source.RemainingCount() != 0) {
            taskTried = true;
            if (auto title = Text::Invariant(allocator_, PendingFinishTitle)) {
                ProgressTaskDesc desc;
                desc.title = &title.Value();
                desc.presentation = ProgressPresentation::Blocking;
                if (auto begun = ProgressHub::Get().Begin(desc)) {
                    task = Move(begun.Value());
                }
            }
        }

        // 남은 수와 상관없이 부른다(UE FinishAllCompilation과 같다 — 적용 전 결과도 끝낸다).
        const ErrorCode result = FinishSource(source, task.IsValid() ? task.Id() : NoProgressTask);
        if (result != ErrorCode::None && failed == ErrorCode::None) {
            failed = result;
        }
    }

    if (task.IsValid()) {
        (void)task.Finish(failed == ErrorCode::None ? ProgressOutcome::Succeeded
                                                    : ProgressOutcome::Failed);
    }

    inCall_ = false;
    return failed == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failed);
}

Result<void> PendingWorkRegistry::FinishFor(Span<Object* const> targets) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (targets.IsEmpty()) {
        return Result<void>::Success();
    }

    inCall_ = true;
    ErrorCode failed = ErrorCode::None;
    for (const usize index : order_) {
        auto finished = entries_[index].source->FinishFor(targets);
        if (!finished && failed == ErrorCode::None) {
            failed = finished.Error();
        }
    }

    inCall_ = false;
    return failed == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failed);
}

Result<void> PendingWorkRegistry::CancelFor(Span<Object* const> targets) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (targets.IsEmpty()) {
        return Result<void>::Success();
    }

    inCall_ = true;
    for (const usize index : order_) {
        entries_[index].source->CancelFor(targets);
    }

    inCall_ = false;
    return Result<void>::Success();
}

Result<void> PendingWorkRegistry::ApplyCompleted(const PendingWorkPumpParams& params) noexcept {
    if (!IsOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (inCall_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    inCall_ = true;
    for (const usize index : order_) {
        entries_[index].source->ApplyCompleted(params);
    }

    inCall_ = false;
    return Result<void>::Success();
}

void PendingWorkRegistry::Shutdown() noexcept {
    if (!IsOwnerThread()) {
        Log::Error(LogEngine,
                   "Pending work registry Shutdown called off its owner thread; ignored");
        return;
    }

    if (inCall_) {
        Log::Error(LogEngine,
                   "Pending work registry Shutdown called during a registry call; ignored");
        return;
    }

    if (shutDown_) {
        return;
    }

    shutDown_ = true;
    inCall_ = true;
    for (const usize index : order_) {
        entries_[index].source->Shutdown();
    }

    inCall_ = false;
}

usize PendingWorkRegistry::SourceCount() const noexcept {
    return IsOwnerThread() ? order_.Size() : 0;
}

StringView PendingWorkRegistry::SourceNameAt(usize orderIndex) const noexcept {
    if (!IsOwnerThread() || orderIndex >= order_.Size()) {
        return StringView();
    }

    return entries_[order_[orderIndex]].source->Name();
}

} // namespace rocks
