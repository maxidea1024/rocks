#include <UnrealExchange/ExchangeAnimSequenceSettings.h>
#include <UnrealExchange/ExchangeAnimationClip.h>
#include <string.h>

#include "ExchangeMeshCommon.h"

// 교환 애니메이션 클립 → 클립 임포트 명세 변환이다(Docs/Design/UnrealExchange 7.2절).

namespace rocks {

namespace exchange_animation_clip {

using namespace exchange_detail;
using SettingRow = ExchangeAnimSequenceSettingRow;

// UE FRichCurve 열거 값 이름(Rocks 열거 순서)이다. RCTM_SmartAuto는 Auto로 본다(7.1절 2번).
constexpr StringView InterpNames[] = {"RCIM_Linear", "RCIM_Constant", "RCIM_Cubic"};
constexpr StringView TangentNames[] = {"RCTM_Auto", "RCTM_User", "RCTM_Break"};
constexpr StringView SmartAutoTangent = "RCTM_SmartAuto";
constexpr StringView WeightNames[] = {"RCTWM_WeightedNone", "RCTWM_WeightedArrive",
                                      "RCTWM_WeightedLeave", "RCTWM_WeightedBoth"};
constexpr StringView ExtrapolationNames[] = {"RCCE_Cycle", "RCCE_CycleWithOffset", "RCCE_Oscillate",
                                             "RCCE_Linear", "RCCE_Constant"};
// UE EAnimAssetCurveFlags 비트(AnimCurveTypes.h)다.
constexpr int64 CurveEditableFlag = 0x04;
constexpr int64 CurveMetadataFlag = 0x10;
constexpr int64 CurveDisabledFlag = 0x40;
// 변환 커브의 채널 수(이동·회전·크기 x·y·z)다.
constexpr usize TransformChannels = 9;
// 성분 blob의 components다(이동·크기 셋, 회전 넷 — 7.1절 1번).
constexpr uint64 VectorComponents = 3;
constexpr uint64 RotationComponents = 4;
// Rocks 열거자 이름(번역 EnumMap·Enum의 결과 — 열거 순서)이다.
constexpr StringView AdditiveTypeNames[] = {"None", "LocalSpace", "MeshSpaceRotation"};
constexpr StringView BasePoseNames[] = {"None", "RefPose", "Scaled", "Frame", "LocalFrame"};
constexpr StringView RootLockNames[] = {"RefPose", "FirstFrame", "Zero"};
constexpr StringView InterpolationNames[] = {"Linear", "Step"};
// 커브 키의 숫자 칸이다.
constexpr StringView KeyNumberFields[] = {
    "time", "value", "arriveTangent", "arriveTangentWeight", "leaveTangent", "leaveTangentWeight"};
constexpr usize KeyNumberCount = sizeof(KeyNumberFields) / sizeof(KeyNumberFields[0]);

} // namespace exchange_animation_clip

// ExchangeAnimationClip::Build의 단계들이다.
struct ExchangeAnimationClipBuilder : exchange_detail::ExchangeMeshContext {
    ExchangeAnimationClip& clip;
    const ExchangeAnimationClipInput& input;

    // 성분 blob 하나(f32 × components)를 out 뒤에 더한다. start·count는 더한 범위다.
    template <class T>
    Result<void> ReadComponent(JsonValue reference, uint64 components, Array<T>& out, usize& start,
                               usize& count, uint64 detail) noexcept {
        using namespace exchange_animation_clip;
        const usize blob = FindAssetBlob(*input.asset, reference);
        if (blob == input.asset->blobs.Size() ||
            input.asset->blobs[blob].layout != ExchangeBlobLayout::F32 ||
            input.asset->blobs[blob].components != components ||
            input.blobs[blob].Size() != input.asset->blobs[blob].count * sizeof(T)) {
            return Corrupt(detail);
        }

        start = out.Size();
        count = static_cast<usize>(input.asset->blobs[blob].count);
        const Span<const uint8> bytes = input.blobs[blob];
        for (usize at = 0; at < count; ++at) {
            T value;
            memcpy(&value, bytes.Data() + at * sizeof(T), sizeof(T));
            if (auto pushed = out.TryPushBack(value); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> ReadSource() noexcept {
        using namespace exchange_animation_clip;
        const JsonValue data = input.asset->data;
        const JsonValue rate = Member(data, "frameRate");
        int32 numerator = 0;
        int32 denominator = 0;
        uint64 frames = 0;
        if (!ReadInt32(Member(rate, "numerator"), numerator) ||
            !ReadInt32(Member(rate, "denominator"), denominator) ||
            !ReadUInt(Member(data, "frameCount"), frames) ||
            frames > NumericLimits<uint32>::Max()) {
            return Corrupt(0);
        }

        auto made = FrameRate::TryCreate(numerator, denominator);
        if (!made) {
            return Corrupt(0);
        }

        clip.spec_.frameRate = made.Value();
        clip.spec_.frameCount = static_cast<uint32>(frames);
        const JsonValue tracks = Member(data, "boneTracks");
        if (!tracks.IsArray()) {
            return Corrupt(0);
        }

        for (usize index = 0; index < tracks.Size(); ++index) {
            const JsonValue item = tracks.At(index);
            ExchangeAnimationClip::TrackParts parts;
            if (!ReadString(Member(item, "bone"), parts.bone)) {
                return Corrupt(index);
            }

            if (auto read =
                    ReadComponent(Member(item, "translation"), VectorComponents, clip.vectors_,
                                  parts.translationStart, parts.translationCount, index);
                !read) {
                return read;
            }

            if (auto read =
                    ReadComponent(Member(item, "rotation"), RotationComponents, clip.rotations_,
                                  parts.rotationStart, parts.rotationCount, index);
                !read) {
                return read;
            }

            if (auto read = ReadComponent(Member(item, "scale"), VectorComponents, clip.vectors_,
                                          parts.scaleStart, parts.scaleCount, index);
                !read) {
                return read;
            }

            if (auto pushed = clip.trackParts_.TryPushBack(parts); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 교환 FRichCurve 객체(7.1절 2번)를 out에 옮긴다.
    Result<void> ReadTangentCurve(JsonValue object, TangentCurve& out, uint64 detail) noexcept {
        using namespace exchange_animation_clip;
        const JsonValue keys = Member(object, "keys");
        if (!keys.IsArray()) {
            return Corrupt(detail);
        }

        Array<TangentCurveKey> converted(allocator);
        for (usize index = 0; index < keys.Size(); ++index) {
            const JsonValue item = keys.At(index);
            double numbers[KeyNumberCount] = {};
            for (usize at = 0; at < KeyNumberCount; ++at) {
                if (!Number(Member(item, KeyNumberFields[at]), numbers[at])) {
                    return Corrupt(detail);
                }
            }

            StringView interp;
            StringView tangent;
            StringView weight;
            if (!ReadString(Member(item, "interpMode"), interp) ||
                !ReadString(Member(item, "tangentMode"), tangent) ||
                !ReadString(Member(item, "tangentWeightMode"), weight)) {
                return Corrupt(detail);
            }

            const usize interpIndex = FindName(InterpNames, interp);
            const usize tangentIndex =
                tangent == SmartAutoTangent ? 0 : FindName(TangentNames, tangent);
            const usize weightIndex = FindName(WeightNames, weight);
            if (interpIndex == sizeof(InterpNames) / sizeof(InterpNames[0]) ||
                tangentIndex == sizeof(TangentNames) / sizeof(TangentNames[0]) ||
                weightIndex == sizeof(WeightNames) / sizeof(WeightNames[0])) {
                return Corrupt(detail);
            }

            TangentCurveKey key;
            key.time = static_cast<float>(numbers[0]);
            key.value = static_cast<float>(numbers[1]);
            key.arriveTangent = static_cast<float>(numbers[2]);
            key.arriveTangentWeight = static_cast<float>(numbers[3]);
            key.leaveTangent = static_cast<float>(numbers[4]);
            key.leaveTangentWeight = static_cast<float>(numbers[5]);
            key.interp = static_cast<CurveInterp>(interpIndex);
            key.tangentMode = static_cast<CurveTangentMode>(tangentIndex);
            key.tangentWeight = static_cast<CurveTangentWeight>(weightIndex);
            if (auto pushed = converted.TryPushBack(key); !pushed) {
                return pushed;
            }
        }

        StringView pre;
        StringView post;
        double fallback = 0.0;
        if (!ReadString(Member(object, "preInfinityExtrap"), pre) ||
            !ReadString(Member(object, "postInfinityExtrap"), post) ||
            !Number(Member(object, "defaultValue"), fallback)) {
            return Corrupt(detail);
        }

        const usize preIndex = FindName(ExtrapolationNames, pre);
        const usize postIndex = FindName(ExtrapolationNames, post);
        constexpr usize Extrapolations = sizeof(ExtrapolationNames) / sizeof(ExtrapolationNames[0]);
        if (preIndex == Extrapolations || postIndex == Extrapolations) {
            return Corrupt(detail);
        }

        if (auto set = out.TrySetKeys(converted.View()); !set) {
            return set.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(set.Error())
                                                         : Corrupt(detail);
        }

        out.preExtrapolation = static_cast<CurveExtrapolation>(preIndex);
        out.postExtrapolation = static_cast<CurveExtrapolation>(postIndex);
        // UE "기본값 없음"은 float 최대값이다(비움).
        if (static_cast<float>(fallback) != NumericLimits<float>::Max()) {
            out.defaultValue = static_cast<float>(fallback);
        }

        return Result<void>::Success();
    }

    // UE EAnimAssetCurveFlags를 편집·메타데이터·끔으로 옮긴다.
    template <class Curve>
    bool ReadFlags(JsonValue item, Curve& out) noexcept {
        using namespace exchange_animation_clip;
        int32 flags = 0;
        if (!ReadInt32(Member(item, "flags"), flags)) {
            return false;
        }

        out.editable = (flags & CurveEditableFlag) != 0;
        out.metadata = (flags & CurveMetadataFlag) != 0;
        out.disabled = (flags & CurveDisabledFlag) != 0;
        return true;
    }

    Result<void> ReadCurves() noexcept {
        using namespace exchange_animation_clip;
        const JsonValue curves = Member(input.asset->data, "curves");
        const JsonValue transforms = Member(input.asset->data, "transformCurves");
        if (!OptArray(curves) || !OptArray(transforms)) {
            return Corrupt(0);
        }

        for (usize index = 0; index < curves.Size(); ++index) {
            const JsonValue item = curves.At(index);
            ClipImportCurve curve;
            if (!ReadString(Member(item, "name"), curve.name) || !ReadFlags(item, curve)) {
                return Corrupt(index);
            }

            auto made = clip.curveValues_.TryEmplaceBack(allocator);
            if (!made) {
                return Result<void>::Failure(made.Error());
            }

            if (auto read = ReadTangentCurve(Member(item, "curve"), *made.Value(), index); !read) {
                return read;
            }

            if (auto pushed = clip.curves_.TryPushBack(curve); !pushed) {
                return pushed;
            }
        }

        for (usize index = 0; index < transforms.Size(); ++index) {
            const JsonValue item = transforms.At(index);
            const JsonValue channels = Member(item, "channels");
            ClipImportTransformCurve curve;
            if (!ReadString(Member(item, "name"), curve.name) || !ReadFlags(item, curve) ||
                !channels.IsArray() || channels.Size() != TransformChannels) {
                return Corrupt(index);
            }

            for (usize channel = 0; channel < TransformChannels; ++channel) {
                auto made = clip.transformChannels_.TryEmplaceBack(allocator);
                if (!made) {
                    return Result<void>::Failure(made.Error());
                }

                if (auto read = ReadTangentCurve(channels.At(channel), *made.Value(), index);
                    !read) {
                    return read;
                }
            }

            if (auto pushed = clip.transformCurves_.TryPushBack(curve); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> ReadEvents() noexcept {
        using namespace exchange_animation_clip;
        const JsonValue data = input.asset->data;
        const JsonValue events = Member(data, "events");
        const JsonValue tracks = Member(data, "eventTracks");
        const JsonValue markers = Member(data, "syncMarkers");
        if (!OptArray(events) || !OptArray(tracks) || !OptArray(markers)) {
            return Corrupt(0);
        }

        for (usize index = 0; index < events.Size(); ++index) {
            const JsonValue item = events.At(index);
            const JsonValue type = Member(item, "type");
            const JsonValue properties = Member(item, "properties");
            ClipImportEvent event;
            if (!OptFloat(Member(item, "time"), event.time) ||
                !OptFloat(Member(item, "duration"), event.duration) ||
                !ReadInt32(Member(item, "track"), event.track) ||
                !ReadString(Member(item, "name"), event.name) ||
                (!type.IsNull() && !ReadString(type, event.type)) ||
                !OptFloat(Member(item, "triggerChance"), event.triggerChance) ||
                !ReadBool(Member(item, "triggerOnDedicatedServer"),
                          event.triggerOnDedicatedServer) ||
                !ReadBool(Member(item, "triggerOnFollower"), event.triggerOnFollower) ||
                (properties.IsValid() && !properties.IsNull() && !properties.IsObject())) {
                return Corrupt(index);
            }

            event.name = WithoutNone(event.name);
            event.droppedProperties =
                static_cast<uint32>(properties.IsObject() ? properties.Size() : 0);
            if (auto pushed = clip.events_.TryPushBack(event); !pushed) {
                return pushed;
            }
        }

        for (usize index = 0; index < tracks.Size(); ++index) {
            StringView name;
            if (!ReadString(tracks.At(index), name)) {
                return Corrupt(index);
            }

            if (auto pushed = clip.eventTracks_.TryPushBack(name); !pushed) {
                return pushed;
            }
        }

        for (usize index = 0; index < markers.Size(); ++index) {
            const JsonValue item = markers.At(index);
            ClipImportSyncMarker marker;
            if (!ReadString(Member(item, "name"), marker.name) ||
                !OptFloat(Member(item, "time"), marker.time) ||
                !ReadInt32(Member(item, "track"), marker.track)) {
                return Corrupt(index);
            }

            if (auto pushed = clip.syncMarkers_.TryPushBack(marker); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 번역한 enum 이름(Rocks 열거자 이름)을 번호로 읽는다(없으면 그대로).
    template <usize Count, class Enum>
    bool ReadEnum(JsonValue value, const StringView (&names)[Count], Enum& out) noexcept {
        using namespace exchange_animation_clip;
        StringView name;
        if (!value.IsValid()) {
            return true;
        }

        const usize index = ReadString(value, name) ? FindName(names, name) : Count;
        if (index == Count) {
            return false;
        }

        out = static_cast<Enum>(index);
        return true;
    }

    Result<void> ReadSettings() noexcept {
        using namespace exchange_animation_clip;
        const JsonValue root = clip.translated_.Root();
        AnimationClipImportSpec& spec = clip.spec_;
        struct Check {
            bool ok;
            SettingRow row;
        };

        const Check checks[] = {
            {ReadEnum(Member(root, "AdditiveAnimType"), AdditiveTypeNames, spec.additive.type),
             SettingRow::AdditiveAnimType},
            {ReadEnum(Member(root, "RefPoseType"), BasePoseNames, spec.additive.basePose),
             SettingRow::RefPoseType},
            {OptInt32(Member(root, "RefFrameIndex"), spec.additive.basePoseFrame),
             SettingRow::RefFrameIndex},
            {OptString(Member(root, "RetargetSource"), spec.retargetSource),
             SettingRow::RetargetSource},
            {ReadEnum(Member(root, "Interpolation"), InterpolationNames, spec.interpolation),
             SettingRow::Interpolation},
            {OptBool(Member(root, "EnableRootMotion"), spec.rootMotion.enable),
             SettingRow::EnableRootMotion},
            {ReadEnum(Member(root, "RootMotionRootLock"), RootLockNames, spec.rootMotion.rootLock),
             SettingRow::RootMotionRootLock},
            {OptBool(Member(root, "ForceRootLock"), spec.rootMotion.forceRootLock),
             SettingRow::ForceRootLock},
            {OptBool(Member(root, "UseNormalizedRootMotionScale"),
                     spec.rootMotion.useNormalizedScale),
             SettingRow::UseNormalizedRootMotionScale},
            {OptFloat(Member(root, "RateScale"), spec.rateScale), SettingRow::RateScale},
            {OptBool(Member(root, "Loop"), spec.loop), SettingRow::Loop},
            {OptArray(Member(root, "AttributeCurves")), SettingRow::AttributeCurves},
        };

        for (const Check& check : checks) {
            if (!check.ok) {
                return Corrupt(static_cast<uint64>(check.row));
            }
        }

        spec.retargetSource = WithoutNone(spec.retargetSource);
        spec.attributeCount = static_cast<uint32>(Member(root, "AttributeCurves").Size());
        if (auto read = ReadTargetRate(Member(root, "PlatformTargetFrameRate")); !read) {
            return read;
        }

        if (auto read = TranslatedReference(Member(root, "Skeleton"), spec.skeleton,
                                            static_cast<uint64>(SettingRow::Skeleton));
            !read) {
            return read;
        }

        return TranslatedReference(Member(root, "RefPoseSeq"), spec.additive.basePoseClip,
                                   static_cast<uint64>(SettingRow::RefPoseSeq));
    }

    // UE FPerPlatformFrameRate `{Default: {Numerator, Denominator}, PerPlatform}`다.
    Result<void> ReadTargetRate(JsonValue value) noexcept {
        using namespace exchange_animation_clip;
        constexpr uint64 Detail = static_cast<uint64>(SettingRow::PlatformTargetFrameRate);
        if (!value.IsValid()) {
            return Result<void>::Success();
        }

        const JsonValue fallback = Member(value, "Default");
        const JsonValue platforms = Member(value, "PerPlatform");
        int32 numerator = 0;
        int32 denominator = 0;
        if (!ReadInt32(Member(fallback, "Numerator"), numerator) ||
            !ReadInt32(Member(fallback, "Denominator"), denominator) || !OptArray(platforms)) {
            return Corrupt(Detail);
        }

        auto made = FrameRate::TryCreate(numerator, denominator);
        if (!made) {
            return Corrupt(Detail);
        }

        clip.spec_.targetFrameRate = made.Value();
        return platforms.Size() != 0 ? Warn(AssetDiagnosticCode::UnsupportedSetting, Detail,
                                            "PlatformTargetFrameRate")
                                     : Result<void>::Success();
    }

    void Finish() noexcept {
        using namespace exchange_animation_clip;
        for (const ExchangeAnimationClip::TrackParts& parts : clip.trackParts_) {
            ClipImportTrack track;
            track.bone = parts.bone;
            track.translation = Span<const Vec3>(clip.vectors_.Data() + parts.translationStart,
                                                 parts.translationCount);
            track.rotation =
                Span<const Quat>(clip.rotations_.Data() + parts.rotationStart, parts.rotationCount);
            track.scale =
                Span<const Vec3>(clip.vectors_.Data() + parts.scaleStart, parts.scaleCount);
            (void)clip.tracks_.TryPushBack(track);
        }

        for (usize index = 0; index < clip.curves_.Size(); ++index) {
            clip.curves_[index].curve = &clip.curveValues_[index];
        }

        for (usize index = 0; index < clip.transformCurves_.Size(); ++index) {
            clip.transformCurves_[index].channels = Span<const TangentCurve>(
                clip.transformChannels_.Data() + index * TransformChannels, TransformChannels);
        }

        AnimationClipImportSpec& spec = clip.spec_;
        spec.tracks = clip.tracks_.View();
        spec.curves = clip.curves_.View();
        spec.transformCurves = clip.transformCurves_.View();
        spec.events = clip.events_.View();
        spec.syncMarkers = clip.syncMarkers_.View();
        spec.eventTracks = clip.eventTracks_.View();
        spec.replaceSettingsOnReimport = true;
    }
};

ExchangeAnimationClip::ExchangeAnimationClip(Allocator& allocator) noexcept
    : allocator_(&allocator), translated_(allocator), trackParts_(allocator), vectors_(allocator),
      rotations_(allocator), tracks_(allocator), curveValues_(allocator), curves_(allocator),
      transformChannels_(allocator), transformCurves_(allocator), events_(allocator),
      syncMarkers_(allocator), eventTracks_(allocator), warnings_(allocator) {
}

Result<void> ExchangeAnimationClip::Build(const ExchangeAnimationClipInput& input,
                                          ExchangeMeshFailure& failure) noexcept {
    failure = ExchangeMeshFailure{};
    if (input.asset == nullptr || input.registry == nullptr || input.registry->Names() == nullptr ||
        input.asset->exchangeClass != ExchangeClass::AnimSequence ||
        input.blobs.Size() != input.asset->blobs.Size() || started_) {
        failure.code = AssetDiagnosticCode::InvalidSource;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    started_ = true;
    // 트랙 수만큼 미리 잡아 Finish의 추가가 실패하지 않게 한다.
    ExchangeAnimationClipBuilder builder{{failure, *allocator_, *input.registry, input.access,
                                          input.references, *input.registry->Names(), warnings_},
                                         *this,
                                         input};
    if (auto done = builder.Translate(input.asset->settings, ExchangeAnimSequenceSettingRows(),
                                      ExchangeClass::AnimSequence, translated_);
        !done) {
        return done;
    }

    if (auto done = builder.ReadSource(); !done) {
        return done;
    }

    if (auto reserved = tracks_.TryReserve(trackParts_.Size()); !reserved) {
        return reserved;
    }

    if (auto done = builder.ReadCurves(); !done) {
        return done;
    }

    if (auto done = builder.ReadEvents(); !done) {
        return done;
    }

    if (auto done = builder.ReadSettings(); !done) {
        return done;
    }

    builder.Finish();
    return Result<void>::Success();
}

} // namespace rocks
