#pragma once

#include <Animation/AnimationClip.h>
#include <AnimationTools/ClipImportAdditive.h>
#include <AnimationTools/ClipImportCurve.h>
#include <AnimationTools/ClipImportEvent.h>
#include <AnimationTools/ClipImportRate.h>
#include <AnimationTools/ClipImportSyncMarker.h>
#include <AnimationTools/ClipImportTrack.h>
#include <AnimationTools/ClipImportTransformCurve.h>
#include <Core/Containers/Span.h>
#include <Object/AssetId.h>

// 애니메이션 클립 임포트 명세다(Docs/Design/MeshAnimationAssets 7.6.2절). 기본값은 AnimationClip
// 기본값과 같다. span은 ImportAnimationClip이 끝날 때까지 산다.
//
// 사용 예(임포터 Import 안):
//   const Vec3 positions[] = {...};          // frameCount + 1개
//   const ClipImportTrack tracks[] = {{"root", positions, rotations, scales}};
//   AnimationClipImportSpec spec;
//   spec.frameRate = FrameRate::TryCreate(30, 1).Value();
//   spec.frameCount = 30;
//   spec.tracks = tracks;
//   spec.skeleton = skeletonId;
//   auto output = context.Output({}, TypeIdOf<AnimationClip>());
//   return ImportAnimationClip(context, output.Value(), spec);
namespace rocks {

struct AnimationClipImportSpec {
    // 원본(7.2절)이다.
    FrameRate frameRate = FrameRate::TryCreate(30, 1).Value();
    uint32 frameCount = 0;
    Span<const ClipImportTrack> tracks{};
    Span<const ClipImportCurve> curves{};
    Span<const ClipImportTransformCurve> transformCurves{};
    // 이벤트·동기 마커·이벤트 트랙 이름이다(서비스가 정렬한다).
    Span<const ClipImportEvent> events{};
    Span<const ClipImportSyncMarker> syncMarkers{};
    Span<const StringView> eventTracks{};
    // 재생 설정이다.
    ClipInterpolation interpolation = ClipInterpolation::Linear;
    float rateScale = 1.0f;
    bool loop = false;
    RootMotionSettings rootMotion;
    ClipImportAdditive additive;
    FrameRate targetFrameRate = FrameRate::TryCreate(30, 1).Value();
    Span<const ClipImportRate> targetFrameRatePerPlatform{};
    StringView retargetSource{};
    // 참조할 스켈레톤이다(필수 — 트랙 본 이름을 그 스켈레톤과 맞춘다).
    AssetId skeleton{};
    // 원본의 animated bone attribute 수다(후속 — 0보다 크면 UnsupportedSetting 경고, 7.2절).
    uint32 attributeCount = 0;
    // 재임포트에서도 이벤트·동기 마커·이벤트 트랙·재생 설정을 명세 값으로 바꾼다. 거짓이면 보존한다
    // (UE 재임포트가 notify·설정을 지키는 것과 같다).
    bool replaceSettingsOnReimport = false;
};

} // namespace rocks
