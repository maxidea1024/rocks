#include "AnimationInternal.h"

#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>

// 클립 원본 → 쿠킹 데이터 v1 변환과 쿠킹 데이터 저장·로드다(Docs/Design/MeshAnimationAssets.ko.md
// 7.4·7.5절, UE UAnimSequence::CalculateNumberOfSampledKeys).
namespace rocks {

namespace {

// 성분 번호다(진단 related).
enum PlaybackComponent : uint32 {
    TranslationSlot = 0,
    RotationSlot = 1,
    ScaleSlot = 2
};

uint64 GreatestCommonDivisor(uint64 a, uint64 b) noexcept {
    while (b != 0) {
        const uint64 rest = a % b;
        a = b;
        b = rest;
    }

    return a;
}

// 목표 비율의 표본 프레임 수다(frameCount × 목표/원본이 정수여야 한다 — 7.5절 9). 정수가 아니거나
// ClipMaxFrames를 넘으면 false다.
bool ResampledFrames(const ClipSource& source, FrameRate target, uint32& frames) noexcept {
    // 목표/원본 = (tN × sD) / (tD × sN). 각 곱은 int32 둘이라 uint64에 담긴다.
    const uint64 numerator = static_cast<uint64>(target.Numerator()) *
                             static_cast<uint64>(source.frameRate.Denominator());
    const uint64 denominator = static_cast<uint64>(target.Denominator()) *
                               static_cast<uint64>(source.frameRate.Numerator());
    const uint64 common = GreatestCommonDivisor(numerator, denominator);
    const uint64 ratioNumerator = numerator / common;
    const uint64 ratioDenominator = denominator / common;
    if (source.frameCount % ratioDenominator != 0) {
        return false;
    }

    const uint64 steps = source.frameCount / ratioDenominator;
    if (steps != 0 && ratioNumerator > ClipMaxFrames / steps) {
        return false;
    }

    frames = static_cast<uint32>(steps * ratioNumerator);
    return true;
}

// values의 모든 원소가 bit 단위로 같으면 앞 하나만 남긴다(상수 접기 — 7.2절).
template <class T>
void FoldConstant(Array<T>& values) noexcept {
    for (usize at = 1; at < values.Size(); ++at) {
        if (!platform::MemoryEquals(&values[at], &values[0], sizeof(T))) {
            return;
        }
    }

    // 성분은 늘 원소가 있고(원본 검증·표본 수 ≥ 1), 줄이는 크기 조정은 할당하지 않는다.
    ROCKS_ASSERT(values.TryResize(1).IsOk());
}

// track을 표본 frames + 1개로 다시 샘플링한다(목표 비율 k 프레임 시간의 원본 값).
Result<void> ResampleTrack(const ClipSource& source, uint32 track, FrameRate target, uint32 frames,
                           ClipInterpolation interpolation, ClipPlaybackTrack& out) noexcept {
    const uint32 samples = frames + 1;
    if (auto resized = out.translation.TryResize(samples); !resized) {
        return resized;
    }

    if (auto resized = out.rotation.TryResize(samples); !resized) {
        return resized;
    }

    if (auto resized = out.scale.TryResize(samples); !resized) {
        return resized;
    }

    for (uint32 sample = 0; sample < samples; ++sample) {
        const double seconds = target.AsSeconds(FrameTime(FrameNumber(static_cast<int32>(sample))));
        const Transform3f value = SampleClipSource(source, track, seconds, interpolation);
        out.translation[sample] = value.translation;
        out.rotation[sample] = value.rotation;
        out.scale[sample] = value.scale;
    }

    return Result<void>::Success();
}

// 키를 그대로 옮긴다(목표 비율 = 원본 비율).
Result<void> CopyTrack(const ClipTrack& from, ClipPlaybackTrack& to) noexcept {
    if (auto appended = to.translation.TryAppend(from.translation.View()); !appended) {
        return appended;
    }

    if (auto appended = to.rotation.TryAppend(from.rotation.View()); !appended) {
        return appended;
    }

    return to.scale.TryAppend(from.scale.View());
}

// 이름이 스켈레톤 가상 본인지다(가상 본 트랙은 경고 없이 뺀다 — 7.5절 9).
bool IsVirtualBone(const Skeleton& skeleton, Name name) noexcept {
    for (const VirtualBone& bone : skeleton.virtualBones) {
        if (bone.name == name) {
            return true;
        }
    }

    return false;
}

Result<void> CopyCurves(Span<const ClipCurve> curves, Array<ClipCurve>& out) noexcept {
    for (const ClipCurve& curve : curves) {
        ClipCurve copy(out.GetAllocator());
        copy.name = curve.name;
        copy.editable = curve.editable;
        copy.metadata = curve.metadata;
        copy.disabled = curve.disabled;
        if (auto copied = copy.curve.TryCopyFrom(curve.curve); !copied) {
            return copied;
        }

        if (auto pushed = out.TryPushBack(Move(copy)); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> BuildInEnvironment(const ClipBuildInput& input, ClipPlayback& out,
                                ClipBuildReport& report, ClipDiagnostic* failure) noexcept {
    const ClipSource& source = *input.source;
    if (input.targetRate.AsDecimal() > ClipMaxFrameRate) {
        return animation_detail::ClipInvalid(failure, ClipIssue::FrameRateTooHigh);
    }

    const bool same = input.targetRate == source.frameRate;
    uint32 frames = source.frameCount;
    if (!same && !ResampledFrames(source, input.targetRate, frames)) {
        return animation_detail::ClipInvalid(failure, ClipIssue::IncompatibleTargetFrameRate);
    }

    out.sampleRate = input.targetRate;
    out.sampleCount = frames + 1;
    if (auto found = FindUnknownBoneTracks(source, *input.skeleton, report.warnings); !found) {
        return found;
    }

    const BoneHierarchy& bones = input.skeleton->bones;
    for (uint32 track = 0; track < source.tracks.Size(); ++track) {
        const int32 bone = FindBoneIndex(bones, source.tracks[track].bone);
        if (bone == NoParentBone) {
            continue;
        }

        ClipPlaybackTrack cooked(out.tracks.GetAllocator());
        Result<void> filled = same ? CopyTrack(source.tracks[track], cooked)
                                   : ResampleTrack(source, track, input.targetRate, frames,
                                                   input.interpolation, cooked);
        if (!filled) {
            return filled;
        }

        FoldConstant(cooked.translation);
        FoldConstant(cooked.rotation);
        FoldConstant(cooked.scale);
        if (auto pushed = out.tracks.TryPushBack(Move(cooked)); !pushed) {
            return pushed;
        }

        if (auto pushed = out.trackToBone.TryPushBack(static_cast<uint16>(bone)); !pushed) {
            return pushed;
        }
    }

    for (const ClipTransformCurve& curve : input.transformCurves) {
        report.skippedTransformCurves += curve.disabled ? 0 : 1;
    }

    return CopyCurves(input.curves, out.curves);
}

struct BuildJob {
    const ClipBuildInput* input = nullptr;
    ClipPlayback* out = nullptr;
    ClipBuildReport* report = nullptr;
    ClipDiagnostic* failure = nullptr;
    Result<void> status = Result<void>::Success();
};

} // namespace

Result<void> FindUnknownBoneTracks(const ClipSource& source, const Skeleton& skeleton,
                                   Array<ClipDiagnostic>& warnings) noexcept {
    for (uint32 track = 0; track < source.tracks.Size(); ++track) {
        const Name bone = source.tracks[track].bone;
        if (FindBoneIndex(skeleton.bones, bone) != NoParentBone || IsVirtualBone(skeleton, bone)) {
            continue;
        }

        ClipDiagnostic warning;
        warning.code = AssetDiagnosticCode::UnknownBoneTrack;
        warning.issue = ClipIssue::UnknownBoneTrack;
        warning.index = track;
        if (auto pushed = warnings.TryPushBack(warning); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> BuildClipPlayback(const ClipBuildInput& input, ClipPlayback& out,
                               ClipBuildReport& report, ClipDiagnostic* failure) noexcept {
    if (input.source == nullptr || input.skeleton == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = out.tracks.GetAllocator();
    ClipPlayback built(allocator);
    ClipBuildReport notes(report.warnings.GetAllocator());
    ClipDiagnostic diagnostic;
    BuildJob job;
    job.input = &input;
    job.out = &built;
    job.report = &notes;
    job.failure = &diagnostic;
    platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            auto& build = *static_cast<BuildJob*>(context);
            build.status =
                BuildInEnvironment(*build.input, *build.out, *build.report, build.failure);
        },
        &job);
    if (!job.status) {
        if (failure != nullptr) {
            diagnostic.error = job.status.Error();
            *failure = diagnostic;
        }

        return Result<void>::Failure(job.status.Error());
    }

    out = Move(built);
    report = Move(notes);
    return Result<void>::Success();
}

Result<void> StoreClipPlayback(const ClipPlayback& playback, MemoryBulkSource& bulk,
                               ClipPlaybackData& data) noexcept {
    if (playback.trackToBone.Size() != playback.tracks.Size() || playback.sampleCount == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = data.trackToBone.GetAllocator();
    animation_detail::ClipByteWriter writer(allocator);
    for (const ClipPlaybackTrack& track : playback.tracks) {
        const usize counts[3] = {track.translation.Size(), track.rotation.Size(),
                                 track.scale.Size()};
        for (usize count : counts) {
            if (count != 1 && count != playback.sampleCount) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            writer.U32(static_cast<uint32>(count));
        }

        writer.Vec3s(track.translation.View());
        writer.Quats(track.rotation.View());
        writer.Vec3s(track.scale.View());
    }

    if (auto status = writer.Status(); !status) {
        return status;
    }

    Array<uint16> trackToBone(allocator);
    if (auto appended = trackToBone.TryAppend(playback.trackToBone.View()); !appended) {
        return appended;
    }

    Array<ClipCurve> curves(allocator);
    if (auto copied = CopyCurves(playback.curves.View(), curves); !copied) {
        return copied;
    }

    auto ref = bulk.Add(Move(writer.Bytes()));
    if (!ref) {
        return Result<void>::Failure(ref.Error());
    }

    data.sampleRate = playback.sampleRate;
    data.sampleCount = playback.sampleCount;
    data.trackToBone = Move(trackToBone);
    data.tracks = ref.Value();
    data.curves = Move(curves);
    return Result<void>::Success();
}

namespace {

Result<void> CorruptPlayback(ClipDiagnostic* failure, ClipIssue issue, uint32 index = ClipNoIndex,
                             uint32 related = ClipNoIndex) noexcept {
    return animation_detail::ClipFail(failure, AssetDiagnosticCode::CorruptPackage,
                                      ErrorCode::InvalidArgument, issue, index, related);
}

// 쿠킹 트랙 하나를 푼다(원소 수 셋, 값 — 7.5절 10).
Result<void> DecodePlaybackTrack(animation_detail::ClipByteReader& reader, uint32 samples,
                                 uint32 index, ClipPlaybackTrack& track,
                                 ClipDiagnostic* failure) noexcept {
    if (reader.Remaining() < 3 * animation_detail::ClipCountBytes) {
        return CorruptPlayback(failure, ClipIssue::BulkLength, index);
    }

    uint32 counts[3] = {};
    for (uint32 component = 0; component < 3; ++component) {
        counts[component] = reader.U32();
        if (counts[component] != 1 && counts[component] != samples) {
            return CorruptPlayback(failure, ClipIssue::TrackKeyCount, index, component);
        }
    }

    const usize bytes = counts[TranslationSlot] * animation_detail::ClipVec3Bytes +
                        counts[RotationSlot] * animation_detail::ClipQuatBytes +
                        counts[ScaleSlot] * animation_detail::ClipVec3Bytes;
    if (reader.Remaining() < bytes) {
        return CorruptPlayback(failure, ClipIssue::BulkLength, index);
    }

    if (auto resized = track.translation.TryResize(counts[TranslationSlot]); !resized) {
        return resized;
    }

    if (auto resized = track.rotation.TryResize(counts[RotationSlot]); !resized) {
        return resized;
    }

    if (auto resized = track.scale.TryResize(counts[ScaleSlot]); !resized) {
        return resized;
    }

    for (Vec3& value : track.translation) {
        value = reader.ReadVec3();
    }

    for (Quat& value : track.rotation) {
        value = reader.ReadQuat();
    }

    for (Vec3& value : track.scale) {
        value = reader.ReadVec3();
    }

    // 유한하지 않은 원소 수를 센다(성분과 원소마다 분기하지 않는다).
    uint32 nonFinite = 0;
    for (const Vec3& value : track.translation) {
        nonFinite += animation_detail::IsFiniteVec3(value) ? 0 : 1;
    }

    for (const Quat& value : track.rotation) {
        nonFinite += animation_detail::IsFiniteQuat(value) ? 0 : 1;
    }

    for (const Vec3& value : track.scale) {
        nonFinite += animation_detail::IsFiniteVec3(value) ? 0 : 1;
    }

    return nonFinite == 0 ? Result<void>::Success()
                          : CorruptPlayback(failure, ClipIssue::NonFiniteKey, index);
}

} // namespace

Result<void> LoadClipPlayback(const ClipPlaybackData& data, const Package& package,
                              ClipPlayback& out, ClipDiagnostic* failure) noexcept {
    if (data.sampleCount == 0) {
        return CorruptPlayback(failure, ClipIssue::NoSamples);
    }

    if (data.sampleCount - 1 > ClipMaxFrames) {
        return CorruptPlayback(failure, ClipIssue::TooManyFrames);
    }

    if (data.sampleRate.AsDecimal() > ClipMaxFrameRate) {
        return CorruptPlayback(failure, ClipIssue::FrameRateTooHigh);
    }

    auto bytes = package.BulkBytes(data.tracks);
    if (!bytes) {
        return animation_detail::ClipFail(failure, AssetDiagnosticCode::CorruptPackage,
                                          bytes.Error(), ClipIssue::BulkUnreadable);
    }

    Allocator& allocator = out.tracks.GetAllocator();
    ClipPlayback loaded(allocator);
    loaded.sampleRate = data.sampleRate;
    loaded.sampleCount = data.sampleCount;
    animation_detail::ClipByteReader reader(bytes.Value());
    for (uint32 index = 0; reader.Remaining() != 0; ++index) {
        if (index == MeshMaxBones) {
            return CorruptPlayback(failure, ClipIssue::TooManyTracks);
        }

        ClipPlaybackTrack track(allocator);
        if (auto decoded = DecodePlaybackTrack(reader, data.sampleCount, index, track, failure);
            !decoded) {
            return decoded;
        }

        if (auto pushed = loaded.tracks.TryPushBack(Move(track)); !pushed) {
            return pushed;
        }
    }

    if (data.trackToBone.Size() != loaded.tracks.Size()) {
        return CorruptPlayback(failure, ClipIssue::TrackBoneLength);
    }

    if (auto appended = loaded.trackToBone.TryAppend(data.trackToBone.View()); !appended) {
        return appended;
    }

    if (auto copied = CopyCurves(data.curves.View(), loaded.curves); !copied) {
        return copied;
    }

    out = Move(loaded);
    return Result<void>::Success();
}

} // namespace rocks
