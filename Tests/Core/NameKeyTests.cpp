#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

NameKey KeyOf(StringView text) noexcept {
    auto key = TryMakeNameKey(text);
    return key ? key.Value() : NameKey{0xdeadull, 0xdeadu};
}

} // namespace

// 영속 알고리즘 v1의 값은 바뀌면 안 된다. 기대값은 같은 규칙의 독립 구현(Python)으로 계산했다.
TEST(NameKey_GoldenValuesAreStable) {
    CHECK(KeyOf("Player") == (NameKey{0x5d971a6972fa29e7ull, 0}));
    CHECK(KeyOf("Player_7") == (NameKey{0x5d971a6972fa29e7ull, 8}));
    CHECK(KeyOf("Player_07") == (NameKey{0x822a101970d4f733ull, 0})); // 앞자리 0은 접미사가 아니다
    CHECK(KeyOf("a") == (NameKey{0x02c0bdbf481420f8ull, 0}));
    CHECK(KeyOf("Engine/Core") == (NameKey{0x759d76ab3d44cd7dull, 0}));
    CHECK(KeyOf("\xc3\x9c"
                "ber") == (NameKey{0x5a7b52e2a597e590ull, 0}));
    CHECK(KeyOf("\xc3\xbc"
                "ber") == (NameKey{0x3cff78364355f1a2ull, 0}));
    CHECK(KeyOf("Player_2147483646") == (NameKey{0x5d971a6972fa29e7ull, 2147483647u}));
    CHECK(KeyOf("Player_2147483647") == (NameKey{0x05885497e23c2255ull, 0})); // 번호 범위 밖
    CHECK(KeyOf(StringView("a\0b", 3)) == (NameKey{0xeb38c838c62d597eull, 0}));
}

TEST(NameKey_FixedByteEncoding) {
    CHECK(NameKey::AlgorithmVersion == 1 && NameKey::EncodedBytes == 12);
    uint8 bytes[NameKey::EncodedBytes];
    KeyOf("Player_7").Encode(bytes);
    // hash 0x5d971a6972fa29e7과 number 8의 little-endian이다.
    const uint8 expected[NameKey::EncodedBytes] = {0xe7, 0x29, 0xfa, 0x72, 0x69, 0x1a,
                                                   0x97, 0x5d, 0x08, 0x00, 0x00, 0x00};
    bool same = true;
    for (usize index = 0; index < NameKey::EncodedBytes; ++index) {
        same = same && bytes[index] == expected[index];
    }

    CHECK(same);
    CHECK(NameKey::Decode(bytes) == KeyOf("Player_7"));
    const NameKey extreme{~0ull, 0xffffffffu};
    extreme.Encode(bytes);
    CHECK(NameKey::Decode(bytes) == extreme);
}

TEST(NameKey_FollowsPoolComparisonAndNoneRules) {
    // ASCII 대소문자만 무시한다. 비ASCII는 byte 비교다.
    CHECK(KeyOf("player") == KeyOf("PLAYER") && KeyOf("Player_3") == KeyOf("pLAYER_3"));
    CHECK(!(KeyOf("Player_3") == KeyOf("Player_4")) && !(KeyOf("Player") == KeyOf("Player_0")));
    CHECK(!(KeyOf("\xc3\x9c"
                  "ber") == KeyOf("\xc3\xbc"
                                  "ber")));
    // None 규칙.
    CHECK(KeyOf("").IsNone() && KeyOf("None").IsNone() && KeyOf("nOnE").IsNone());
    CHECK(KeyOf("_5").IsNone()); // 빈 base의 번호는 None이다
    CHECK(KeyOf("None_3") == (NameKey{0, 4}) && !KeyOf("None_3").IsNone());
    CHECK(KeyOf("none_3") == KeyOf("None_3"));
    CHECK(!KeyOf("Nonex").IsNone() && KeyOf("Nonex").hash != 0);
    auto invalid = TryMakeNameKey(StringView("\xff", 1));
    CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
}

TEST(NameKey_MatchesNameEqualityAcrossPools) {
    // 같은 이름이라도 pool(domain)마다 runtime token은 다르지만 key는 같다. 이름 쌍의 동등성과
    // key의 동등성이 일치해야 한다.
    SystemAllocator allocator;
    NamePool first(allocator);
    NamePool second(allocator);
    REQUIRE(first.Initialize());
    REQUIRE(second.Initialize());
    const char* texts[] = {"Actor",
                           "actor",
                           "ACTOR_1",
                           "Actor_1",
                           "Actor_01",
                           "None",
                           "none",
                           "None_0",
                           "NONE_0",
                           "",
                           "_9",
                           "Mesh",
                           "Mesh_10",
                           "mesh_10",
                           "Mesh_2",
                           "Ab",
                           "aB",
                           "a_b",
                           "a_B_1",
                           "A_b_1",
                           "x_",
                           "X_",
                           "Player",
                           "PLAYER_2147483646",
                           "None_2147483646",
                           "_2147483647",
                           "Ã¼",
                           "Ã",
                           "a_9999999999"};
    constexpr usize Count = sizeof(texts) / sizeof(texts[0]);
    Name inFirst[Count];
    Name inSecond[Count];
    NameKey keys[Count];
    for (usize index = 0; index < Count; ++index) {
        auto a = first.TryIntern(texts[index]);
        auto b = second.TryIntern(texts[index]);
        REQUIRE(a && b);
        inFirst[index] = a.Value();
        inSecond[index] = b.Value();
        keys[index] = KeyOf(texts[index]);
        // lease에서 얻은 key도 같다.
        auto lease = first.Acquire(a.Value());
        REQUIRE(lease);
        CHECK(lease.Value().Key() == keys[index]);
        // 일반 이름은 pool(domain)마다 token이 다르다. None 계열은 설계상 domain을 무시한다.
        if (a.Value().kind == NameKind::Normal) {
            CHECK(!(a.Value() == b.Value()));
        }
    }
    bool consistent = true;
    for (usize left = 0; left < Count; ++left) {
        for (usize right = 0; right < Count; ++right) {
            const bool sameName = inFirst[left] == inFirst[right];
            const bool sameKey = keys[left] == keys[right];
            consistent = consistent && sameName == sameKey &&
                         (inSecond[left] == inSecond[right]) == sameName;
        }
    }
    CHECK(consistent);
    NameLease empty;
    CHECK(empty.Key().IsNone());
    REQUIRE(first.Shutdown());
    REQUIRE(second.Shutdown());
}

TEST(NameKey_RandomizedCaseVariantsShareKey) {
    RandomStream random(test::CurrentSeed() ^ 0x4e4bull);
    char text[24];
    char variant[24];
    usize collisions = 0;
    NameKey previous{};
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const usize length = 1 + random.NextUInt32Below(16);
        for (usize index = 0; index < length; ++index) {
            const char letter = static_cast<char>('a' + random.NextUInt32Below(26));
            text[index] = letter;
            const bool upper = random.NextUInt32Below(2) == 0;
            variant[index] = upper ? static_cast<char>(letter - 'a' + 'A') : letter;
        }
        const NameKey key = KeyOf(StringView(text, length));
        CHECK(key == KeyOf(StringView(variant, length)) && key.number == 0 && key.hash != 0);
        collisions += iteration != 0 && key.hash == previous.hash ? 1 : 0;
        previous = key;
    }
    // 연속한 무작위 이름이 같은 hash가 되는 일은 사실상 없어야 한다(같은 이름이 다시 나온 경우 제외
    // 허용).
    CHECK(collisions <= 2);
}
