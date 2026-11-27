#include "ProgressInternal.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <TraceCapture/TraceMark.h>

namespace rocks {

namespace {

using detail::ProgressBindingFrame;
using detail::ProgressTaskRecord;
using detail::ProgressWarning;

// 작업량 입력 검사(6.2절): 음수·NaN·무한은 0으로 본다.
double ScopeSanitizeWork(double work) noexcept {
    if (!platform::PlatformMath::IsFinite(work) || work < 0.0) {
        detail::ProgressWarn(ProgressWarning::InvalidWork,
                             "ProgressScope work was negative, NaN or infinite; it counts as 0");
        return 0.0;
    }

    return work;
}

uint64 ScopeNextSerial(ProgressTaskRecord& record) noexcept {
    return record.nextStepSerial.FetchAdd<platform::MemoryOrder::Relaxed>(1);
}

// 깊이 depth의 문구 칸에 text를 Clone해 넣는다(문구 잠금). 실패하면 문구만 버린다.
bool ScopeStoreMessage(ProgressTaskRecord& record, uint32 depth, const Text& text, bool step,
                       uint64 serial) noexcept {
    auto clone = text.IsValid() ? text.Clone() : Result<Text>::Failure(ErrorCode::InvalidArgument);
    if (!clone) {
        if (text.IsValid()) {
            detail::ProgressWarn(ProgressWarning::MessageClone,
                                 "A progress message could not be kept; progress continues "
                                 "without it");
        }

        return false;
    }

    Text previous;
    {
        platform::LockGuard lock(record.messageMutex);
        detail::ProgressDepthMessage& message = record.depthMessages[depth];
        Text& slot = step ? message.stepText : message.defaultText;
        previous = Move(slot);
        slot = Move(clone.Value());
        if (step) {
            message.stepTextSerial = serial;
        }
    }

    return true;
}

} // namespace

ProgressScope::ProgressScope(double totalWork) noexcept {
    Open(totalWork, nullptr);
}

ProgressScope::ProgressScope(double totalWork, const Text& defaultMessage) noexcept {
    Open(totalWork, &defaultMessage);
}

void ProgressScope::Open(double totalWork, const Text* defaultMessage) noexcept {
    detail::ProgressThreadState& thread = detail::ProgressThread;
    if (thread.depth == 0) {
        return;
    }

    ProgressBindingFrame& frame = thread.frames[thread.depth - 1];
    if (frame.record == nullptr) {
        return;
    }

    // 깊이를 넘는 구간도 취소는 본다(record_만 두고 frame_은 0 — 소멸은 아무 일도 하지 않는다).
    record_ = frame.record;
    if (frame.scopeDepth >= MaxProgressScopeDepth) {
        detail::ProgressWarn(ProgressWarning::ScopeDepth,
                             "ProgressScope nested deeper than MaxProgressScopeDepth; the deeper "
                             "scopes report nothing");
        return;
    }

    parent_ = frame.topScope;
    frame_ = thread.depth;
    depth_ = frame.scopeDepth;
    primary_ = frame.primary;
    ++frame.scopeDepth;
    frame.topScope = this;
    // 끝난 작업(Shutdown 포함)의 구간은 취소 조회만 한다(5.3절 — 끝난 뒤 보고는 no-op).
    if (record_->finished.Load<platform::MemoryOrder::Relaxed>() != 0) {
        primary_ = false;
    }

    if (!primary_) {
        return;
    }

    // 총량 0은 1로 보고 범위 전체를 한 구간으로 둔다(UE FScopedSlowTask와 같다).
    total_ = ScopeSanitizeWork(totalWork);
    if (total_ == 0.0) {
        total_ = 1.0;
        current_ = 1.0;
    }

    if (parent_ != nullptr && parent_->primary_) {
        rangeBase_ =
            parent_->rangeBase_ + parent_->rangeWidth_ * parent_->completed_ / parent_->total_;
        rangeWidth_ = parent_->rangeWidth_ * parent_->current_ / parent_->total_;
    }

    stepSerial_ = ScopeNextSerial(*record_);
    if (depth_ < MaxReportedProgressDepth) {
        record_->depthStepSerial[depth_].Store<platform::MemoryOrder::Relaxed>(stepSerial_);
        if (defaultMessage != nullptr) {
            hasDefaultMessage_ = ScopeStoreMessage(*record_, depth_, *defaultMessage, false, 0);
        }

        record_->publishedDepth.Store<platform::MemoryOrder::Relaxed>(depth_ + 1);
    }

    // trace span(6.2절): 깊이 MaxTracedProgressDepth까지, 이름은 문구 원문.
    if (depth_ < MaxTracedProgressDepth && TraceSpanChannel.IsEnabled()) {
        TraceSpanId span;
        if (defaultMessage != nullptr && defaultMessage->IsValid()) {
            if (auto name = detail::ProgressSourceText(*record_->allocator, *defaultMessage)) {
                span = TraceBeginSpan(name.Value().value.View());
            }
        }

        if (span.value == 0) {
            span = TraceBeginSpan("Progress");
        }

        spanValue_ = span.value;
        spanSerial_ = span.serial;
    }

    Publish();
}

ProgressScope::~ProgressScope() noexcept {
    if (frame_ == 0) {
        return;
    }

    detail::ProgressThreadState& thread = detail::ProgressThread;
    ProgressBindingFrame& frame = thread.frames[frame_ - 1];
    if (thread.depth != frame_ || frame.topScope != this) {
        platform::Fatal("ProgressScope closed out of order or on another thread");
    }

    frame.topScope = parent_;
    --frame.scopeDepth;
    if (!primary_) {
        return;
    }

    // 범위 소멸은 남은 양을 채운다: 범위 끝 값을 발행한다(5.5절 1).
    detail::PublishSelfProgress(*record_, ScopeNextSerial(*record_), rangeBase_ + rangeWidth_, 0.0);
    TraceEndSpan(TraceSpanId{spanValue_, spanSerial_});
    if (depth_ < MaxReportedProgressDepth) {
        if (hasDefaultMessage_ || hasStepMessage_) {
            Text defaultText;
            Text stepText;
            platform::LockGuard lock(record_->messageMutex);
            detail::ProgressDepthMessage& message = record_->depthMessages[depth_];
            defaultText = Move(message.defaultText);
            stepText = Move(message.stepText);
            message.stepTextSerial = 0;
        }

        record_->publishedDepth.Store<platform::MemoryOrder::Relaxed>(depth_);
    }

    detail::MaybePumpUi();
}

void ProgressScope::EnterStep(double work) noexcept {
    Enter(work, nullptr);
}

void ProgressScope::EnterStep(double work, const Text& message) noexcept {
    Enter(work, &message);
}

void ProgressScope::Enter(double work, const Text* message) noexcept {
    if (frame_ == 0) {
        return;
    }

    if (!primary_) {
        detail::MaybePumpUi();
        return;
    }

    if (record_->finished.Load<platform::MemoryOrder::Relaxed>() != 0) {
        return;
    }

    work = ScopeSanitizeWork(work);
    completed_ += current_;
    double remaining = total_ - completed_;
    remaining = remaining > 0.0 ? remaining : 0.0;
    if (work > remaining) {
        if (work > remaining + ProgressWorkTolerance * total_) {
            detail::ProgressWarn(ProgressWarning::StepOverflow,
                                 "ProgressScope::EnterStep went past the scope total; the step is "
                                 "clamped to the remaining work");
        }

        work = remaining;
    }

    current_ = work;
    stepSerial_ = ScopeNextSerial(*record_);
    if (depth_ < MaxReportedProgressDepth) {
        record_->depthStepSerial[depth_].Store<platform::MemoryOrder::Relaxed>(stepSerial_);
        if (message != nullptr) {
            hasStepMessage_ =
                ScopeStoreMessage(*record_, depth_, *message, true, stepSerial_) || hasStepMessage_;
        }
    }

    Publish();
    detail::MaybePumpUi();
}

void ProgressScope::Publish() noexcept {
    const double base = rangeBase_ + rangeWidth_ * completed_ / total_;
    const double width = rangeWidth_ * current_ / total_;
    detail::PublishSelfProgress(*record_, stepSerial_, base, width);
}

void ProgressScope::Tick() noexcept {
    if (frame_ != 0) {
        detail::MaybePumpUi();
    }
}

bool ProgressScope::IsCancellationRequested() const noexcept {
    return record_ != nullptr && record_->token.IsCancellationRequested();
}

Result<void> ProgressScope::CheckCancelled() const noexcept {
    return record_ != nullptr ? record_->token.CheckCancelled() : Result<void>::Success();
}

Result<ProgressCounter> ProgressScope::MakeCounter(uint64 total) noexcept {
    if (frame_ == 0 || !primary_) {
        return Result<ProgressCounter>::Success(ProgressCounter());
    }

    uint32 expected = 0;
    if (!record_->scopeCounterLive.CompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                                         platform::MemoryOrder::Acquire>(expected,
                                                                                         1u)) {
        return Result<ProgressCounter>::Failure(ErrorCode::AlreadyExists);
    }

    record_->scopeDone.Store<platform::MemoryOrder::Relaxed>(0);
    record_->scopeTotal.Store<platform::MemoryOrder::Relaxed>(total);
    record_->scopeCounterSerial.Store<platform::MemoryOrder::Release>(stepSerial_);
    record_->selfReported.Store<platform::MemoryOrder::Relaxed>(1u);
    detail::RetainRecord(record_);
    return Result<ProgressCounter>::Success(ProgressCounter(record_, ProgressCounter::Kind::Scope));
}

} // namespace rocks
