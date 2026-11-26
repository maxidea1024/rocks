#include "AnimationInternal.h"

#include <Platform/Memory.h>

// 클립 원본 본 트랙의 검증·저장·로드·샘플링이다(Docs/Design/MeshAnimationAssets 7.2·7.5절,
// UE UAnimDataModel::Evaluate·FAnimationRuntime::GetKeyIndicesFromTime·FTransform::Blend).
namespace rocks::animation_detail {

Result<void> ClipFail(ClipDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                      ClipIssue issue, uint32 index, uint32 related) noexcept {
    if (failure != nullptr) {
        ClipDiagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.error = error;
        diagnostic.issue = issue;
        diagnostic.index = index;
        diagnostic.related = related;
        *failure = diagnostic;
    }

    return Result<void>::Failure(error);
}

int32 CompareFoldedText(StringView a, StringView b) noexcept {
    const usize common = a.ByteCount() < b.ByteCount() ? a.ByteCount() : b.ByteCount();
    for (usize at = 0; at < common; ++at) {
        auto fold = [](char value) noexcept {
            const auto byte = static_cast<uint8>(value);
            return byte >= 'A' && byte <= 'Z' ? static_cast<uint8>(byte - 'A' + 'a') : byte;
        };
        const uint8 left = fold(a.Data()[at]);
        const uint8 right = fold(b.Data()[at]);
        if (left != right) {
            return left < right ? -1 : 1;
        }
    }

    if (a.ByteCount() == b.ByteCount()) {
        return 0;
    }

    return a.ByteCount() < b.ByteCount() ? -1 : 1;
}

uint8* ClipByteWriter::Grow(usize count) noexcept {
    if (error_ != ErrorCode::None) {
        return nullptr;
    }

    const usize at = bytes_.Size();
    if (auto resized = bytes_.TryResize(at + count); !resized) {
        error_ = resized.Error();
        return nullptr;
    }

    return bytes_.Data() + at;
}

void ClipByteWriter::U32(uint32 value) noexcept {
    if (uint8* at = Grow(ClipCountBytes)) {
        StoreLittleEndian<uint32>(at, value);
    }
}

void ClipByteWriter::F32(float value) noexcept {
    U32(platform::BitCast<uint32>(value));
}

void ClipByteWriter::Vec3s(Span<const Vec3> values) noexcept {
    for (const Vec3& value : values) {
        F32(value.x);
        F32(value.y);
        F32(value.z);
    }
}

void ClipByteWriter::Quats(Span<const Quat> values) noexcept {
    for (const Quat& value : values) {
        F32(value.x);
        F32(value.y);
        F32(value.z);
        F32(value.w);
    }
}

uint32 ClipByteReader::U32() noexcept {
    const uint32 value = LoadLittleEndian<uint32>(bytes_.Data() + offset_);
    offset_ += ClipCountBytes;
    return value;
}

float ClipByteReader::F32() noexcept {
    return platform::BitCast<float>(U32());
}

Vec3 ClipByteReader::ReadVec3() noexcept {
    const float x = F32();
    const float y = F32();
    const float z = F32();
    return Vec3(x, y, z);
}

Quat ClipByteReader::ReadQuat() noexcept {
    const float x = F32();
    const float y = F32();
    const float z = F32();
    const float w = F32();
    return Quat(x, y, z, w);
}

// v − v는 유한하면 0이고 NaN·±Inf면 NaN이다(큰 유한값도 넘치지 않는다). 성분 차의 합이 0인지로
// 한 번에 본다.
bool IsFiniteVec3(const Vec3& value) noexcept {
    return (value.x - value.x) + (value.y - value.y) + (value.z - value.z) == 0.0f;
}

bool IsFiniteQuat(const Quat& value) noexcept {
    return (value.x - value.x) + (value.y - value.y) + (value.z - value.z) + (value.w - value.w) ==
           0.0f;
}

namespace {

template <class T>
const T& KeyAt(Span<const T> values, uint32 key) noexcept {
    return values.Size() == 1 ? values[0] : values[key];
}

// UE FMath::Lerp(A, B, Alpha) = A + Alpha × (B − A).
Vec3 LerpVec3(const Vec3& a, const Vec3& b, float alpha) noexcept {
    return Vec3(a.x + alpha * (b.x - a.x), a.y + alpha * (b.y - a.y), a.z + alpha * (b.z - a.z));
}

// UE FQuat::FastLerp(A, B, Alpha)(내적 부호로 최단 경로) 뒤 Normalize(제곱합이 하한 미만이면 단위).
Quat BlendRotation(const Quat& a, const Quat& b, float alpha) noexcept {
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float bias = dot >= 0.0f ? 1.0f : -1.0f;
    const float weight = bias * (1.0f - alpha);
    Quat blended(b.x * alpha + a.x * weight, b.y * alpha + a.y * weight, b.z * alpha + a.z * weight,
                 b.w * alpha + a.w * weight);
    const float squared = blended.x * blended.x + blended.y * blended.y + blended.z * blended.z +
                          blended.w * blended.w;
    if (squared >= ClipRotationMinimumLengthSquared) {
        const float scale = math::InvSqrt(squared);
        return Quat(blended.x * scale, blended.y * scale, blended.z * scale, blended.w * scale);
    }

    return Quat(0.0f, 0.0f, 0.0f, 1.0f);
}

} // namespace

Transform3f EvaluateClipKeys(FrameRate rate, uint32 keyCount, Span<const Vec3> translation,
                             Span<const Quat> rotation, Span<const Vec3> scale, double seconds,
                             ClipInterpolation interpolation) noexcept {
    uint32 first = 0;
    uint32 second = 0;
    float alpha = 0.0f;
    // UE GetKeyIndicesFromTime: 0 이하·키 하나는 키 0, 마지막 키 시간 이상은 마지막 키. NaN 시간은
    // 비교가 거짓이라 키 0이다. 양수 시간의 프레임은 0 이상이다.
    if (seconds > 0.0 && keyCount != 1) {
        const FrameTime frameTime = rate.AsFrameTime(seconds);
        const uint32 last = keyCount - 1;
        if (frameTime >= FrameTime(FrameNumber(static_cast<int32>(last)))) {
            first = last;
            second = last;
        } else {
            first = static_cast<uint32>(frameTime.Frame().value);
            second = first + 1;
            alpha = frameTime.SubFrame();
        }
    }

    if (interpolation == ClipInterpolation::Step || alpha < ClipKeySnapThreshold) {
        second = first;
    } else if (alpha > 1.0f - ClipKeySnapThreshold) {
        first = second;
    }

    Transform3f result;
    if (first == second) {
        result.translation = KeyAt(translation, first);
        result.rotation = KeyAt(rotation, first);
        result.scale = KeyAt(scale, first);
        return result;
    }

    result.translation = LerpVec3(KeyAt(translation, first), KeyAt(translation, second), alpha);
    result.scale = LerpVec3(KeyAt(scale, first), KeyAt(scale, second), alpha);
    result.rotation = BlendRotation(KeyAt(rotation, first), KeyAt(rotation, second), alpha);
    return result;
}

} // namespace rocks::animation_detail

namespace rocks {

namespace {

// 성분 번호다(진단 related).
enum ClipComponent : uint32 {
    TranslationComponent = 0,
    RotationComponent = 1,
    ScaleComponent = 2
};

bool ValidKeyCount(usize count, uint32 keys) noexcept {
    return count == 1 || count == keys;
}

Result<void> ValidateTrack(const ClipTrack& track, uint32 index, uint32 keys,
                           ClipDiagnostic* failure) noexcept {
    if (!animation_detail::IsUsableName(track.bone)) {
        return animation_detail::ClipInvalid(failure, ClipIssue::InvalidTrackName, index);
    }

    const usize counts[3] = {track.translation.Size(), track.rotation.Size(), track.scale.Size()};
    for (uint32 component = 0; component < 3; ++component) {
        if (!ValidKeyCount(counts[component], keys)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::TrackKeyCount, index,
                                                 component);
        }
    }

    for (const Vec3& value : track.translation) {
        if (!animation_detail::IsFiniteVec3(value)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::NonFiniteKey, index,
                                                 TranslationComponent);
        }
    }

    for (const Quat& value : track.rotation) {
        if (!animation_detail::IsFiniteQuat(value)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::NonFiniteKey, index,
                                                 RotationComponent);
        }

        const double length =
            static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y +
            static_cast<double>(value.z) * value.z + static_cast<double>(value.w) * value.w;
        if (math::Abs(length - 1.0) > BoneRotationNormalizedTolerance) {
            return animation_detail::ClipInvalid(failure, ClipIssue::RotationNotNormalized, index);
        }
    }

    for (const Vec3& value : track.scale) {
        if (!animation_detail::IsFiniteVec3(value)) {
            return animation_detail::ClipInvalid(failure, ClipIssue::NonFiniteKey, index,
                                                 ScaleComponent);
        }
    }

    return Result<void>::Success();
}

// 모든 원소가 bit 단위로 같으면 true다(상수 성분 — 7.2절).
template <class T>
bool IsConstant(Span<const T> values) noexcept {
    for (usize at = 1; at < values.Size(); ++at) {
        if (!platform::MemoryEquals(&values[at], &values[0], sizeof(T))) {
            return false;
        }
    }

    return true;
}

// 성분 하나를 접어(상수면 1원소) bulk entry로 더한다.
template <class T>
Result<BulkDataRef> StoreComponent(Span<const T> values, MemoryBulkSource& bulk,
                                   Allocator& allocator) noexcept {
    const Span<const T> stored = IsConstant(values) ? Span<const T>(values.Data(), 1) : values;
    animation_detail::ClipByteWriter writer(allocator);
    if constexpr (sizeof(T) == animation_detail::ClipQuatBytes) {
        writer.Quats(stored);
    } else {
        writer.Vec3s(stored);
    }

    if (auto status = writer.Status(); !status) {
        return Result<BulkDataRef>::Failure(status.Error());
    }

    return bulk.Add(Move(writer.Bytes()));
}

// 이름 원문을 lease로 읽어 트랙 번호와 함께 담는다(정렬용).
struct TrackText {
    uint32 track = 0;
    NameLease lease;
};

} // namespace

double ClipSource::Length() const noexcept {
    return frameRate.AsSeconds(FrameTime(FrameNumber(static_cast<int32>(frameCount))));
}

double ClipPlayback::Length() const noexcept {
    return sampleRate.AsSeconds(FrameTime(FrameNumber(static_cast<int32>(sampleCount - 1))));
}

Result<void> ValidateClipSource(const ClipSource& source, Allocator& scratch,
                                ClipDiagnostic* failure) noexcept {
    if (source.frameRate.AsDecimal() > ClipMaxFrameRate) {
        return animation_detail::ClipInvalid(failure, ClipIssue::FrameRateTooHigh);
    }

    if (source.frameCount > ClipMaxFrames) {
        return animation_detail::ClipFail(failure, AssetDiagnosticCode::InvalidClipData,
                                          ErrorCode::CapacityExceeded, ClipIssue::TooManyFrames);
    }

    if (source.tracks.Size() > MeshMaxBones) {
        return animation_detail::ClipFail(failure, AssetDiagnosticCode::InvalidClipData,
                                          ErrorCode::CapacityExceeded, ClipIssue::TooManyTracks);
    }

    const uint32 keys = source.KeyCount();
    for (uint32 track = 0; track < source.tracks.Size(); ++track) {
        if (auto valid = ValidateTrack(source.tracks[track], track, keys, failure); !valid) {
            return valid;
        }
    }

    Array<animation_detail::NamedItem> names(scratch);
    if (auto reserved = names.TryReserve(source.tracks.Size()); !reserved) {
        return reserved;
    }

    for (uint32 track = 0; track < source.tracks.Size(); ++track) {
        ROCKS_ASSERT(
            names.TryPushBack(animation_detail::MakeNamedItem(source.tracks[track].bone, track))
                .IsOk());
    }

    animation_detail::SortNamedItems(names.View());
    uint32 later = 0;
    uint32 earlier = 0;
    if (animation_detail::FindFirstDuplicate(names.View(), later, earlier)) {
        return animation_detail::ClipInvalid(failure, ClipIssue::DuplicateTrack, later, earlier);
    }

    return Result<void>::Success();
}

Result<void> StoreClipSource(const ClipSource& source, MemoryBulkSource& bulk, NamePool& names,
                             ClipSourceData& data, ClipDiagnostic* failure) noexcept {
    Allocator& allocator = data.boneTracks.GetAllocator();
    if (auto valid = ValidateClipSource(source, allocator, failure); !valid) {
        return valid;
    }

    // 이름 원문 순(ASCII 소문자로 접은 byte)으로 트랙 순서를 정한다.
    Array<TrackText> order(allocator);
    if (auto reserved = order.TryReserve(source.tracks.Size()); !reserved) {
        return reserved;
    }

    for (uint32 track = 0; track < source.tracks.Size(); ++track) {
        auto lease = names.Acquire(source.tracks[track].bone);
        if (!lease) {
            return animation_detail::ClipFail(failure, AssetDiagnosticCode::None, lease.Error(),
                                              ClipIssue::None, track);
        }

        ROCKS_ASSERT(order.TryPushBack(TrackText{track, Move(lease.Value())}).IsOk());
    }

    for (usize at = 1; at < order.Size(); ++at) {
        for (usize back = at;
             back > 0 && animation_detail::CompareFoldedText(order[back].lease.View(),
                                                             order[back - 1].lease.View()) < 0;
             --back) {
            TrackText moved = Move(order[back]);
            order[back] = Move(order[back - 1]);
            order[back - 1] = Move(moved);
        }
    }

    Array<ClipBoneTrack> tracks(allocator);
    if (auto reserved = tracks.TryReserve(source.tracks.Size()); !reserved) {
        return reserved;
    }

    for (const TrackText& entry : order) {
        const ClipTrack& track = source.tracks[entry.track];
        ClipBoneTrack stored;
        stored.bone = track.bone;
        auto translation = StoreComponent(track.translation.View(), bulk, allocator);
        if (!translation) {
            return Result<void>::Failure(translation.Error());
        }

        auto rotation = StoreComponent(track.rotation.View(), bulk, allocator);
        if (!rotation) {
            return Result<void>::Failure(rotation.Error());
        }

        auto scale = StoreComponent(track.scale.View(), bulk, allocator);
        if (!scale) {
            return Result<void>::Failure(scale.Error());
        }

        stored.translation = translation.Value();
        stored.rotation = rotation.Value();
        stored.scale = scale.Value();
        ROCKS_ASSERT(tracks.TryPushBack(stored).IsOk());
    }

    data.frameRate = source.frameRate;
    data.frameCount = source.frameCount;
    data.boneTracks = Move(tracks);
    return Result<void>::Success();
}

namespace {

// 원본 bulk를 읽는 곳이다(쿠킹 패키지·에디터 패키지는 Package, 쿠킹 처리기는 열린 패키지의 bulk
// 원본 — MeshSource::Load와 같은 두 경로).
struct ClipBulkReader {
    const Package* package = nullptr;
    PackageBulkSource* source = nullptr;

    Result<Span<const uint8>> Read(const BulkDataRef& ref) const noexcept {
        return package != nullptr ? package->BulkBytes(ref) : source->ReadBulk(ref);
    }
};

// ref의 bulk를 읽는다. 못 읽으면 CorruptPackage(BulkUnreadable)다.
Result<Span<const uint8>> ReadClipBulk(const ClipBulkReader& reader, const BulkDataRef& ref,
                                       uint32 index, ClipDiagnostic* failure) noexcept {
    auto bytes = reader.Read(ref);
    if (!bytes) {
        (void)animation_detail::ClipFail(failure, AssetDiagnosticCode::CorruptPackage,
                                         bytes.Error(), ClipIssue::BulkUnreadable, index);
    }

    return bytes;
}

// 성분 bulk를 values로 푼다(원소 크기의 배수이고 원소 수 1 또는 keys).
template <class T>
Result<void> DecodeComponent(Span<const uint8> bytes, uint32 keys, uint32 index, uint32 component,
                             Array<T>& values, ClipDiagnostic* failure) noexcept {
    const usize count = bytes.Size() / sizeof(T);
    if (bytes.Size() % sizeof(T) != 0 || !ValidKeyCount(count, keys)) {
        return animation_detail::ClipFail(failure, AssetDiagnosticCode::CorruptPackage,
                                          ErrorCode::InvalidArgument, ClipIssue::BulkLength, index,
                                          component);
    }

    if (auto resized = values.TryResize(count); !resized) {
        return resized;
    }

    animation_detail::ClipByteReader reader(bytes);
    for (T& value : values) {
        if constexpr (sizeof(T) == animation_detail::ClipQuatBytes) {
            value = reader.ReadQuat();
        } else {
            value = reader.ReadVec3();
        }
    }

    return Result<void>::Success();
}

Result<void> LoadTrack(const ClipBoneTrack& stored, const ClipBulkReader& reader, uint32 keys,
                       uint32 index, ClipTrack& track, ClipDiagnostic* failure) noexcept {
    track.bone = stored.bone;
    auto translation = ReadClipBulk(reader, stored.translation, index, failure);
    if (!translation) {
        return Result<void>::Failure(translation.Error());
    }

    if (auto decoded = DecodeComponent(translation.Value(), keys, index, TranslationComponent,
                                       track.translation, failure);
        !decoded) {
        return decoded;
    }

    auto rotation = ReadClipBulk(reader, stored.rotation, index, failure);
    if (!rotation) {
        return Result<void>::Failure(rotation.Error());
    }

    if (auto decoded = DecodeComponent(rotation.Value(), keys, index, RotationComponent,
                                       track.rotation, failure);
        !decoded) {
        return decoded;
    }

    auto scale = ReadClipBulk(reader, stored.scale, index, failure);
    if (!scale) {
        return Result<void>::Failure(scale.Error());
    }

    return DecodeComponent(scale.Value(), keys, index, ScaleComponent, track.scale, failure);
}

Result<void> LoadClipSourceFrom(const ClipSourceData& data, const ClipBulkReader& reader,
                                NamePool& names, ClipSource& out,
                                ClipDiagnostic* failure) noexcept {
    Allocator& allocator = out.tracks.GetAllocator();
    ClipSource loaded(allocator);
    loaded.frameRate = data.frameRate;
    loaded.frameCount = data.frameCount;
    // 키 수 계산이 넘치지 않게 프레임 상한을 배치보다 먼저 본다.
    if (data.frameCount > ClipMaxFrames || data.boneTracks.Size() > MeshMaxBones) {
        return animation_detail::ClipFail(
            failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument,
            data.frameCount > ClipMaxFrames ? ClipIssue::TooManyFrames : ClipIssue::TooManyTracks);
    }

    if (auto reserved = loaded.tracks.TryReserve(data.boneTracks.Size()); !reserved) {
        return reserved;
    }

    const uint32 keys = loaded.KeyCount();
    NameLease previous;
    for (uint32 index = 0; index < data.boneTracks.Size(); ++index) {
        const ClipBoneTrack& stored = data.boneTracks[index];
        auto lease = names.Acquire(stored.bone);
        if (!lease) {
            return animation_detail::ClipFail(failure, AssetDiagnosticCode::None, lease.Error(),
                                              ClipIssue::None, index);
        }

        if (index != 0 &&
            animation_detail::CompareFoldedText(previous.View(), lease.Value().View()) >= 0) {
            return animation_detail::ClipFail(failure, AssetDiagnosticCode::CorruptPackage,
                                              ErrorCode::InvalidArgument, ClipIssue::TrackOrder,
                                              index, index - 1);
        }

        previous = Move(lease.Value());
        ClipTrack track(allocator);
        if (auto loadedTrack = LoadTrack(stored, reader, keys, index, track, failure);
            !loadedTrack) {
            return loadedTrack;
        }

        ROCKS_ASSERT(loaded.tracks.TryPushBack(Move(track)).IsOk());
    }

    ClipDiagnostic validation;
    if (auto valid = ValidateClipSource(loaded, allocator, &validation); !valid) {
        if (validation.code == AssetDiagnosticCode::None) {
            return valid;
        }

        return animation_detail::ClipFail(failure, AssetDiagnosticCode::CorruptPackage,
                                          ErrorCode::InvalidArgument, validation.issue,
                                          validation.index, validation.related);
    }

    out = Move(loaded);
    return Result<void>::Success();
}

} // namespace

Result<void> LoadClipSource(const ClipSourceData& data, const Package& package, NamePool& names,
                            ClipSource& out, ClipDiagnostic* failure) noexcept {
    ClipBulkReader reader;
    reader.package = &package;
    return LoadClipSourceFrom(data, reader, names, out, failure);
}

Result<void> LoadClipSource(const ClipSourceData& data, PackageBulkSource& bulk, NamePool& names,
                            ClipSource& out, ClipDiagnostic* failure) noexcept {
    ClipBulkReader reader;
    reader.source = &bulk;
    return LoadClipSourceFrom(data, reader, names, out, failure);
}

Transform3f SampleClipSource(const ClipSource& source, uint32 track, double seconds,
                             ClipInterpolation interpolation) noexcept {
    const ClipTrack& keys = source.tracks[track];
    return animation_detail::EvaluateClipKeys(source.frameRate, source.KeyCount(),
                                              keys.translation.View(), keys.rotation.View(),
                                              keys.scale.View(), seconds, interpolation);
}

Transform3f SampleClipPlayback(const ClipPlayback& playback, uint32 track, double seconds,
                               ClipInterpolation interpolation) noexcept {
    const ClipPlaybackTrack& keys = playback.tracks[track];
    return animation_detail::EvaluateClipKeys(playback.sampleRate, playback.sampleCount,
                                              keys.translation.View(), keys.rotation.View(),
                                              keys.scale.View(), seconds, interpolation);
}

} // namespace rocks
