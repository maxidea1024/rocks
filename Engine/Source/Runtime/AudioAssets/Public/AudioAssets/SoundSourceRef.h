#pragma once

#include <Audio/DistanceAttenuation.h>
#include <AudioAssets/AttenuationTypes.h>
#include <Core/Math/Vector3.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Optional.h>
#include <Curves/TangentCurve.h>
#include <AudioAssets/SoundPcm.h>
#include <AudioAssets/SoundConcurrencyEntry.h>
#include <AudioAssets/SoundPlaybackSettings.h>
#include <AudioAssets/SoundStreamData.h>
#include <Object/AssetId.h>

// 재생 요청이 넘기는 사운드 스냅숏이다(Docs/Design/AudioSystem 3.35.3절). 재생은 시작 때의
// 스냅숏을 끝까지 쓰므로, 원본 객체가 GC되거나 다시 임포트되어도 이미 시작한 재생에 영향이 없다.
// 게임 스레드에서 만들고, 제어 pipe가 공유 참조로 받는다.
//
// 사용 예:
//   SoundPlayable playable;
//   playable.pcm = pcm;              // SharedPtr<const SoundPcm>
//   playable.looping = true;
//   auto sound = SoundSourceRef::TryCreate(allocator, playable);
//   (void)audio->Play2D(sound.Value());
namespace rocks {

// 감쇠 스냅숏이다(AttenuationSettings 값 — 3.13.1·3.42.1절). 거리는 cm, 각도는 도, 주파수는 Hz다.
// 곡선은 에셋 커브를 깊이 복사한 공유 사본이다(제어 쪽이 Registry 없이 평가한다). 쓰이는 곡선(모델·
// 방법이 Custom)은 null이면 안 된다(ValidateSoundAttenuation).
struct SoundAttenuation {
    // 거리 감쇠를 한다(UE bAttenuate).
    bool attenuate = true;
    // 리스너 방향으로 패닝한다(UE bSpatialize). false면 Direct(2D) 출력이다.
    bool spatialize = true;
    audio::DistanceAttenuationSettings distance;
    // 모양과 크기(3.42.2절 — 성분의 뜻은 SoundAttenuationShape)다.
    SoundAttenuationShape shape = SoundAttenuationShape::Sphere;
    Vec3 shapeExtents = Vec3(DefaultAttenuationShapeRadius, 0.0f, 0.0f);
    float coneOffset = 0.0f;
    float coneSphereRadius = 0.0f;
    float coneSphereFalloffDistance = 0.0f;
    // distance.model이 Custom일 때의 거리 곡선(X = falloff로 정규화한 거리)이다.
    SharedPtr<const TangentCurve> customDistanceCurve;
    // 공기 흡수(3.42.3절)다.
    bool attenuateWithLowPass = false;
    SoundAbsorptionMethod absorptionMethod = SoundAbsorptionMethod::Linear;
    bool logFrequencyScaling = false;
    float absorptionDistanceMin = DefaultAbsorptionDistanceMin;
    float absorptionDistanceMax = DefaultAbsorptionDistanceMax;
    float lowPassFrequencyAtMin = MaxAttenuationFilterFrequency;
    float lowPassFrequencyAtMax = MaxAttenuationFilterFrequency;
    float highPassFrequencyAtMin = 0.0f;
    float highPassFrequencyAtMax = 0.0f;
    SharedPtr<const TangentCurve> customLowPassAbsorptionCurve;
    SharedPtr<const TangentCurve> customHighPassAbsorptionCurve;
    // 우선순위 거리 감쇠(3.42.4절)다.
    bool enablePriorityAttenuation = false;
    SoundPriorityAttenuationMethod priorityAttenuationMethod =
        SoundPriorityAttenuationMethod::Linear;
    float priorityAttenuationMin = 1.0f;
    float priorityAttenuationMax = 1.0f;
    float priorityAttenuationDistanceMin = DefaultPriorityAttenuationDistanceMin;
    float priorityAttenuationDistanceMax = DefaultPriorityAttenuationDistanceMax;
    float manualPriorityAttenuation = 1.0f;
    SharedPtr<const TangentCurve> customPriorityAttenuationCurve;
    // 리스너 focus(3.42.5절)다.
    bool enableListenerFocus = false;
    bool enableFocusInterpolation = false;
    float focusAzimuth = DefaultFocusAzimuth;
    float nonFocusAzimuth = DefaultNonFocusAzimuth;
    float focusDistanceScale = 1.0f;
    float nonFocusDistanceScale = 1.0f;
    float focusPriorityScale = 1.0f;
    float nonFocusPriorityScale = 1.0f;
    float focusVolumeAttenuation = 1.0f;
    float nonFocusVolumeAttenuation = 1.0f;
    float focusAttackInterpSpeed = 1.0f;
    float focusReleaseInterpSpeed = 1.0f;
    // 스테레오 원본의 좌우 간격(3.42.6절)이다.
    float stereoSpread = DefaultStereoSpread;
    // UE NonSpatializedRadiusStart·End·Mode다(3.35.7·3.42.6절).
    float nonSpatializedRadiusStart = 0.0f;
    float nonSpatializedRadiusEnd = 0.0f;
    SoundNonSpatializedMode nonSpatializedMode = SoundNonSpatializedMode::OmniDirectional;
    // 차폐(3.42.7절)다.
    bool enableOcclusion = false;
    float occlusionLowPassFrequency = MaxAttenuationFilterFrequency;
    float occlusionVolumeAttenuation = 1.0f;
    float occlusionInterpolationSeconds = DefaultOcclusionInterpolationSeconds;
};

// pcm(통째 PCM)과 stream(실시간 디코드할 압축 데이터 — 3.40.3절) 중 하나만 쓴다. 채널·rate·frame
// 수는 있는 쪽에서 읽는다(ChannelCount 등 — 스트림 재생에서 pcm을 역참조하지 않게).
struct SoundPlayable {
    SharedPtr<const SoundPcm> pcm;
    SharedPtr<const SoundStreamData> stream;
    bool looping = false;
    // loop 구간이다(frame). 둘 다 0이면 clip 전체다.
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    // 사운드 기본 volume([0, MaxVoiceGain])·pitch([MinPitch, MaxPitch])다.
    float volume = 1.0f;
    float pitch = 1.0f;
    // 없으면 비공간화(2D)다.
    Optional<SoundAttenuation> attenuation;
    // 재생 우선순위([MinSoundPriority, MaxSoundPriority] — 3.41.1절)와 voice 배정 때 들리는 크기를
    // 곱하지 않을지(UE bBypassVolumeScaleForPriority)다.
    float priority = DefaultSoundPriority;
    bool bypassVolumeScaleForPriority = false;
    // loop 재생의 가상화 모드다(3.41.3절). voice를 잃었다 되찾을 때의 시작 위치도 정한다.
    SoundVirtualizationMode virtualizationMode = SoundVirtualizationMode::Restart;
    // 동시 발음 그룹이다(3.41.2절 — 앞 concurrencyCount개, 없으면 제한 없음).
    uint32 concurrencyCount = 0;
    SoundConcurrencyEntry concurrency[MaxConcurrencyGroups];
    // 진단용 원본 에셋과 revision이다(메모리 생성이면 빈 값).
    AssetId asset;
    uint64 revision = 0;

    uint32 ChannelCount() const noexcept {
        return pcm ? pcm->ChannelCount() : (stream ? stream->ChannelCount() : 0);
    }

    uint32 SampleRate() const noexcept {
        return pcm ? pcm->SampleRate() : (stream ? stream->SampleRate() : 0);
    }

    uint32 FrameCount() const noexcept {
        return pcm ? pcm->FrameCount() : (stream ? static_cast<uint32>(stream->FrameCount()) : 0u);
    }
};

class ROCKS_AUDIOASSETS_API SoundSourceRef {
public:
    SoundSourceRef() noexcept = default;

    // 스냅숏을 검사해 공유 참조로 만든다. 제어 pipe가 마지막 참조를 놓을 수 있으므로 allocator는
    // 스레드 안전해야 한다(엔진 기본 할당자·SystemAllocator — SoundPcm과 같은 조건). pcm이 없거나
    // 유한하지 않은 표본이 있거나, loop 구간이 잘못됐거나(끝 ≤ 시작, 끝 > frame 수),
    // volume·pitch·감쇠 값이 범위 밖·비유한이면 InvalidArgument다. 할당 실패는 OutOfMemory다.
    [[nodiscard]] static Result<SoundSourceRef> TryCreate(Allocator& allocator,
                                                          const SoundPlayable& playable) noexcept;

    bool IsValid() const noexcept {
        return static_cast<bool>(playable_);
    }

    // 빈 참조면 nullptr이다.
    const SoundPlayable* Get() const noexcept {
        return playable_.Get();
    }

    // 공유 참조 수다(진단·시험).
    uint64 UseCount() const noexcept;

private:
    explicit SoundSourceRef(SharedPtr<const SoundPlayable> playable) noexcept
        : playable_(Move(playable)) {
    }

    SharedPtr<const SoundPlayable> playable_;
};

// 스냅숏 검사만 한다(TryCreate와 같은 규칙). AttenuationSettings 등이 값 확인에 쓴다.
ROCKS_AUDIOASSETS_API Result<void> ValidateSoundAttenuation(const SoundAttenuation& value) noexcept;

} // namespace rocks
