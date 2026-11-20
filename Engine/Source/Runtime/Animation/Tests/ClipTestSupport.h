#pragma once

#include "AnimationTestSupport.h"

#include <Animation/AnimationClip.generated.h>
#include <Animation/ClipSource.h>

// 애니메이션 클립 시험 공용 도우미다(Docs/Design/MeshAnimationAssets.ko.md 12절 "클립"): 원본
// 트랙 만들기, 클립 에셋 만들기·채우기, 패키지 왕복, 비교.
namespace rocks::clip_test {

using namespace rocks::animation_test;

inline TypeId ClipType() noexcept {
    return GeneratedType<AnimationClip>::Id();
}

inline float ClipNaN() noexcept {
    return platform::BitCast<float>(0x7fc00000u);
}

inline float ClipInfinity() noexcept {
    return platform::BitCast<float>(0x7f800000u);
}

inline FrameRate Rate(int32 numerator, int32 denominator = 1) noexcept {
    return FrameRate::TryCreate(numerator, denominator).Value();
}

// 키 k의 시험 값이다(트랙 seed마다 다르고 회전은 단위 quaternion).
inline Vec3 TranslationKey(uint32 seed, uint32 key) noexcept {
    return Vec3(static_cast<float>(seed) + 0.5f * static_cast<float>(key),
                -2.0f * static_cast<float>(key), 3.0f);
}

inline Quat RotationKey(uint32 seed, uint32 key) noexcept {
    // z축 회전(각 = 0.1 × (seed + key) 라디안의 반각 — 결정적 다항식 근사가 아니라 고정 표).
    const float half = 0.05f * static_cast<float>(seed + key);
    const float sine = half - half * half * half / 6.0f;
    const float cosine = 1.0f - half * half / 2.0f;
    const float length = math::Sqrt(sine * sine + cosine * cosine);
    return Quat(0.0f, 0.0f, sine / length, cosine / length);
}

inline Vec3 ScaleKey(uint32, uint32) noexcept {
    return Vec3(1.0f, 1.0f, 1.0f);
}

// name 트랙을 keys개 키로 만든다(constantRotation이면 회전 1키).
inline bool AddTrack(NamePool& names, ClipSource& source, StringView name, uint32 seed,
                     bool constantRotation = false) noexcept {
    ClipTrack track(source.tracks.GetAllocator());
    track.bone = Intern(names, name);
    const uint32 keys = source.KeyCount();
    for (uint32 key = 0; key < keys; ++key) {
        if (!track.translation.TryPushBack(TranslationKey(seed, key)) ||
            (!(constantRotation && key != 0) &&
             !track.rotation.TryPushBack(RotationKey(seed, key))) ||
            !track.scale.TryPushBack(ScaleKey(seed, key))) {
            return false;
        }
    }

    return source.tracks.TryPushBack(Move(track)).IsOk();
}

// 사람 계층 이름의 트랙(일부)과 스켈레톤에 없는 트랙·가상 본 트랙을 담은 원본이다(30 fps, frames
// 프레임). 트랙은 이름 순이 아니다(Store가 정렬한다).
inline bool BuildHumanSource(NamePool& names, ClipSource& source, uint32 frames) noexcept {
    source.frameRate = Rate(30);
    source.frameCount = frames;
    return AddTrack(names, source, "spine", 3) && AddTrack(names, source, "root", 1, true) &&
           AddTrack(names, source, "Tail", 9) && AddTrack(names, source, "pelvis", 2) &&
           AddTrack(names, source, "VB spine_head", 7) && AddTrack(names, source, "hand_l", 5);
}

// 사람 계층 스켈레톤(가상 본 "VB spine_head" 포함)을 skeleton에 채운다.
inline bool FillHumanSkeleton(NamePool& names, Skeleton& skeleton) noexcept {
    if (!BuildHierarchy(names, HumanBones, skeleton.bones)) {
        return false;
    }

    const VirtualBone bone{Intern(names, "spine"), Intern(names, "head"),
                           Intern(names, "VB spine_head")};
    return skeleton.virtualBones.TryPushBack(bone).IsOk();
}

// 키 셋이 bit 단위로 같은지 본다.
template <class T>
bool SameKeys(const Array<T>& a, const Array<T>& b) noexcept {
    return a.Size() == b.Size() &&
           (a.IsEmpty() || platform::MemoryEquals(a.Data(), b.Data(), a.Size() * sizeof(T)));
}

inline bool SameTrack(const ClipTrack& a, const ClipTrack& b) noexcept {
    return a.bone == b.bone && SameKeys(a.translation, b.translation) &&
           SameKeys(a.rotation, b.rotation) && SameKeys(a.scale, b.scale);
}

inline bool SamePlaybackTrack(const ClipPlaybackTrack& a, const ClipPlaybackTrack& b) noexcept {
    return SameKeys(a.translation, b.translation) && SameKeys(a.rotation, b.rotation) &&
           SameKeys(a.scale, b.scale);
}

inline bool SameTransform3f(const Transform3f& a, const Transform3f& b) noexcept {
    return platform::MemoryEquals(&a, &b, sizeof(Transform3f));
}

// 커브 이름·표시·키 bit가 같은지 본다.
inline bool SameCurve(const ClipCurve& a, const ClipCurve& b) noexcept {
    const Span<const TangentCurveKey> left = a.curve.Keys();
    const Span<const TangentCurveKey> right = b.curve.Keys();
    return a.name == b.name && a.editable == b.editable && a.metadata == b.metadata &&
           a.disabled == b.disabled && left.Size() == right.Size() &&
           (left.IsEmpty() || platform::MemoryEquals(left.Data(), right.Data(), left.ByteCount()));
}

inline bool SamePlayback(const ClipPlayback& a, const ClipPlayback& b) noexcept {
    if (!(a.sampleRate == b.sampleRate) || a.sampleCount != b.sampleCount ||
        !SameKeys(a.trackToBone, b.trackToBone) || a.tracks.Size() != b.tracks.Size() ||
        a.curves.Size() != b.curves.Size()) {
        return false;
    }

    for (usize at = 0; at < a.tracks.Size(); ++at) {
        if (!SamePlaybackTrack(a.tracks[at], b.tracks[at])) {
            return false;
        }
    }

    for (usize at = 0; at < a.curves.Size(); ++at) {
        if (!SameCurve(a.curves[at], b.curves[at])) {
            return false;
        }
    }

    return true;
}

// 키 둘인 TangentCurve 커브를 만든다.
inline bool MakeCurve(NamePool& names, StringView name, ClipCurve& out) noexcept {
    out.name = Intern(names, name);
    return out.curve.TryAddKey(0.0f, 1.0f).IsOk() && out.curve.TryAddKey(1.0f, 2.0f).IsOk();
}

// 클립의 이벤트·동기 마커·설정을 채운다(길이 ≥ 1초 가정 — 정렬된 상태).
inline bool FillClipSettings(NamePool& names, AnimationClip& clip) noexcept {
    ClipEvent step;
    step.time = 0.25f;
    step.track = 1;
    step.name = Intern(names, "Footstep");
    step.type = Intern(names, "AnimNotify_PlaySound");
    ClipEvent window = step;
    window.time = 0.25f;
    window.track = 2;
    window.duration = 0.5f;
    window.name = Intern(names, "Window");
    window.triggerChance = 0.5f;
    window.triggerOnDedicatedServer = false;
    window.triggerOnFollower = true;
    ClipSyncMarker left;
    left.name = Intern(names, "LeftFoot");
    left.time = 0.1f;
    ClipSyncMarker right = left;
    right.name = Intern(names, "RightFoot");
    right.time = 0.6f;
    right.track = 1;
    clip.interpolation = ClipInterpolation::Step;
    clip.rateScale = 1.5f;
    clip.loop = true;
    clip.rootMotion.enable = true;
    clip.rootMotion.rootLock = RootMotionRootLock::FirstFrame;
    clip.additive.type = AdditiveType::LocalSpace;
    clip.additive.basePose = AdditiveBasePose::Frame;
    clip.additive.basePoseFrame = 3;
    clip.retargetSource = Intern(names, "TPose");
    clip.targetFrameRate.defaultRate = Rate(30);
    return clip.eventTracks.TryPushBack(Intern(names, "Feet")).IsOk() &&
           clip.eventTracks.TryPushBack(Intern(names, "Sound")).IsOk() &&
           clip.eventTracks.TryPushBack(Intern(names, "Window")).IsOk() &&
           clip.events.TryPushBack(step).IsOk() && clip.events.TryPushBack(window).IsOk() &&
           clip.syncMarkers.TryPushBack(left).IsOk() &&
           clip.syncMarkers.TryPushBack(right).IsOk() &&
           clip.targetFrameRate.overrides.InsertOrAssign(Intern(names, "Mobile"), Rate(15)).IsOk();
}

// env.source에 id 패키지와 AnimationClip을 만들고 handle을 돌려준다.
inline ObjectHandle MakeClip(asset_test::AssetEnv& env, uint64 id, PackageKind kind,
                             ObjectHandle& package) noexcept {
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(id), kind);
    if (!made) {
        return ObjectHandle{};
    }

    package = made.Value().Handle();
    return asset_test::Make<AnimationClip>(registry, ClipType(), package, "");
}

// 패키지를 bulk와 함께 쓰고 target에 로드한다. 실패하면 빈 handle이다.
struct LoadedClip {
    explicit LoadedClip(Allocator& allocator) noexcept : bytes(allocator) {
    }

    RootRef<Object> root;
    ObjectHandle package{};
    ObjectHandle clip{};
    Array<uint8> bytes;
};

inline bool WriteAndLoad(asset_test::AssetEnv& env, ObjectHandle package, PackageKind kind,
                         MemoryBulkSource& bulk, LoadedClip& out,
                         AssetDiagnostic* failure = nullptr) noexcept {
    PackageWriteOptions options =
        kind == PackageKind::Cooked ? asset_test::CookedOptions() : asset_test::EditorOptions();
    options.bulkSource = &bulk;
    out.bytes = asset_test::Write(env.allocator, *env.source, package, options);
    if (out.bytes.IsEmpty()) {
        return false;
    }

    Array<uint8> files[1] = {asset_test::Copy(env.allocator, out.bytes.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    if (!asset_test::LoadAll(env, *env.target, files, kind, nullptr, roots, handles, failure)) {
        return false;
    }

    out.root = Move(roots[0]);
    out.package = handles[0];
    auto found = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    if (!found) {
        return false;
    }

    out.clip = found.Value();
    return true;
}

} // namespace rocks::clip_test
