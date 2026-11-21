#include <AssetRuntime/ContentSignature.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 콘텐츠 서명 봉투 시험이다(Docs/Design/ContentSigning.ko.md 3·4·7절 — A1f): 독립 구현(RFC 8032
// 참조 Python 코드)과 같은 golden 봉투, 모든 byte·bit 변조의 판정 단계, 자르기·늘리기, 신뢰 정보
// 조합, 겹친 위반의 판정 순서, scope 이름 규칙, 쓰기·검증 인자 오류와 할당 실패.
using namespace rocks;

namespace content_signature_tests_file {

namespace {

using Field = ContentEnvelopeField;

// golden 입력이다(ed25519ph_ref.py와 같은 값): seed 1..32, keyId byte 0x40..0x4f, 버전 7, scope
// ("rocks.test", "main"), 매니페스트 byte "cooked-manifest-golden-bytes-v1".
constexpr uint64 GoldenVersion = 7;
constexpr char GoldenManifest[] = "cooked-manifest-golden-bytes-v1";
constexpr usize GoldenManifestBytes = sizeof(GoldenManifest) - 1;
constexpr const char* GoldenPublicHex =
    "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664";
constexpr const char* GoldenEnvelopeHex =
    "5253494701020000404142434445464748494a4b4c4d4e4f0700000000000000"
    "90247830ed4611caa2c6bbd614ba47aadb1a18adf4272a86df95503548e1116a"
    "1f000000000000009ac8d7fc0045177e58f04bd19bcb4eeddef81c7481abdba6"
    "c98b2884f1f8b4f5c6906590e651aaf9fa8c0db558cfaaa938279f4c3365b6a9"
    "3e59b49b4f5d130f636f6f6b65642d6d616e69666573742d676f6c64656e2d62"
    "797465732d7631";
constexpr usize GoldenEnvelopeBytes = ContentEnvelopeManifestOffset + GoldenManifestBytes;

// 봉투 칸 offset이다(3절 표 — 판정 단계 기대값 계산용).
constexpr usize VersionOffset = 4;
constexpr usize AlgorithmOffset = 5;
constexpr usize ReservedOffset = 6;
constexpr usize KeyIdOffset = 8;
constexpr usize ContentVersionOffset = 24;
constexpr usize ScopeOffset = 32;
constexpr usize ManifestLengthOffset = 64;
constexpr usize SignatureOffset = ContentEnvelopeHeaderBytes;
constexpr uint32 BitsPerByte = 8;

uint8 HexDigit(char c) noexcept {
    return static_cast<uint8>(c <= '9' ? c - '0' : c - 'a' + 10);
}

bool FromHex(const char* hex, Array<uint8>& out) noexcept {
    const usize length = strlen(hex);
    if (length % 2 != 0 || !out.TryResize(length / 2)) {
        return false;
    }

    for (usize index = 0; index < length / 2; ++index) {
        out[index] =
            static_cast<uint8>(HexDigit(hex[2 * index]) << 4 | HexDigit(hex[2 * index + 1]));
    }

    return true;
}

Span<const uint8> ManifestView() noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(GoldenManifest), GoldenManifestBytes);
}

crypto::KeyId GoldenKeyId() noexcept {
    crypto::KeyId id;
    id.value.high = 0x4041424344454647ull;
    id.value.low = 0x48494a4b4c4d4e4full;
    return id;
}

crypto::KeyId OtherKeyId(uint64 low) noexcept {
    crypto::KeyId id;
    id.value.high = 0x0102030405060708ull;
    id.value.low = low;
    return id;
}

crypto::Ed25519SigningKey SeedKey(uint8 first) noexcept {
    uint8 seed[32];
    for (uint8 index = 0; index < 32; ++index) {
        seed[index] = static_cast<uint8>(first + index);
    }

    auto key = crypto::Ed25519SigningKey::TryFromSeed(seed);
    return key ? Move(key.Value()) : crypto::Ed25519SigningKey();
}

Sha256Digest GoldenScope() noexcept {
    auto scope = MakeContentScope("rocks.test", "main");
    return scope ? scope.Value() : Sha256Digest{};
}

// golden 키 하나를 신뢰하는 정보다(최소 버전 GoldenVersion).
struct GoldenTrust {
    GoldenTrust() noexcept {
        keys[0].id = GoldenKeyId();
        keys[0].key = SeedKey(1).PublicKey();
        trust.keys = Span<const TrustedContentKey>(keys, 1);
        trust.minimumContentVersion = GoldenVersion;
        trust.scope = GoldenScope();
    }

    TrustedContentKey keys[1];
    ContentTrust trust;
};

bool WriteGolden(Allocator& allocator, Array<uint8>& out) noexcept {
    return WriteContentEnvelope(allocator, ManifestView(), SeedKey(1), GoldenKeyId(), GoldenVersion,
                                GoldenScope(), out)
        .IsOk();
}

// 검증이 실패하고 진단이 (code, error, field)인지다.
bool FailsAt(Allocator& allocator, Span<const uint8> envelope, const ContentTrust& trust,
             AssetDiagnosticCode code, ErrorCode error, Field field) noexcept {
    AssetDiagnostic diagnostic;
    auto verified = VerifyContentEnvelope(allocator, envelope, trust, nullptr, &diagnostic);
    const bool same = !verified && verified.Error() == error && diagnostic.code == code &&
                      diagnostic.error == error && diagnostic.section == static_cast<uint32>(field);
    if (!same) {
        printf("  expected %s/%s field %u, got %s/%s field %u\n", AssetDiagnosticName(code),
               ErrorCodeName(error), static_cast<unsigned>(field),
               AssetDiagnosticName(diagnostic.code),
               verified ? "None" : ErrorCodeName(verified.Error()),
               static_cast<unsigned>(diagnostic.section));
    }

    return same;
}

bool NotTrustedAt(Allocator& allocator, Span<const uint8> envelope, const ContentTrust& trust,
                  Field field) noexcept {
    return FailsAt(allocator, envelope, trust, AssetDiagnosticCode::ContentNotTrusted,
                   ErrorCode::AuthenticationFailed, field);
}

bool CorruptAt(Allocator& allocator, Span<const uint8> envelope, const ContentTrust& trust,
               Field field) noexcept {
    return FailsAt(allocator, envelope, trust, AssetDiagnosticCode::CorruptPackage,
                   ErrorCode::InvalidArgument, field);
}

bool UnsupportedAt(Allocator& allocator, Span<const uint8> envelope, const ContentTrust& trust,
                   Field field) noexcept {
    return FailsAt(allocator, envelope, trust, AssetDiagnosticCode::UnsupportedVersion,
                   ErrorCode::Unsupported, field);
}

uint64 LoadLe64(const uint8* in) noexcept {
    uint64 value = 0;
    for (usize index = 0; index < sizeof(uint64); ++index) {
        value |= static_cast<uint64>(in[index]) << (8 * index);
    }

    return value;
}

void StoreLe64(uint8* out, uint64 value) noexcept {
    for (usize index = 0; index < sizeof(uint64); ++index) {
        out[index] = static_cast<uint8>(value >> (8 * index));
    }
}

} // namespace

// golden(7절 7): 고정 seed 키로 쓴 봉투가 독립 구현(RFC 8032 참조 Python)이 만든 byte와 같고,
// 공개키도 같다. 검증은 같은 버퍼 안의 매니페스트 span(복사 없음)과 헤더 값을 돌려준다. 같은 입력을
// 다시 쓰면 같은 byte다.
TEST(ContentSignature_GoldenEnvelope) {
    SystemAllocator allocator;
    Array<uint8> expected(allocator);
    REQUIRE(FromHex(GoldenEnvelopeHex, expected));
    REQUIRE(expected.Size() == GoldenEnvelopeBytes);
    Array<uint8> publicKey(allocator);
    REQUIRE(FromHex(GoldenPublicHex, publicKey));
    CHECK(memcmp(SeedKey(1).PublicKey().bytes, publicKey.Data(), crypto::Ed25519PublicKeyBytes) ==
          0);
    Array<uint8> envelope(allocator);
    REQUIRE(WriteGolden(allocator, envelope));
    REQUIRE(envelope.Size() == expected.Size());
    CHECK(memcmp(envelope.Data(), expected.Data(), expected.Size()) == 0);
    Array<uint8> again(allocator);
    REQUIRE(WriteGolden(allocator, again));
    CHECK(again.Size() == envelope.Size() &&
          memcmp(again.Data(), envelope.Data(), again.Size()) == 0);
    GoldenTrust golden;
    ContentEnvelopeInfo info;
    AssetDiagnostic diagnostic;
    auto verified =
        VerifyContentEnvelope(allocator, expected.View(), golden.trust, &info, &diagnostic);
    REQUIRE(verified);
    CHECK(verified.Value().Data() == expected.Data() + ContentEnvelopeManifestOffset);
    CHECK(verified.Value().Size() == GoldenManifestBytes);
    CHECK(memcmp(verified.Value().Data(), GoldenManifest, GoldenManifestBytes) == 0);
    CHECK(info.keyId == GoldenKeyId());
    CHECK(info.contentVersion == GoldenVersion);
    CHECK(info.scope == GoldenScope());
    CHECK(diagnostic.code == AssetDiagnosticCode::None);
    // 진단·info 자리는 없어도 된다.
    CHECK(VerifyContentEnvelope(allocator, expected.View(), golden.trust));
}

// 모든 bit 변조(7절 7, 4절 순서): 봉투의 모든 byte의 모든 bit를 하나씩 뒤집으면 검증이 실패하고
// 단계가 칸으로 정해진다 — magic·예약·매니페스트 길이는 CorruptPackage, 버전·알고리즘은
// UnsupportedVersion, keyId는 모르는 키, contentVersion은 최소 미만이면 그 단계·아니면 서명, scope,
// 서명·매니페스트 byte는 서명 불일치다.
TEST(ContentSignature_EveryBitFlip) {
    SystemAllocator allocator;
    Array<uint8> envelope(allocator);
    REQUIRE(WriteGolden(allocator, envelope));
    GoldenTrust golden;
    uint32 flips = 0;
    uint32 mismatches = 0;
    for (usize offset = 0; offset < envelope.Size(); ++offset) {
        for (uint32 bit = 0; bit < BitsPerByte; ++bit) {
            envelope[offset] = static_cast<uint8>(envelope[offset] ^ (1u << bit));
            bool ok = false;
            if (offset < VersionOffset) {
                ok = CorruptAt(allocator, envelope.View(), golden.trust, Field::Magic);
            } else if (offset == VersionOffset) {
                ok = UnsupportedAt(allocator, envelope.View(), golden.trust, Field::FormatVersion);
            } else if (offset == AlgorithmOffset) {
                ok = UnsupportedAt(allocator, envelope.View(), golden.trust, Field::Algorithm);
            } else if (offset < KeyIdOffset) {
                ok = CorruptAt(allocator, envelope.View(), golden.trust, Field::Reserved);
            } else if (offset < ContentVersionOffset) {
                ok = NotTrustedAt(allocator, envelope.View(), golden.trust, Field::Key);
            } else if (offset < ScopeOffset) {
                const uint64 version = LoadLe64(envelope.Data() + ContentVersionOffset);
                ok = NotTrustedAt(allocator, envelope.View(), golden.trust,
                                  version < GoldenVersion ? Field::ContentVersion
                                                          : Field::Signature);
            } else if (offset < ManifestLengthOffset) {
                ok = NotTrustedAt(allocator, envelope.View(), golden.trust, Field::Scope);
            } else if (offset < SignatureOffset) {
                ok = CorruptAt(allocator, envelope.View(), golden.trust, Field::ManifestLength);
            } else {
                ok = NotTrustedAt(allocator, envelope.View(), golden.trust, Field::Signature);
            }

            if (!ok) {
                printf("  offset %zu bit %u\n", offset, bit);
                ++mismatches;
            }

            envelope[offset] = static_cast<uint8>(envelope[offset] ^ (1u << bit));
            ++flips;
        }
    }

    CHECK(mismatches == 0);
    CHECK(flips == GoldenEnvelopeBytes * BitsPerByte);
    // 되돌린 봉투는 다시 통과한다.
    CHECK(VerifyContentEnvelope(allocator, envelope.View(), golden.trust));
}

// 자르기·늘리기(7절 7): 136 byte 미만으로 자르면 File, 그 이상에서 자르거나 1 byte 늘리면
// 매니페스트 길이가 남은 byte와 달라 ManifestLength다. 길이 칸까지 맞춰 고친 잘린 봉투는 서명이
// 거부한다. 봉투 상한(CookedManifestMaxFileBytes + 136)을 넘는 크기는 내용을 읽기 전에 File이다.
TEST(ContentSignature_TruncateAndExtend) {
    SystemAllocator allocator;
    Array<uint8> envelope(allocator);
    REQUIRE(WriteGolden(allocator, envelope));
    GoldenTrust golden;
    for (usize size = 0; size < envelope.Size(); ++size) {
        const Span<const uint8> cut = envelope.View().Slice(0, size);
        CHECK(
            CorruptAt(allocator, cut, golden.trust,
                      size < ContentEnvelopeManifestOffset ? Field::File : Field::ManifestLength));
    }

    Array<uint8> longer(allocator);
    REQUIRE(longer.TryAppend(envelope.View()));
    REQUIRE(longer.TryPushBack(uint8{0}));
    CHECK(CorruptAt(allocator, longer.View(), golden.trust, Field::ManifestLength));
    // 길이 칸을 잘린 길이로 고쳐도 서명이 덮으므로 거부한다.
    Array<uint8> shortened(allocator);
    REQUIRE(shortened.TryAppend(envelope.View().Slice(0, envelope.Size() - 1)));
    StoreLe64(shortened.Data() + ManifestLengthOffset, GoldenManifestBytes - 1);
    CHECK(NotTrustedAt(allocator, shortened.View(), golden.trust, Field::Signature));
    // 상한 초과: 크기 판정이 먼저라 그 뒤 byte를 읽지 않는다(실제 1 GiB 버퍼를 만들지 않는다).
    const Span<const uint8> oversized(envelope.Data(),
                                      static_cast<usize>(ContentEnvelopeMaxBytes + 1));
    CHECK(CorruptAt(allocator, oversized, golden.trust, Field::File));
}

// 신뢰 정보 조합(7절 1·2, CS3·CS4): 최소 버전 3(봉투보다 낮음·같음·높음) × 신뢰 키 위치 3(하나뿐·
// 여럿 가운데 마지막·없음) × scope 2(같음·다름) = 18. 버전이 높으면 실패(ContentVersion), 키가
// 없으면 Key, scope가 다르면 Scope이고 겹치면 4절 순서(Key → ContentVersion → Scope)가 이긴다. 모두
// 맞으면 성공하고 info가 그 keyId다.
TEST(ContentSignature_TrustCombinations) {
    SystemAllocator allocator;
    Array<uint8> envelope(allocator);
    REQUIRE(WriteGolden(allocator, envelope));
    const crypto::Ed25519PublicKey goldenPublic = SeedKey(1).PublicKey();
    const crypto::Ed25519PublicKey otherPublic = SeedKey(100).PublicKey();
    auto otherScope = MakeContentScope("rocks.test", "beta");
    REQUIRE(otherScope);
    const uint64 minimums[] = {GoldenVersion - 1, GoldenVersion, GoldenVersion + 1};
    uint32 cases = 0;
    for (uint64 minimum : minimums) {
        for (uint32 placement = 0; placement < 3; ++placement) {
            for (uint32 scopeDiffers = 0; scopeDiffers < 2; ++scopeDiffers) {
                TrustedContentKey keys[3];
                usize count = 1;
                if (placement == 0) {
                    keys[0] = {GoldenKeyId(), goldenPublic};
                } else if (placement == 1) {
                    keys[0] = {OtherKeyId(1), otherPublic};
                    keys[1] = {OtherKeyId(2), otherPublic};
                    keys[2] = {GoldenKeyId(), goldenPublic};
                    count = 3;
                } else {
                    keys[0] = {OtherKeyId(1), otherPublic};
                    keys[1] = {OtherKeyId(2), goldenPublic};
                    count = 2;
                }

                ContentTrust trust;
                trust.keys = Span<const TrustedContentKey>(keys, count);
                trust.minimumContentVersion = minimum;
                trust.scope = scopeDiffers != 0 ? otherScope.Value() : GoldenScope();
                bool ok = false;
                if (placement == 2) {
                    ok = NotTrustedAt(allocator, envelope.View(), trust, Field::Key);
                } else if (minimum > GoldenVersion) {
                    ok = NotTrustedAt(allocator, envelope.View(), trust, Field::ContentVersion);
                } else if (scopeDiffers != 0) {
                    ok = NotTrustedAt(allocator, envelope.View(), trust, Field::Scope);
                } else {
                    ContentEnvelopeInfo info;
                    ok = VerifyContentEnvelope(allocator, envelope.View(), trust, &info).IsOk() &&
                         info.keyId == GoldenKeyId();
                }

                CHECK(ok);
                ++cases;
            }
        }
    }

    CHECK(cases == 18);
}

// 신뢰 정보 오류(7절 1): 키 없음·null id·같은 id 두 번은 봉투를 보기 전에 InvalidArgument(진단
// None)다. 신뢰 목록의 id는 맞는데 공개키가 다르면 서명 불일치다(Signature).
TEST(ContentSignature_InvalidTrust) {
    SystemAllocator allocator;
    Array<uint8> envelope(allocator);
    REQUIRE(WriteGolden(allocator, envelope));
    const crypto::Ed25519PublicKey goldenPublic = SeedKey(1).PublicKey();
    auto invalid = [&](Span<const TrustedContentKey> keys) noexcept {
        ContentTrust trust;
        trust.keys = keys;
        trust.minimumContentVersion = GoldenVersion;
        trust.scope = GoldenScope();
        AssetDiagnostic diagnostic;
        diagnostic.code = AssetDiagnosticCode::CorruptPackage;
        auto verified =
            VerifyContentEnvelope(allocator, envelope.View(), trust, nullptr, &diagnostic);
        return !verified && verified.Error() == ErrorCode::InvalidArgument &&
               diagnostic.code == AssetDiagnosticCode::None;
    };
    CHECK(invalid({}));
    const TrustedContentKey nil[] = {{crypto::KeyId{}, goldenPublic}};
    CHECK(invalid(nil));
    const TrustedContentKey twice[] = {{GoldenKeyId(), goldenPublic},
                                       {OtherKeyId(1), goldenPublic},
                                       {GoldenKeyId(), goldenPublic}};
    CHECK(invalid(twice));
    const TrustedContentKey wrongKey[] = {{GoldenKeyId(), SeedKey(100).PublicKey()}};
    ContentTrust trust;
    trust.keys = wrongKey;
    trust.minimumContentVersion = GoldenVersion;
    trust.scope = GoldenScope();
    CHECK(NotTrustedAt(allocator, envelope.View(), trust, Field::Signature));
}

// 판정 순서(7절 2): 이웃한 두 단계를 함께 어기면 앞 단계가 이긴다 — magic > 형식 버전 > 알고리즘 >
// 예약 > 키 > contentVersion > scope > 매니페스트 길이 > 서명.
TEST(ContentSignature_OverlappingViolations) {
    SystemAllocator allocator;
    Array<uint8> golden(allocator);
    REQUIRE(WriteGolden(allocator, golden));
    GoldenTrust trust;
    trust.trust.minimumContentVersion = GoldenVersion;
    // 단계마다 그 단계만 어기는 변경이다.
    auto breakStage = [](Array<uint8>& bytes, Field field) noexcept {
        switch (field) {
        case Field::Magic:
            bytes[0] = 'X';
            break;
        case Field::FormatVersion:
            bytes[VersionOffset] = 2;
            break;
        case Field::Algorithm:
            bytes[AlgorithmOffset] = 1;
            break;
        case Field::Reserved:
            bytes[ReservedOffset + 1] = 1;
            break;
        case Field::Key:
            bytes[KeyIdOffset] = static_cast<uint8>(bytes[KeyIdOffset] ^ 0xffu);
            break;
        case Field::ContentVersion:
            StoreLe64(bytes.Data() + ContentVersionOffset, GoldenVersion - 1);
            break;
        case Field::Scope:
            bytes[ScopeOffset] = static_cast<uint8>(bytes[ScopeOffset] ^ 0xffu);
            break;
        case Field::ManifestLength:
            StoreLe64(bytes.Data() + ManifestLengthOffset, GoldenManifestBytes + 1);
            break;
        default:
            bytes[SignatureOffset] = static_cast<uint8>(bytes[SignatureOffset] ^ 0xffu);
            break;
        }
    };
    const Field order[] = {
        Field::Magic,    Field::FormatVersion,  Field::Algorithm, Field::Reserved,
        Field::Key,      Field::ContentVersion, Field::Scope,     Field::ManifestLength,
        Field::Signature};
    uint32 pairs = 0;
    for (usize index = 0; index + 1 < sizeof(order) / sizeof(order[0]); ++index) {
        Array<uint8> bytes(allocator);
        REQUIRE(bytes.TryAppend(golden.View()));
        breakStage(bytes, order[index]);
        breakStage(bytes, order[index + 1]);
        AssetDiagnostic diagnostic;
        CHECK(!VerifyContentEnvelope(allocator, bytes.View(), trust.trust, nullptr, &diagnostic));
        CHECK(diagnostic.section == static_cast<uint32>(order[index]));
        ++pairs;
    }

    CHECK(pairs == 8);
}

// scope 이름(7절 1): productId·channel은 비지 않은 [A-Za-z0-9_.-]이고 아니면 InvalidArgument(빈 값·
// NUL·공백·비 ASCII). 구분자 0이 있어 ("ab","c")와 ("a","bc")는 다르다. 값은 SHA-256(productId ‖ 0
// ‖ channel)이다.
TEST(ContentSignature_ScopeNames) {
    CHECK(MakeContentScope("Game_1.x-y", "live"));
    CHECK(MakeContentScope("", "live").Error() == ErrorCode::InvalidArgument);
    CHECK(MakeContentScope("Game", "").Error() == ErrorCode::InvalidArgument);
    CHECK(MakeContentScope(StringView("Ga\0me", 5), "live").Error() == ErrorCode::InvalidArgument);
    CHECK(MakeContentScope("Game", "be ta").Error() == ErrorCode::InvalidArgument);
    CHECK(MakeContentScope("G\xc3\xa9", "live").Error() == ErrorCode::InvalidArgument);
    CHECK(MakeContentScope("Game", "a/b").Error() == ErrorCode::InvalidArgument);
    // 범위 바로 바깥 문자도 받지 않는다: 'a'·'A'·'0' 바로 앞('`'·'@'·'/'), 'z'·'Z'·'9' 바로
    // 뒤('{'·'['·':'), 그리고 ASCII 끝 쪽('~').
    const char* const outside[] = {"a`b", "A@B", "0/1", "a{b", "A[B", "0:1", "a~b"};
    for (const char* name : outside) {
        CHECK(MakeContentScope(name, "live").Error() == ErrorCode::InvalidArgument);
        CHECK(MakeContentScope("live", name).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(MakeContentScope("az_AZ.09-", "AZaz09._-"));
    auto left = MakeContentScope("ab", "c");
    auto right = MakeContentScope("a", "bc");
    REQUIRE(left && right);
    CHECK(!(left.Value() == right.Value()));
    Sha256 hash;
    constexpr uint8 Expected[] = {'r', 'o', 'c', 'k', 's', '.', 't', 'e',
                                  's', 't', 0,   'm', 'a', 'i', 'n'};
    hash.Update(Expected);
    CHECK(GoldenScope() == hash.Finish());
}

// 쓰기 인자 오류(7절 1): null keyId InvalidArgument, 매니페스트 상한 초과 CapacityExceeded(내용을
// 읽기 전), 빈 키 StaleHandle — 실패하면 out은 그대로다. 빈 매니페스트도 봉투가 된다.
TEST(ContentSignature_WriteErrors) {
    SystemAllocator allocator;
    const crypto::Ed25519SigningKey key = SeedKey(1);
    Array<uint8> out(allocator);
    REQUIRE(out.TryPushBack(uint8{0xab}));
    CHECK(WriteContentEnvelope(allocator, ManifestView(), key, crypto::KeyId{}, GoldenVersion,
                               GoldenScope(), out)
              .Error() == ErrorCode::InvalidArgument);
    const Span<const uint8> oversized(reinterpret_cast<const uint8*>(GoldenManifest),
                                      static_cast<usize>(CookedManifestMaxFileBytes + 1));
    CHECK(WriteContentEnvelope(allocator, oversized, key, GoldenKeyId(), GoldenVersion,
                               GoldenScope(), out)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(WriteContentEnvelope(allocator, ManifestView(), crypto::Ed25519SigningKey(),
                               GoldenKeyId(), GoldenVersion, GoldenScope(), out)
              .Error() == ErrorCode::StaleHandle);
    CHECK(out.Size() == 1 && out[0] == 0xab);
    REQUIRE(
        WriteContentEnvelope(allocator, {}, key, GoldenKeyId(), GoldenVersion, GoldenScope(), out));
    CHECK(out.Size() == ContentEnvelopeManifestOffset);
    GoldenTrust golden;
    auto verified = VerifyContentEnvelope(allocator, out.View(), golden.trust);
    REQUIRE(verified);
    CHECK(verified.Value().Size() == 0);
}

// 할당 실패(7절 7): 쓰기(서명 상태·봉투 버퍼)와 검증(검증 상태)의 모든 할당 지점에서 한 번 실패하면
// 그 오류이고 쓰기의 out은 그대로, 검증의 진단은 None이다. 실패가 없으면 golden과 같다.
TEST(ContentSignature_AllocationFailures) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    Array<uint8> golden(system);
    REQUIRE(WriteGolden(system, golden));
    GoldenTrust trust;
    uint32 writeFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        Array<uint8> out(failing);
        failing.FailOnAttempt(attempt);
        const auto written = WriteContentEnvelope(failing, ManifestView(), SeedKey(1),
                                                  GoldenKeyId(), GoldenVersion, GoldenScope(), out);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(written);
            CHECK(out.Size() == golden.Size() &&
                  memcmp(out.Data(), golden.Data(), out.Size()) == 0);
            break;
        }

        CHECK(written.Error() == ErrorCode::OutOfMemory);
        CHECK(out.IsEmpty());
        ++writeFailures;
    }

    uint32 verifyFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        AssetDiagnostic diagnostic;
        const auto verified =
            VerifyContentEnvelope(failing, golden.View(), trust.trust, nullptr, &diagnostic);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(verified);
            break;
        }

        CHECK(verified.Error() == ErrorCode::OutOfMemory);
        CHECK(diagnostic.code == AssetDiagnosticCode::None);
        ++verifyFailures;
    }

    CHECK(writeFailures >= 2);
    CHECK(verifyFailures >= 1);
}

// 신뢰 키 한 줄 텍스트(7절 5): golden 키의 텍스트는 keyId hex ‖ 공백 ‖ 공개키 hex(소문자)이고
// 읽으면 같은 키다(끝의 "\n"·"\r\n" 하나, 대문자 hex도 받는다). 길이·구분자·hex가 아닌 문자·줄바꿈
// 둘·null keyId는 InvalidArgument다.
TEST(ContentSignature_TrustedKeyText) {
    TrustedContentKey key;
    key.id = GoldenKeyId();
    key.key = SeedKey(1).PublicKey();
    char line[TrustedContentKeyTextChars + 1];
    FormatTrustedContentKey(key, line);
    char expected[TrustedContentKeyTextChars + 1];
    snprintf(expected, sizeof(expected), "%s %s", "404142434445464748494a4b4c4d4e4f",
             GoldenPublicHex);
    CHECK(strcmp(line, expected) == 0);
    auto check = [&](StringView text) noexcept {
        auto parsed = ParseTrustedContentKey(text);
        return parsed && parsed.Value().id == key.id &&
               memcmp(parsed.Value().key.bytes, key.key.bytes, crypto::Ed25519PublicKeyBytes) == 0;
    };
    CHECK(check(StringView(line, TrustedContentKeyTextChars)));
    char withLf[TrustedContentKeyTextChars + 2];
    snprintf(withLf, sizeof(withLf), "%s\n", line);
    CHECK(check(withLf));
    char withCrLf[TrustedContentKeyTextChars + 3];
    snprintf(withCrLf, sizeof(withCrLf), "%s\r\n", line);
    CHECK(check(withCrLf));
    char upper[TrustedContentKeyTextChars + 1];
    memcpy(upper, line, sizeof(upper));
    for (char& c : upper) {
        c = c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
    }

    CHECK(check(upper));
    auto invalid = [](StringView text) noexcept {
        return ParseTrustedContentKey(text).Error() == ErrorCode::InvalidArgument;
    };
    CHECK(invalid(""));
    CHECK(invalid("\n"));
    CHECK(invalid(StringView(line, TrustedContentKeyTextChars - 1)));
    char twoLines[TrustedContentKeyTextChars + 3];
    snprintf(twoLines, sizeof(twoLines), "%s\n\n", line);
    CHECK(invalid(twoLines));
    char separator[TrustedContentKeyTextChars + 1];
    memcpy(separator, line, sizeof(separator));
    separator[ContentKeyIdHexChars] = ':';
    CHECK(invalid(separator));
    char notHex[TrustedContentKeyTextChars + 1];
    memcpy(notHex, line, sizeof(notHex));
    notHex[TrustedContentKeyTextChars - 1] = 'g';
    CHECK(invalid(notHex));
    notHex[TrustedContentKeyTextChars - 1] = line[TrustedContentKeyTextChars - 1];
    notHex[0] = 'z';
    CHECK(invalid(notHex));
    TrustedContentKey nil;
    nil.key = key.key;
    char nilLine[TrustedContentKeyTextChars + 1];
    FormatTrustedContentKey(nil, nilLine);
    CHECK(invalid(nilLine));
}

// 칸 이름(7절 2): 봉투 칸마다 고유한 이름이고 칸이 아닌 값(0·끝 다음)은 "None"이다.
TEST(ContentSignature_FieldNames) {
    constexpr uint32 First = static_cast<uint32>(Field::File);
    constexpr uint32 Last = static_cast<uint32>(Field::Signature);
    for (uint32 value = First; value <= Last; ++value) {
        const StringView name(ContentEnvelopeFieldName(static_cast<Field>(value)));
        CHECK(name != "None");
        for (uint32 other = First; other < value; ++other) {
            CHECK(name != StringView(ContentEnvelopeFieldName(static_cast<Field>(other))));
        }
    }

    CHECK(StringView(ContentEnvelopeFieldName(Field::Key)) == "Key");
    CHECK(StringView(ContentEnvelopeFieldName(static_cast<Field>(0))) == "None");
    CHECK(StringView(ContentEnvelopeFieldName(static_cast<Field>(Last + 1))) == "None");
}

} // namespace content_signature_tests_file
