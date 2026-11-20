#include "AnimationTestSupport.h"

#include <TestSupport/NFailAllocator.h>

// 본 계층 병합 시험이다(Docs/Design/MeshAnimationAssets.ko.md 6.2·6.4절, 12절 "병합 성공·부모 사슬
// 실패"): 빈 스켈레톤, 없는 본 더하기와 대응표, 대소문자만 다른 이름, 부모 사슬 실패·조상 없음,
// 입력 검증과 결과 본 수 상한, 결정성, 할당 실패 주입, 고정 seed 퍼징.
using namespace rocks;
namespace skeleton_merge_tests_file {

using namespace rocks::animation_test;

constexpr uint64 MergeFuzzSeed = 0x4d4134614d455247ull;
constexpr uint32 MergeFuzzCount = 500;
constexpr uint32 MergeFuzzMaxBones = 24;

// 큰 계층 시험이 쓰는 pool entry 상한이다(기본 65536보다 많은 이름을 만든다).
constexpr uint64 LargePoolEntries = 1u << 18;

struct MergeFixture {
    MergeFixture() noexcept : names(allocator) {
        NamePoolConfig config;
        config.maxEntries = LargePoolEntries;
        ready = names.Initialize(config).IsOk();
    }

    SystemAllocator allocator;
    NamePool names;
    bool ready = false;
};

bool SameMerge(const BoneMergeResult& a, const BoneMergeResult& b) noexcept {
    if (!SameHierarchy(a.bones, b.bones) || a.meshToMerged.Size() != b.meshToMerged.Size() ||
        a.addedBones != b.addedBones || a.renamedBones != b.renamedBones) {
        return false;
    }

    for (usize at = 0; at < a.meshToMerged.Size(); ++at) {
        if (a.meshToMerged[at] != b.meshToMerged[at]) {
            return false;
        }
    }

    return true;
}

// 병합 결과 불변식: 결과 계층이 검증을 통과하고, 메시 본 m의 결과 본은 같은 이름이며 그 부모는 메시
// 부모의 결과 본이다. 스켈레톤 본은 같은 번호·부모를 유지한다.
bool MergeConsistent(const BoneHierarchy& skeleton, const BoneHierarchy& mesh,
                     const BoneMergeResult& merged, Allocator& allocator) noexcept {
    if (!ValidateBoneHierarchy(merged.bones, allocator).IsOk() ||
        merged.meshToMerged.Size() != mesh.bones.Size() ||
        merged.bones.bones.Size() != skeleton.bones.Size() + merged.addedBones) {
        return false;
    }

    for (usize bone = 0; bone < skeleton.bones.Size(); ++bone) {
        if (!(merged.bones.bones[bone].name == skeleton.bones[bone].name) ||
            merged.bones.bones[bone].parentIndex != skeleton.bones[bone].parentIndex) {
            return false;
        }
    }

    for (usize bone = 0; bone < mesh.bones.Size(); ++bone) {
        const int32 target = merged.meshToMerged[bone];
        if (target < 0 || static_cast<usize>(target) >= merged.bones.bones.Size() ||
            !(merged.bones.bones[static_cast<usize>(target)].name == mesh.bones[bone].name)) {
            return false;
        }

        const int32 parent = mesh.bones[bone].parentIndex;
        const int32 expected =
            parent == NoParentBone ? NoParentBone : merged.meshToMerged[static_cast<usize>(parent)];
        if (merged.bones.bones[static_cast<usize>(target)].parentIndex != expected) {
            return false;
        }
    }

    return true;
}

// 빈 스켈레톤에 합치면 결과는 메시 계층 그대로이고 대응은 항등이다.
TEST(Skeleton_MergeIntoEmptySkeleton) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy empty(fixture.allocator);
    BoneHierarchy mesh(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, mesh));
    BoneMergeResult merged(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(empty, mesh, merged));
    CHECK(SameHierarchy(merged.bones, mesh));
    CHECK(merged.addedBones == 8 && merged.renamedBones == 0);
    for (int32 bone = 0; bone < 8; ++bone) {
        CHECK(merged.meshToMerged[static_cast<usize>(bone)] == bone);
    }

    // 빈 메시는 스켈레톤 그대로다.
    BoneMergeResult same(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(mesh, empty, same));
    CHECK(SameHierarchy(same.bones, mesh) && same.addedBones == 0 && same.meshToMerged.IsEmpty());
}

// 없는 본 더하기: 메시가 순서가 다르고 스켈레톤 일부(arm_r 쪽 없음)와 새 본(finger_l, jaw)을 가지면
// 새 본만 메시 순서로 뒤에 붙고 부모는 결과 번호, pose는 메시 것이다. 있는 본은 스켈레톤 pose를
// 유지한다.
TEST(Skeleton_MergeAddsMissingBones) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy skeleton(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, skeleton));
    const BoneSpec meshBones[] = {
        {"root", NoParentBone}, {"pelvis", 0},   {"spine", 1}, {"arm_l", 2},
        {"hand_l", 3},          {"finger_l", 4}, {"head", 2},  {"jaw", 6}};
    BoneHierarchy mesh(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, meshBones, mesh, 100.0));
    BoneMergeResult merged(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(skeleton, mesh, merged));
    CHECK(merged.addedBones == 2 && merged.renamedBones == 0);
    REQUIRE(merged.bones.bones.Size() == 10);
    const int32 expected[] = {0, 1, 2, 4, 5, 8, 3, 9};
    for (usize bone = 0; bone < 8; ++bone) {
        CHECK(merged.meshToMerged[bone] == expected[bone]);
    }

    CHECK(merged.bones.bones[8].parentIndex == 5 && merged.bones.bones[9].parentIndex == 3);
    CHECK(SameTransform(merged.bones.referencePose[8], mesh.referencePose[5]));
    CHECK(SameTransform(merged.bones.referencePose[2], skeleton.referencePose[2]));
    CHECK(merged.bones.bones[9].exportName.View() == StringView("jaw"));
    CHECK(MergeConsistent(skeleton, mesh, merged, fixture.allocator));

    // 같은 입력이면 같은 결과다(결정성).
    BoneMergeResult again(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(skeleton, mesh, again));
    CHECK(SameMerge(merged, again));
}

// 대소문자만 다른 이름(같은 토큰, 다른 표시)은 같은 본이고 결과는 메시 표시·exportName으로
// 바뀐다(UE와 같다).
TEST(Skeleton_MergeCaseOnlyRename) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy skeleton(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, skeleton));
    const BoneSpec meshBones[] = {{"root", NoParentBone}, {"pelvis", 0}, {"Spine", 1}, {"HEAD", 2}};
    BoneHierarchy mesh(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, meshBones, mesh));
    BoneMergeResult merged(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(skeleton, mesh, merged));
    CHECK(merged.renamedBones == 2 && merged.addedBones == 0);
    CHECK(merged.bones.bones[2].name.displayId == mesh.bones[2].name.displayId);
    CHECK(merged.bones.bones[3].exportName.View() == StringView("HEAD"));
    auto lease = fixture.names.Acquire(merged.bones.bones[2].name);
    REQUIRE(lease);
    CHECK(lease.Value().View() == StringView("Spine"));
    CHECK(MergeConsistent(skeleton, mesh, merged, fixture.allocator));
}

// 부모 사슬 실패(SkeletonMismatch): 중간 본이 빠짐(arm_l이 spine 대신 pelvis 아래), 새 본의 조상
// 사슬이 다름, 메시 루트가 스켈레톤에 없음(MissingAncestor), 스켈레톤 루트와 이름이 같은 메시 본이
// 메시 루트가 아님. 실패하면 결과는 그대로다.
TEST(Skeleton_MergeRejectsParentChain) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy skeleton(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, skeleton));
    struct Case {
        BoneSpec bones[4];
        uint32 count;
        SkeletonIssue issue;
        uint32 index;
        uint32 related;
    };
    const Case cases[] = {{{{"root", NoParentBone}, {"pelvis", 0}, {"arm_l", 1}, {}},
                           3,
                           SkeletonIssue::ParentChainMismatch,
                           2,
                           4},
                          {{{"root", NoParentBone}, {"spine", 0}, {"finger", 1}, {}},
                           3,
                           SkeletonIssue::ParentChainMismatch,
                           1,
                           2},
                          {{{"origin", NoParentBone}, {"pelvis", 0}, {}, {}},
                           2,
                           SkeletonIssue::MissingAncestor,
                           0,
                           SkeletonNoIndex},
                          {{{"world", NoParentBone}, {"root", 0}, {}, {}},
                           2,
                           SkeletonIssue::MissingAncestor,
                           0,
                           SkeletonNoIndex},
                          {{{"root", NoParentBone}, {"pelvis", 0}, {"extra", 1}, {"root2", 2}},
                           4,
                           SkeletonIssue::None,
                           0,
                           0}};
    BoneMergeResult previous(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(skeleton, skeleton, previous));
    for (const Case& item : cases) {
        BoneHierarchy mesh(fixture.allocator);
        REQUIRE(BuildHierarchy(fixture.names, Span<const BoneSpec>(item.bones, item.count), mesh));
        BoneMergeResult merged(fixture.allocator);
        REQUIRE(MergeBoneHierarchy(skeleton, skeleton, merged));
        SkeletonDiagnostic failure;
        auto result = MergeBoneHierarchy(skeleton, mesh, merged, &failure);
        if (item.issue == SkeletonIssue::None) {
            CHECK(result.IsOk() && merged.addedBones == 2);
            continue;
        }

        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::SkeletonMismatch);
        CHECK(failure.issue == item.issue && failure.role == BoneHierarchyRole::Mesh);
        CHECK(failure.index == item.index && failure.related == item.related);
        CHECK(SameMerge(merged, previous));
    }

    // 스켈레톤 루트에서 대조를 시작하면 두 루트 이름만 비교한다: 메시 루트가 다르고 스켈레톤 루트
    // 이름의 본이 메시 안쪽에 있으면 실패한다.
    const BoneSpec rerooted[] = {{"origin", NoParentBone}, {"root", 0}, {"pelvis", 1}};
    BoneHierarchy mesh(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, rerooted, mesh));
    BoneMergeResult merged(fixture.allocator);
    SkeletonDiagnostic failure;
    CHECK(!MergeBoneHierarchy(skeleton, mesh, merged, &failure));
    CHECK(failure.issue == SkeletonIssue::MissingAncestor && failure.index == 0);
}

// 입력 검증과 상한: 잘못된 스켈레톤·메시 계층은 그 검증 진단(role로 어느 쪽인지)이고, 결과 본 수가
// MeshMaxBones를 넘으면 TooManyBones(CapacityExceeded)다.
TEST(Skeleton_MergeRejectsInvalidInputs) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy good(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, good));
    BoneHierarchy bad(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, bad));
    bad.bones[3].parentIndex = 5;
    BoneMergeResult merged(fixture.allocator);
    SkeletonDiagnostic failure;
    CHECK(!MergeBoneHierarchy(bad, good, merged, &failure));
    CHECK(failure.issue == SkeletonIssue::ParentOrder &&
          failure.role == BoneHierarchyRole::Skeleton);
    CHECK(!MergeBoneHierarchy(good, bad, merged, &failure));
    CHECK(failure.issue == SkeletonIssue::ParentOrder && failure.role == BoneHierarchyRole::Mesh);
    CHECK(merged.bones.bones.IsEmpty());

    // 스켈레톤 40000 + 새 메시 본 30000 > MeshMaxBones. 메시 본은 모두 같은 루트의
    // 자식이다.
    BoneHierarchy big(fixture.allocator);
    BoneHierarchy wide(fixture.allocator);
    char text[16] = {};
    auto add = [&](BoneHierarchy& to, char prefix, uint32 index, int32 parent) {
        BoneInfo info(fixture.allocator);
        info.name = Intern(fixture.names, LetterName(prefix, index, text));
        info.parentIndex = parent;
        return to.bones.TryPushBack(Move(info)).IsOk() &&
               to.referencePose.TryPushBack(Transform()).IsOk();
    };
    REQUIRE(add(big, 'r', 0, NoParentBone));
    REQUIRE(add(wide, 'r', 0, NoParentBone));
    for (uint32 bone = 1; bone < 40000; ++bone) {
        REQUIRE(add(big, 's', bone, 0));
    }

    for (uint32 bone = 1; bone <= 30000; ++bone) {
        REQUIRE(add(wide, 'm', bone, 0));
    }

    auto tooMany = MergeBoneHierarchy(big, wide, merged, &failure);
    CHECK(!tooMany && tooMany.Error() == ErrorCode::CapacityExceeded);
    CHECK(failure.code == AssetDiagnosticCode::InvalidSkeletonData &&
          failure.issue == SkeletonIssue::TooManyBones && failure.index == MeshMaxBones);
    // 25535개면 정확히 상한이라 받는다.
    (void)wide.bones.TryRemoveAt(25536, wide.bones.Size() - 25536);
    (void)wide.referencePose.TryResize(25536);
    REQUIRE(MergeBoneHierarchy(big, wide, merged));
    CHECK(merged.bones.bones.Size() == MeshMaxBones);
}

// 할당 실패 주입: 결과 할당기의 n번째 할당을 실패시키면 OutOfMemory이고 결과는 그대로이며, 실패
// 없이 끝난 결과는 실패 없는 병합과 같다.
TEST(Skeleton_MergeAllocationFailures) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy skeleton(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, HumanBones, skeleton));
    const BoneSpec meshBones[] = {
        {"root", NoParentBone}, {"pelvis", 0}, {"spine", 1}, {"Head", 2}, {"jaw", 3},
        {"tongue", 4}};
    BoneHierarchy mesh(fixture.allocator);
    REQUIRE(BuildHierarchy(fixture.names, meshBones, mesh));
    // 이름 바꾸기가 긴 exportName(작은 문자열 최적화 밖)을 복사하게 해 그 할당도 실패시킨다.
    REQUIRE(mesh.bones[3].exportName.TryAssign("Head_exported_with_a_long_original_name"));
    BoneMergeResult expected(fixture.allocator);
    REQUIRE(MergeBoneHierarchy(skeleton, mesh, expected));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(fixture.allocator);
        BoneMergeResult merged(failing);
        failing.FailOnAttempt(attempt);
        SkeletonDiagnostic failure;
        auto result = MergeBoneHierarchy(skeleton, mesh, merged, &failure);
        if (result) {
            CHECK(SameMerge(expected, merged));
            break;
        }

        CHECK(result.Error() == ErrorCode::OutOfMemory &&
              failure.code == AssetDiagnosticCode::None);
        CHECK(merged.bones.bones.IsEmpty() && merged.meshToMerged.IsEmpty());
        ++failures;
    }

    CHECK(failures >= 10);
}

// 메시를 스켈레톤에서 만든다: 부모가 이미 고른 본인 스켈레톤 본을 무작위로 고르고(루트는 늘), 새
// 본을 더하고, 확률로 부모 하나를 바꾼다(사슬 불일치가 나올 수 있다).
bool DeriveMesh(RandomStream& random, NamePool& names, const BoneHierarchy& skeleton,
                BoneHierarchy& mesh) noexcept {
    Array<int32> chosen(mesh.bones.GetAllocator());
    for (usize bone = 0; bone < skeleton.bones.Size(); ++bone) {
        const int32 parent = skeleton.bones[bone].parentIndex;
        const bool parentChosen = parent == NoParentBone || chosen[static_cast<usize>(parent)] >= 0;
        const bool keep = bone == 0 || (parentChosen && random.NextUInt32Below(100) < 70);
        int32 index = -1;
        if (keep) {
            BoneInfo info(mesh.bones.GetAllocator());
            info.name = skeleton.bones[bone].name;
            info.parentIndex =
                parent == NoParentBone ? NoParentBone : chosen[static_cast<usize>(parent)];
            if (random.NextUInt32Below(100) < 3 && info.parentIndex > 0) {
                info.parentIndex = static_cast<int32>(
                    random.NextUInt32Below(static_cast<uint32>(info.parentIndex)));
            }

            index = static_cast<int32>(mesh.bones.Size());
            if (!mesh.bones.TryPushBack(Move(info)) ||
                !mesh.referencePose.TryPushBack(skeleton.referencePose[bone])) {
                return false;
            }
        }

        if (!chosen.TryPushBack(index)) {
            return false;
        }
    }

    const uint32 extra = random.NextUInt32Below(5);
    char text[8] = {'x', 0, 0, 0};
    for (uint32 at = 0; at < extra && !mesh.bones.IsEmpty(); ++at) {
        text[1] = static_cast<char>('a' + random.NextUInt32Below(26));
        text[2] = static_cast<char>('a' + random.NextUInt32Below(26));
        BoneInfo info(mesh.bones.GetAllocator());
        info.name = Intern(names, StringView(text, 3));
        info.parentIndex =
            static_cast<int32>(random.NextUInt32Below(static_cast<uint32>(mesh.bones.Size())));
        if (FindBoneIndex(mesh, info.name) != NoParentBone) {
            continue;
        }

        if (!mesh.bones.TryPushBack(Move(info)) || !mesh.referencePose.TryPushBack(Transform())) {
            return false;
        }
    }

    return true;
}

// 고정 seed 퍼징: 무작위 계층의 검증은 정해진 진단이거나 통과이고, 통과한 스켈레톤에서 만든
// 메시와의 병합은 성공(결과 불변식 성립, 두 번 같은 결과)이거나 SkeletonMismatch·검증 진단이다.
// 아무 계층 둘의 병합도 단언 없이 끝난다. 성공·실패가 모두 충분히 나와야 한다.
TEST(Skeleton_FuzzHierarchiesAndMerges) {
    MergeFixture fixture;
    REQUIRE(fixture.ready);
    RandomStream random(MergeFuzzSeed);
    uint32 valid = 0;
    uint32 invalid = 0;
    uint32 merged = 0;
    uint32 mismatched = 0;
    for (uint32 iteration = 0; iteration < MergeFuzzCount; ++iteration) {
        BoneHierarchy skeleton(fixture.allocator);
        REQUIRE(RandomHierarchy(random, fixture.names, skeleton, MergeFuzzMaxBones));
        SkeletonDiagnostic failure;
        if (!ValidateBoneHierarchy(skeleton, fixture.allocator, &failure)) {
            ++invalid;
            CHECK(failure.code == AssetDiagnosticCode::InvalidSkeletonData);
            CHECK(failure.issue != SkeletonIssue::None);
            continue;
        }

        ++valid;
        BoneHierarchy mesh(fixture.allocator);
        REQUIRE(DeriveMesh(random, fixture.names, skeleton, mesh));
        BoneMergeResult result(fixture.allocator);
        SkeletonDiagnostic mergeFailure;
        if (MergeBoneHierarchy(skeleton, mesh, result, &mergeFailure)) {
            ++merged;
            CHECK(MergeConsistent(skeleton, mesh, result, fixture.allocator));
            BoneMergeResult again(fixture.allocator);
            REQUIRE(MergeBoneHierarchy(skeleton, mesh, again));
            CHECK(SameMerge(result, again));
        } else {
            ++mismatched;
            CHECK(mergeFailure.code == AssetDiagnosticCode::SkeletonMismatch ||
                  mergeFailure.code == AssetDiagnosticCode::InvalidSkeletonData);
        }

        // 서로 무관한 무작위 계층과의 병합도 단언 없이 끝난다.
        BoneHierarchy other(fixture.allocator);
        REQUIRE(RandomHierarchy(random, fixture.names, other, MergeFuzzMaxBones));
        BoneMergeResult unrelated(fixture.allocator);
        if (MergeBoneHierarchy(skeleton, other, unrelated)) {
            CHECK(MergeConsistent(skeleton, other, unrelated, fixture.allocator));
        }
    }

    CHECK(valid >= MergeFuzzCount / 4 && invalid >= MergeFuzzCount / 20);
    CHECK(merged >= MergeFuzzCount / 8 && mismatched >= MergeFuzzCount / 50);
}

} // namespace skeleton_merge_tests_file
