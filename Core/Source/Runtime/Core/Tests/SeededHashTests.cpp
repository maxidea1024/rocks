#include <Base/Types.h>
#include <Core/Hash.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

// SipHash 참조 구현(vectors.h)의 SipHash-2-4 64비트 출력 표다: key 00 01 .. 0f, 메시지 00 01 ..
// (n-1), n = 0..63. 출력 8 byte를 little-endian uint64로 적었다.
constexpr uint64 SipHash24ReferenceVectors[64] = {
    0x726fdb47dd0e0e31ull, 0x74f839c593dc67fdull, 0x0d6c8009d9a94f5aull, 0x85676696d7fb7e2dull,
    0xcf2794e0277187b7ull, 0x18765564cd99a68dull, 0xcbc9466e58fee3ceull, 0xab0200f58b01d137ull,
    0x93f5f5799a932462ull, 0x9e0082df0ba9e4b0ull, 0x7a5dbbc594ddb9f3ull, 0xf4b32f46226bada7ull,
    0x751e8fbc860ee5fbull, 0x14ea5627c0843d90ull, 0xf723ca908e7af2eeull, 0xa129ca6149be45e5ull,
    0x3f2acc7f57c29bdbull, 0x699ae9f52cbe4794ull, 0x4bc1b3f0968dd39cull, 0xbb6dc91da77961bdull,
    0xbed65cf21aa2ee98ull, 0xd0f2cbb02e3b67c7ull, 0x93536795e3a33e88ull, 0xa80c038ccd5ccec8ull,
    0xb8ad50c6f649af94ull, 0xbce192de8a85b8eaull, 0x17d835b85bbb15f3ull, 0x2f2e6163076bcfadull,
    0xde4daaaca71dc9a5ull, 0xa6a2506687956571ull, 0xad87a3535c49ef28ull, 0x32d892fad841c342ull,
    0x7127512f72f27cceull, 0xa7f32346f95978e3ull, 0x12e0b01abb051238ull, 0x15e034d40fa197aeull,
    0x314dffbe0815a3b4ull, 0x027990f029623981ull, 0xcadcd4e59ef40c4dull, 0x9abfd8766a33735cull,
    0x0e3ea96b5304a7d0ull, 0xad0c42d6fc585992ull, 0x187306c89bc215a9ull, 0xd4a60abcf3792b95ull,
    0xf935451de4f21df2ull, 0xa9538f0419755787ull, 0xdb9acddff56ca510ull, 0xd06c98cd5c0975ebull,
    0xe612a3cb9ecba951ull, 0xc766e62cfcadaf96ull, 0xee64435a9752fe72ull, 0xa192d576b245165aull,
    0x0a8787bf8ecb74b2ull, 0x81b3e73d20b49b6full, 0x7fa8220ba3b2eceaull, 0x245731c13ca42499ull,
    0xb78dbfaf3a8d83bdull, 0xea1ad565322a1a0bull, 0x60e61c23a3795013ull, 0x6606d7e446282b93ull,
    0x6ca4ecb15c5f91e1ull, 0x9f626da15c9625f3ull, 0xe51b38608ef25f57ull, 0x958a324ceb064572ull,
};

enum class SeedTestKind : uint16 {
    First = 3,
    Second = 900
};

} // namespace

// 참조 표 64개 전부(꼬리 길이 0–7, word 수 0–7)를 대조한다. 시작 주소를 1 byte 어긋나게 둔 복사본도
// 같은 값이어야 한다(정렬되지 않은 주소).
TEST(SeededHash_SipHashMatchesReferenceVectors) {
    const HashSeed key{0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull};
    uint8 message[64] = {};
    uint8 shifted[65] = {};
    for (usize index = 0; index != 64; ++index) {
        message[index] = static_cast<uint8>(index);
        shifted[index + 1] = static_cast<uint8>(index);
    }

    CHECK(SipHash24(key, nullptr, 0) == SipHash24ReferenceVectors[0]);
    usize mismatches = 0;
    for (usize length = 0; length != 64; ++length) {
        if (SipHash24(key, message, length) != SipHash24ReferenceVectors[length] ||
            SipHash24(key, shifted + 1, length) != SipHash24ReferenceVectors[length]) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);
}

// word 경로는 같은 word를 little-endian byte로 이어 붙인 byte 경로와 결과가 같다. 정수·enum·Guid·
// Name 특수화가 word 경로를 쓰므로 참조 벡터와 같은 함수임을 이것으로 고정한다.
TEST(SeededHash_WordPathMatchesBytePath) {
    const HashSeed key{0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull};
    // 메시지 00 01 .. 3f를 word 8개로 읽은 값이다.
    uint64 words[8] = {};
    for (usize word = 0; word != 8; ++word) {
        for (usize byte = 0; byte != 8; ++byte) {
            words[word] |= static_cast<uint64>(word * 8 + byte) << (byte * 8);
        }
    }

    for (usize count = 0; count != 8; ++count) {
        CHECK(SipHash24Words(key, words, count) == SipHash24ReferenceVectors[count * 8]);
        CHECK(SipHash24Words(key, words, count) == SipHash24(key, words, count * sizeof(uint64)));
    }

    const HashSeed seed{0x1234, 0x5678};
    const int64 negative = -5;
    const uint64 widened = static_cast<uint64>(negative);
    CHECK(SeededHash<int64>(seed)(negative) == SipHash24(seed, &widened, sizeof(widened)));
    const uint64 guidWords[2] = {0x0102030405060708ull, 0x090a0b0c0d0e0f10ull};
    CHECK(SeededHash<Guid>(seed)(Guid{guidWords[0], guidWords[1]}) ==
          SipHash24(seed, guidWords, sizeof(guidWords)));
}

TEST(SeededHash_EqualKeysAgreeAndSeedEntersTheHash) {
    SystemAllocator allocator;
    {
        const HashSeed first{1, 2};
        const HashSeed second{3, 4};
        String owned(allocator);
        REQUIRE(owned.TryAssign("Weapon"));
        // 소유 문자열과 StringView 조회가 같은 값이어야 이종 조회가 된다.
        CHECK(SeededHash<String>(first)(owned) == SeededHash<String>(first)(StringView("Weapon")));
        CHECK(SeededHash<String>(first)(owned) ==
              SeededHash<StringView>(first)(StringView("Weapon")));
        // seed가 해시 계산 안에 들어간다(같은 입력, 다른 seed).
        CHECK(SeededHash<String>(first)(owned) != SeededHash<String>(second)(owned));
        CHECK(SeededHash<int32>(first)(7) != SeededHash<int32>(second)(7));
        CHECK(SeededHash<Guid>(first)(Guid{1, 2}) != SeededHash<Guid>(second)(Guid{1, 2}));

        // 기본 생성은 프로세스 seed 하나를 같이 쓴다.
        const SeededHash<int32> processA;
        const SeededHash<int32> processB;
        CHECK(processA.Seed().k0 == processB.Seed().k0 && processA.Seed().k1 == processB.Seed().k1);
        CHECK(processA(42) == processB(42));

        // 같은 Name(ASCII 대소문자 표시 변형)은 같은 값이다.
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto upper = pool.TryIntern("Weapon_3");
        auto lower = pool.TryIntern("wEAPON_3");
        auto other = pool.TryIntern("Weapon_4");
        REQUIRE(upper && lower && other);
        CHECK(upper.Value() == lower.Value());
        CHECK(SeededHash<Name>(first)(upper.Value()) == SeededHash<Name>(first)(lower.Value()));
        CHECK(SeededHash<Name>(first)(upper.Value()) != SeededHash<Name>(first)(other.Value()));
        CHECK(SeededHash<Name>(first)(Name{}) == SeededHash<Name>(first)(Name{}));
        CHECK(SeededHash<Name>(first)(upper.Value()) != SeededHash<Name>(second)(upper.Value()));

        // enum 키는 기반 정수 값으로 넓혀 해시한다. 값이 같으면 같고, seed가 다르면 다르다.
        const uint64 enumWidened = static_cast<uint64>(SeedTestKind::Second);
        CHECK(SeededHash<SeedTestKind>(first)(SeedTestKind::Second) ==
              SipHash24Words(first, &enumWidened, 1));
        CHECK(SeededHash<SeedTestKind>(first)(SeedTestKind::Second) ==
              SeededHash<uint16>(first)(uint16{900}));
        CHECK(SeededHash<SeedTestKind>(first)(SeedTestKind::First) !=
              SeededHash<SeedTestKind>(first)(SeedTestKind::Second));
        CHECK(SeededHash<SeedTestKind>(first)(SeedTestKind::First) !=
              SeededHash<SeedTestKind>(second)(SeedTestKind::First));
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 한 seed에서 색인 칸(낮은 6 bit)이 모두 같은 키를 골라도, seed가 다르면 칸이 흩어진다. 상대가
// seed를 모르면 충돌 집합을 미리 만들 수 없다는 뜻이다. 시간을 재지 않고 칸 수를 센다(고정 seed라
// 결정적이다).
TEST(SeededHash_MaskCollisionsUnderOneSeedScatterUnderAnother) {
    constexpr uint64 Mask = 63;
    constexpr usize Chosen = 32;
    const SeededHash<int32> attacked(HashSeed{0x1111, 0x2222});
    const SeededHash<int32> actual(HashSeed{0x3333, 0x4444});
    int32 keys[Chosen] = {};
    usize found = 0;
    for (int32 key = 0; found != Chosen; ++key) {
        if ((attacked(key) & Mask) == 0) {
            keys[found++] = key;
        }
    }

    bool used[Mask + 1] = {};
    usize distinct = 0;
    for (int32 key : keys) {
        const uint64 slot = actual(key) & Mask;
        if (!used[slot]) {
            used[slot] = true;
            ++distinct;
        }
    }

    // 32개를 64칸에 고르게 던지면 평균 약 25칸이다. 한 칸에 몰리지 않음을 넉넉한 하한으로 본다.
    CHECK(distinct >= 16);
}
