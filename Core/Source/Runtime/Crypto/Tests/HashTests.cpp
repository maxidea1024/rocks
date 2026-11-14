#include <Core/Memory/SystemAllocator.h>
#include <Crypto/Bytes.h>
#include <Crypto/Hash.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처: BLAKE2b·SHA-512·SHA3·SHAKE는 Python hashlib(OpenSSL)로 계산하고 libsodium
// 1.0.22-stable test/default(hash.exp·hash_sha3*.exp·xof_shake*.c)와 대조한 값이다.
// BLAKE2b-512("abc")는 RFC 7693 부록 A, TurboSHAKE는 upstream test/default/xof_turboshake*.c(RFC
// 9861·Zig 표준 라이브러리 벡터, domain 0x1F)다.
namespace {

constexpr const char* Fox = "The quick brown fox jumps over the lazy dog";

Span<const uint8> Text(const char* text) {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

// actual이 hex 문자열과 같은 byte인가.
bool Equals(Span<const uint8> actual, const char* hex) {
    uint8 expected[128];
    auto decoded = TryFromHex(StringView(hex), Span<uint8>(expected));
    return decoded && decoded.Value() == actual.Size() &&
           ConstantTimeEquals(actual, Span<const uint8>(expected, decoded.Value()));
}

// 0..999를 251로 나눈 나머지(여러 블록에 걸친 입력, 조각 시험용).
struct LongInput {
    uint8 bytes[1000];

    LongInput() {
        for (usize i = 0; i < 1000; ++i) {
            bytes[i] = static_cast<uint8>(i % 251);
        }
    }
};

constexpr const char* LongBlake2b64 =
    "c11e1c0340bd7e5a1b275f1230c962fad215ecb1391486e74e31b960a2f29963"
    "81a5fad092da06841d5f26e38f6ecfeaf441acbcd1c2de61aef121e7927175f5";
constexpr const char* LongSha512 =
    "5096498d96f50f9a137c4db5b8b0cd38383ad55350fb5a98805fedc31fa1262f"
    "1f0cf4d6f12d7ecd8dedd933a4c9126344fe22e937a8ad35fdeae1e876ae698b";
constexpr const char* LongSha3_256 =
    "48e66a01861d0eadaacdb7a6ae7db6b9ac79242ecced4154a9fbb33c4e3cc571";
constexpr const char* LongShake256_100 = "34833f03ed88bb5f083ce590c7ae5af93ede33e11f53c70e47916c704"
                                         "4746acbdca19a73ff13905e91f8dc25ce6e41ae"
                                         "59fe75441bd548dda9114aca1da7180231fc22b353327cd25e00749aa"
                                         "277ae0fb1103ffd454d17ae8334090a8f3fb2a5"
                                         "6df10ec6";

template <usize N>
bool Blake2bEquals(Span<const uint8> input, const char* hex) {
    uint8 digest[N];
    return TryBlake2b(input, Span<uint8>(digest)).IsOk() && Equals(Span<const uint8>(digest), hex);
}

template <usize N, typename Function>
bool XofEquals(Function function, Span<const uint8> input, const char* hex) {
    uint8 output[N];
    return function(input, Span<uint8>(output)).IsOk() && Equals(Span<const uint8>(output), hex);
}

} // namespace

TEST(CryptoHashKnownAnswers) {
    const uint8 rate168[168] = {};
    const uint8 rate136[136] = {};

    CHECK(Blake2bEquals<64>(Text("abc"),
                            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
                            "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"));
    CHECK(Blake2bEquals<32>(Text(""),
                            "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8"));
    CHECK(Blake2bEquals<16>(Text(Fox), "249df9a49f517ddcd37f5c897620ec73"));
    CHECK(Blake2bEquals<32>(Text(Fox),
                            "01718cec35cd3d796dd00020e0bfecb473ad23457d063b75eff29c0ffa2e58a9"));

    CHECK(Equals(Sha512(Text("")).View(),
                 "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    CHECK(Equals(Sha512(Text("abc")).View(),
                 "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    CHECK(Equals(Sha512(Text("testing\n")).View(),
                 "24f950aac7b9ea9b3cb728228a0c82b67c39e96b4b344798870d5daee93e3ae5"
                 "931baae8c7cacfea4b629452c38026a81d138bc7aad1af3ef7bfd5ec646d6c28"));

    CHECK(Equals(Sha3_256(Text("")).View(),
                 "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"));
    CHECK(Equals(Sha3_256(Text("abc")).View(),
                 "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"));
    CHECK(Equals(Sha3_512(Text("abc")).View(),
                 "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
                 "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0"));
    CHECK(Equals(Sha3_512(Text(Fox)).View(),
                 "01dedd5de4ef14642445ba5f5b97c15e47b9ad931326e4b0727cd94cefc44fff"
                 "23f07bf543139939b49128caf436dc1bdee54fcb24023a08d9403f9b4bf0d450"));

    CHECK(XofEquals<32>(TryShake128, Text(""),
                        "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26"));
    CHECK(XofEquals<32>(TryShake128, Text(Fox),
                        "f4202e3c5852f9182a0430fd8144f0a74b95e7417ecae17db0f8cfeed0e3e66e"));
    CHECK(XofEquals<32>(TryShake128, Span<const uint8>(rate168),
                        "7c00ff4748870cb26da4dc078aff74477ab153fa1191c7b636fea6c01ecc1fab"));
    CHECK(XofEquals<64>(TryShake256, Text(""),
                        "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f"
                        "d75dc4ddd8c0f200cb05019d67b592f6fc821c49479ab48640292eacb3b7c4be"));
    CHECK(XofEquals<32>(TryShake256, Text("abc"),
                        "483366601360a8771c6863080cc4114d8db44530f8f1e1ee4f94ea37e78b5739"));
    CHECK(XofEquals<64>(TryShake256, Text(Fox),
                        "2f671343d9b2e1604dc9dcf0753e5fe15c7c64a0d283cbbf722d411a0e36f6ca"
                        "1d01d1369a23539cd80f7c054b6e5daf9c962cad5b8ed5bd11998b40d5734442"));
    CHECK(XofEquals<32>(TryShake256, Span<const uint8>(rate136),
                        "ea947b835fec1f9b0a7eabba901deb7881fd9999a1cbd5ccbb5a9afab7f6fe70"));

    auto turbo128 = [](Span<const uint8> input, Span<uint8> output) {
        return TryTurboShake128(input, output);
    };
    auto turbo256 = [](Span<const uint8> input, Span<uint8> output) {
        return TryTurboShake256(input, output);
    };
    CHECK(XofEquals<64>(turbo128, Text(""),
                        "1e415f1c5983aff2169217277d17bb538cd945a397ddec541f1ce41af2c1b74c"
                        "3e8ccae2a4dae56c84a04c2385c03c15e8193bdf58737363321691c05462c8df"));
    CHECK(XofEquals<32>(turbo128, Text("abc"),
                        "dcf1646dfe993a8eb6b782d1faaca6d82416a5dcf1de98ee3c6dbc5e1dc63018"));
    CHECK(XofEquals<64>(turbo128, Text(Fox),
                        "76a1720a4848ab64e67e563f16b8c5aa492b698a4d93429735fd02354657fbf7"
                        "a0689ec77b4c795fda9daab410c63092f54200846c34120ff2b253e9fd8d9fc4"));
    CHECK(XofEquals<32>(turbo128, Span<const uint8>(rate168),
                        "dba6e267bdd567db0ad2636e61f1ae589a81c1a9c11f7f76930a35ea424756d0"));
    CHECK(XofEquals<64>(turbo256, Text(""),
                        "367a329dafea871c7802ec67f905ae13c57695dc2c6663c61035f59a18f8e7db"
                        "11edc0e12e91ea60eb6b32df06dd7f002fbafabb6e13ec1cc20d995547600db0"));
    CHECK(XofEquals<32>(turbo256, Text("abc"),
                        "63824b1431a7372e85edc022c9d7afdd027472fcfa33c887d6f5aaf8dc5d4db6"));
    CHECK(XofEquals<64>(turbo256, Text(Fox),
                        "b6e91a412c262c7936b069f67bd21c2f8ecc48bda8dc6eebfbaf6fcaa82191c3"
                        "974462707ab2a5c5d704b0e874860a2a3fddb588f507c9b4f0417e2b66316090"));
    CHECK(XofEquals<32>(turbo256, Span<const uint8>(rate136),
                        "91effd08dd4cccb689c626b4649367ad5a2ebfab61611769a37493fa701228ea"));
}

TEST(CryptoHashPiecesAndIncrementalMatchOneShot) {
    const LongInput input;
    const Span<const uint8> all(input.bytes);

    // 조각 입력: 앞 1 byte, 다음 127 byte, 빈 조각, 나머지.
    const Span<const uint8> pieces[] = {all.Left(1), all.Mid(1, 127), Span<const uint8>(),
                                        all.RightChop(128)};
    uint8 blake[64];
    REQUIRE(TryBlake2b(Span<const Span<const uint8>>(pieces), Span<uint8>(blake)));
    CHECK(Equals(Span<const uint8>(blake), LongBlake2b64));
    CHECK(Equals(Sha512(Span<const Span<const uint8>>(pieces)).View(), LongSha512));
    CHECK(Equals(Sha3_256(Span<const Span<const uint8>>(pieces)).View(), LongSha3_256));
    CHECK(Equals(Sha512(all).View(), LongSha512));

    // 증분 객체: 조각 크기를 바꿔도 같은 결과다.
    const usize chunks[] = {1, 7, 128, 999, 1000};
    for (usize chunk : chunks) {
        auto blake2b = HashState::TryCreate(HashAlgorithm::Blake2b, 64);
        auto sha512 = HashState::TryCreate(HashAlgorithm::Sha512);
        auto sha3 = HashState::TryCreate(HashAlgorithm::Sha3_256);
        auto shake = XofState::TryCreate(XofAlgorithm::Shake256);
        REQUIRE(blake2b && sha512 && sha3 && shake);
        for (usize offset = 0; offset < all.Size(); offset += chunk) {
            const Span<const uint8> part = all.Mid(offset, chunk);
            REQUIRE(blake2b.Value().TryUpdate(part));
            REQUIRE(sha512.Value().TryUpdate(part));
            REQUIRE(sha3.Value().TryUpdate(part));
            REQUIRE(shake.Value().TryUpdate(part));
        }

        uint8 digest64[64];
        uint8 digest32[32];
        REQUIRE(blake2b.Value().TryFinish(Span<uint8>(digest64)));
        CHECK(Equals(Span<const uint8>(digest64), LongBlake2b64));
        REQUIRE(sha512.Value().TryFinish(Span<uint8>(digest64)));
        CHECK(Equals(Span<const uint8>(digest64), LongSha512));
        REQUIRE(sha3.Value().TryFinish(Span<uint8>(digest32)));
        CHECK(Equals(Span<const uint8>(digest32), LongSha3_256));

        // XOF는 여러 번 짜내도 한 번에 짜낸 것과 같다.
        uint8 squeezed[100];
        REQUIRE(shake.Value().TrySqueeze(Span<uint8>(squeezed).Left(1)));
        REQUIRE(shake.Value().TrySqueeze(Span<uint8>(squeezed).Mid(1, 31)));
        REQUIRE(shake.Value().TrySqueeze(Span<uint8>(squeezed).RightChop(32)));
        CHECK(Equals(Span<const uint8>(squeezed), LongShake256_100));
    }
}

TEST(CryptoHashStateLifecycle) {
    // Finish 뒤 Update·Finish는 InvalidArgument, Reset하면 처음부터 다시 쓴다.
    auto state = HashState::TryCreate(HashAlgorithm::Sha3_256);
    REQUIRE(state);
    CHECK(state.Value().Algorithm() == HashAlgorithm::Sha3_256 &&
          state.Value().DigestBytes() == 32);
    REQUIRE(state.Value().TryUpdate(Text("abc")));
    uint8 digest[32];
    REQUIRE(state.Value().TryFinish(Span<uint8>(digest)));
    CHECK(Equals(Span<const uint8>(digest),
                 "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"));
    CHECK(state.Value().TryUpdate(Text("x")).Error() == ErrorCode::InvalidArgument);
    CHECK(state.Value().TryFinish(Span<uint8>(digest)).Error() == ErrorCode::InvalidArgument);
    state.Value().Reset();
    REQUIRE(state.Value().TryFinish(Span<uint8>(digest)));
    CHECK(Equals(Span<const uint8>(digest),
                 "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"));

    // 이동된 객체는 StaleHandle이다.
    HashState moved(Move(state.Value()));
    CHECK(state.Value().TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    CHECK(state.Value().TryFinish(Span<uint8>(digest)).Error() == ErrorCode::StaleHandle);
    moved.Reset();
    REQUIRE(moved.TryUpdate(Text("abc")));

    // 잘못된 길이: 출력 길이 불일치는 상태를 바꾸지 않는다.
    uint8 wrong[31];
    CHECK(moved.TryFinish(Span<uint8>(wrong)).Error() == ErrorCode::InvalidArgument);
    REQUIRE(moved.TryFinish(Span<uint8>(digest)));
    CHECK(Equals(Span<const uint8>(digest),
                 "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"));

    // XOF: 짜낸 뒤에는 Update가 InvalidArgument다.
    auto xof = XofState::TryCreate(XofAlgorithm::Shake128);
    REQUIRE(xof);
    uint8 out[8];
    REQUIRE(xof.Value().TrySqueeze(Span<uint8>(out)));
    CHECK(xof.Value().TryUpdate(Text("x")).Error() == ErrorCode::InvalidArgument);
    xof.Value().Reset();
    REQUIRE(xof.Value().TryUpdate(Text("x")));
}

TEST(CryptoHashStateUsesTheGivenAllocator) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    {
        auto state = HashState::TryCreate(HashAlgorithm::Blake2b);
        REQUIRE(state);
        CHECK(GetSecretMemoryStatus().bufferAllocations == before.bufferAllocations + 1);
    }

    CHECK(GetSecretMemoryStatus().bufferAllocations == before.bufferAllocations);

    // 다른 allocator를 주면 그 allocator에서 할당하고 돌려준다(SystemAllocator는 소멸 때 남은
    // 할당이 있으면 Fatal).
    SystemAllocator allocator;
    {
        auto state = HashState::TryCreate(HashAlgorithm::Sha512, 0, allocator);
        auto xof = XofState::TryCreate(XofAlgorithm::TurboShake256, 0x0B, allocator);
        REQUIRE(state && xof);
        CHECK(allocator.Stats().liveAllocations == 2);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CryptoHashRejectsMisuse) {
    uint8 buffer[80] = {};
    const Span<uint8> whole(buffer);

    // BLAKE2b digest 길이는 16–64다.
    CHECK(TryBlake2b(Text("abc"), whole.Left(15)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryBlake2b(Text("abc"), whole.Left(65)).Error() == ErrorCode::InvalidArgument);

    // 출력이 입력이나 조각 배열과 겹치면 쓰기 전에 거부한다.
    CHECK(TryBlake2b(Span<const uint8>(buffer).Left(40), whole.Mid(32, 32)).Error() ==
          ErrorCode::InvalidArgument);
    const Span<const uint8> pieces[] = {Text("a"), Span<const uint8>(buffer).Mid(70, 4)};
    CHECK(TryBlake2b(Span<const Span<const uint8>>(pieces), whole.Mid(60, 16)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryShake128(Span<const uint8>(buffer).Left(16), whole.Mid(8, 16)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryShake128(Text("abc"), Span<uint8>()));

    // TurboSHAKE domain은 0x01–0x7F다. 다른 domain은 다른 출력을 낸다.
    CHECK(TryTurboShake128(Text("abc"), whole.Left(32), 0x00).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryTurboShake128(Text("abc"), whole.Left(32), 0x80).Error() ==
          ErrorCode::InvalidArgument);
    uint8 standard[32];
    uint8 custom[32];
    uint8 incremental[32];
    REQUIRE(TryTurboShake128(Text("abc"), Span<uint8>(standard)));
    REQUIRE(TryTurboShake128(Text("abc"), Span<uint8>(custom), 0x0B));
    CHECK(!ConstantTimeEquals(Span<const uint8>(standard), Span<const uint8>(custom)));
    auto domain = XofState::TryCreate(XofAlgorithm::TurboShake128, 0x0B);
    REQUIRE(domain);
    REQUIRE(domain.Value().TryUpdate(Text("abc")));
    REQUIRE(domain.Value().TrySqueeze(Span<uint8>(incremental)));
    CHECK(ConstantTimeEquals(Span<const uint8>(custom), Span<const uint8>(incremental)));

    // 증분 객체 인자: 고정 길이와 다른 digest, BLAKE2b 범위 밖, SHAKE에 domain, 열거값이 아닌
    // 알고리즘.
    CHECK(HashState::TryCreate(HashAlgorithm::Sha512, 32).Error() == ErrorCode::InvalidArgument);
    CHECK(HashState::TryCreate(HashAlgorithm::Blake2b, 15).Error() == ErrorCode::InvalidArgument);
    CHECK(HashState::TryCreate(HashAlgorithm::Blake2b, 65).Error() == ErrorCode::InvalidArgument);
    CHECK(HashState::TryCreate(static_cast<HashAlgorithm>(9)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(XofState::TryCreate(XofAlgorithm::Shake256, 0x0B).Error() == ErrorCode::InvalidArgument);
    CHECK(XofState::TryCreate(XofAlgorithm::TurboShake256, 0x80).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(XofState::TryCreate(static_cast<XofAlgorithm>(9)).Error() == ErrorCode::InvalidArgument);

    // 기본 생성한 객체는 이동된 객체와 같다.
    HashState empty;
    CHECK(empty.TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    XofState emptyXof;
    CHECK(emptyXof.TrySqueeze(whole.Left(4)).Error() == ErrorCode::StaleHandle);
}
