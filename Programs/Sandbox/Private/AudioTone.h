#pragma once

#include <Base/Types.h>
#include <Core/Memory/Allocator.h>

namespace rocks {
class ModuleLoader;
} // namespace rocks

namespace sandbox {

// 시험음 상한(초)이다.
inline constexpr rocks::uint32 MaxAudioToneSeconds = 60;
inline constexpr rocks::uint32 DefaultAudioToneSeconds = 3;

// 장치 출력 시험음이다(Docs/Design/AudioSystem 3.34.5절). 출력 backend 레지스트리를 호스트 서비스로
// 올리고 엔진 모듈 묶음을 시작해(AudioMiniaudio가 factory를 넣는다) 기본 장치로 440 Hz 사인을
// seconds초 재생한 뒤 협상값·우선순위·콜백·underrun을 로그로 남긴다.
//
// 종료 코드: 0 = 실장치로 열었고 underrun 0, 1 = 실패(열기 실패·underrun), 2 = 장치가 없어 Null로
// 열림(판정 불가 — 원래 오류를 로그에 남긴다).
int RunAudioTone(rocks::Allocator& allocator, rocks::ModuleLoader& modules,
                 rocks::uint32 seconds) noexcept;

} // namespace sandbox
