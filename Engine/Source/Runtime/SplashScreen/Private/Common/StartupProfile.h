#pragma once

#include <Core/Text/String.h>
#include <SplashScreen/StartupProgress.h>

// 시작 단계 보정 프로필(Docs/Design/SplashScreen 8.2절, SP5)이다. 지난 실행의 단계 시간을 읽어
// 가중치로 쓰고, 끝까지 간 시작의 시간을 지수 평균으로 섞어 다시 쓴다. StartupProgress만 쓴다.
namespace rocks::splash::detail {

// 한 대상의 저장값이다. has가 false인 단계는 저장값이 없다.
struct StartupProfileEntry {
    double seconds[StartupStepCount]{};
    bool has[StartupStepCount]{};
    uint64 samples = 0;
};

enum class StartupProfileLoad : uint8 {
    // 파일이 없다(조용히 코드 기본값).
    Missing,
    // 대상 항목을 읽었다(항목이 없으면 빈 entry).
    Loaded,
    // 깨진 JSON·버전 불일치·대상 항목 형식 오류(Warning 뒤 코드 기본값, 쓸 때 덮어쓴다). 파일
    // 전체가 유효하고 대상 항목만 깨졌으면 text에 원문이 남아 다른 대상 항목은 쓸 때 보존된다.
    Invalid,
    // 파일을 읽지 못했다(잠금·권한 같은 IO 오류 — 없음이 아니다). 이번 실행은 쓰지 않는다(다른 대상
    // 항목을 모르는 채 덮어쓰지 않게).
    ReadFailed
};

// path 파일에서 target 항목을 읽는다. 형식 버전까지 유효한 파일이면 전체 내용을 text에 남긴다(쓸 때
// 다른 대상 항목을 옮기려고).
StartupProfileLoad LoadStartupProfile(Allocator& allocator, StringView path, StringView target,
                                      StartupProfileEntry& out, String& text) noexcept;

// 밝힌 단계(included)의 가중치를 정한다(8.2절 SP5 구현 결정 — 저장값, 없는 단계는 코드 기본값 비율,
// 바닥 StartupStepWeightFloor). 밝히지 않은 단계는 0이다.
void ComputeStartupWeights(const bool (&included)[StartupStepCount],
                           const StartupProfileEntry& saved,
                           double (&weights)[StartupStepCount]) noexcept;

// 이번 시간(들어간 단계만)을 저장값과 섞는다(StartupProfileBlend). 들어가지 않은 단계는 저장값을
// 두지 않는다.
StartupProfileEntry BlendStartupProfile(const StartupProfileEntry& saved,
                                        const StartupStepTimes& measured,
                                        const bool (&entered)[StartupStepCount]) noexcept;

// path에 target 항목을 entry로 바꿔 쓴다(previousText의 다른 대상 항목은 그대로 옮긴다). 부모
// 폴더를 만들고 임시 파일 뒤 Replace한다. 오류: 형식·할당·파일 IO 오류.
Result<void> WriteStartupProfile(Allocator& allocator, StringView path, StringView target,
                                 const StartupProfileEntry& entry,
                                 StringView previousText) noexcept;

} // namespace rocks::splash::detail
