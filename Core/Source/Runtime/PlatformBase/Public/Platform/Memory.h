#pragma once

#include <Base/Types.h>

namespace rocks::platform {

// OS/CRT 정렬 할당의 최소 경계다. bytes는 byte 수, alignment는 0이 아닌 2의 거듭제곱이다.
// 0-byte·잘못된 인자·overflow·메모리 부족은 nullptr로 반환한다.
// 실패 원인을 구분해야 하는 상위 코드는 Core Allocator의 Result 계약을 사용한다.
void* AlignedAllocate(usize bytes, usize alignment) noexcept;
// 같은 플랫폼 adapter가 반환한 살아 있는 주소만 해제한다. nullptr는 무효 연산이다.
// 이 계층에는 소유권 ledger가 없으므로 foreign pointer나 중복 해제를 검증하지 않는다.
void AlignedFree(void* address) noexcept;

// ---- byte 범위 연산(UE FMemory::Memcpy·Memmove·Memset·Memzero·Memcmp 대응) ----
// C 표준 함수(memcpy 등)를 직접 부르지 않고 이 함수를 쓴다. 컴파일러 builtin으로 내려가므로 크기가
// 상수면 인라인 명령으로 바뀐다. 모든 함수는 bytes가 0이면 포인터를 보지 않는다(null이어도 된다 —
// C 표준 함수와 달리 정의된 동작이다). bytes가 0이 아니면 두 범위 모두 유효해야 한다.

// 겹치지 않는 두 범위를 복사한다. 겹치면 MemoryMove를 쓴다.
inline void MemoryCopy(void* destination, const void* source, usize bytes) noexcept {
    if (bytes != 0) {
        __builtin_memcpy(destination, source, bytes);
    }
}

// 겹칠 수 있는 두 범위를 복사한다.
inline void MemoryMove(void* destination, const void* source, usize bytes) noexcept {
    if (bytes != 0) {
        __builtin_memmove(destination, source, bytes);
    }
}

// 범위의 모든 byte를 value로 채운다.
inline void MemorySet(void* destination, uint8 value, usize bytes) noexcept {
    if (bytes != 0) {
        __builtin_memset(destination, value, bytes);
    }
}

inline void MemoryZero(void* destination, usize bytes) noexcept {
    MemorySet(destination, 0, bytes);
}

// 사전식 byte 비교다. 같으면 0, left가 작으면 음수, 크면 양수다(부호만 의미가 있다).
inline int MemoryCompare(const void* left, const void* right, usize bytes) noexcept {
    return bytes != 0 ? __builtin_memcmp(left, right, bytes) : 0;
}

inline bool MemoryEquals(const void* left, const void* right, usize bytes) noexcept {
    return MemoryCompare(left, right, bytes) == 0;
}

// ---- 객체 표현 ----

// value의 byte 표현을 To로 다시 읽는다(표준 라이브러리 bit_cast 대응, constexpr). 크기가 같고 둘 다
// trivially copyable이어야 한다.
template <class To, class From>
constexpr To BitCast(const From& value) noexcept {
    static_assert(sizeof(To) == sizeof(From), "BitCast requires types of the same size");
    static_assert(__is_trivially_copyable(To) && __is_trivially_copyable(From),
                  "BitCast requires trivially copyable types");
    return __builtin_bit_cast(To, value);
}

// 같은 저장소에 새로 만든 객체를 가리키는 포인터를 얻는다(표준 라이브러리 launder 대응). placement
// new로 저장소를 다시 쓴 뒤 이전 포인터·저장소 주소로 새 객체에 접근할 때 쓴다.
template <class T>
constexpr T* Launder(T* pointer) noexcept {
    return __builtin_launder(pointer);
}

// operator&를 재정의한 타입에서도 실제 주소를 얻는다(표준 라이브러리 addressof 대응).
template <class T>
constexpr T* AddressOf(T& value) noexcept {
    return __builtin_addressof(value);
}

// ---- NUL 종료 C 문자열 ----
// 길이를 이미 아는 곳(StringView 등)에서는 쓰지 않는다. text는 null이 아니어야 한다.

// NUL 앞까지의 byte 수다(strlen 대응, constexpr).
constexpr usize CStringLength(const char* text) noexcept {
    return __builtin_strlen(text);
}

// byte 단위 사전식 비교다(strcmp 대응). 같으면 0, left가 작으면 음수, 크면 양수다.
constexpr int CStringCompare(const char* left, const char* right) noexcept {
    return __builtin_strcmp(left, right);
}

constexpr bool CStringEquals(const char* left, const char* right) noexcept {
    return CStringCompare(left, right) == 0;
}

} // namespace rocks::platform
