#include <WorldIntegration/Components/TimelineVectorTrack.h>

// 타임라인 트랙 값의 깊은 복사다(Docs/Design/EngineRuntime 32.2절).
namespace rocks {

Result<void> TimelineVectorTrack::TryCopyFrom(const TimelineVectorTrack& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    TimelineVectorTrack copy(curve.x.GetAllocator());
    if (auto copied = copy.curve.TryCopyFrom(other.curve); !copied) {
        return copied;
    }

    if (auto assigned = copy.curveAsset.TryAssign(other.curveAsset); !assigned) {
        return assigned;
    }

    trackName = other.trackName;
    curve = Move(copy.curve);
    curveAsset = Move(copy.curveAsset);
    return Result<void>::Success();
}

} // namespace rocks
