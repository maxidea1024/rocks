#include <AssetNugget/NuggetScc.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// SCC Record 값 번호·meta 시험이다(Docs/Design/AssetSystem 9.1절 결정 15, "A3c1 구현 세부"):
// 값 번호 경계, meta golden(high → low big-endian), 순번 찾기, 거부(빈 목록·null·중복·상한
// 초과·잘못된 크기·상한을 넘는 meta), 할당 실패.
using namespace rocks;

namespace nugget_scc_tests_file {

namespace {

AssetId Member(uint64 seed) noexcept {
    return AssetId{Guid{0x1000000000000000ull + seed, 0x2000000000000000ull + seed * 3}};
}

} // namespace

// 값 번호: (순번 << 16) | 부분, 경계(순번 4095·부분 65535), 순번 되읽기.
TEST(NuggetScc_ValueIds) {
    CHECK(NuggetSccValueId(0, 0) == 0 && NuggetSccValueId(1, 0) == 0x10000 &&
          NuggetSccValueId(1, 2) == 0x10002 &&
          NuggetSccValueId(NuggetSccMaxMembers - 1, NuggetSccMaxParts - 1) == 0x0fffffffu);
    CHECK(NuggetSccMemberOf(0x10002) == 1 && NuggetSccMemberOf(0x0fffffffu) == 4095 &&
          NuggetSccMemberOf(7) == 0);
}

// meta: golden byte, 순번 찾기(처음·가운데·끝), 없음 NotFound, 다시 쓰면 비우고 채운다, 4,096
// 구성원 경계.
TEST(NuggetScc_Meta) {
    SystemAllocator allocator;
    const AssetId members[] = {Member(1), Member(2), Member(3)};
    Array<uint8> meta(allocator);
    REQUIRE(meta.TryPushBack(0xee));
    REQUIRE(WriteNuggetSccMeta(members, meta));
    REQUIRE(meta.Size() == 3 * NuggetSccMemberBytes);
    const uint8 first[NuggetSccMemberBytes] = {0x10, 0, 0, 0, 0, 0, 0, 1,
                                               0x20, 0, 0, 0, 0, 0, 0, 3};
    for (usize index = 0; index < NuggetSccMemberBytes; ++index) {
        CHECK(meta[index] == first[index]);
    }

    for (uint32 index = 0; index < 3; ++index) {
        auto found = FindNuggetSccMember(meta.View(), members[index]);
        CHECK(found && found.Value() == index);
    }

    CHECK(FindNuggetSccMember(meta.View(), Member(9)).Error() == ErrorCode::NotFound);
    // high만 같은 AssetId는 다른 구성원이다.
    CHECK(FindNuggetSccMember(meta.View(), AssetId{Guid{members[1].value.high, 0}}).Error() ==
          ErrorCode::NotFound);
    CHECK(FindNuggetSccMember(Span<const uint8>(), Member(1)).Error() == ErrorCode::NotFound);

    Array<AssetId> many(allocator);
    REQUIRE(many.TryReserve(NuggetSccMaxMembers + 1));
    for (usize index = 0; index < NuggetSccMaxMembers; ++index) {
        REQUIRE(many.TryPushBack(Member(index + 10)));
    }

    REQUIRE(WriteNuggetSccMeta(many.View(), meta));
    auto last = FindNuggetSccMember(meta.View(), many[NuggetSccMaxMembers - 1]);
    CHECK(last && last.Value() == NuggetSccMaxMembers - 1);
    REQUIRE(many.TryPushBack(Member(1)));
    CHECK(WriteNuggetSccMeta(many.View(), meta).Error() == ErrorCode::CapacityExceeded &&
          meta.IsEmpty());
}

// 거부: 빈 목록·null·중복(붙음·떨어짐) InvalidArgument(meta는 빈다), meta 크기가 16의 배수 아님·
// 상한을 넘는 구성원 InvalidArgument, 할당 실패 OutOfMemory.
TEST(NuggetScc_Rejects) {
    SystemAllocator allocator;
    Array<uint8> meta(allocator);
    REQUIRE(meta.TryPushBack(1));
    CHECK(WriteNuggetSccMeta(Span<const AssetId>(), meta).Error() == ErrorCode::InvalidArgument &&
          meta.IsEmpty());
    const AssetId withNull[] = {Member(1), AssetId{}};
    const AssetId adjacent[] = {Member(1), Member(1)};
    const AssetId apart[] = {Member(1), Member(2), Member(1)};
    const Span<const AssetId> rejects[] = {withNull, adjacent, apart};
    for (Span<const AssetId> members : rejects) {
        CHECK(WriteNuggetSccMeta(members, meta).Error() == ErrorCode::InvalidArgument &&
              meta.IsEmpty());
    }

    const uint8 odd[NuggetSccMemberBytes + 1] = {};
    CHECK(FindNuggetSccMember(odd, Member(1)).Error() == ErrorCode::InvalidArgument);
    Array<uint8> huge(allocator);
    REQUIRE(huge.TryResize((NuggetSccMaxMembers + 1) * NuggetSccMemberBytes));
    CHECK(FindNuggetSccMember(huge.View(), Member(1)).Error() == ErrorCode::InvalidArgument);

    test::NFailAllocator failing(allocator);
    Array<uint8> failingMeta(failing);
    const AssetId members[] = {Member(1)};
    failing.FailOnAttempt(1);
    CHECK(WriteNuggetSccMeta(members, failingMeta).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(WriteNuggetSccMeta(members, failingMeta));
}

} // namespace nugget_scc_tests_file
