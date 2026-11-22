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

// Soft<T>.Load(): host가 로드를 시작한 요청 handle(SparksRuntime 2.6절 — LC6d5).
VmFaultKind SoftLoad(W& call) noexcept {
    const uint64 request = VmLibrary::StartLoad(call, call.Scalars(0)[0], call.Scalars(0)[1]);
    if (request == 0) {
        return VmFaultKind::ContractViolation; // host 없음·계약 위반
    }

    call.r[0] = request;
    return VmFaultKind::None;
}

// LoadRequest<T>.IsDone: host가 끝났다고 알린 요청인지다.
VmFaultKind RequestIsDone(W& call) noexcept {
    call.s[0] = VmLibrary::LoadDone(call, call.Ref(0)) ? 1 : 0;
    return VmFaultKind::None;
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
    {"Soft.Load", "zs2>x", SoftLoad},
    {"LoadRequest.IsDone", "x>b", RequestIsDone},
};

} // namespace

Span<const VmLibraryEntry> VmWaitEntries() noexcept {
    return Span<const VmLibraryEntry>(VmWaitTable, sizeof(VmWaitTable) / sizeof(VmWaitTable[0]));
}

uint64 VmLibrary::StartLoad(VmLibraryContext& context, uint64 assetLo, uint64 assetHi) noexcept {
    VmHost* host = context.execution->link_ != nullptr ? context.execution->link_->Host() : nullptr;
    return host == nullptr ? 0 : host->StartLoad(assetLo, assetHi);
}

bool VmLibrary::LoadDone(VmLibraryContext& context, uint64 request) noexcept {
    VmHost* host = context.execution->link_ != nullptr ? context.execution->link_->Host() : nullptr;
    uint64 value = 0;
    uint64 error = 0;
    return host != nullptr && host->WatchLoad(request, value, error) == VmHost::LoadState::Ready;
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
