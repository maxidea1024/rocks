#include "AnimationTestSupport.h"

#include <TestSupport/NFailAllocator.h>

// 본 계층·스켈레톤 시험이다(Docs/Design/MeshAnimationAssets.ko.md 6.1·6.2·6.4절, 12절
// "스켈레톤·스킨"): 계층 불변식(규칙마다 하나씩 어기기, 경계), pose 정규화, 스켈레톤 필드 하나씩
// 변조 거부, 저장 → 로드 → 재저장 byte 일치, 진단 이름.
using namespace rocks;
namespace skeleton_tests_file {

using namespace rocks::animation_test;

// 본 수 상한 시험이 기본 pool 상한(65536 entry)보다 많은 이름을 만든다.
constexpr uint64 BoneLimitPoolEntries = 1u << 18;

struct PoolFixture {
    PoolFixture() noexcept : names(allocator) {
        NamePoolConfig config;
        config.maxEntries = BoneLimitPoolEntries;
        ready = names.Initialize(config).IsOk();
    }

    SystemAllocator allocator;
    NamePool names;
    bool ready = false;
};

SkeletonDiagnostic Validate(const BoneHierarchy& hierarchy, Allocator& allocator,
                            bool& valid) noexcept {
    SkeletonDiagnostic failure;
    valid = ValidateBoneHierarchy(hierarchy, allocator, &failure).IsOk();
    return failure;
}

// 정상 계층: 빈 계층, 루트 하나, 사람 모양(가지), 이름 찾기는 Name 비교 규칙(대소문자 무시)이다.
TEST(Skeleton_HierarchyAcceptsValid) {
    PoolFixture pool;
    REQUIRE(pool.ready);
    BoneHierarchy empty(pool.allocator);
    CHECK(ValidateBoneHierarchy(empty, pool.allocator).IsOk());
    BoneHierarchy human(pool.allocator);
    REQUIRE(BuildHierarchy(pool.names, HumanBones, human));
    CHECK(ValidateBoneHierarchy(human, pool.allocator).IsOk());
    CHECK(FindBoneIndex(human, Intern(pool.names, "HAND_L")) == 5);
    CHECK(FindBoneIndex(human, Intern(pool.names, "tail")) == NoParentBone);
    // 번호만 다른 이름(arm_1·arm_2)과 다른 pool의 이름은 서로 다른 이름이다.
    const BoneSpec numbered[] = {{"root", NoParentBone}, {"arm_1", 0}, {"arm_2", 0}, {"arm", 0}};
    BoneHierarchy mixed(pool.allocator);
    REQUIRE(BuildHierarchy(pool.names, numbered, mixed));
    NamePool other(pool.allocator);
    REQUIRE(other.Initialize());
    BoneInfo foreign(pool.allocator);
    foreign.name = Intern(other, "arm_1");
    foreign.parentIndex = 0;
    REQUIRE(mixed.bones.TryPushBack(Move(foreign)));
    REQUIRE(mixed.referencePose.TryPushBack(Transform()));
    CHECK(ValidateBoneHierarchy(mixed, pool.allocator).IsOk());
    CHECK(FindBoneIndex(mixed, Intern(pool.names, "ARM_2")) == 2);
    CHECK(FindBoneIndex(mixed, mixed.bones[4].name) == 4);
    const BoneSpec root[] = {{"root", NoParentBone}};
    BoneHierarchy single(pool.allocator);
    REQUIRE(BuildHierarchy(pool.names, root, single));
    CHECK(ValidateBoneHierarchy(single, pool.allocator).IsOk());
}

// 규칙마다 하나씩 어긴 계층은 정해진 세부 규칙·위치로 InvalidSkeletonData다(본 수만
// CapacityExceeded).
TEST(Skeleton_HierarchyRejectsEachRule) {
    PoolFixture pool;
    REQUIRE(pool.ready);
    struct Case {
        SkeletonIssue issue;
        uint32 index;
        uint32 related;
    };
    // 변조 방법 번호와 기대 진단(번호 순서대로 아래 switch가 계층을 고친다).
    const Case cases[] = {{SkeletonIssue::PoseLengthMismatch, SkeletonNoIndex, SkeletonNoIndex},
                          {SkeletonIssue::RootParent, 0, SkeletonNoIndex},
                          {SkeletonIssue::ParentOrder, 3, SkeletonNoIndex},
                          {SkeletonIssue::ParentOrder, 4, SkeletonNoIndex},
                          {SkeletonIssue::ParentOrder, 5, SkeletonNoIndex},
                          {SkeletonIssue::ParentOrder, 6, SkeletonNoIndex},
                          {SkeletonIssue::InvalidBoneName, 2, SkeletonNoIndex},
                          {SkeletonIssue::InvalidBoneName, 7, SkeletonNoIndex},
                          {SkeletonIssue::DuplicateBoneName, 6, 4},
                          {SkeletonIssue::NonFinitePose, 1, SkeletonNoIndex},
                          {SkeletonIssue::NonFinitePose, 2, SkeletonNoIndex},
                          {SkeletonIssue::NonFinitePose, 3, SkeletonNoIndex},
                          {SkeletonIssue::RotationNotNormalized, 4, SkeletonNoIndex},
                          {SkeletonIssue::RotationNotNormalized, 5, SkeletonNoIndex}};
    const double nan = static_cast<double>(platform::BitCast<float>(0x7fc00000u));
    for (uint32 variant = 0; variant < sizeof(cases) / sizeof(cases[0]); ++variant) {
        BoneHierarchy bones(pool.allocator);
        REQUIRE(BuildHierarchy(pool.names, HumanBones, bones));
        switch (variant) {
        case 0:
            (void)bones.referencePose.TryResize(7);
            break;
        case 1:
            bones.bones[0].parentIndex = 0;
            break;
        case 2:
            bones.bones[3].parentIndex = 3; // 자기 자신
            break;
        case 3:
            bones.bones[4].parentIndex = 6; // 뒤 본
            break;
        case 4:
            bones.bones[5].parentIndex = NoParentBone; // 두 번째 루트
            break;
        case 5:
            bones.bones[6].parentIndex = -2;
            break;
        case 6:
            bones.bones[2].name = Name{}; // None
            break;
        case 7:
            bones.bones[7].name.displayId = 0; // 구조가 잘못된 토큰
            break;
        case 8:
            bones.bones[6].name = Intern(pool.names, "ARM_L"); // 대소문자만 다른 같은 이름
            break;
        case 9:
            bones.referencePose[1].translation.x = nan;
            break;
        case 10:
            bones.referencePose[2].scale.z = nan;
            break;
        case 11:
            bones.referencePose[3].rotation.w = nan;
            break;
        case 12:
            bones.referencePose[4].rotation = Quatd(0.0, 0.0, 0.0, 1.02); // 길이² ≈ 1.04
            break;
        default:
            bones.referencePose[5].rotation = Quatd(0.0, 0.0, 0.0, 0.98); // 길이² ≈ 0.96
            break;
        }

        bool valid = true;
        const SkeletonDiagnostic failure = Validate(bones, pool.allocator, valid);
        CHECK(!valid);
        CHECK(failure.code == AssetDiagnosticCode::InvalidSkeletonData);
        CHECK(failure.error == ErrorCode::InvalidArgument);
        CHECK(failure.issue == cases[variant].issue);
        CHECK(failure.index == cases[variant].index && failure.related == cases[variant].related);
    }

    // 회전 허용 경계 안쪽(길이² ≈ 1.00999…, 0.99001…)은 받는다.
    BoneHierarchy near(pool.allocator);
    REQUIRE(BuildHierarchy(pool.names, HumanBones, near));
    near.referencePose[1].rotation = Quatd(0.0, 0.0, 0.0, 1.004987);
    near.referencePose[2].rotation = Quatd(0.0, 0.0, 0.0, 0.994988);
    CHECK(ValidateBoneHierarchy(near, pool.allocator).IsOk());
}

// 본 수 상한: MeshMaxBones개는 받고 하나 더면 TooManyBones(CapacityExceeded)다.
TEST(Skeleton_HierarchyBoneLimit) {
    PoolFixture pool;
    REQUIRE(pool.ready);
    BoneHierarchy bones(pool.allocator);
    REQUIRE(bones.bones.TryReserve(MeshMaxBones + 1));
    char text[16] = {};
    for (uint32 bone = 0; bone <= MeshMaxBones; ++bone) {
        BoneInfo info(pool.allocator);
        info.name = Intern(pool.names, LetterName('q', bone, text));
        info.parentIndex = bone == 0 ? NoParentBone : static_cast<int32>(bone - 1);
        REQUIRE(bones.bones.TryPushBack(Move(info)));
        REQUIRE(bones.referencePose.TryPushBack(Transform()));
    }

    SkeletonDiagnostic failure;
    auto tooMany = ValidateBoneHierarchy(bones, pool.allocator, &failure);
    CHECK(!tooMany && tooMany.Error() == ErrorCode::CapacityExceeded);
    CHECK(failure.issue == SkeletonIssue::TooManyBones && failure.index == MeshMaxBones);
    (void)bones.bones.TryRemoveAt(MeshMaxBones, 1);
    (void)bones.referencePose.TryResize(MeshMaxBones);
    CHECK(ValidateBoneHierarchy(bones, pool.allocator).IsOk());
}

// pose 정규화: 길이가 1이 아닌 회전은 단위로, 길이² < BoneRotationMinimumLengthSquared는 단위
// 회전으로 바꾸고, 이미 단위·비유한은 그대로다. 바꾼 수를 돌려준다. 정규화 뒤 검증을 통과한다.
TEST(Skeleton_NormalizeReferencePose) {
    PoolFixture pool;
    REQUIRE(pool.ready);
    BoneHierarchy bones(pool.allocator);
    REQUIRE(BuildHierarchy(pool.names, HumanBones, bones));
    bones.referencePose[1].rotation = Quatd(0.0, 0.0, 0.0, 3.0);
    bones.referencePose[2].rotation = Quatd(0.0, 1.0e-5, 0.0, 0.0);
    bones.referencePose[3].rotation = Quatd(1.0, 1.0, 1.0, 1.0);
    const double nan = static_cast<double>(platform::BitCast<float>(0x7fc00000u));
    bones.referencePose[4].rotation = Quatd(nan, 0.0, 0.0, 1.0);
    CHECK(NormalizeBoneReferencePose(bones) == 3);
    CHECK(bones.referencePose[1].rotation.w == 1.0);
    CHECK(bones.referencePose[2].rotation.w == 1.0 && bones.referencePose[2].rotation.y == 0.0);
    CHECK(bones.referencePose[3].rotation.x == 0.5 && bones.referencePose[3].rotation.w == 0.5);
    CHECK(bones.referencePose[4].rotation.x != bones.referencePose[4].rotation.x);
    CHECK(NormalizeBoneReferencePose(bones) == 0);
    bones.referencePose[4].rotation = Quatd::Identity();
    CHECK(ValidateBoneHierarchy(bones, pool.allocator).IsOk());
}

// 스켈레톤 필드 하나씩 변조: 모든 필드를 채운 정상 스켈레톤은 통과하고, 필드마다 규칙 하나를 어기면
// 정해진 세부 규칙·위치로 거부한다.
TEST(Skeleton_ValidateRejectsEachField) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    const ObjectHandle handle = MakeSkeleton(env, 0x100);
    Skeleton* skeleton = asset_test::Pointer<Skeleton>(*env.source, handle);
    REQUIRE(skeleton != nullptr);
    REQUIRE(FillSkeleton(env.names, *skeleton));
    CHECK(ValidateSkeleton(*skeleton, env.names, env.allocator).IsOk());

    struct Case {
        SkeletonIssue issue;
        uint32 index;
        uint32 related;
    };
    const Case cases[] = {{SkeletonIssue::ParentOrder, 3, SkeletonNoIndex},
                          {SkeletonIssue::RetargetModeCount, SkeletonNoIndex, SkeletonNoIndex},
                          {SkeletonIssue::RetargetModeRange, 2, SkeletonNoIndex},
                          {SkeletonIssue::VirtualBoneName, 0, SkeletonNoIndex},
                          {SkeletonIssue::VirtualBoneName, 1, SkeletonNoIndex},
                          {SkeletonIssue::VirtualBoneName, 1, SkeletonNoIndex},
                          {SkeletonIssue::DuplicateVirtualBone, 1, SkeletonNoIndex},
                          {SkeletonIssue::DuplicateVirtualBone, 1, 0},
                          {SkeletonIssue::VirtualBoneTarget, 0, SkeletonNoIndex},
                          {SkeletonIssue::VirtualBoneTarget, 0, SkeletonNoIndex},
                          {SkeletonIssue::InvalidSocket, 0, SkeletonNoIndex},
                          {SkeletonIssue::InvalidSocket, 1, 0},
                          {SkeletonIssue::SocketBone, 1, SkeletonNoIndex},
                          {SkeletonIssue::NonFiniteSocket, 0, SkeletonNoIndex},
                          {SkeletonIssue::NonFiniteSocket, 0, SkeletonNoIndex},
                          {SkeletonIssue::NonFiniteSocket, 1, SkeletonNoIndex},
                          {SkeletonIssue::InvalidCurve, 1, SkeletonNoIndex},
                          {SkeletonIssue::InvalidCurve, 1, 0},
                          {SkeletonIssue::CurveBone, 0, 1},
                          {SkeletonIssue::InvalidSlotGroup, 0, SkeletonNoIndex},
                          {SkeletonIssue::InvalidSlotGroup, 1, 0},
                          {SkeletonIssue::InvalidSlot, 1, 0},
                          {SkeletonIssue::InvalidSlot, 1, 0},
                          {SkeletonIssue::InvalidRetargetSource, 0, SkeletonNoIndex},
                          {SkeletonIssue::NonFiniteRetargetPose, 0, 2},
                          {SkeletonIssue::DuplicateVirtualBone, 1, 7},
                          {SkeletonIssue::NonFiniteSocket, 0, SkeletonNoIndex},
                          {SkeletonIssue::NonFiniteSocket, 0, SkeletonNoIndex},
                          {SkeletonIssue::NonFiniteSocket, 1, SkeletonNoIndex}};
    const double nan = static_cast<double>(platform::BitCast<float>(0x7fc00000u));
    for (uint32 variant = 0; variant < sizeof(cases) / sizeof(cases[0]); ++variant) {
        const ObjectHandle copyHandle = MakeSkeleton(env, 0x200 + variant);
        Skeleton* copy = asset_test::Pointer<Skeleton>(*env.source, copyHandle);
        REQUIRE(copy != nullptr);
        REQUIRE(FillSkeleton(env.names, *copy));
        switch (variant) {
        case 0:
            copy->bones.bones[3].parentIndex = 7;
            break;
        case 1:
            (void)copy->retargetModes.TryResize(3);
            break;
        case 2:
            copy->retargetModes[2] =
                static_cast<BoneTranslationRetarget>(BoneTranslationRetargetCount);
            break;
        case 3:
            copy->virtualBones[0].name = Name{};
            break;
        case 4:
            copy->virtualBones[1].name = Intern(env.names, "chain"); // 접두사 없음
            break;
        case 5:
            copy->virtualBones[1].name = Intern(env.names, "VB "); // 접두사뿐
            break;
        case 6:
            copy->virtualBones[1].name = Intern(env.names, "VB SPINE_HAND_L"); // 앞 가상 본과 같음
            break;
        case 7:
            copy->virtualBones[1].sourceBone =
                copy->virtualBones[0].sourceBone; // 같은 (원본, 대상)
            copy->virtualBones[1].targetBone = copy->virtualBones[0].targetBone;
            break;
        case 8:
            copy->virtualBones[0].targetBone = Intern(env.names, "tail");
            break;
        case 9:
            copy->virtualBones[0].sourceBone = Intern(env.names, "vb chain"); // 뒤 가상 본
            break;
        case 10:
            copy->sockets[0].name = Name{};
            break;
        case 11:
            copy->sockets[1].name = Intern(env.names, "WEAPON");
            break;
        case 12:
            copy->sockets[1].boneName = Intern(env.names, "tail");
            break;
        case 13:
            copy->sockets[0].relativeLocation.y = nan;
            break;
        case 14:
            copy->sockets[0].relativeRotation.roll = nan;
            break;
        case 15:
            copy->sockets[1].relativeScale.x = nan;
            break;
        case 16:
            copy->curves[1].name = Name{};
            break;
        case 17:
            copy->curves[1].name = Intern(env.names, "blink");
            break;
        case 18:
            copy->curves[0].linkedBones[1] = Intern(env.names, "VB chain"); // 가상 본은 안 된다
            break;
        case 19:
            copy->slotGroups[0].name = Name{};
            break;
        case 20:
            copy->slotGroups[1].name = Intern(env.names, "defaultgroup");
            break;
        case 21:
            copy->slotGroups[1].slots[0] = Name{};
            break;
        case 22:
            copy->slotGroups[1].slots[0] =
                Intern(env.names, "UPPERBODY"); // 다른 묶음의 슬롯과 같음
            break;
        case 23:
            copy->retargetSources[0].name = Name{};
            break;
        case 24:
            copy->retargetSources[0].pose[2].translation.x = nan;
            break;
        case 25:
            copy->bones.bones[7].name = Intern(env.names, "VB CHAIN"); // 본 이름과 같은 가상 본
            break;
        case 26:
            copy->sockets[0].relativeRotation.pitch = nan;
            break;
        case 27:
            copy->sockets[0].relativeRotation.yaw = nan;
            break;
        default:
            copy->sockets[1].relativeLocation.z = nan;
            break;
        }

        SkeletonDiagnostic failure;
        auto valid = ValidateSkeleton(*copy, env.names, env.allocator, &failure);
        CHECK(!valid && valid.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::InvalidSkeletonData);
        CHECK(failure.issue == cases[variant].issue);
        CHECK(failure.index == cases[variant].index && failure.related == cases[variant].related);
    }

    // 리타깃 원본 이름 중복과 다른 pool의 가상 본 이름(원문을 읽을 수 없다 — pool 오류, 진단 코드
    // 없음).
    REQUIRE(skeleton->retargetSources.TryPushBack(NamedReferencePose(env.allocator)));
    skeleton->retargetSources[1].name = Intern(env.names, "tpose");
    SkeletonDiagnostic duplicate;
    CHECK(!ValidateSkeleton(*skeleton, env.names, env.allocator, &duplicate));
    CHECK(duplicate.issue == SkeletonIssue::InvalidRetargetSource && duplicate.index == 1 &&
          duplicate.related == 0);
    skeleton->retargetSources[1].name = Intern(env.names, "APose");
    CHECK(ValidateSkeleton(*skeleton, env.names, env.allocator).IsOk());
    NamePool other(env.allocator);
    REQUIRE(other.Initialize());
    skeleton->virtualBones[1].name = Intern(other, "VB foreign");
    SkeletonDiagnostic stale;
    auto foreign = ValidateSkeleton(*skeleton, env.names, env.allocator, &stale);
    CHECK(!foreign && stale.code == AssetDiagnosticCode::None && stale.error == foreign.Error());
    CHECK(stale.index == 1);
}

// 스켈레톤 검증·계층 검증의 할당 실패: n번째 할당을 실패시키면 그 오류(OutOfMemory)이고 진단 코드는
// None이다. 실패 없이 끝나는 시도가 나올 때까지 모든 지점을 지난다.
TEST(Skeleton_ValidateAllocationFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    Skeleton* skeleton = asset_test::Pointer<Skeleton>(*env.source, MakeSkeleton(env, 0x300));
    REQUIRE(skeleton != nullptr);
    REQUIRE(FillSkeleton(env.names, *skeleton));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        failing.FailOnAttempt(attempt);
        SkeletonDiagnostic failure;
        auto valid = ValidateSkeleton(*skeleton, env.names, failing, &failure);
        if (valid) {
            break;
        }

        CHECK(valid.Error() == ErrorCode::OutOfMemory && failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures >= 3);
}

// 저장 → 로드 → 재저장: 모든 필드를 채운 스켈레톤을 에디터 패키지로 쓰고 다른 Registry에 로드하면
// 필드가 같고(이름 토큰·표시·pose bit·soft 참조) 검증을 통과하며, 읽은 설정으로 다시 쓴 byte가
// 처음과 같다. 같은 스켈레톤을 두 번 쓴 byte도 같다(결정성).
TEST(Skeleton_PackageRoundTripResaveIdentical) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    ObjectRegistry& source = *env.source;
    auto package =
        NewPackage(source, source.OwnerAccess(), asset_test::FixedId(0x400), PackageKind::Editor);
    REQUIRE(package);
    const ObjectHandle handle =
        asset_test::Make<Skeleton>(source, SkeletonType(), package.Value().Handle(), "");
    Skeleton* skeleton = asset_test::Pointer<Skeleton>(source, handle);
    REQUIRE(skeleton != nullptr);
    REQUIRE(FillSkeleton(env.names, *skeleton));
    Array<uint8> first = asset_test::Write(env.allocator, source, package.Value().Handle(),
                                           asset_test::EditorOptions());
    REQUIRE(!first.IsEmpty());
    const Array<uint8> again = asset_test::Write(env.allocator, source, package.Value().Handle(),
                                                 asset_test::EditorOptions());
    CHECK(asset_test::SameBytes(first.View(), again.View()));

    Array<uint8> files[1] = {asset_test::Copy(env.allocator, first.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(
        asset_test::LoadAll(env, *env.target, files, PackageKind::Editor, nullptr, roots, handles));
    auto loadedHandle = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    REQUIRE(loadedHandle);
    Skeleton* loaded = asset_test::Pointer<Skeleton>(*env.target, loadedHandle.Value());
    REQUIRE(loaded != nullptr);
    CHECK(SameHierarchy(skeleton->bones, loaded->bones));
    REQUIRE(loaded->retargetModes.Size() == skeleton->retargetModes.Size());
    for (usize bone = 0; bone < loaded->retargetModes.Size(); ++bone) {
        CHECK(loaded->retargetModes[bone] == skeleton->retargetModes[bone]);
    }

    REQUIRE(loaded->virtualBones.Size() == 3 && loaded->sockets.Size() == 2);
    CHECK(loaded->virtualBones[1].sourceBone == skeleton->virtualBones[1].sourceBone);
    CHECK(loaded->sockets[0].relativeRotation.yaw == -20.0 &&
          !loaded->sockets[0].forceAlwaysAnimated);
    REQUIRE(loaded->curves.Size() == 2 && loaded->curves[0].linkedBones.Size() == 2);
    CHECK(loaded->curves[0].maxLod == 2 && loaded->curves[1].isMaterial);
    REQUIRE(loaded->slotGroups.Size() == 2 && loaded->slotGroups[0].slots.Size() == 2);
    REQUIRE(loaded->retargetSources.Size() == 1 && loaded->retargetSources[0].pose.Size() == 3);
    CHECK(SameTransform(loaded->retargetSources[0].pose[2], PoseFor(12)));
    REQUIRE(loaded->compatibleSkeletons.Size() == 1);
    CHECK(loaded->compatibleSkeletons[0].asset == asset_test::FixedId(0x5150));
    CHECK(loaded->useRetargetModesFromCompatibleSkeleton);
    CHECK(ValidateSkeleton(*loaded, env.names, env.allocator).IsOk());

    auto contents =
        ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(PackageKind::Editor).read);
    REQUIRE(contents);
    const Array<uint8> resaved =
        asset_test::ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
    CHECK(asset_test::SameBytes(first.View(), resaved.View()));
}

// 진단 이름표: 모든 SkeletonIssue가 고유한 이름을 갖고 열거 밖은 "Unknown"이다. 새 진단
// 코드는 4.6절 이름표에 있고, ToAssetDiagnostic은 위치를 detail로 옮긴다.
TEST(Skeleton_DiagnosticNames) {
    constexpr uint32 last = static_cast<uint32>(SkeletonIssue::MissingAncestor);
    for (uint32 index = 0; index <= last; ++index) {
        const StringView name(SkeletonIssueName(static_cast<SkeletonIssue>(index)));
        CHECK(name != StringView("Unknown"));
        for (uint32 other = 0; other < index; ++other) {
            CHECK(name != StringView(SkeletonIssueName(static_cast<SkeletonIssue>(other))));
        }
    }

    CHECK(StringView(SkeletonIssueName(static_cast<SkeletonIssue>(last + 1))) ==
          StringView("Unknown"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::SkeletonMismatch)) ==
          StringView("SkeletonMismatch"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::InvalidSkeletonData)) ==
          StringView("InvalidSkeletonData"));
    SkeletonDiagnostic diagnostic;
    diagnostic.code = AssetDiagnosticCode::SkeletonMismatch;
    diagnostic.error = ErrorCode::InvalidArgument;
    CHECK(ToAssetDiagnostic(diagnostic, AssetId{}).detail == 0);
    diagnostic.index = 9;
    const AssetDiagnostic converted = ToAssetDiagnostic(diagnostic, asset_test::FixedId(3));
    CHECK(converted.detail == 9 && converted.code == AssetDiagnosticCode::SkeletonMismatch &&
          converted.asset == asset_test::FixedId(3) &&
          converted.error == ErrorCode::InvalidArgument);
}

} // namespace skeleton_tests_file
