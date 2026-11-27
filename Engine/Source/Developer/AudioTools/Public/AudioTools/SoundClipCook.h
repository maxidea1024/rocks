#pragma once

#include <AssetCooker/CookProcessor.h>
#include <Audio/AudioPlatformData.h>
#include <AudioAssets/SoundClip.h>
#include <Core/Containers/Array.h>
#include <Core/Text/StringView.h>
#include <TargetProfiles/TargetProfile.h>

// 사운드 clip 쿠킹 처리기다(Docs/Design/AudioSystem 3.37.3절 — AU5b). SoundClip의 쿠킹 패키지를
// 만든다: codec 해석(3.37.2절) → 쿠킹 배치(layout — 인코딩 rate·frame·위치) → 너겟 조회(bucket
// audio, SCC Record) → 적중 수용 검사 또는 원본 디코드·리샘플·인코드·플랫폼 데이터 쓰기 → 쿠킹 값을
// 지역에 만든 뒤 객체 필드와 교환 → 쿠킹 패키지 쓰기 → 다시 교환(성공·실패 모두) → 미적중이면 Put.
//
// 사용 예(호스트 — 명령 동안 처리기를 쥔다):
//   SoundClipCookProcessor soundCook;
//   context.cookProcessors = {&RegisterSoundClipCookProcessor, &soundCook};
namespace rocks {

// PlatformDefault가 ADPCM을 고르는 길이 상한(초)이다(3.11.2절 — 원본 rate 기준 frameCount <
// sampleRate × ShortClipSeconds면 ADPCM, 아니면 Opus).
inline constexpr uint32 ShortClipSeconds = 1;

// 쿠킹 libopus의 고정 SIMD 수준이다(ThirdParty Opus는 SIMD 없는 float 빌드 — 3.11.4절). 툴체인
// 지문에 들어가지 않으므로 인코더 판 문자열에 넣는다. SIMD 빌드로 바꾸면 이 값을 바꾼다.
inline constexpr StringView SoundCookOpusSimdLevel = "none";

// 처리기 정체성이다(플랫폼 데이터 채우기·쿠킹 필드 배치가 바뀌면 버전 Guid를 바꾼다).
inline constexpr NuggetProcessorId SoundClipCookProcessorId{
    Guid{0x524f434b534e4443ull, 0x0000000000000001ull},
    Guid{0x524f434b534e4456ull, 0x0000000000000001ull}};

// 너겟 키 머리의 정체성이다(쿠킹 처리기 정체성과 나눈다 — R-0). 너겟 값(플랫폼 데이터 byte)의 뜻이
// 바뀌면 이 버전을 올린다.
// 버전 2: 인코드 전 clip 정규화(최대 절댓값 > 1이면 1/max — 3.39.4절, 2026-11-27).
inline constexpr NuggetProcessorId SoundNuggetProcessorId{
    Guid{0x524f434b534e4443ull, 0x0000000000000001ull},
    Guid{0x524f434b534e444eull, 0x0000000000000002ull}};

// 쿠킹 배치다(3.37.3절 4번 — 적중·미적중이 같은 값을 쓴다). 위치는 모두 인코딩 rate 기준이다.
struct ROCKS_AUDIOTOOLS_API SoundCookedLayout {
    explicit SoundCookedLayout(Allocator& allocator) noexcept : cueFrames(allocator) {
    }

    audio::AudioCodecId codec = audio::AudioCodecId::None;
    // 원본 rate와 다르면 쿠킹 리샘플러를 쓴다(Opus만).
    uint32 sampleRate = 0;
    uint32 frames = 0;
    bool resampled = false;
    // 파일 loop·loopOverride 구간이다(그 구간이 없으면 원본 값 그대로 — 둘 다 0이 아닐 수 있다).
    uint32 fileLoopBegin = 0;
    uint32 fileLoopEnd = 0;
    uint32 overrideBegin = 0;
    uint32 overrideEnd = 0;
    // 재생 loop 구간이다(EffectiveLoop 환산 — (0, 0)은 clip 전체이고 플랫폼 데이터 머리 loop도
    // 0이다).
    uint32 playLoopBegin = 0;
    uint32 playLoopEnd = 0;
    // cue마다 환산한 frame이다(cues 순서).
    Array<uint32> cueFrames;
};

// PlatformDefault 해석이다(3.37.2절): Pcm·Adpcm·Opus는 그대로, PlatformDefault는 frameCount <
// sampleRate × ShortClipSeconds면 Adpcm, 아니면 Opus(정수 비교 — 경계가 정확하다). 범위 밖 값은
// None이다.
ROCKS_AUDIOTOOLS_API audio::AudioCodecId ResolveSoundCompression(SoundCompression compression,
                                                                 uint32 sampleRate,
                                                                 uint32 frameCount) noexcept;

// 인코딩 rate다(3.39.3절, UE GetSampleRateForCompressionOverrides·GetResampleRate): Opus는
// OpusPlatformSampleRate(48 kHz), PCM16·ADPCM은 overrides.resampleForDevice가 켜졌고 quality 등급의
// 플랫폼 rate가 0이 아니면 min(플랫폼 rate, sourceRate), 아니면 sourceRate다(원본보다 높이지
// 않는다). codec None·범위 밖 quality는 sourceRate다.
ROCKS_AUDIOTOOLS_API uint32 SoundCookEncodingRate(audio::AudioCodecId codec, uint32 sourceRate,
                                                  SoundSampleRateQuality quality,
                                                  const AudioCookOverrides& overrides) noexcept;

// 실효 압축 품질 = clamp(floor(float(quality) × modifier), MinAudioCompressionQuality,
// MaxAudioCompressionQuality)다(3.39.3절, UE CalculateModifiedCompressionQuality — 곱은 UE처럼
// float로 한다, 0.7 같은 배수에서 double과 결과가 다르다). 곱이 유한하지 않으면(큰 배수) Max다.
// modifier가 유한하지 않거나 음수면 quality를 범위로 자른 값이다(프로파일 검증이 막는 값).
ROCKS_AUDIOTOOLS_API uint32 SoundCookEffectiveQuality(uint32 quality, float modifier) noexcept;

// loop 구간 하나를 인코딩 frame 수에 맞춘다(3.37.3절 4번): begin = min(begin, frames − 1),
// end = min(max(end, begin + 1), frames) — 결과는 늘 길이 1 이상이다((0, 0)도 (0, 1)이 된다). clip
// 전체 loop인지는 호출자가 환산 전 원본 값으로 정한다(환산 뒤 (0, 0)은 짧은 loop일 수 있다).
// frames가 0이면 아무것도 하지 않는다.
ROCKS_AUDIOTOOLS_API void ClampSoundCookedLoop(uint32& begin, uint32& end, uint32 frames) noexcept;

// clip 필드로 codec·encodingRate(SoundCookEncodingRate)의 쿠킹 배치를 만든다(out을 채운다).
// encodingRate가 원본 rate와 다르면 frame·위치를 CookResampledFrames·CookResampledPosition으로
// 바꾼다. 오류: codec None·채널 0·frame 0·원본 rate나 encodingRate가 [MinPlatformSampleRate,
// MaxPlatformSampleRate] 밖·Opus인데 encodingRate ≠ 48000·파일 loop나 loopOverride((0, 0)이 아닌데
// begin ≥ end거나 end > frameCount)·cue frame > frameCount InvalidArgument, 할당 실패 OutOfMemory.
ROCKS_AUDIOTOOLS_API Result<void> ComputeSoundCookedLayout(const SoundClip& clip,
                                                           audio::AudioCodecId codec,
                                                           uint32 encodingRate,
                                                           SoundCookedLayout& out) noexcept;

// 사운드 clip 쿠킹 처리기다(TypeIdOf<SoundClip>()에 등록). 한 스레드에서 쓴다(쿠킹 실행기와 같다).
// 대상 프로파일 audioCook(context.profile — null이면 기본값)으로 인코딩 rate와 실효 품질을 정한다
// (3.39절). 너겟 키에는 프로파일 id가 아니라 그 두 값이 들어간다. 인코드 전 PCM의 최대 절댓값이 1을
// 넘으면 전체를 1/max로 낮춘다(UE "Clip Normalize" — 3.39.4절).
// Cook 오류(진단 코드 — 오류):
// - 대표 객체가 SoundClip이 아님 TypeMismatch — TypeMismatch.
// - 원본 없음(source.format None·bulk 빔 — 메모리 생성 clip) InvalidSource — NotReady.
// - 채널이 2보다 많음·rate가 [MinPlatformSampleRate, MaxPlatformSampleRate] 밖
//   UnsupportedSourceFormat — Unsupported(다채널은 AU9, detail = 채널 수·rate).
// - 채널 0·frame 0(원본 사실 손상) InvalidSource — CorruptData.
// - compression 범위 밖·compressionQuality 범위 밖·loop·cue가 frame 밖 InvalidSetting —
//   InvalidArgument.
// - 원본 bulk 읽기 오류·손상 InvalidSource — 그 오류(손상 CorruptData).
// - verify 불일치 NuggetMismatch — InvalidArgument, required 너겟 캐시 실패 None — 원인 오류.
// - 리샘플·인코드·쓰기 오류(None — 그 오류), 할당 실패 OutOfMemory.
// 실패해도 에디터 객체는 원래 값이다(쿠킹 값 교환은 할당하지 않는다 — 3.37.3절 7번). 전제: 되돌리는
// 쓰기 Borrow가 얻어진다(첫 교환이 같은 Borrow를 얻었다). 그래도 실패하면 그 오류를 쓰기 오류보다
// 먼저 돌려준다(에디터 객체에 쿠킹 값이 남았다는 뜻이다).
class ROCKS_AUDIOTOOLS_API SoundClipCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return SoundClipCookProcessorId;
    }

    Result<void> Cook(CookContext& context) const noexcept override;

    // 인코드한 수다(시험 — 너겟 적중이면 오르지 않는다).
    uint64 Builds() const noexcept {
        return builds_;
    }

    // 마지막 Cook이 만든 SCC key다(시험 — 너겟 캐시가 없었으면 앞 값 그대로).
    const Sha256Digest& LastNuggetKey() const noexcept {
        return lastKey_;
    }

private:
    mutable uint64 builds_ = 0;
    mutable Sha256Digest lastKey_{};
};

// 쿠킹 처리기 등록이다(AssetCookProcessorSetup — state = SoundClipCookProcessor*, 명령 동안 산다).
// 오류: null state InvalidArgument, CookProcessorTable::Register의 오류.
ROCKS_AUDIOTOOLS_API Result<void>
RegisterSoundClipCookProcessor(void* state, CookProcessorTable& table) noexcept;

} // namespace rocks
