#include <AnimationTools/AnimationClipImport.h>

#include <Animation/AnimationClip.generated.h>
#include <Animation/ClipSource.h>
#include <Animation/Skeleton.generated.h>
#include <AssetDatabase/AssetImportBulk.h>
#include <Core/Text/NamePool.h>
#include <Object/ObjectRegistry.h>

// 애니메이션 클립 임포트 서비스다(Docs/Design/MeshAnimationAssets 7.6.2절 — MA6b2).
namespace rocks {
// 유니티 빌드에서 다른 임포트 서비스의 같은 이름 도우미와 겹치지 않게 파일 전용 namespace에 둔다.
namespace animation_clip_import {

// 참조의 TypeMismatch detail이다(스켈레톤 0, 기준 클립 1).
constexpr uint64 SkeletonPosition = 0;
constexpr uint64 BasePoseClipPosition = 1;

Result<void> Invalid(AssetImportContext& context, uint64 detail,
                     ErrorCode error = ErrorCode::InvalidArgument) noexcept {
    return context.Fail(AssetDiagnosticCode::InvalidSetting, error, detail);
}

Result<void> OutOfMemory(AssetImportContext& context) noexcept {
    return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory, 0);
}

// 클립 진단을 문맥 실패로 옮긴다(할당 실패는 진단 코드 없이 — detail = index, 없으면 0).
Result<void> ClipFail(AssetImportContext& context, const ClipDiagnostic& diagnostic,
                      ErrorCode error, ClipDiagnostic* out) noexcept {
    if (out != nullptr) {
        *out = diagnostic;
    }

    const uint64 detail = diagnostic.index == ClipNoIndex ? 0 : diagnostic.index;
    return context.Fail(error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None
                                                        : diagnostic.code,
                        error, detail);
}

// text를 intern한다(실패는 InvalidSetting과 원래 오류).
Result<Name> InternChecked(AssetImportContext& context, NamePool& names, StringView text,
                           uint64 detail) noexcept {
    auto name = names.TryIntern(text);
    if (!name) {
        (void)Invalid(context, detail, name.Error());
    }

    return name;
}

// id의 대표 객체를 열고 그 타입이 expected 파생인지 본다(null id는 비움).
Result<ObjectHandle> OpenTyped(AssetImportContext& context, AssetId id, TypeId expected,
                               uint64 position) noexcept {
    if (id.IsNull()) {
        return Result<ObjectHandle>::Success(ObjectHandle{});
    }

    auto opened = context.OpenAsset(id);
    if (!opened) {
        return opened;
    }

    auto type = context.Registry().AcquireType(context.Access(), opened.Value());
    if (!type) {
        (void)context.Fail(AssetDiagnosticCode::None, type.Error(), position);
        return Result<ObjectHandle>::Failure(type.Error());
    }

    if (!type.Value().IsA(expected)) {
        (void)context.Fail(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch, position);
        return Result<ObjectHandle>::Failure(ErrorCode::TypeMismatch);
    }

    return opened;
}

template <class T>
Result<void> CopySpan(Span<const T> source, Array<T>& out) noexcept {
    out.Clear();
    return out.TryAppend(source);
}

// 명세 트랙으로 원본을 만든다(이름 intern — 실패는 InvalidSetting·detail = 트랙).
Result<void> BuildSource(AssetImportContext& context, NamePool& names,
                         const AnimationClipImportSpec& spec, ClipSource& out) noexcept {
    out.frameRate = spec.frameRate;
    out.frameCount = spec.frameCount;
    if (auto reserved = out.tracks.TryReserve(spec.tracks.Size()); !reserved) {
        return OutOfMemory(context);
    }

    for (usize index = 0; index < spec.tracks.Size(); ++index) {
        const ClipImportTrack& source = spec.tracks[index];
        auto bone = InternChecked(context, names, source.bone, index);
        if (!bone) {
            return Result<void>::Failure(bone.Error());
        }

        ClipTrack track(out.tracks.GetAllocator());
        track.bone = bone.Value();
        if (!CopySpan(source.translation, track.translation) ||
            !CopySpan(source.rotation, track.rotation) || !CopySpan(source.scale, track.scale)) {
            return OutOfMemory(context);
        }

        (void)out.tracks.TryPushBack(Move(track));
    }

    return Result<void>::Success();
}

// 커브 표시를 복사한다.
template <class Target, class Source>
void CopyFlags(const Source& source, Target& out) noexcept {
    out.editable = source.editable;
    out.metadata = source.metadata;
    out.disabled = source.disabled;
}

// 명세의 커브·변환 커브를 만든다(이름 intern·curve null — InvalidSetting, detail = 위치).
Result<void> BuildCurves(AssetImportContext& context, NamePool& names, Allocator& allocator,
                         const AnimationClipImportSpec& spec, Array<ClipCurve>& curves,
                         Array<ClipTransformCurve>& transformCurves) noexcept {
    for (usize index = 0; index < spec.curves.Size(); ++index) {
        const ClipImportCurve& source = spec.curves[index];
        if (source.curve == nullptr) {
            return Invalid(context, index);
        }

        auto name = InternChecked(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        ClipCurve curve(allocator);
        curve.name = name.Value();
        CopyFlags(source, curve);
        if (!curve.curve.TryCopyFrom(*source.curve) || !curves.TryPushBack(Move(curve))) {
            return OutOfMemory(context);
        }
    }

    for (usize index = 0; index < spec.transformCurves.Size(); ++index) {
        const ClipImportTransformCurve& source = spec.transformCurves[index];
        auto name = InternChecked(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        ClipTransformCurve curve(allocator);
        curve.name = name.Value();
        CopyFlags(source, curve);
        if (!curve.channels.TryReserve(source.channels.Size())) {
            return OutOfMemory(context);
        }

        for (const TangentCurve& channel : source.channels) {
            TangentCurve copy(allocator);
            if (!copy.TryCopyFrom(channel)) {
                return OutOfMemory(context);
            }

            (void)curve.channels.TryPushBack(Move(copy));
        }

        if (!transformCurves.TryPushBack(Move(curve))) {
            return OutOfMemory(context);
        }
    }

    return Result<void>::Success();
}

// 명세의 재생 설정 값이다(BuildSettings가 만든다).
struct ClipSettings {
    explicit ClipSettings(Allocator& allocator) noexcept
        : events(allocator), markers(allocator), eventTracks(allocator), rates(allocator) {
    }

    Array<ClipEvent> events;
    Array<ClipSyncMarker> markers;
    Array<Name> eventTracks;
    Name retargetSource;
    PlatformFrameRate rates;
    uint64 droppedProperties = 0;
};

// 이벤트·동기 마커·이벤트 트랙·retargetSource·플랫폼 목표 비율을 만든다(intern — InvalidSetting,
// 플랫폼 키는 None으로 접히는 문자열(빈 문자열·"None")도 거부 — Resolve가 None을 보지 않는다).
Result<void> BuildSettings(AssetImportContext& context, NamePool& names,
                           const AnimationClipImportSpec& spec, ClipSettings& out) noexcept {
    for (usize index = 0; index < spec.events.Size(); ++index) {
        const ClipImportEvent& source = spec.events[index];
        auto name = InternChecked(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        auto type = InternChecked(context, names, source.type, index);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        ClipEvent event;
        event.time = source.time;
        event.duration = source.duration;
        event.track = source.track;
        event.name = name.Value();
        event.type = type.Value();
        event.triggerChance = source.triggerChance;
        event.triggerOnDedicatedServer = source.triggerOnDedicatedServer;
        event.triggerOnFollower = source.triggerOnFollower;
        out.droppedProperties += source.droppedProperties;
        if (!out.events.TryPushBack(event)) {
            return OutOfMemory(context);
        }
    }

    for (usize index = 0; index < spec.syncMarkers.Size(); ++index) {
        auto name = InternChecked(context, names, spec.syncMarkers[index].name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        ClipSyncMarker marker;
        marker.name = name.Value();
        marker.time = spec.syncMarkers[index].time;
        marker.track = spec.syncMarkers[index].track;
        if (!out.markers.TryPushBack(marker)) {
            return OutOfMemory(context);
        }
    }

    for (usize index = 0; index < spec.eventTracks.Size(); ++index) {
        auto name = InternChecked(context, names, spec.eventTracks[index], index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (!out.eventTracks.TryPushBack(name.Value())) {
            return OutOfMemory(context);
        }
    }

    auto retarget = InternChecked(context, names, spec.retargetSource, 0);
    if (!retarget) {
        return Result<void>::Failure(retarget.Error());
    }

    out.retargetSource = retarget.Value();
    out.rates.defaultRate = spec.targetFrameRate;
    for (usize index = 0; index < spec.targetFrameRatePerPlatform.Size(); ++index) {
        const ClipImportRate& source = spec.targetFrameRatePerPlatform[index];
        auto profile = InternChecked(context, names, source.profile, index);
        if (!profile) {
            return Result<void>::Failure(profile.Error());
        }

        if (profile.Value().IsNone() || out.rates.overrides.Contains(profile.Value())) {
            return Invalid(context, index);
        }

        if (!out.rates.overrides.Insert(profile.Value(), source.rate)) {
            return OutOfMemory(context);
        }
    }

    return Result<void>::Success();
}

// 재임포트 보존: 옛 커브 가운데 명세에 같은 이름이 없는 것을 뒤에 둔다(UE Interchange
// bDeleteExistingCustomAttributeCurves 기본 거짓 — 기존 커브를 지우지 않는다). 할당하지 않고
// 옮긴다.
template <class Curve>
Result<void> KeepOldCurves(Array<Curve>& old, Array<Curve>& curves) noexcept {
    for (Curve& curve : old) {
        bool present = false;
        for (const Curve& fresh : curves) {
            present = present || fresh.name == curve.name;
        }

        if (!present) {
            if (auto pushed = curves.TryPushBack(Move(curve)); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// 목표 비율 표를 복사한다(맵은 복사 생성이 없다).
Result<void> CopyRates(const PlatformFrameRate& source, PlatformFrameRate& out) noexcept {
    out.defaultRate = source.defaultRate;
    out.overrides.Clear();
    for (const auto& item : source.overrides) {
        if (auto inserted = out.overrides.Insert(item.key, item.value); !inserted) {
            return Result<void>::Failure(inserted.Error());
        }
    }

    return Result<void>::Success();
}

} // namespace animation_clip_import

Result<void> ImportAnimationClip(AssetImportContext& context, ObjectHandle output,
                                 const AnimationClipImportSpec& spec,
                                 ClipDiagnostic* failure) noexcept {
    using namespace animation_clip_import;
    ObjectRegistry& registry = context.Registry();
    const ObjectAccessContext& access = context.Access();
    NamePool* names = registry.Names();
    if (names == nullptr) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument, 0);
    }

    auto outputType = registry.AcquireType(access, output);
    if (!outputType) {
        return context.Fail(AssetDiagnosticCode::None, outputType.Error(), 0);
    }

    if (outputType.Value().Id() != TypeIdOf<AnimationClip>()) {
        return context.Fail(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch, 0);
    }

    if (spec.skeleton.IsNull()) {
        return Invalid(context, 0);
    }

    Allocator& allocator = registry.ObjectAllocator();

    // 1. 원본(이름·검증).
    ClipSource source(allocator);
    if (auto built = BuildSource(context, *names, spec, source); !built) {
        return built;
    }

    ClipDiagnostic diagnostic;
    if (auto valid = ValidateClipSource(source, allocator, &diagnostic); !valid) {
        return ClipFail(context, diagnostic, valid.Error(), failure);
    }

    // 2. 커브·설정 이름.
    Array<ClipCurve> curves(allocator);
    Array<ClipTransformCurve> transformCurves(allocator);
    if (auto built = BuildCurves(context, *names, allocator, spec, curves, transformCurves);
        !built) {
        return built;
    }

    // 보존하는 재임포트는 명세의 재생 설정·기준 클립을 버리므로 만들지도 열지도 않는다(7.6.2절 5).
    const bool keepSettings = context.IsReimport() && !spec.replaceSettingsOnReimport;
    ClipSettings settings(allocator);
    if (!keepSettings) {
        if (auto built = BuildSettings(context, *names, spec, settings); !built) {
            return built;
        }
    }

    // 3. 참조.
    auto skeleton = OpenTyped(context, spec.skeleton, TypeIdOf<Skeleton>(), SkeletonPosition);
    if (!skeleton) {
        return Result<void>::Failure(skeleton.Error());
    }

    auto basePose = OpenTyped(context, keepSettings ? AssetId{} : spec.additive.basePoseClip,
                              TypeIdOf<AnimationClip>(), BasePoseClipPosition);
    if (!basePose) {
        return Result<void>::Failure(basePose.Error());
    }

    // 4. 필드 쓰기(재임포트는 이벤트·마커·설정을 보존 — 7.6.2절 3).
    {
        auto borrowed = registry.ResolveForWrite<AnimationClip>(access, output);
        if (!borrowed) {
            return context.Fail(AssetDiagnosticCode::None, borrowed.Error(), 0);
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return context.Fail(AssetDiagnosticCode::None, value.Error(), 0);
        }

        AnimationClip& clip = *value.Value();
        clip.source.frameRate = spec.frameRate;
        clip.source.frameCount = spec.frameCount;
        clip.source.boneTracks.Clear();
        Result<void> step = Result<void>::Success();
        if (context.IsReimport()) {
            step = KeepOldCurves(clip.source.curves, curves);
            if (step) {
                step = KeepOldCurves(clip.source.transformCurves, transformCurves);
            }
        }

        clip.source.curves = Move(curves);
        clip.source.transformCurves = Move(transformCurves);
        if (step) {
            step = registry.SetReference(access, output, clip.skeleton, skeleton.Value());
        }

        if (step && !keepSettings) {
            clip.events = Move(settings.events);
            clip.syncMarkers = Move(settings.markers);
            clip.eventTracks = Move(settings.eventTracks);
            clip.interpolation = spec.interpolation;
            clip.rateScale = spec.rateScale;
            clip.loop = spec.loop;
            clip.rootMotion = spec.rootMotion;
            clip.additive.type = spec.additive.type;
            clip.additive.basePose = spec.additive.basePose;
            clip.additive.basePoseFrame = spec.additive.basePoseFrame;
            clip.retargetSource = settings.retargetSource;
            step = CopyRates(settings.rates, clip.targetFrameRate);
            if (step) {
                step = registry.SetReference(access, output, clip.additive.basePoseClip,
                                             basePose.Value());
            }
        }

        if (!step) {
            return context.Fail(AssetDiagnosticCode::None, step.Error(), 0);
        }

        SortClipEvents(clip);
        SortClipSyncMarkers(clip);
        ClipPlaybackData empty(clip.playback.trackToBone.GetAllocator());
        clip.playback = Move(empty);
        if (auto valid = ValidateAnimationClip(clip, allocator, &diagnostic); !valid) {
            return ClipFail(context, diagnostic, valid.Error(), failure);
        }
    }

    // 5. 경고(스켈레톤에 없는 트랙 → 버린 이벤트 속성 → animated attribute).
    {
        auto borrowed = registry.Resolve<Skeleton>(access, skeleton.Value());
        if (!borrowed) {
            return context.Fail(AssetDiagnosticCode::None, borrowed.Error(), 0);
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return context.Fail(AssetDiagnosticCode::None, value.Error(), 0);
        }

        Array<ClipDiagnostic> unknown(allocator);
        if (auto found = FindUnknownBoneTracks(source, *value.Value(), unknown); !found) {
            return OutOfMemory(context);
        }

        for (const ClipDiagnostic& warning : unknown) {
            if (auto warned = context.Warn(AssetDiagnosticCode::UnknownBoneTrack, warning.index);
                !warned) {
                return warned;
            }
        }
    }

    if (settings.droppedProperties != 0) {
        if (auto warned = context.Warn(AssetDiagnosticCode::EventPropertiesDropped,
                                       settings.droppedProperties);
            !warned) {
            return warned;
        }
    }

    if (spec.attributeCount != 0) {
        if (auto warned =
                context.Warn(AssetDiagnosticCode::UnsupportedSetting, spec.attributeCount);
            !warned) {
            return warned;
        }
    }

    // 6. 원본 bulk(검증이 모두 끝난 뒤).
    MemoryBulkSource bulk(allocator);
    ClipSourceData stored(allocator);
    if (auto written = StoreClipSource(source, bulk, *names, stored, &diagnostic); !written) {
        return ClipFail(context, diagnostic, written.Error(), failure);
    }

    for (ClipBoneTrack& track : stored.boneTracks) {
        BulkDataRef* refs[] = {&track.translation, &track.rotation, &track.scale};
        for (BulkDataRef* ref : refs) {
            if (auto moved = TransferImportBulk(context, output, bulk, *ref, allocator); !moved) {
                return context.Fail(AssetDiagnosticCode::None, moved.Error(), 0);
            }
        }
    }

    auto borrowed = registry.ResolveForWrite<AnimationClip>(access, output);
    if (!borrowed) {
        return context.Fail(AssetDiagnosticCode::None, borrowed.Error(), 0);
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return context.Fail(AssetDiagnosticCode::None, value.Error(), 0);
    }

    value.Value()->source.boneTracks = Move(stored.boneTracks);
    return Result<void>::Success();
}

} // namespace rocks
