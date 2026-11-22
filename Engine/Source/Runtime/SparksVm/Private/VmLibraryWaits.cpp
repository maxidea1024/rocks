#include <SparksVm/VmSession.h>

#include "VmLibrary.h"

// 대기 언어 함수다(Docs/Design/SparksRuntime.ko.md 2.4절, LC6c): Delay·NextTick·NextFixedTick·
// All·Race는 session 대기 표에 항목을 만들고 대기 handle(Ref — VmWaitTag)을 돌려준다. session 밖의
// 실행 문맥에서는 ContractViolation이다.

namespace rocks::sparks::detail {

namespace {

using W = VmLibraryContext;
constexpr uint32 VmWaitMaxChildren = VmMaxOperands;

VmFaultKind WaitPut(W& call, uint64 handle) noexcept {
    if (handle == 0) {
        return call.error != ErrorCode::None ? VmFaultKind::None : VmFaultKind::ContractViolation;
    }

    call.r[0] = handle;
    return VmFaultKind::None;
}

VmFaultKind WaitDelay(W& call) noexcept {
    // Delay(seconds, clock) — clock은 enum Clock(Game 0, UI 1).
    const uint8 clock = call.Scalars(1)[0] == 0 ? 0 : 1;
    return WaitPut(call, VmLibrary::NewWait(call, 0, clock, call.Real(0), Span<const uint64>()));
}

VmFaultKind WaitNextTick(W& call) noexcept {
    return WaitPut(call, VmLibrary::NewWait(call, 1, 0, 0.0, Span<const uint64>()));
}

VmFaultKind WaitNextFixedTick(W& call) noexcept {
    return WaitPut(call, VmLibrary::NewWait(call, 2, 0, 0.0, Span<const uint64>()));
}

// All·Race: 인자 대기(Ref 창)를 자식으로 묶는다.
VmFaultKind WaitGroup(W& call, uint8 kind) noexcept {
    uint64 children[VmWaitMaxChildren] = {};
    const uint32 count = call.signature->operandCount;
    if (count == 0 || count > VmWaitMaxChildren) {
        return VmFaultKind::ContractViolation;
    }

    for (uint32 at = 0; at < count; ++at) {
        children[at] = call.Ref(at);
    }

    return WaitPut(call,
                   VmLibrary::NewWait(call, kind, 0, 0.0, Span<const uint64>(children, count)));
}

VmFaultKind WaitAll(W& call) noexcept {
    return WaitGroup(call, 3);
}

VmFaultKind WaitRace(W& call) noexcept {
    return WaitGroup(call, 4);
}

constexpr VmLibraryEntry VmWaitTable[] = {
    {"Delay", "F,e>w", WaitDelay},
    {"NextTick", ">w", WaitNextTick},
    {"NextFixedTick", ">w", WaitNextFixedTick},
    {"All", "w*>w", WaitAll},
    {"Race", "w*>w", WaitRace},
};

} // namespace

Span<const VmLibraryEntry> VmWaitEntries() noexcept {
    return Span<const VmLibraryEntry>(VmWaitTable, sizeof(VmWaitTable) / sizeof(VmWaitTable[0]));
}

uint64 VmLibrary::NewWait(VmLibraryContext& context, uint8 kind, uint8 clock, double seconds,
                          Span<const uint64> children) noexcept {
    VmSession* session = context.execution->session_;
    if (session == nullptr) {
        return 0;
    }

    ErrorCode error = ErrorCode::None;
    const uint64 handle =
        session->NewWait(static_cast<VmSession::WaitKind>(kind), clock, seconds, children, error);
    context.error = error;
    return handle;
}

} // namespace rocks::sparks::detail
