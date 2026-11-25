#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Error.h>

// MiniaudioOutput의 순수 도움 함수다(Docs/Design/AudioSystem 3.34.2·3.34.3절). miniaudio 타입을
// 쓰지 않아 시험이 miniaudio를 의존하지 않고 부른다(전역 상태 서드파티는 소비 모듈이 하나여야 한다
// — BD29).
namespace rocks::audio::miniaudio_support {

// ma_result 값이다(miniaudio.h와 같다 — MiniaudioSupport.cpp가 static_assert로 맞춘다).
inline constexpr int32 MaSuccess = 0;
inline constexpr int32 MaOutOfMemory = -4;
inline constexpr int32 MaDoesNotExist = -7;
inline constexpr int32 MaBusy = -19;
inline constexpr int32 MaNoBackend = -203;
inline constexpr int32 MaNoDevice = -204;

// ma_result를 ErrorCode로 바꾼다(3.34.2절 표). MaSuccess는 None이다.
ROCKS_AUDIOMINIAUDIO_API ErrorCode ErrorCodeFromMaResult(int32 result) noexcept;

// 콜백 한 번이 넘기는 frame 수의 상한이다(3.34.2절). 장치 내부 주기 period는 내부 rate 기준이라,
// 요청 rate가 다르면 miniaudio가 변환하며 한 주기에 ceil(period × clientRate / internalRate)까지
// 가져간다. 그 값 + 1을 낸다(miniaudio 변환 입력 계산의 반올림 여유). 두 rate가 같거나 0이면
// period다. uint32를 넘으면 uint32 최대다.
ROCKS_AUDIOMINIAUDIO_API uint32 CallbackFramesUpperBound(uint32 period, uint32 clientRate,
                                                         uint32 internalRate) noexcept;

// 장치 id 문자열 변환이다(3.34.3절). 모두 out에 NUL까지 쓰고 길이(NUL 제외)를 돌려준다. id는 잘리면
// 다른 장치를 가리킬 수 있으므로 out에 다 들어가지 않으면 0이다(out은 빈 문자열). 빈 입력도 0이다.
//
// WASAPI endpoint id(UTF-16, 최대 units개 — 그 안의 NUL에서 멈춘다). 짝 없는 대리 값은 U+FFFD다.
ROCKS_AUDIOMINIAUDIO_API usize EncodeUtf16DeviceId(const char16_t* text, usize units,
                                                   Span<char> out) noexcept;
// Core Audio UID(char, 최대 bytes byte — 그 안의 NUL에서 멈춘다). 그대로 옮긴다.
ROCKS_AUDIOMINIAUDIO_API usize EncodeTextDeviceId(const char* text, usize bytes,
                                                  Span<char> out) noexcept;
// AAudio(int32)·OpenSL ES(uint32) id의 10진수다.
ROCKS_AUDIOMINIAUDIO_API usize EncodeIntegerDeviceId(int64 value, Span<char> out) noexcept;

// 장치 이름(UTF-8, 최대 bytes byte — NUL에서 멈춘다)을 out에 옮긴다. 모자라면 UTF-8 문자 경계에서
// 자른다(이름은 표시용이라 잘려도 된다). 길이(NUL 제외)를 돌려준다.
ROCKS_AUDIOMINIAUDIO_API usize CopyDeviceName(const char* name, usize bytes,
                                              Span<char> out) noexcept;

// ma_allocation_callbacks 다리다(3.34.2절). userData는 Allocator*다. 할당 앞
// 머리(MiniaudioAllocationHeader byte)에 크기를 둬 realloc이 옛 크기를 안다. 실패하면
// nullptr(realloc 실패면 옛 블록은 그대로)다.
inline constexpr usize MiniaudioAllocationHeader = 16;
ROCKS_AUDIOMINIAUDIO_API void* AllocateForMiniaudio(usize bytes, void* allocator) noexcept;
ROCKS_AUDIOMINIAUDIO_API void* ReallocateForMiniaudio(void* address, usize bytes,
                                                      void* allocator) noexcept;
ROCKS_AUDIOMINIAUDIO_API void FreeForMiniaudio(void* address, void* allocator) noexcept;

} // namespace rocks::audio::miniaudio_support
