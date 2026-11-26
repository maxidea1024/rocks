#pragma once

#include <Base/Types.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

// 오디오 실행 판정이다(Docs/Design/AudioSystem 3.8.4·3.35.2절, UE FApp::CanEverRenderAudio·
// IsAudioMuted 대응). AppInfo RunTraits(AppInfoCommandLine 10.2절)가 구현되기 전의 임시 판정이며,
// 그때 이 판정을 RunTraits로 옮긴다.
namespace rocks {

struct AudioRunTraits {
    // 장치를 만든다. false면 모든 재생 요청이 Ignored다.
    bool audioOutput = true;
    // 장치·믹서는 돌고 마스터 gain만 0이다.
    bool audioMuted = false;
};

// 판정 규칙(순수 함수 — 시험에서 바로 단언한다).
// - audioOutput = false: Server 타깃, Program 타깃(commandlet·도구 — `--allow-commandlet-audio`가
//   있으면 켠다), `--no-sound`, `--benchmark`.
// - audioMuted = true: `--mute-audio`.
// - acceptDevelopmentFlags가 false(출시 구성)면 `--allow-commandlet-audio`·`--benchmark`를
// 무시한다.
// - 인자는 정확히 같은 철자만 맞춘다(`=값`을 붙인 꼴은 받지 않는다 — 값 없는 플래그다).
ROCKS_ENGINEAUDIO_API AudioRunTraits ResolveAudioRunTraits(build::TargetType target,
                                                           Span<const StringView> arguments,
                                                           bool acceptDevelopmentFlags) noexcept;

// 이 프로세스의 판정이다: build::CurrentTargetType(), ProcessCommandLine(잡지 않았으면 인자 없음),
// 출시 구성이 아니면 개발 플래그를 받는다.
ROCKS_ENGINEAUDIO_API AudioRunTraits CurrentAudioRunTraits() noexcept;

} // namespace rocks
