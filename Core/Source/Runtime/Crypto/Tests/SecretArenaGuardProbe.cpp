#include <Crypto/Private/SecretArena.h>

#include <stdio.h>

namespace rocks::test {

bool PrepareVirtualMemoryAccessProbe() noexcept;

// 독립 프로세스 전용이다. 키 저장소 첫 arena 바로 앞(after=false) 또는 바로 뒤(after=true)의 guard
// page를 읽는다. 접근 위반으로 끝나야 하며, 돌아오면 guard page가 없다는 뜻이다(종료 코드 6). 준비
// 실패는 2–5로 따로 알린다.
int RunSecretArenaGuardProbe(bool after) noexcept {
    if (!PrepareVirtualMemoryAccessProbe()) {
        return 2;
    }

    auto slot = crypto::detail::TryAllocateSecretSlot(64, 64);
    if (!slot) {
        return 3;
    }

    const uint8* begin = nullptr;
    usize bytes = 0;
    if (!crypto::detail::TryGetSecretArenaRange(0, begin, bytes)) {
        return 4;
    }

    // 부모가 준비 완료를 확인한 뒤 접근 위반을 기대한다.
    if (fprintf(stderr, "VM_ACCESS_READY\n") < 0 || fflush(stderr) != 0) {
        return 5;
    }

    const uptr address =
        after ? reinterpret_cast<uptr>(begin) + bytes : reinterpret_cast<uptr>(begin) - 1;
    const uint8 value = *reinterpret_cast<const volatile uint8*>(address);
    (void)value;
    return 6;
}

// 독립 프로세스 전용이다. 해제한 키 슬롯을 읽는다. Asan 구성에서는 해제한 슬롯을 poison하므로
// use-after-poison 보고로 끝나야 한다. 그 밖의 구성에서는 해제 때 지운 0을 읽고 0을 돌려준다(남아
// 있으면 7).
int RunSecretSlotUseAfterFreeProbe() noexcept {
    auto slot = crypto::detail::TryAllocateSecretSlot(32, 64);
    if (!slot) {
        return 3;
    }

    auto* bytes = static_cast<uint8*>(slot.Value());
    for (usize i = 0; i < 32; ++i) {
        bytes[i] = 0xA5;
    }

    crypto::detail::FreeSecretSlot(bytes);
    const volatile uint8* stale = bytes;
    for (usize i = 0; i < 32; ++i) {
        if (stale[i] != 0) {
            return 7;
        }
    }

    return 0;
}

// 독립 프로세스 전용이다. 32 byte 요청(64 byte 등급 슬롯)의 요청 크기 바로 뒤 byte를 읽는다. Asan
// 구성에서는 꼬리를 poison하므로 use-after-poison 보고로 끝나야 한다. 그 밖의 구성에서는 꼬리가
// 0이므로 0을 돌려준다.
int RunSecretSlotTailOverflowProbe() noexcept {
    auto slot = crypto::detail::TryAllocateSecretSlot(32, 64);
    if (!slot) {
        return 3;
    }

    const volatile uint8* bytes = static_cast<const uint8*>(slot.Value());
    const uint8 tail = bytes[32];
    crypto::detail::FreeSecretSlot(slot.Value());
    return tail == 0 ? 0 : 7;
}

} // namespace rocks::test
