#pragma once

#include <AnimationTools/AnimationClipImportSpec.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Curves/TangentCurve.h>
#include <Object/ObjectRegistry.h>
#include <UnrealExchange/ExchangeMeshSource.h>
#include <UnrealExchange/ExchangeSettings.h>

// 교환 애니메이션 클립 → 클립 임포트 명세 변환이다(Docs/Design/UnrealExchange 7.2절 — UX8b). A2f
// 임포터 unreal_anim_sequence가 문서·blob을 검사한 뒤 이 변환으로 AnimationClipImportSpec을 만들어
// ImportAnimationClip에 넘긴다.
//
// 사용 예(A2f 임포터 안):
//   ExchangeAnimationClipInput input{&asset, blobs.View(), &options->references,
//                                    &context.Registry(), context.Access()};
//   ExchangeAnimationClip clip(allocator);
//   ExchangeMeshFailure failure;
//   if (auto built = clip.Build(input, failure); !built) { ... }
//   return ImportAnimationClip(context, output, clip.Spec());
namespace rocks {

struct ExchangeAnimationClipInput {
    // 클립 교환 에셋(Ready)과 asset.blobs 순서의 검사한 byte다.
    const ExchangeAsset* asset = nullptr;
    Span<const Span<const uint8>> blobs{};
    // 참조로 걸 대상이다(스켈레톤·기준 클립). null이면 참조는 모두 비운다.
    const OrderedMap<String, AssetId>* references = nullptr;
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
};

// 변환 결과다. Spec()은 이 객체와 input.asset이 살아 있는 동안 유효하다. 복사·이동하지 않는다.
class ROCKS_UNREALEXCHANGE_API ExchangeAnimationClip {
public:
    explicit ExchangeAnimationClip(Allocator& allocator) noexcept;
    ExchangeAnimationClip(const ExchangeAnimationClip&) = delete;
    ExchangeAnimationClip& operator=(const ExchangeAnimationClip&) = delete;

    // input으로 명세를 만든다(7.2절): 설정 번역(ExchangeAnimSequenceSettings.h 표) → 원본(비율·
    // 프레임·본 트랙) → 커브·변환 커브 → 이벤트·이벤트 트랙·동기 마커 → 재생 설정.
    // replaceSettingsOnReimport는 참이다. 한 객체에 한 번만 부른다.
    //
    // 경고: 번역의 UnsupportedSetting(detail = 표 행 번호, 모르는 키 ExchangeUnknownSettingDetail),
    // PerPlatform 목표 비율(detail = PlatformTargetFrameRate 행).
    //
    // 오류(failure.code·detail): 호출 계약(asset·registry 없음, 클립이 아님, blobs 수, 두 번 부름)
    // InvalidSource / InvalidArgument, 덤프 형식(비율·프레임 — detail 0, 본 트랙·커브·변환 커브·
    // 이벤트·동기 마커 — detail = 그 배열 번호, 설정 — 행 번호) CorruptBundle / CorruptData, 번역
    // 값 종류 위반 InvalidSource / CorruptData, 코드 결함·할당 오류는 그 오류(코드 None).
    Result<void> Build(const ExchangeAnimationClipInput& input,
                       ExchangeMeshFailure& failure) noexcept;

    const AnimationClipImportSpec& Spec() const noexcept {
        return spec_;
    }

    Span<const ExchangeSettingWarning> Warnings() const noexcept {
        return warnings_.View();
    }

private:
    // 본 트랙 하나의 성분 범위다(vectors_·rotations_ 안).
    struct TrackParts {
        StringView bone;
        usize translationStart = 0;
        usize translationCount = 0;
        usize rotationStart = 0;
        usize rotationCount = 0;
        usize scaleStart = 0;
        usize scaleCount = 0;
    };

    friend struct ExchangeAnimationClipBuilder;

    Allocator* allocator_;
    bool started_ = false;
    JsonDocument translated_;
    Array<TrackParts> trackParts_;
    Array<Vec3> vectors_;
    Array<Quat> rotations_;
    Array<ClipImportTrack> tracks_;
    Array<TangentCurve> curveValues_;
    Array<ClipImportCurve> curves_;
    Array<TangentCurve> transformChannels_;
    Array<ClipImportTransformCurve> transformCurves_;
    Array<ClipImportEvent> events_;
    Array<ClipImportSyncMarker> syncMarkers_;
    Array<StringView> eventTracks_;
    Array<ExchangeSettingWarning> warnings_;
    AnimationClipImportSpec spec_;
};

} // namespace rocks
