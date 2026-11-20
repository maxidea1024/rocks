#include "AnimationInternal.h"

// 클립 설정·이벤트·동기 마커·커브 검증과 정렬, 클립 진단 이름이다
// (Docs/Design/MeshAnimationAssets.ko.md 7.1·7.3·7.5절, UE FAnimNotifyEvent::operator<·
// UAnimSequence::SortSyncMarkers).
namespace rocks {

namespace {

// 이벤트 a가 b보다 앞인지다(시간 오름차순, 같으면 track 오름차순 — 정확 비교).
bool EventBefore(const ClipEvent& a, const ClipEvent& b) noexcept {
    return a.time < b.time || (a.time == b.time && a.track < b.track);
}

// 유한하고 [0, length] 안인지다(float 시간을 double 길이와 비교한다).
bool InClip(float time, double length) noexcept {
    return math::IsFinite(time) && time >= 0.0f && static_cast<double>(time) <= length;
}

// track이 0 이상이고 eventTracks가 있으면 그 범위인지다(7.5절 11).
bool ValidTrack(int32 track, usize tracks) noexcept {
    return track >= 0 && (tracks == 0 || static_cast<usize>(track) < tracks);
}

// 짧은 배열의 안정 삽입 정렬이다.
template <class T, class Before>
void StableSort(Array<T>& values, Before&& before) noexcept {
    for (usize at = 1; at < values.Size(); ++at) {
        for (usize back = at; back > 0 && before(values[back], values[back - 1]); --back) {
            const T moved = values[back];
            values[back] = values[back - 1];
            values[back - 1] = moved;
        }
    }
}

// 커브 이름(None·중복)을 본다(진단 번호는 그 배열의 번호다). 커브 키(시간 순·유한)는 TangentCurve
// API와 리플렉션 검증 hook이 이미 지킨다.
template <class Curve>
Result<void> CheckCurveNames(Span<const Curve> curves, Allocator& scratch,
                             ClipDiagnostic* failure) noexcept {
    Array<animation_detail::NamedItem> names(scratch);
    if (auto reserved = names.TryReserve(curves.Size()); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < curves.Size(); ++index) {
        if (!animation_detail::IsUsableName(curves[index].name)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::InvalidCurveName, index);
        }

        ROCKS_ASSERT(
            names.TryPushBack(animation_detail::MakeNamedItem(curves[index].name, index)).IsOk());
    }

    animation_detail::SortNamedItems(names.View());
    uint32 later = 0;
    uint32 earlier = 0;
    if (animation_detail::FindFirstDuplicate(names.View(), later, earlier)) {
        return animation_detail::ClipInvalid(failure, ClipIssue::DuplicateCurve, later, earlier);
    }

    return Result<void>::Success();
}

Result<void> ValidateClipCurves(const ClipSourceData& source, Allocator& scratch,
                                ClipDiagnostic* failure) noexcept {
    if (auto checked = CheckCurveNames(source.curves.View(), scratch, failure); !checked) {
        return checked;
    }

    // 변환 커브의 진단 번호는 그 배열 번호다(issue가 묶음을 가른다 — 이름 판정은 같은 규칙).
    if (auto checked = CheckCurveNames(source.transformCurves.View(), scratch, failure); !checked) {
        return checked;
    }

    for (uint32 index = 0; index < source.transformCurves.Size(); ++index) {
        const ClipTransformCurve& curve = source.transformCurves[index];
        if (curve.channels.Size() != ClipTransformCurveChannels) {
            return animation_detail::ClipInvalid(failure, ClipIssue::TransformCurveChannels, index);
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateEvents(const AnimationClip& clip, double length,
                            ClipDiagnostic* failure) noexcept {
    const usize tracks = clip.eventTracks.Size();
    for (uint32 index = 0; index < clip.events.Size(); ++index) {
        const ClipEvent& event = clip.events[index];
        if (!InClip(event.time, length)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::EventTime, index);
        }

        if (!math::IsFinite(event.duration) || event.duration < 0.0f ||
            static_cast<double>(event.time) + event.duration > length) {
            return animation_detail::ClipInvalid(failure, ClipIssue::EventDuration, index);
        }

        if (!(event.triggerChance >= 0.0f && event.triggerChance <= 1.0f)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::EventChance, index);
        }

        if (!ValidTrack(event.track, tracks)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::EventTrack, index);
        }

        if (index != 0 && EventBefore(event, clip.events[index - 1])) {
            return animation_detail::ClipInvalid(failure, ClipIssue::EventOrder, index, index - 1);
        }
    }

    for (uint32 index = 0; index < clip.syncMarkers.Size(); ++index) {
        const ClipSyncMarker& marker = clip.syncMarkers[index];
        if (!animation_detail::IsUsableName(marker.name)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::InvalidSyncMarker, index);
        }

        if (!InClip(marker.time, length)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::SyncMarkerTime, index);
        }

        if (!ValidTrack(marker.track, tracks)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::SyncMarkerTrack, index);
        }

        if (index != 0 && marker.time < clip.syncMarkers[index - 1].time) {
            return animation_detail::ClipInvalid(failure, ClipIssue::SyncMarkerOrder, index,
                                                 index - 1);
        }
    }

    return Result<void>::Success();
}

} // namespace

FrameRate PlatformFrameRate::Resolve(Name platform) const noexcept {
    const FrameRate* found = platform.IsNone() ? nullptr : overrides.Find(platform);
    return found != nullptr ? *found : defaultRate;
}

Result<void> ValidateAnimationClip(const AnimationClip& clip, Allocator& scratch,
                                   ClipDiagnostic* failure) noexcept {
    const ClipSourceData& source = clip.source;
    if (source.frameRate.AsDecimal() > ClipMaxFrameRate) {
        return animation_detail::ClipInvalid(failure, ClipIssue::FrameRateTooHigh);
    }

    if (source.frameCount > ClipMaxFrames) {
        return animation_detail::ClipFail(failure, AssetDiagnosticCode::InvalidClipData,
                                          ErrorCode::CapacityExceeded, ClipIssue::TooManyFrames);
    }

    if (auto valid = ValidateClipCurves(source, scratch, failure); !valid) {
        return valid;
    }

    const double length =
        source.frameRate.AsSeconds(FrameTime(FrameNumber(static_cast<int32>(source.frameCount))));
    if (auto valid = ValidateEvents(clip, length, failure); !valid) {
        return valid;
    }

    if (!math::IsFinite(clip.rateScale)) {
        return animation_detail::ClipInvalid(failure, ClipIssue::RateScale);
    }

    if (clip.additive.basePoseFrame < 0) {
        return animation_detail::ClipInvalid(failure, ClipIssue::BasePoseFrame);
    }

    if (clip.targetFrameRate.defaultRate.AsDecimal() > ClipMaxFrameRate) {
        return animation_detail::ClipInvalid(failure, ClipIssue::TargetFrameRateTooHigh);
    }

    uint32 index = 0;
    for (const auto& entry : clip.targetFrameRate.overrides) {
        if (entry.value.AsDecimal() > ClipMaxFrameRate) {
            return animation_detail::ClipInvalid(failure, ClipIssue::TargetFrameRateTooHigh, index);
        }

        ++index;
    }

    return Result<void>::Success();
}

void SortClipEvents(AnimationClip& clip) noexcept {
    StableSort(clip.events, EventBefore);
}

void SortClipSyncMarkers(AnimationClip& clip) noexcept {
    StableSort(clip.syncMarkers, [](const ClipSyncMarker& a, const ClipSyncMarker& b) noexcept {
        return a.time < b.time;
    });
}

const char* ClipIssueName(ClipIssue issue) noexcept {
    switch (issue) {
    case ClipIssue::None:
        return "None";
    case ClipIssue::FrameRateTooHigh:
        return "FrameRateTooHigh";
    case ClipIssue::TooManyFrames:
        return "TooManyFrames";
    case ClipIssue::TooManyTracks:
        return "TooManyTracks";
    case ClipIssue::InvalidTrackName:
        return "InvalidTrackName";
    case ClipIssue::TrackKeyCount:
        return "TrackKeyCount";
    case ClipIssue::NonFiniteKey:
        return "NonFiniteKey";
    case ClipIssue::RotationNotNormalized:
        return "RotationNotNormalized";
    case ClipIssue::DuplicateTrack:
        return "DuplicateTrack";
    case ClipIssue::TrackOrder:
        return "TrackOrder";
    case ClipIssue::BulkUnreadable:
        return "BulkUnreadable";
    case ClipIssue::BulkLength:
        return "BulkLength";
    case ClipIssue::TrackBoneLength:
        return "TrackBoneLength";
    case ClipIssue::InvalidCurveName:
        return "InvalidCurveName";
    case ClipIssue::DuplicateCurve:
        return "DuplicateCurve";
    case ClipIssue::TransformCurveChannels:
        return "TransformCurveChannels";
    case ClipIssue::EventTime:
        return "EventTime";
    case ClipIssue::EventDuration:
        return "EventDuration";
    case ClipIssue::EventChance:
        return "EventChance";
    case ClipIssue::EventTrack:
        return "EventTrack";
    case ClipIssue::EventOrder:
        return "EventOrder";
    case ClipIssue::InvalidSyncMarker:
        return "InvalidSyncMarker";
    case ClipIssue::SyncMarkerTime:
        return "SyncMarkerTime";
    case ClipIssue::SyncMarkerTrack:
        return "SyncMarkerTrack";
    case ClipIssue::SyncMarkerOrder:
        return "SyncMarkerOrder";
    case ClipIssue::RateScale:
        return "RateScale";
    case ClipIssue::BasePoseFrame:
        return "BasePoseFrame";
    case ClipIssue::TargetFrameRateTooHigh:
        return "TargetFrameRateTooHigh";
    case ClipIssue::IncompatibleTargetFrameRate:
        return "IncompatibleTargetFrameRate";
    case ClipIssue::UnknownBoneTrack:
        return "UnknownBoneTrack";
    }

    return "Unknown";
}

AssetDiagnostic ToAssetDiagnostic(const ClipDiagnostic& diagnostic, AssetId asset) noexcept {
    AssetDiagnostic out;
    out.code = diagnostic.code;
    out.error = diagnostic.error;
    out.asset = asset;
    out.detail = diagnostic.index == ClipNoIndex ? 0 : diagnostic.index;
    return out;
}

} // namespace rocks
