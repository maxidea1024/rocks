#include <Core/Diagnostics/Correlation.h>

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace detail {

namespace {

// 이 스레드 스택의 깊이다(CorrelationDepth가 읽는다).
thread_local constinit uint32 correlationDepth = 0;

} // namespace

} // namespace detail

usize CorrelationDepth() noexcept {
    return detail::correlationDepth;
}

namespace {

using platform::MemoryOrder;

// 스레드별 상관 id 스택 칸이다(정적 TLS, 힙 없음). overflow는 깊이 상한을 넘겨 세기만 한 쌓기
// 수다.
struct CorrelationStack {
    CorrelationEntry entries[MaxCorrelationDepth];
    uint32 overflow = 0;
};

thread_local constinit CorrelationStack stack{};

// 쌓기·내리기 관찰자(원자 포인터, 시작·종료 단계에서만 바꾼다)
constinit const CorrelationObserver* observer = nullptr;

const CorrelationObserver* CurrentObserver() noexcept {
    return platform::AtomicLoad<MemoryOrder::Acquire>(&observer);
}

} // namespace

namespace detail {

CorrelationPushState PushCorrelation(const CorrelationEntry& source, bool restored) noexcept {
    uint32& depth = correlationDepth;
    if (depth == MaxCorrelationDepth) {
        ++stack.overflow;
        return CorrelationPushState::Counted;
    }

    CorrelationEntry& entry = stack.entries[depth++];
    entry = source;
    // 쌓기 칸 확장(CorrelationPushExtension)은 쌓기마다 0에서 시작한다(사본에 실려 다니지 않는다).
    entry.ext[CorrelationPushExtension] = 0;
    if (const CorrelationObserver* current = CurrentObserver();
        current != nullptr && current->onPush != nullptr) {
        current->onPush(entry, restored);
    }

    return CorrelationPushState::Pushed;
}

void PopCorrelation(CorrelationPushState state) noexcept {
    if (state == CorrelationPushState::Counted) {
        --stack.overflow;
        return;
    }

    // 상한을 넘겨 세기만 한 쌓기가 남아 있으면 LIFO 순서가 깨진 것이다.
    uint32& depth = correlationDepth;
    if (state != CorrelationPushState::Pushed || depth == 0 || stack.overflow != 0) {
        platform::Fatal("CorrelationScope destroyed out of order");
    }

    const CorrelationEntry& entry = stack.entries[depth - 1];
    if (const CorrelationObserver* current = CurrentObserver();
        current != nullptr && current->onPop != nullptr) {
        current->onPop(entry);
    }

    --depth;
}

} // namespace detail

CorrelationToken CaptureCorrelation() noexcept {
    const uint32 depth = detail::correlationDepth;
    if (depth == 0) {
        return {};
    }

    return CorrelationToken{stack.entries[depth - 1], true};
}

bool CurrentCorrelation(CorrelationIds& out) noexcept {
    const uint32 depth = detail::correlationDepth;
    if (depth == 0) {
        return false;
    }

    out = stack.entries[depth - 1].ids;
    return true;
}

Result<void> SetCorrelationObserver(const CorrelationObserver* next) noexcept {
    if (next == nullptr) {
        platform::AtomicStore<MemoryOrder::Release>(&observer, next);
        return Result<void>::Success();
    }

    const CorrelationObserver* expected = nullptr;
    if (platform::AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &observer, expected, next) ||
        expected == next) {
        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::AlreadyExists);
}

namespace detail {

CorrelationEntry* TopCorrelationEntry() noexcept {
    return correlationDepth == 0 ? nullptr : &stack.entries[correlationDepth - 1];
}

const CorrelationObserver* CurrentCorrelationObserver() noexcept {
    return CurrentObserver();
}

} // namespace detail

} // namespace rocks
