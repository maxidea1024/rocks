#pragma once

#include <Base/Types.h>
#include <Core/Result.h>

// 키·키 상태 저장소(SecretArena)의 모듈 내부 API다(Docs/Design/Crypto.ko.md 10.2절). 공개 경로는
// SecretBytes와 키 타입이며, libsodium·AES 경량 계층의 키 의존 상태도 이 슬롯에 둔다.
//
// - arena 하나는 [guard page][64 KiB][guard page]이고, 4 KiB chunk 16개가 처음 쓸 때 크기 등급(64
//   B–4 KiB) 하나를 맡는다.
// - 모든 arena가 차면 새 arena를 덧붙인다(최대 MaxSecretArenas개). 힙으로 넘기지 않으므로 키는
//   언제나 guard page 사이, 잠금을 시도한 페이지에 있다. 잠금은 기기 한도에 따라 일부 arena만 될 수
//   있다(GetSecretMemoryStatus가 보고한다).
// - 해제한 슬롯은 즉시 전체를 지운다. ASan 구성에서는 빈 슬롯과 요청 크기 뒤 꼬리를 poison해 해제
//   뒤 접근을 잡는다.
// - 여러 스레드에서 동시에 불러도 된다. arena는 프로세스 끝까지 해제하지 않는다.
namespace rocks::crypto::detail {

inline constexpr usize SecretSlotMaxBytes = 4096;
inline constexpr usize SecretArenaBytes = 64 * 1024;
inline constexpr usize MaxSecretArenas = 64;

// bytes(1–4096) 크기의 0으로 채운 슬롯이다. alignment는 4096 이하의 2의 거듭제곱이고, 슬롯은 등급
// 크기(최소 64 byte)로 정렬된다. 인자가 범위 밖이면 InvalidArgument, arena가 한도까지 찼거나 OS가
// 예약·commit을 거부하면 OutOfMemory다.
[[nodiscard]] Result<void*> TryAllocateSecretSlot(usize bytes, usize alignment) noexcept;

// 슬롯 전체를 지우고 돌려준다. nullptr는 아무 일도 하지 않는다. 이 저장소가 준 살아 있는 슬롯이
// 아니면 Fatal이다(키 저장소의 이중 해제·남의 주소는 결함이므로 계속하지 않는다).
void FreeSecretSlot(void* slot) noexcept;

// 시험용: index번째 arena의 사용 영역 [begin, begin + bytes)다. 앞뒤 바로 옆은 guard page다. 없으면
// false다.
[[nodiscard]] bool TryGetSecretArenaRange(usize index, const uint8*& begin, usize& bytes) noexcept;

} // namespace rocks::crypto::detail
