#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Crypto/Aead.h>
#include <Crypto/AesModes.h>
#include <Crypto/Bytes.h>
#include <TestSupport/Test.h>

#include <stdio.h>
#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 저수준 AES 모드(Crypto/AesModes.h)의 공개 계약 시험이다(Docs/Design/Crypto.ko.md 9.1·9.3·12절).
// - CTR: NIST SP 800-38A 부록 F.5.1(AES-128)·F.5.5(AES-256) 알려진 답. Python cryptography
//   50.0.2와 같은 값을 냄을 확인했다(2026-11-15). 카운터 폭 경계·용량은 AesLayerTests(스칼라
//   참조)가 맡는다.
// - GCM 멀티파트: 일회성 hazmat::TrySealDetached·TryOpenDetached(AES-128은 경량 계층 일회성,
// AES-256은
//   libsodium)와 무작위 조각 나누기로 대조한다. 단계·sticky·이동·Reset 계약을 확인한다.
// - AES HW가 없으면 모든 생성이 Unsupported인지 확인하고 나머지는 건너뛴다고 출력한다.
namespace {

struct TestRandom {
    uint64 state;

    uint64 Next() noexcept {
        state += 0x9e3779b97f4a7c15ull;
        uint64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    usize Below(usize bound) noexcept {
        return static_cast<usize>(Next() % bound);
    }

    void Fill(Span<uint8> bytes) noexcept {
        for (uint8& byte : bytes) {
            byte = static_cast<uint8>(Next());
        }
    }
};

struct HexBytes {
    uint8 bytes[128];
    usize size = 0;

    explicit HexBytes(const char* hex) noexcept {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : MaxSize;
    }

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes, size);
    }
};

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

bool AesAvailable() noexcept {
    return TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available;
}

// SP 800-38A 부록 F.5: 평문 4블록, 초기 카운터 f0f1...ff(전체 블록 증가 = Bits128).
constexpr const char* CtrPlaintext =
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
constexpr const char* CtrCounter = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

struct CtrCase {
    const char* key;
    const char* ciphertext;
};

constexpr CtrCase CtrCases[] = {
    // F.5.1 CTR-AES128.Encrypt
    {"2b7e151628aed2a6abf7158809cf4f3c",
     "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
     "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee"},
    // F.5.5 CTR-AES256.Encrypt
    {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
     "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
     "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"},
};

// GCM 멀티파트로 봉인한다. AD와 평문을 무작위 조각(빈 조각 포함)으로 넣는다. inPlace면 output에
// 평문을 먼저 복사하고 제자리로 처리한다.
bool SealMultipart(TestRandom& random, const AeadKey& key, Span<const uint8> nonce,
                   Span<const uint8> associatedData, Span<const uint8> plaintext,
                   Span<uint8> output, Span<uint8> tag, bool inPlace) noexcept {
    auto encryptor = hazmat::AesGcmEncryptor::TryCreate(key, nonce);
    if (!encryptor) {
        return false;
    }

    usize offset = 0;
    while (offset < associatedData.Size()) {
        const usize piece = random.Below(associatedData.Size() - offset + 1);
        if (!encryptor.Value().TryUpdateAssociatedData(associatedData.Mid(offset, piece))) {
            return false;
        }

        offset += piece;
    }

    if (inPlace) {
        memcpy(output.Data(), plaintext.Data(), plaintext.Size());
    }

    offset = 0;
    usize steps = 0;
    while (offset < plaintext.Size() || steps == 0) {
        const usize left = plaintext.Size() - offset;
        // 작은 조각(블록 경계 근처)과 큰 조각을 섞는다.
        const usize piece =
            random.Below(4) == 0 ? random.Below(left + 1) : random.Below(left < 40 ? left + 1 : 40);
        const Span<const uint8> input =
            inPlace ? Span<const uint8>(output.Mid(offset, piece)) : plaintext.Mid(offset, piece);
        auto written = encryptor.Value().TryUpdate(input, output.RightChop(offset));
        if (!written || written.Value() != piece) {
            return false;
        }

        offset += piece;
        ++steps;
    }

    return static_cast<bool>(encryptor.Value().TryFinish(tag));
}

// GCM 멀티파트로 연다(검증 전 평문을 output에 쓴다). 성공하면 true다.
Result<void> OpenMultipart(TestRandom& random, const AeadKey& key, Span<const uint8> nonce,
                           Span<const uint8> associatedData, Span<const uint8> ciphertext,
                           Span<const uint8> tag, Span<uint8> output) noexcept {
    auto decryptor = hazmat::AesGcmDecryptor::TryCreate(key, nonce);
    if (!decryptor) {
        return Result<void>::Failure(decryptor.Error());
    }

    const usize split = random.Below(associatedData.Size() + 1);
    if (!decryptor.Value().TryUpdateAssociatedData(associatedData.Left(split)) ||
        !decryptor.Value().TryUpdateAssociatedData(associatedData.RightChop(split))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize offset = 0;
    while (offset < ciphertext.Size()) {
        const usize piece = 1 + random.Below(ciphertext.Size() - offset);
        auto written = decryptor.Value().TryUpdateUnverified(ciphertext.Mid(offset, piece),
                                                             output.RightChop(offset));
        if (!written || written.Value() != piece) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        offset += piece;
    }

    return decryptor.Value().TryFinishVerify(tag);
}

} // namespace

TEST(CryptoAesCtrKnownAnswers) {
    const HexBytes plaintext(CtrPlaintext);
    const HexBytes counter(CtrCounter);
    for (const CtrCase& item : CtrCases) {
        const HexBytes keyBytes(item.key);
        const HexBytes expected(item.ciphertext);
        auto key = hazmat::AesKey::TryImport(keyBytes.View());
        if (!AesAvailable()) {
            CHECK(key.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(key);
        CHECK(key.Value().KeyBytes() == keyBytes.size);

        // 일회성(기본 폭 Bits128). 복호(F.5.2·F.5.6)는 같은 연산이다.
        uint8 output[64];
        REQUIRE(hazmat::TryAesCtrXor(key.Value(), counter.View(), plaintext.View(),
                                     Span<uint8>(output)));
        CHECK(Same(Span<const uint8>(output), expected.View()));
        REQUIRE(hazmat::TryAesCtrXor(key.Value(), counter.View(), Span<const uint8>(output),
                                     Span<uint8>(output)));
        CHECK(Same(Span<const uint8>(output), plaintext.View()));

        // 스트림: 1·15·17·31 byte 조각이 이어지는 keystream을 쓴다(제자리).
        auto ctr = hazmat::AesCtr::TryCreate(key.Value(), counter.View());
        REQUIRE(ctr);
        memcpy(output, plaintext.bytes, sizeof(output));
        const usize pieces[] = {1, 15, 17, 31};
        usize offset = 0;
        for (usize piece : pieces) {
            const Span<uint8> chunk = Span<uint8>(output).Mid(offset, piece);
            REQUIRE(ctr.Value().TryProcess(chunk, chunk));
            offset += piece;
        }

        CHECK(offset == sizeof(output));
        CHECK(Same(Span<const uint8>(output), expected.View()));
    }

    if (!AesAvailable()) {
        printf("  CryptoAesCtrKnownAnswers: AES is unavailable on this CPU (Unsupported path)\n");
    }
}

TEST(CryptoAesKeyAndCtrContract) {
    uint8 bytes[64] = {};
    // 길이 검사가 HW 판정보다 먼저다(AES-192·다른 길이는 기기와 관계없이 InvalidArgument).
    CHECK(hazmat::AesKey::TryImport(Span<const uint8>(bytes).Left(24)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(hazmat::AesKey::TryImport(Span<const uint8>()).Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::AesKey::TryGenerate(24).Error() == ErrorCode::InvalidArgument);
    if (!AesAvailable()) {
        CHECK(hazmat::AesKey::TryImport(Span<const uint8>(bytes).Left(16)).Error() ==
              ErrorCode::Unsupported);
        CHECK(hazmat::AesKey::TryGenerate(32).Error() == ErrorCode::Unsupported);
        printf("  CryptoAesKeyAndCtrContract: AES is unavailable on this CPU (Unsupported path)\n");
        return;
    }

    auto generated = hazmat::AesKey::TryGenerate(32);
    REQUIRE(generated);
    CHECK(generated.Value().KeyBytes() == 32 && !generated.Value().IsEmpty());

    // 이동하면 원래 키는 빈 키다.
    hazmat::AesKey key = Move(generated.Value());
    CHECK(generated.Value().IsEmpty() && generated.Value().KeyBytes() == 0);
    const uint8 counter[16] = {};
    CHECK(hazmat::AesCtr::TryCreate(generated.Value(), Span<const uint8>(counter)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(hazmat::TryAesCtrXor(generated.Value(), Span<const uint8>(counter),
                               Span<const uint8>(bytes), Span<uint8>(bytes))
              .Error() == ErrorCode::StaleHandle);

    // 카운터 블록 길이·폭·출력 길이·겹침은 InvalidArgument다.
    CHECK(hazmat::AesCtr::TryCreate(key, Span<const uint8>(counter).Left(15)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(hazmat::AesCtr::TryCreate(key, Span<const uint8>(bytes).Left(17)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(hazmat::AesCtr::TryCreate(key, Span<const uint8>(counter),
                                    static_cast<hazmat::AesCtrCounterWidth>(16))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryAesCtrXor(key, Span<const uint8>(counter), Span<const uint8>(bytes).Left(8),
                               Span<uint8>(bytes).Mid(8, 9))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryAesCtrXor(key, Span<const uint8>(counter), Span<const uint8>(bytes).Left(8),
                               Span<uint8>(bytes).Mid(1, 8))
              .Error() == ErrorCode::InvalidArgument);

    auto ctr = hazmat::AesCtr::TryCreate(key, Span<const uint8>(counter),
                                         hazmat::AesCtrCounterWidth::Bits32);
    REQUIRE(ctr);
    uint8 shared[48] = {};
    CHECK(ctr.Value()
              .TryProcess(Span<const uint8>(shared).Left(16), Span<uint8>(shared).Mid(1, 16))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ctr.Value()
              .TryProcess(Span<const uint8>(shared).Left(16), Span<uint8>(shared).Mid(16, 15))
              .Error() == ErrorCode::InvalidArgument);

    // 실패한 호출은 상태를 바꾸지 않는다: 이어서 처리한 결과가 일회성과 같다.
    uint8 expected[32];
    REQUIRE(hazmat::TryAesCtrXor(key, Span<const uint8>(counter),
                                 Span<const uint8>(shared).Left(32), Span<uint8>(expected),
                                 hazmat::AesCtrCounterWidth::Bits32));
    REQUIRE(
        ctr.Value().TryProcess(Span<const uint8>(shared).Left(32), Span<uint8>(shared).Left(32)));
    CHECK(Same(Span<const uint8>(shared).Left(32), Span<const uint8>(expected)));

    // 키를 버려도 스트림은 스케줄 사본으로 이어 간다. 이동된 스트림과 Reset 뒤는 StaleHandle이다.
    key = hazmat::AesKey();
    hazmat::AesCtr moved = Move(ctr.Value());
    CHECK(ctr.Value().IsEmpty());
    CHECK(ctr.Value()
              .TryProcess(Span<const uint8>(shared).Left(1), Span<uint8>(shared).Left(1))
              .Error() == ErrorCode::StaleHandle);
    CHECK(moved.TryProcess(Span<const uint8>(shared).Left(16), Span<uint8>(shared).Left(16)));
    moved.Reset();
    CHECK(moved.IsEmpty());
    CHECK(
        moved.TryProcess(Span<const uint8>(shared).Left(1), Span<uint8>(shared).Left(1)).Error() ==
        ErrorCode::StaleHandle);
}

TEST(CryptoAesGcmMultipartMatchesOneShot) {
    if (!AesAvailable()) {
        auto key = AeadKey::TryGenerate(AeadAlgorithm::Aes128Gcm);
        CHECK(key.Error() == ErrorCode::Unsupported);
        printf("  CryptoAesGcmMultipartMatchesOneShot: AES is unavailable on this CPU\n");
        return;
    }

    TestRandom random{test::CurrentSeed() ^ 0x6c3a17ull};
    SystemAllocator allocator;
    constexpr usize MaxMessage = 9000;
    Array<uint8> plaintext(allocator);
    Array<uint8> expected(allocator);
    Array<uint8> actual(allocator);
    Array<uint8> opened(allocator);
    Array<uint8> associated(allocator);
    REQUIRE(plaintext.TryResize(MaxMessage));
    REQUIRE(expected.TryResize(MaxMessage));
    REQUIRE(actual.TryResize(MaxMessage));
    REQUIRE(opened.TryResize(MaxMessage));
    REQUIRE(associated.TryResize(300));
    const usize specialLengths[] = {0, 1, 15, 16, 17, 4031, 4032, 4033, 8064, MaxMessage};
    constexpr usize SpecialCount = sizeof(specialLengths) / sizeof(specialLengths[0]);
    for (usize iteration = 0; iteration < 2 * SpecialCount + 40; ++iteration) {
        const AeadAlgorithm algorithm =
            iteration % 2 == 0 ? AeadAlgorithm::Aes128Gcm : AeadAlgorithm::Aes256Gcm;
        const usize keyBytes = algorithm == AeadAlgorithm::Aes128Gcm ? 16 : 32;
        uint8 keyData[32];
        uint8 nonce[12];
        random.Fill(Span<uint8>(keyData));
        random.Fill(Span<uint8>(nonce));
        auto key = AeadKey::TryImport(algorithm, Span<const uint8>(keyData).Left(keyBytes));
        REQUIRE(key);
        const usize length = iteration < 2 * SpecialCount ? specialLengths[iteration / 2]
                                                          : random.Below(MaxMessage + 1);
        const Span<uint8> message = plaintext.View().Left(length);
        const Span<uint8> ad = associated.View().Left(random.Below(301));
        random.Fill(message);
        random.Fill(ad);

        uint8 expectedTag[16];
        REQUIRE(hazmat::TrySealDetached(key.Value(), Span<const uint8>(nonce), ad, message,
                                        expected.View().Left(length), Span<uint8>(expectedTag)));
        uint8 tag[16];
        const bool inPlace = random.Below(2) == 0;
        REQUIRE(SealMultipart(random, key.Value(), Span<const uint8>(nonce), ad, message,
                              actual.View().Left(length), Span<uint8>(tag), inPlace));
        CHECK(Same(actual.View().Left(length), expected.View().Left(length)));
        CHECK(Same(Span<const uint8>(tag), Span<const uint8>(expectedTag)));

        // 멀티파트 복호: 맞는 tag는 성공하고 평문이 같다. 변조한 tag는 AuthenticationFailed다.
        CHECK(OpenMultipart(random, key.Value(), Span<const uint8>(nonce), ad,
                            expected.View().Left(length), Span<const uint8>(tag),
                            opened.View().Left(length)));
        CHECK(Same(opened.View().Left(length), message));
        tag[random.Below(16)] ^= 0x04;
        CHECK(OpenMultipart(random, key.Value(), Span<const uint8>(nonce), ad,
                            expected.View().Left(length), Span<const uint8>(tag),
                            opened.View().Left(length))
                  .Error() == ErrorCode::AuthenticationFailed);
    }
}

TEST(CryptoAesGcmMultipartContract) {
    if (!AesAvailable()) {
        printf("  CryptoAesGcmMultipartContract: AES is unavailable on this CPU\n");
        return;
    }

    auto keyResult = AeadKey::TryGenerate(AeadAlgorithm::Aes128Gcm);
    REQUIRE(keyResult);
    AeadKey key = Move(keyResult.Value());
    const uint8 nonce[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const Span<const uint8> nonce12 = Span<const uint8>(nonce).Left(12);

    // 생성 오류: 빈 키 StaleHandle, AES-GCM이 아닌 키 InvalidArgument, 96 bit가 아닌 nonce
    // Unsupported.
    CHECK(hazmat::AesGcmEncryptor::TryCreate(AeadKey(), nonce12).Error() == ErrorCode::StaleHandle);
    auto chacha = AeadKey::TryGenerate(AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(chacha);
    CHECK(hazmat::AesGcmDecryptor::TryCreate(chacha.Value(), nonce12).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(hazmat::AesGcmEncryptor::TryCreate(key, Span<const uint8>(nonce).Left(11)).Error() ==
          ErrorCode::Unsupported);
    CHECK(hazmat::AesGcmDecryptor::TryCreate(key, Span<const uint8>(nonce)).Error() ==
          ErrorCode::Unsupported);

    // 일회성 기준값.
    const uint8 ad[5] = {'h', 'e', 'a', 'd', 'r'};
    uint8 message[40];
    for (usize i = 0; i < sizeof(message); ++i) {
        message[i] = static_cast<uint8>(i * 3 + 1);
    }

    uint8 expected[40];
    uint8 expectedTag[16];
    REQUIRE(hazmat::TrySealDetached(key, nonce12, Span<const uint8>(ad), Span<const uint8>(message),
                                    Span<uint8>(expected), Span<uint8>(expectedTag)));

    auto encryptor = hazmat::AesGcmEncryptor::TryCreate(key, nonce12);
    REQUIRE(encryptor);
    // 키를 버려도 상태는 스케줄 사본으로 이어 간다.
    key = AeadKey();
    REQUIRE(encryptor.Value().TryUpdateAssociatedData(Span<const uint8>(ad).Left(2)));
    REQUIRE(encryptor.Value().TryUpdateAssociatedData(Span<const uint8>(ad).RightChop(2)));
    uint8 output[64] = {};
    // 출력 부족은 CapacityExceeded, 제자리가 아닌 겹침은 InvalidArgument이고 상태는 그대로다.
    CHECK(encryptor.Value()
              .TryUpdate(Span<const uint8>(message).Left(20), Span<uint8>(output).Left(19))
              .Error() == ErrorCode::CapacityExceeded);
    memcpy(output, message, sizeof(message));
    CHECK(encryptor.Value()
              .TryUpdate(Span<const uint8>(output).Left(20), Span<uint8>(output).Mid(1, 20))
              .Error() == ErrorCode::InvalidArgument);
    // 한도: 평문 합이 상한을 넘는 길이는 읽기 전에 거부한다(제자리 view의 길이만 크다).
    CHECK(encryptor.Value()
              .TryUpdate(Span<const uint8>(output, hazmat::AesGcmMaxMessageBytes + 1),
                         Span<uint8>(output, hazmat::AesGcmMaxMessageBytes + 1))
              .Error() == ErrorCode::CapacityExceeded);
    // 제자리로 앞 20 byte, 따로 뒤 20 byte(출력이 더 커도 된다).
    auto first = encryptor.Value().TryUpdate(Span<const uint8>(output).Left(20),
                                             Span<uint8>(output).Left(20));
    REQUIRE(first && first.Value() == 20);
    uint8 second[32] = {};
    auto rest =
        encryptor.Value().TryUpdate(Span<const uint8>(message).Mid(20, 20), Span<uint8>(second));
    REQUIRE(rest && rest.Value() == 20);
    // 데이터 뒤의 AD는 InvalidArgument다(상태 그대로).
    CHECK(encryptor.Value().TryUpdateAssociatedData(Span<const uint8>(ad)).Error() ==
          ErrorCode::InvalidArgument);
    uint8 tag[17] = {};
    CHECK(encryptor.Value().TryFinish(Span<uint8>(tag).Left(15)).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(encryptor.Value().TryFinish(Span<uint8>(tag).Left(16)));
    CHECK(Same(Span<const uint8>(output).Left(20), Span<const uint8>(expected).Left(20)));
    CHECK(Same(Span<const uint8>(second).Left(20), Span<const uint8>(expected).Mid(20, 20)));
    CHECK(Same(Span<const uint8>(tag).Left(16), Span<const uint8>(expectedTag)));
    // 끝난 뒤의 호출은 InvalidArgument다.
    CHECK(encryptor.Value().TryFinish(Span<uint8>(tag).Left(16)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(encryptor.Value()
              .TryUpdate(Span<const uint8>(message).Left(1), Span<uint8>(output).Left(1))
              .Error() == ErrorCode::InvalidArgument);

    // 이동·Reset 뒤는 StaleHandle이다.
    hazmat::AesGcmEncryptor moved = Move(encryptor.Value());
    CHECK(encryptor.Value().IsEmpty() && !moved.IsEmpty());
    CHECK(encryptor.Value().TryUpdateAssociatedData(Span<const uint8>(ad)).Error() ==
          ErrorCode::StaleHandle);
    moved.Reset();
    CHECK(moved.IsEmpty());
    CHECK(moved.TryFinish(Span<uint8>(tag).Left(16)).Error() == ErrorCode::StaleHandle);

    // 복호: 잘린 tag·틀린 tag는 AuthenticationFailed이고 sticky다(Reset 전까지).
    auto restored = AeadKey::TryGenerate(AeadAlgorithm::Aes256Gcm);
    REQUIRE(restored);
    uint8 tag256[16];
    REQUIRE(hazmat::TrySealDetached(restored.Value(), nonce12, Span<const uint8>(ad),
                                    Span<const uint8>(message), Span<uint8>(expected),
                                    Span<uint8>(tag256)));
    auto decryptor = hazmat::AesGcmDecryptor::TryCreate(restored.Value(), nonce12);
    REQUIRE(decryptor);
    REQUIRE(decryptor.Value().TryUpdateAssociatedData(Span<const uint8>(ad)));
    REQUIRE(decryptor.Value().TryUpdateUnverified(Span<const uint8>(expected),
                                                  Span<uint8>(output).Left(40)));
    CHECK(Same(Span<const uint8>(output).Left(40), Span<const uint8>(message)));
    CHECK(decryptor.Value().TryFinishVerify(Span<const uint8>(tag256).Left(15)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(decryptor.Value().HasFailed());
    CHECK(decryptor.Value().TryFinishVerify(Span<const uint8>(tag256)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(decryptor.Value().TryUpdateAssociatedData(Span<const uint8>()).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(decryptor.Value()
              .TryUpdateUnverified(Span<const uint8>(expected).Left(1), Span<uint8>(output).Left(1))
              .Error() == ErrorCode::AuthenticationFailed);
    decryptor.Value().Reset();
    CHECK(decryptor.Value().IsEmpty() && !decryptor.Value().HasFailed());
    CHECK(decryptor.Value().TryFinishVerify(Span<const uint8>(tag256)).Error() ==
          ErrorCode::StaleHandle);

    // 성공한 검증 뒤의 호출은 InvalidArgument다. AD 한도도 읽기 전에 거부한다.
    auto verified = hazmat::AesGcmDecryptor::TryCreate(restored.Value(), nonce12);
    REQUIRE(verified);
    CHECK(verified.Value()
              .TryUpdateAssociatedData(Span<const uint8>(ad, hazmat::AesGcmMaxAssociatedBytes + 1))
              .Error() == ErrorCode::CapacityExceeded);
    REQUIRE(verified.Value().TryUpdateAssociatedData(Span<const uint8>(ad)));
    REQUIRE(verified.Value().TryUpdateUnverified(Span<const uint8>(expected),
                                                 Span<uint8>(output).Left(40)));
    REQUIRE(verified.Value().TryFinishVerify(Span<const uint8>(tag256)));
    CHECK(!verified.Value().HasFailed());
    CHECK(verified.Value().TryFinishVerify(Span<const uint8>(tag256)).Error() ==
          ErrorCode::InvalidArgument);
}

// 일회성 AES-128-GCM도 GCM AD 상한을 넘는 AD를 Fatal 없이 오류로 돌려준다(경량 계층은 이 길이를
// 계약 위반으로 보므로 어댑터가 먼저 거른다). 길이만 큰 view라 실제 메모리를 읽기 전에 거부되어야
// 한다. 이 view는 주소 공간 대부분을 덮어 출력 버퍼와도 겹칠 수 있으므로, 여는 쪽은 문서의 판정
// 순서대로 겹침(InvalidArgument, 출력 불변)이 먼저 걸리거나 AuthenticationFailed(출력 소거)다.
TEST(CryptoAesGcmOneShotAssociatedDataLimit) {
    if (!AesAvailable()) {
        printf("  CryptoAesGcmOneShotAssociatedDataLimit: AES is unavailable on this CPU\n");
        return;
    }

    auto key = AeadKey::TryGenerate(AeadAlgorithm::Aes128Gcm);
    REQUIRE(key);
    const uint8 nonce[12] = {};
    const uint8 ad = 0;
    const Span<const uint8> huge(&ad, hazmat::AesGcmMaxAssociatedBytes + 1);
    uint8 message[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8 sealed[sizeof(message) + 16] = {};
    uint8 tag[16] = {};
    CHECK(hazmat::TrySealWithNonce(key.Value(), Span<const uint8>(nonce), huge,
                                   Span<const uint8>(message), Span<uint8>(sealed))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(hazmat::TrySealDetached(key.Value(), Span<const uint8>(nonce), huge,
                                  Span<const uint8>(message), Span<uint8>(sealed).Left(8),
                                  Span<uint8>(tag))
              .Error() == ErrorCode::CapacityExceeded);
    const uint8 untouched[sizeof(message)] = {9, 9, 9, 9, 9, 9, 9, 9};
    const uint8 zero[sizeof(message)] = {};
    auto rejected = [&](ErrorCode error, Span<const uint8> opened) {
        return (error == ErrorCode::InvalidArgument &&
                Same(opened, Span<const uint8>(untouched))) ||
               (error == ErrorCode::AuthenticationFailed && Same(opened, Span<const uint8>(zero)));
    };
    uint8 opened[sizeof(message)] = {9, 9, 9, 9, 9, 9, 9, 9};
    auto withNonce = hazmat::TryOpenWithNonce(key.Value(), Span<const uint8>(nonce), huge,
                                              Span<const uint8>(sealed), Span<uint8>(opened));
    CHECK(!withNonce && rejected(withNonce.Error(), Span<const uint8>(opened)));
    memcpy(opened, untouched, sizeof(opened));
    auto detached = hazmat::TryOpenDetached(key.Value(), Span<const uint8>(nonce), huge,
                                            Span<const uint8>(sealed).Left(8),
                                            Span<const uint8>(tag), Span<uint8>(opened));
    CHECK(!detached && rejected(detached.Error(), Span<const uint8>(opened)));
}
