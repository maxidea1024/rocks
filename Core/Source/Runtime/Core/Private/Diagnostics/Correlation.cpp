#include <Core/Diagnostics/Correlation.h>

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace {

using platform::MemoryOrder;

// 스레드별 상관 id 스택이다(정적 TLS, 힙 없음). overflow는 깊이 상한을 넘겨 세기만 한 쌓기 수다.
struct CorrelationStack {
    CorrelationEntry entries[MaxCorrelationDepth];
    uint32 depth = 0;
    uint32 overflow = 0;
};

thread_local constinit CorrelationStack stack{};

// 쌓기·내리기 관찰자(원자 포인터, 시작·종료 단계에서만 바꾼다)
constinit const CorrelationObserver* observer = nullptr;

const CorrelationObserver* CurrentObserver() noexcept {
    return platform::AtomicLoad<MemoryOrder::Acquire>(&observer);
}

uint8 Push(const CorrelationEntry& source, bool restored) noexcept {
    if (stack.depth == MaxCorrelationDepth) {
        ++stack.overflow;
        return 2;
    }

    CorrelationEntry& entry = stack.entries[stack.depth++];
    entry = source;
    if (const CorrelationObserver* current = CurrentObserver();
        current != nullptr && current->onPush != nullptr) {
        current->onPush(entry, restored);
    }

    return 1;
}

} // namespace

CorrelationScope::CorrelationScope(const CorrelationIds& ids) noexcept
    : pushed_(Push(CorrelationEntry{ids, {}}, false)) {
}

CorrelationScope::CorrelationScope(const CorrelationToken& token) noexcept
    : pushed_(token.valid ? Push(token.entry, true) : 0) {
}

CorrelationScope::~CorrelationScope() noexcept {
    if (pushed_ == 0) {
        return;
    }

    if (pushed_ == 2) {
        --stack.overflow;
        return;
    }

    // 상한을 넘겨 세기만 한 쌓기가 남아 있으면 LIFO 순서가 깨진 것이다.
    if (stack.depth == 0 || stack.overflow != 0) {
        platform::Fatal("CorrelationScope destroyed out of order");
    }

    const CorrelationEntry& entry = stack.entries[stack.depth - 1];
    if (const CorrelationObserver* current = CurrentObserver();
        current != nullptr && current->onPop != nullptr) {
        current->onPop(entry);
    }

    --stack.depth;
}

CorrelationToken CaptureCorrelation() noexcept {
    if (stack.depth == 0) {
        return {};
    }

    return CorrelationToken{stack.entries[stack.depth - 1], true};
}

bool CurrentCorrelation(CorrelationIds& out) noexcept {
    if (stack.depth == 0) {
        return false;
    }

    out = stack.entries[stack.depth - 1].ids;
    return true;
}

usize CorrelationDepth() noexcept {
    return stack.depth;
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
    return stack.depth == 0 ? nullptr : &stack.entries[stack.depth - 1];
}

const CorrelationObserver* CurrentCorrelationObserver() noexcept {
    return CurrentObserver();
}

} // namespace detail

} // namespace rocks
