#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// SoundClip 임포트 옵션이다(Docs/Design/AudioSystem 3.10.2·3.36.2절). 바뀌면 재임포트한다. 에셋
// 명령 세션이 등록하는 모듈(AudioAssets)에 둔다 — 텍스처 옵션이 Texture 모듈에 있는 것과 같은
// 이유다. 필드 이름은 SoundClip 노출 속성·A2f 예약 이름과 겹치지 않는다(등록 때 A2f가 검사한다).
namespace rocks {

// normalizePeakDb의 범위(dB)다. 하한 아래는 사실상 무음으로 만드는 값이라 옵션 오류로 본다.
inline constexpr float MinNormalizePeakDb = -96.0f;
inline constexpr float MaxNormalizePeakDb = 0.0f;

// 원본 채널 처리다.
ROCKS_ENUM()
enum class SoundImportChannelHandling : uint8 {
    // 원본 채널 그대로.
    Keep = 0,
    // 채널 평균으로 mono를 만든다(mono 원본이면 하지 않는다).
    DownmixToMono = 1
};

ROCKS_STRUCT()
struct SoundClipImportOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() SoundImportChannelHandling channelHandling = SoundImportChannelHandling::Keep;
    // 파일 `smpl` loop·`cue `를 쓸지다.
    ROCKS_PROPERTY() bool loopFromFile = true;
    // 모든 채널 공통 peak를 normalizePeakDb([MinNormalizePeakDb, MaxNormalizePeakDb])로 맞춘다.
    ROCKS_PROPERTY() bool normalize = false;
    ROCKS_PROPERTY() float normalizePeakDb = -1.0f;
};

} // namespace rocks
