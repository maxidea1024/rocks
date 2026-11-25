#pragma once

#include <Animation/Skeleton.generated.h>
#include <Animation/Skeleton.h>
#include <Animation/SkeletonMerge.h>
#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// Animation 시험 공용 도우미다(Docs/Design/MeshAnimationAssets 12절 "스켈레톤·스킨"):
// 이름으로 본 계층 만들기, 스켈레톤 에셋 만들기와 채우기, 계층 비교, 무작위 계층.
namespace rocks::animation_test {

inline TypeId SkeletonType() noexcept {
    return GeneratedType<Skeleton>::Id();
}

// 본 하나의 이름·부모 번호다.
struct BoneSpec {
    StringView name;
    int32 parent = NoParentBone;
};

// 시험 pose: 본 번호로 정한 이동과 단위 회전·크기.
inline Transform PoseFor(uint32 bone) noexcept {
    return Transform(Quatd::Identity(), Vec3d(static_cast<double>(bone), 2.0 * bone, -0.5),
                     Vec3d(1.0, 1.0, 1.0));
}

// specs 순서대로 본을 더한 계층을 만든다(exportName = 이름 원문, pose = PoseFor(번호) +
// poseOffset).
inline bool BuildHierarchy(NamePool& names, Span<const BoneSpec> specs, BoneHierarchy& out,
                           double poseOffset = 0.0) noexcept {
    for (uint32 at = 0; at < specs.Size(); ++at) {
        auto name = names.TryIntern(specs[at].name);
        if (!name) {
            return false;
        }

        BoneInfo bone(out.bones.GetAllocator());
        bone.name = name.Value();
        bone.parentIndex = specs[at].parent;
        Transform pose = PoseFor(at);
        pose.translation.z += poseOffset;
        if (!bone.exportName.TryAssign(specs[at].name) || !out.bones.TryPushBack(Move(bone)) ||
            !out.referencePose.TryPushBack(pose)) {
            return false;
        }
    }

    return true;
}

// prefix 뒤에 index를 소문자 26진수로 붙인 이름 원문이다(숫자 접미사가 아니라 Name 번호 해석을 타지
// 않는다). 많은 본을 만드는 상한 시험이 쓴다. prefix가 'n'이면 "none"(None)이 나올 수 있어 쓰지
// 않는다.
inline StringView LetterName(char prefix, uint32 index, char (&text)[16]) noexcept {
    usize length = 0;
    text[length++] = prefix;
    char letters[8] = {};
    usize count = 0;
    do {
        letters[count++] = static_cast<char>('a' + index % 26);
        index /= 26;
    } while (index != 0);
    while (count != 0) {
        text[length++] = letters[--count];
    }

    return StringView(text, length);
}

inline Name Intern(NamePool& names, StringView text) noexcept {
    auto name = names.TryIntern(text);
    return name ? name.Value() : Name{};
}

inline bool SameTransform(const Transform& a, const Transform& b) noexcept {
    return platform::MemoryEquals(&a, &b, sizeof(Transform));
}

// 두 계층이 같은지 본다(이름 토큰·표시, 부모, exportName, pose bit).
inline bool SameHierarchy(const BoneHierarchy& a, const BoneHierarchy& b) noexcept {
    if (a.bones.Size() != b.bones.Size() || a.referencePose.Size() != b.referencePose.Size()) {
        return false;
    }

    for (usize at = 0; at < a.bones.Size(); ++at) {
        if (!(a.bones[at].name == b.bones[at].name) ||
            a.bones[at].name.displayId != b.bones[at].name.displayId ||
            a.bones[at].parentIndex != b.bones[at].parentIndex ||
            !(a.bones[at].exportName.View() == b.bones[at].exportName.View())) {
            return false;
        }
    }

    for (usize at = 0; at < a.referencePose.Size(); ++at) {
        if (!SameTransform(a.referencePose[at], b.referencePose[at])) {
            return false;
        }
    }

    return true;
}

// 사람 모양 시험 계층: root → pelvis → spine → {head, arm_l → hand_l, arm_r → hand_r}.
inline constexpr BoneSpec HumanBones[] = {
    {"root", NoParentBone}, {"pelvis", 0}, {"spine", 1}, {"head", 2},
    {"arm_l", 2},           {"hand_l", 4}, {"arm_r", 2}, {"hand_r", 6}};

// env.source에 id 패키지와 Skeleton을 만들고 handle을 돌려준다(실패하면 null handle).
inline ObjectHandle MakeSkeleton(asset_test::AssetEnv& env, uint64 id) noexcept {
    ObjectRegistry& registry = *env.source;
    auto package =
        NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(id), PackageKind::Editor);
    if (!package) {
        return ObjectHandle{};
    }

    return asset_test::Make<Skeleton>(registry, SkeletonType(), package.Value().Handle(), "");
}

// 모든 필드를 채운 정상 스켈레톤으로 만든다(사람 계층, 리타깃 방식, 가상 본 둘, 소켓 둘, 커브 둘,
// 슬롯 묶음 둘, 리타깃 원본 하나, 호환 스켈레톤 하나).
inline bool FillSkeleton(NamePool& names, Skeleton& skeleton) noexcept {
    Allocator& allocator = skeleton.bones.bones.GetAllocator();
    if (!BuildHierarchy(names, HumanBones, skeleton.bones)) {
        return false;
    }

    for (usize bone = 0; bone < skeleton.bones.bones.Size(); ++bone) {
        const auto mode = static_cast<BoneTranslationRetarget>(bone % BoneTranslationRetargetCount);
        if (!skeleton.retargetModes.TryPushBack(mode)) {
            return false;
        }
    }

    const VirtualBone virtuals[] = {
        {Intern(names, "spine"), Intern(names, "hand_l"), Intern(names, "VB spine_hand_l")},
        {Intern(names, "VB spine_hand_l"), Intern(names, "head"), Intern(names, "vb chain")},
        {Intern(names, "spine"), Intern(names, "head"), Intern(names, "VB spine_head")}};
    for (const VirtualBone& bone : virtuals) {
        if (!skeleton.virtualBones.TryPushBack(bone)) {
            return false;
        }
    }

    BoneSocket socket;
    socket.name = Intern(names, "weapon");
    socket.boneName = Intern(names, "hand_r");
    socket.relativeLocation = Vec3d(1.5, -2.0, 0.25);
    socket.relativeRotation = Rotator<double>(10.0, -20.0, 30.0);
    socket.relativeScale = Vec3d(1.0, 2.0, 1.0);
    socket.forceAlwaysAnimated = false;
    BoneSocket aim = socket;
    aim.name = Intern(names, "aim");
    aim.boneName = Intern(names, "VB chain");
    if (!skeleton.sockets.TryPushBack(socket) || !skeleton.sockets.TryPushBack(aim)) {
        return false;
    }

    SkeletonCurveInfo blink(allocator);
    blink.name = Intern(names, "Blink");
    blink.isMorphTarget = true;
    blink.maxLod = 2;
    SkeletonCurveInfo tint(allocator);
    tint.name = Intern(names, "Tint");
    tint.isMaterial = true;
    if (!blink.linkedBones.TryPushBack(Intern(names, "head")) ||
        !blink.linkedBones.TryPushBack(Intern(names, "spine")) ||
        !skeleton.curves.TryPushBack(Move(blink)) || !skeleton.curves.TryPushBack(Move(tint))) {
        return false;
    }

    AnimSlotGroup body(allocator);
    body.name = Intern(names, "DefaultGroup");
    AnimSlotGroup face(allocator);
    face.name = Intern(names, "Face");
    if (!body.slots.TryPushBack(Intern(names, "DefaultSlot")) ||
        !body.slots.TryPushBack(Intern(names, "UpperBody")) ||
        !face.slots.TryPushBack(Intern(names, "Mouth")) ||
        !skeleton.slotGroups.TryPushBack(Move(body)) ||
        !skeleton.slotGroups.TryPushBack(Move(face))) {
        return false;
    }

    NamedReferencePose pose(allocator);
    pose.name = Intern(names, "TPose");
    for (uint32 bone = 0; bone < 3; ++bone) {
        if (!pose.pose.TryPushBack(PoseFor(bone + 10))) {
            return false;
        }
    }

    if (!skeleton.retargetSources.TryPushBack(Move(pose))) {
        return false;
    }

    SoftObjectRef<Skeleton> compatible(allocator);
    return compatible.TryAssign(asset_test::FixedId(0x5150), "") &&
           skeleton.compatibleSkeletons.TryPushBack(Move(compatible)) && [&] {
               skeleton.useRetargetModesFromCompatibleSkeleton = true;
               return true;
           }();
}

// 고정 seed 퍼징용 무작위 계층이다: 대부분 정상(부모는 앞 본, 이름 "b<번호>")이고 확률로 불변식
// 하나를 어긴다(부모 범위·두 번째 루트·같은 이름·NaN·정규화되지 않은 회전·pose 길이).
inline bool RandomHierarchy(RandomStream& random, NamePool& names, BoneHierarchy& out,
                            uint32 maxBones) noexcept {
    const uint32 count = random.NextUInt32Below(maxBones + 1);
    char text[16] = {};
    for (uint32 bone = 0; bone < count; ++bone) {
        BoneInfo info(out.bones.GetAllocator());
        uint32 label = random.NextUInt32Below(100) < 2 && bone != 0
                           ? random.NextUInt32Below(bone)
                           : bone + random.NextUInt32Below(3) * 1000;
        usize length = 0;
        text[length++] = 'b';
        char digits[12] = {};
        usize digitCount = 0;
        do {
            digits[digitCount++] = static_cast<char>('0' + label % 10);
            label /= 10;
        } while (label != 0);
        while (digitCount != 0) {
            text[length++] = digits[--digitCount];
        }

        auto name = names.TryIntern(StringView(text, length));
        if (!name) {
            return false;
        }

        info.name = name.Value();
        info.parentIndex =
            bone == 0 ? NoParentBone : static_cast<int32>(random.NextUInt32Below(bone));
        const uint32 noise = random.NextUInt32Below(200);
        if (noise == 0) {
            info.parentIndex =
                static_cast<int32>(bone) + static_cast<int32>(random.NextUInt32Below(3));
        } else if (noise == 1) {
            info.parentIndex = NoParentBone;
        }

        Transform pose = PoseFor(bone);
        const uint32 poseNoise = random.NextUInt32Below(300);
        if (poseNoise == 0) {
            pose.translation.y = static_cast<double>(platform::BitCast<float>(0x7fc00000u));
        } else if (poseNoise == 1) {
            pose.rotation = Quatd(0.0, 0.0, 0.0, 2.0);
        }

        if (!out.bones.TryPushBack(Move(info)) || !out.referencePose.TryPushBack(pose)) {
            return false;
        }
    }

    if (count != 0 && random.NextUInt32Below(100) == 0) {
        (void)out.referencePose.TryResize(count - 1);
    }

    return true;
}

} // namespace rocks::animation_test
