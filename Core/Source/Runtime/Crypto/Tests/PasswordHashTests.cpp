#include <Crypto/Bytes.h>
#include <Crypto/PasswordHash.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace password_hash_tests_file {

using namespace rocks::crypto;

// 알려진 답의 출처: libsodium 1.0.22-stable test/default/pwhash_argon2id.c tv()의 4·6번 항목과 .exp
// 출력이며, argon2-cffi(Argon2 참조 구현, Argon2id v19, lane 1)로 독립 재현했다. 문자열 사례는 같은
// 파일 tv3()다(2·4·5번은 성공, 0·3번은 반복 0회라 실패).
namespace {

struct HexBytes {
    uint8 bytes[256];
    usize size = 0;

    explicit HexBytes(const char* hex) {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : MaxSize;
    }

    Span<const uint8> View() const {
        return Span<const uint8>(bytes, size);
    }
};

Span<const uint8> Text(const char* text) {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

// 시험용 작은 비용(빠르게 끝난다). 실제 저장에는 프리셋을 쓴다.
constexpr PasswordCost FastCost{1, 64 * 1024};

} // namespace

TEST(CryptoPasswordArgon2idKnownAnswers) {
    struct Case {
        const char* password;
        const char* salt;
        PasswordCost cost;
        const char* output;
    };

    const Case cases[] = {
        {"4a857e2ee8aa9b6056f2424e84d24a72473378906ee04a46cb05311502d5250b82ad86b83c8f20a23dbb74f6d"
         "a60b0b6ecffd67134d459"
         "46ac8ebfb3064294bc097d43ced68642bfb8bbbdd0f50b30118f5e",
         "39d82eef32010b8b79cc5ba88ed539fb",
         {3, 1432947},
         "08d8cd330c57e1b4643241d05bb468ba4ee4e932cd0858816be9ef15360b27bbd06a87130ee92222be267a29b"
         "81f5ae8fe8613324cfc48"
         "32dc49387fd0602f1c57b4d0f3855db94fb7e12eb05f9a484aed4a4307abf586cd3d55c809bc081541e00b682"
         "772fb2066504ff935b8eb"
         "c551a2083882f874bc0fae68e56848ae34c91097c3bf0cca8e75c0797eef3efde3f75e005815018db3cf7c109"
         "a812264c4de69dcb22322"
         "dbbcfa447f5b00ecd1b04a7be1569c8e556adb7bba48adf81d"},
        {"b540beb016a5366524d4605156493f9874514a5aa58818cd0c6dfffaa9e90205f17b",
         "44071f6d181561670bda728d43fb79b4",
         {1, 1631659},
         "7fb72409b0987f8190c3729710e98c3f80c5a8727d425fdcde7f3644d467fe973f5b5fee683bd3fce812cb9ae"
         "5e9921a2d06c2f1905e4e"
         "839692f2b934b682f11a2fe2b90482ea5dd234863516dba6f52dc0702d324ec77d860c2e181f84472bd7104fe"
         "dce071ffa93c5309494ad"
         "51623d214447a7b2b1462dc7d5d55a1f6fd5b54ce024118d86f0c6489d16545aaa87b6689dad9f2fb47fda989"
         "4f8e12b87d978b483ccd4"
         "cc5fd9595cdc7a818452f915ce2f7df95ec12b1c72e3788d473441d884f9748eb14703c21b45d82fd667b85f5"
         "b2d98c13303b3fe762855"
         "31a826b6fc0fe8e3dddecf"},
    };
    for (const Case& item : cases) {
        const HexBytes password(item.password);
        const HexBytes salt(item.salt);
        const HexBytes output(item.output);
        REQUIRE(password.size != MaxSize && salt.size == PasswordSaltBytes &&
                output.size != MaxSize);
        auto derived =
            TryDerivePasswordSecret(password.View(), salt.View(), item.cost, output.size);
        REQUIRE(derived);
        CHECK(ConstantTimeEquals(derived.Value().View(), output.View()));
    }
}

TEST(CryptoPasswordStringInterop) {
    // upstream tv3: 2번(빈 비밀번호), 4번(반복 19회), 5번(lane 3)은 확인되고, 0번(반복 0회)은
    // 실패한다.
    CHECK(TryVerifyPassword(StringView("$argon2id$v=19$m=4882,t=2,p=1$bA81arsiXysd3WbTRzmEOw$"
                                       "Nm8QBM+7RH1DXo9rvp5cwKEOOOfD2g6JuxlXihoNcpE"),
                            Span<const uint8>()));
    CHECK(TryVerifyPassword(
        StringView("$argon2id$v=19$m=4096,t=19,p=1$PkEgMTYtYnl0ZXMgc2FsdA$ltB/"
                   "ue1kPtBMBGfsysMpPigE6hiNEKZ9vs8vLNVDQGA"),
        Text("^T5H$JYt39n%K*j:W]!1s?vg!:jGi]Ax?..l7[p0v:1jHTpla9;]bUN;?bWyCbtqg ")));
    CHECK(TryVerifyPassword(StringView("$argon2id$v=19$m=4096,t=1,p=3$PkEgcHJldHR5IGxvbmcgc2FsdA$"
                                       "HUqx5Z1b/ZypnUrvvJ5UC2Q+T6Q1WwASK/Kr9dRbGA0"),
                            Text("K3S=KyH#)36_?]LxeR8QNKw6X=gFbxai$C%29V*")));
    CHECK(TryVerifyPassword(StringView("$argon2id$v=19$m=4096,t=0,p=1$X1NhbHQAAAAAAAAAAAAAAA$bWh++"
                                       "MKN1OiFHKgIWTLvIi1iHicmHH7+Fv3K88ifFfI"),
                            Span<const uint8>())
              .Error() == ErrorCode::AuthenticationFailed);
}

TEST(CryptoPasswordHashAndVerify) {
    char stored[PasswordHashStringBytes];
    auto written =
        TryHashPassword(Text("correct horse battery staple"), FastCost, Span<char>(stored));
    REQUIRE(written);
    CHECK(written.Value() == strlen(stored));
    CHECK(strncmp(stored, "$argon2id$v=19$m=64,t=1,p=1$", 28) == 0);

    const StringView encoded(stored, written.Value());
    CHECK(TryVerifyPassword(encoded, Text("correct horse battery staple")));
    CHECK(TryVerifyPassword(encoded, Text("correct horse battery stapler")).Error() ==
          ErrorCode::AuthenticationFailed);

    // 같은 비밀번호도 salt가 달라 문자열이 다르다.
    char again[PasswordHashStringBytes];
    REQUIRE(TryHashPassword(Text("correct horse battery staple"), FastCost, Span<char>(again)));
    CHECK(strcmp(stored, again) != 0);

    // 비용이 바뀌면 다시 해시할 때다.
    CHECK(TryPasswordNeedsRehash(encoded, FastCost).Value() == false);
    CHECK(TryPasswordNeedsRehash(encoded, PasswordCost{2, 64 * 1024}).Value() == true);
    CHECK(TryPasswordNeedsRehash(encoded, PasswordCost{1, 128 * 1024}).Value() == true);

    // 변조한 문자열은 AuthenticationFailed다(hash 끝 문자 바꾸기, 잘라 내기).
    char tampered[PasswordHashStringBytes];
    memcpy(tampered, stored, sizeof(stored));
    const usize middle = written.Value() - 10;
    tampered[middle] = tampered[middle] == 'A' ? 'B' : 'A';
    CHECK(TryVerifyPassword(StringView(tampered, written.Value()),
                            Text("correct horse battery staple"))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyPassword(StringView(stored, 30), Text("correct horse battery staple")).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoPasswordRejectsUntrustedParameters) {
    // 상한을 넘는 비용(메모리 2 GiB, 반복 33회, 반복 × 메모리 5 GiB·pass, 10 자리 4 TiB)은 계산하지
    // 않고 CapacityExceeded다.
    const char* overCap[] = {
        "$argon2id$v=19$m=2097152,t=2,p=1$bA81arsiXysd3WbTRzmEOw$Nm8QBM+"
        "7RH1DXo9rvp5cwKEOOOfD2g6JuxlXihoNcpE",
        "$argon2id$v=19$m=4096,t=33,p=1$bA81arsiXysd3WbTRzmEOw$Nm8QBM+"
        "7RH1DXo9rvp5cwKEOOOfD2g6JuxlXihoNcpE",
        "$argon2id$v=19$m=1048576,t=5,p=1$bA81arsiXysd3WbTRzmEOw$Nm8QBM+"
        "7RH1DXo9rvp5cwKEOOOfD2g6JuxlXihoNcpE",
        "$argon2id$v=19$m=4294967296,t=1,p=1$bA81arsiXysd3WbTRzmEOw$Nm8QBM+"
        "7RH1DXo9rvp5cwKEOOOfD2g6JuxlXihoNcpE",
    };
    for (const char* text : overCap) {
        CHECK(TryVerifyPassword(StringView(text), Span<const uint8>()).Error() ==
              ErrorCode::CapacityExceeded);
    }

    // 11 자리 숫자, 선행 0, 문자열 안의 NUL, "$"로 시작하지 않는 문자열은 형식 오류다.
    CHECK(TryVerifyPassword(StringView("$argon2id$v=19$m=99999999999,t=2,p=1$bA81$Nm8Q"),
                            Span<const uint8>())
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyPassword(
              StringView("$argon2id$v=19$m=04882,t=2,p=1$bA81arsiXysd3WbTRzmEOw$Nm8QBM+7RH1DXo9rvp5"
                         "cwKEOOOfD2g6JuxlXihoNcpE"),
              Span<const uint8>())
              .Error() == ErrorCode::AuthenticationFailed);
    const char withNul[] =
        "$argon2id$v=19$m=4882,t=2,p=1$bA81arsiXy\0d3WbTRzmEOw$Nm8QBM+7RH1DXo9rvp5cwKEOOOfD2g6J";
    CHECK(
        TryVerifyPassword(StringView(withNul, sizeof(withNul) - 1), Span<const uint8>()).Error() ==
        ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyPassword(StringView("argon2id$v=19$m=4096,t=3,p=1$c29tZQ$aGFzaA"), Text("x"))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyPassword(StringView("$argon2id$v=19$m=4096,t=,p=1$c29tZQ$aGFzaA"), Text("x"))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyPassword(StringView(""), Text("x")).Error() == ErrorCode::AuthenticationFailed);

    // "$<id>$" 꼴의 다른 형식은 Unsupported다: argon2i, v=16, v 없는 Argon2 1.0, scrypt.
    const char* unsupported[] = {
        "$argon2i$v=19$m=4096,t=3,p=1$c29tZXNhbHQ$aGFzaA",
        "$argon2id$v=16$m=4096,t=3,p=1$c29tZXNhbHQ$aGFzaA",
        "$argon2id$m=4096,t=3,p=1$c29tZXNhbHQ$aGFzaA",
        "$scrypt$ln=14",
    };
    for (const char* text : unsupported) {
        CHECK(TryVerifyPassword(StringView(text), Text("x")).Error() == ErrorCode::Unsupported);
    }

    // 길이 경계: 127자는 libsodium까지 가서 형식 오류, 128자는 libsodium 형식보다 길어
    // Unsupported다.
    char longText[128];
    memset(longText, 'a', sizeof(longText));
    memcpy(longText, "$argon2id$v=19$m=4096,t=3,p=1$", 30);
    CHECK(TryVerifyPassword(StringView(longText, 127), Text("x")).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyPassword(StringView(longText, 128), Text("x")).Error() ==
          ErrorCode::Unsupported);
    CHECK(TryPasswordNeedsRehash(StringView("$argon2i$v=19$m=4096,t=3,p=1$c29tZXNhbHQ$aGFzaA"),
                                 FastCost)
              .Error() == ErrorCode::Unsupported);
}

TEST(CryptoPasswordArgumentContract) {
    const uint8 salt[PasswordSaltBytes] = {1, 2, 3};
    char text[PasswordHashStringBytes];

    // 비용 범위: 0회·8 KiB 미만은 InvalidArgument, 상한 초과는 CapacityExceeded다.
    CHECK(
        TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt), PasswordCost{0, 64 * 1024}, 32)
            .Error() == ErrorCode::InvalidArgument);
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt), PasswordCost{1, 4096}, 32)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt), PasswordCost{33, 64 * 1024},
                                  32)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt),
                                  PasswordCost{1, (usize{1} << 30) + 1024}, 32)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt),
                                  PasswordCost{5, usize{1} << 30}, 32)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(TryPasswordNeedsRehash(StringView("$argon2id$v=19$m=64,t=1,p=1$c29tZQ$aGFzaA"),
                                 PasswordCost{0, 65536})
              .Error() == ErrorCode::InvalidArgument);

    // 빈 비밀번호도 파생한다(null이 아닌 주소로 넘긴다).
    CHECK(TryDerivePasswordSecret(Span<const uint8>(), Span<const uint8>(salt), FastCost, 32));

    // salt 16 byte, 출력 16–4096 byte.
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt).Left(15), FastCost, 32)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt), FastCost, 15).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt), FastCost, 4097).Error() ==
          ErrorCode::InvalidArgument);

    // 출력 버퍼 부족, 비밀번호와 겹친 출력.
    CHECK(TryHashPassword(Text("pw"), FastCost, Span<char>(text).Left(127)).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(TryHashPassword(AsBytes(Span<const char>(text, 8)), FastCost, Span<char>(text)).Error() ==
          ErrorCode::InvalidArgument);

    // 비밀번호에서 KdfKey: 결정적이고, 같은 결과가 TryDerivePasswordSecret(32)와 같다.
    auto key = TryDeriveKdfKeyFromPassword(Text("pw"), Span<const uint8>(salt), FastCost);
    auto secret =
        TryDerivePasswordSecret(Text("pw"), Span<const uint8>(salt), FastCost, KdfKeyBytes);
    REQUIRE(key && secret);
    uint8 exported[KdfKeyBytes];
    REQUIRE(hazmat::TryExportKey(key.Value(), Span<uint8>(exported)));
    CHECK(ConstantTimeEquals(Span<const uint8>(exported), secret.Value().View()));
}
} // namespace password_hash_tests_file
