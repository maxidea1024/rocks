#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Crypto/Bytes.h>
#include <Crypto/Kdf.h>
#include <Crypto/SecretStream.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 메시지 스트림(Crypto/SecretStream.h)의 공개 계약 시험이다(Docs/Design/Crypto.ko.md 9.4·12절).
// - 상호운용: PyNaCl 1.6.2(libsodium)가 만든 스트림을 연다. tag 0x04 메시지는 0–3 밖이라
//   AuthenticationFailed다.
// - 음성: bit 뒤집기, 순서 바꾸기, 잘린 메시지, 다른 키·AD, Final 없는 끝(절단), Final 뒤 입력,
// 길이
//   상한(misuse handler 조건)이 Fatal 없이 오류로 돌아오는지, sticky와 출력 소거.
// - 수명: 이동 뒤 StaleHandle, Reset, 키 파생·내보내기.
// 상태 벡터(Wycheproof secretstream)는 WycheproofTests가 맡는다.
namespace {

constexpr usize Overhead = SecretStreamWriter::OverheadBytes;

struct HexBytes {
    uint8 bytes[96];
    usize size = 0;

    explicit HexBytes(const char* hex) noexcept {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : 0;
    }

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes, size);
    }
};

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

SecretStreamKey FixedKey() noexcept {
    uint8 bytes[SecretStreamKeyBytes];
    for (usize i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<uint8>(i);
    }

    auto key = SecretStreamKey::TryImport(Span<const uint8>(bytes));
    return Move(key.Value());
}

// 메시지 셋(Message·Push·Final)을 봉인해 sealed에 이어 붙이고 각 길이를 lengths에 둔다.
struct Sealed {
    uint8 header[SecretStreamWriter::HeaderBytes];
    uint8 bytes[3 * 64];
    usize lengths[3] = {};
    usize offsets[3] = {};

    Span<const uint8> Message(usize index) const noexcept {
        return Span<const uint8>(bytes + offsets[index], lengths[index]);
    }
};

bool SealThree(const SecretStreamKey& key, Sealed& out) noexcept {
    auto writer = SecretStreamWriter::TryCreate(key, Span<uint8>(out.header));
    if (!writer) {
        return false;
    }

    const char* messages[3] = {"first message", "", "the end"};
    const SecretStreamTag tags[3] = {SecretStreamTag::Message, SecretStreamTag::Push,
                                     SecretStreamTag::Final};
    usize offset = 0;
    for (usize i = 0; i < 3; ++i) {
        auto written = writer.Value().TryPush(Text(messages[i]), Text("ad"), tags[i],
                                              Span<uint8>(out.bytes + offset, 64));
        if (!written) {
            return false;
        }

        out.offsets[i] = offset;
        out.lengths[i] = written.Value();
        offset += written.Value();
    }

    return writer.Value().IsFinished();
}

} // namespace

TEST(CryptoSecretStreamRoundTrip) {
    const SecretStreamKey key = FixedKey();
    Sealed sealed;
    REQUIRE(SealThree(key, sealed));
    CHECK(sealed.lengths[0] == strlen("first message") + Overhead);
    CHECK(sealed.lengths[1] == Overhead);

    auto reader = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
    REQUIRE(reader);
    uint8 plain[64];
    auto first = reader.Value().TryPull(sealed.Message(0), Text("ad"), Span<uint8>(plain));
    REQUIRE(first);
    CHECK(first.Value().tag == SecretStreamTag::Message);
    CHECK(Same(Span<const uint8>(plain, first.Value().bytes), Text("first message")));
    // 끝나기 전의 TryFinish는 절단이다(아래 시험). 여기서는 끝까지 읽는다.
    auto second = reader.Value().TryPull(sealed.Message(1), Text("ad"), Span<uint8>(plain));
    REQUIRE(second);
    CHECK(second.Value().tag == SecretStreamTag::Push && second.Value().bytes == 0);
    CHECK(!reader.Value().IsFinished());
    auto third = reader.Value().TryPull(sealed.Message(2), Text("ad"), Span<uint8>(plain));
    REQUIRE(third);
    CHECK(third.Value().tag == SecretStreamTag::Final);
    CHECK(Same(Span<const uint8>(plain, third.Value().bytes), Text("the end")));
    CHECK(reader.Value().IsFinished());
    CHECK(reader.Value().TryFinish());

    // Final 뒤의 입력은 AuthenticationFailed이고 sticky다.
    memset(plain, 0x5a, sizeof(plain));
    CHECK(reader.Value().TryPull(sealed.Message(0), Text("ad"), Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(IsAllZero(Span<const uint8>(plain)));
    CHECK(reader.Value().TryFinish().Error() == ErrorCode::AuthenticationFailed);
}

TEST(CryptoSecretStreamPyNaClInterop) {
    // PyNaCl 1.6.2 crypto_secretstream_xchacha20poly1305: 키 00 01 .. 1f, Message(AD
    // "record-ad") → Push → Rekey → Final.
    const SecretStreamKey key = FixedKey();
    const HexBytes header("3014a1dfb22bfc1bb40374de6cef29d382590c13c7e742ee");
    const HexBytes messages[4] = {
        HexBytes("9c8e7aedb6f7df8ca7b731e9601fe6054912a56449a707e098cb2a1daf15350472f6a1"),
        HexBytes("ac65f5ee592eac7d209f1a72ecc461d14ef4dfa551288571b2ad48"),
        HexBytes("17bd9e713e6fdfc2487fc86b9446ca938c2794a729bbf6aa"),
        HexBytes("9484381a2eceab7ecaffeaf26369d8ec7a9ffe5acd"),
    };
    const char* expected[4] = {"Rocks secretstream", "after push", "rekeyed", "tail"};
    const SecretStreamTag tags[4] = {SecretStreamTag::Message, SecretStreamTag::Push,
                                     SecretStreamTag::Rekey, SecretStreamTag::Final};
    auto reader = SecretStreamReader::TryCreate(key, header.View());
    REQUIRE(reader);
    for (usize i = 0; i < 4; ++i) {
        uint8 plain[64];
        auto pulled = reader.Value().TryPull(messages[i].View(),
                                             i == 0 ? Text("record-ad") : Span<const uint8>(),
                                             Span<uint8>(plain));
        REQUIRE(pulled);
        CHECK(pulled.Value().tag == tags[i]);
        CHECK(Same(Span<const uint8>(plain, pulled.Value().bytes), Text(expected[i])));
    }

    CHECK(reader.Value().TryFinish());

    // libsodium은 tag byte로 0–255를 모두 받는다. Rocks writer가 만들지 않는 값은 거부한다.
    const HexBytes badHeader("219e7b00cecddb9dc62f0b18b73b95289d5f6cf76892684d");
    const HexBytes badTag("b504893277f80173c3eb93ccb4338d7f0d9bd7e99a771ed9");
    auto bad = SecretStreamReader::TryCreate(key, badHeader.View());
    REQUIRE(bad);
    uint8 plain[16];
    memset(plain, 0x5a, sizeof(plain));
    CHECK(bad.Value().TryPull(badTag.View(), {}, Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(IsAllZero(Span<const uint8>(plain)) && bad.Value().HasFailed());
}

TEST(CryptoSecretStreamTamperRejected) {
    const SecretStreamKey key = FixedKey();
    Sealed sealed;
    REQUIRE(SealThree(key, sealed));
    uint8 plain[64];

    // bit 하나 뒤집기(모든 byte 위치), 잘린 메시지, 다른 AD, 순서 바꾸기, 다른 키는 모두 거부된다.
    for (usize position = 0; position < sealed.lengths[0]; ++position) {
        uint8 copy[64];
        memcpy(copy, sealed.Message(0).Data(), sealed.lengths[0]);
        copy[position] ^= 0x01;
        auto reader = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
        REQUIRE(reader);
        CHECK(
            reader.Value()
                .TryPull(Span<const uint8>(copy, sealed.lengths[0]), Text("ad"), Span<uint8>(plain))
                .Error() == ErrorCode::AuthenticationFailed);
    }

    auto truncated = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
    REQUIRE(truncated);
    CHECK(truncated.Value()
              .TryPull(sealed.Message(0).LeftChop(1), Text("ad"), Span<uint8>(plain))
              .Error() == ErrorCode::AuthenticationFailed);
    auto tooShort = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
    REQUIRE(tooShort);
    CHECK(tooShort.Value()
              .TryPull(sealed.Message(0).Left(Overhead - 1), Text("ad"), Span<uint8>(plain))
              .Error() == ErrorCode::AuthenticationFailed);

    auto wrongAd = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
    REQUIRE(wrongAd);
    CHECK(wrongAd.Value().TryPull(sealed.Message(0), Text("da"), Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);

    auto reordered = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
    REQUIRE(reordered);
    CHECK(reordered.Value().TryPull(sealed.Message(2), Text("ad"), Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);
    // sticky: 올바른 첫 메시지도 이제 거부한다.
    CHECK(reordered.Value().TryPull(sealed.Message(0), Text("ad"), Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(reordered.Value().HasFailed());
    reordered.Value().Reset();
    CHECK(reordered.Value().IsEmpty() && !reordered.Value().HasFailed());

    auto otherKey = SecretStreamKey::TryGenerate();
    REQUIRE(otherKey);
    auto wrongKey =
        SecretStreamReader::TryCreate(otherKey.Value(), Span<const uint8>(sealed.header));
    REQUIRE(wrongKey);
    CHECK(wrongKey.Value().TryPull(sealed.Message(0), Text("ad"), Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);

    // header 길이는 메시지에서 온 값이다.
    CHECK(
        SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header).LeftChop(1)).Error() ==
        ErrorCode::AuthenticationFailed);
}

TEST(CryptoSecretStreamTruncationDetected) {
    const SecretStreamKey key = FixedKey();
    Sealed sealed;
    REQUIRE(SealThree(key, sealed));
    uint8 plain[64];
    // Final 앞에서 끝난 스트림은 TryFinish가 AuthenticationFailed다(sticky).
    for (usize received = 0; received < 3; ++received) {
        auto reader = SecretStreamReader::TryCreate(key, Span<const uint8>(sealed.header));
        REQUIRE(reader);
        for (usize i = 0; i < received; ++i) {
            REQUIRE(reader.Value().TryPull(sealed.Message(i), Text("ad"), Span<uint8>(plain)));
        }

        CHECK(reader.Value().TryFinish().Error() == ErrorCode::AuthenticationFailed);
        CHECK(reader.Value().HasFailed());
        CHECK(reader.Value()
                  .TryPull(sealed.Message(received), Text("ad"), Span<uint8>(plain))
                  .Error() == ErrorCode::AuthenticationFailed);
    }
}

TEST(CryptoSecretStreamRekeyAndArguments) {
    const SecretStreamKey key = FixedKey();
    uint8 header[SecretStreamWriter::HeaderBytes];
    auto writer = SecretStreamWriter::TryCreate(key, Span<uint8>(header));
    REQUIRE(writer);
    uint8 first[32];
    uint8 second[32];
    REQUIRE(writer.Value().TryPush(Text("one"), {}, SecretStreamTag::Message, Span<uint8>(first)));
    writer.Value().Rekey();
    REQUIRE(writer.Value().TryPush(Text("two"), {}, SecretStreamTag::Final, Span<uint8>(second)));
    const usize secondBytes = 3 + Overhead;

    // 받는 쪽도 같은 위치에서 Rekey해야 연다.
    uint8 plain[32];
    auto synced = SecretStreamReader::TryCreate(key, Span<const uint8>(header));
    REQUIRE(synced);
    REQUIRE(synced.Value().TryPull(Span<const uint8>(first, 3 + Overhead), {}, Span<uint8>(plain)));
    synced.Value().Rekey();
    auto opened =
        synced.Value().TryPull(Span<const uint8>(second, secondBytes), {}, Span<uint8>(plain));
    REQUIRE(opened);
    CHECK(Same(Span<const uint8>(plain, opened.Value().bytes), Text("two")));
    auto unsynced = SecretStreamReader::TryCreate(key, Span<const uint8>(header));
    REQUIRE(unsynced);
    REQUIRE(
        unsynced.Value().TryPull(Span<const uint8>(first, 3 + Overhead), {}, Span<uint8>(plain)));
    CHECK(unsynced.Value()
              .TryPull(Span<const uint8>(second, secondBytes), {}, Span<uint8>(plain))
              .Error() == ErrorCode::AuthenticationFailed);

    // Final 뒤의 push, 열거값이 아닌 tag, 겹침은 InvalidArgument, 작은 출력은 CapacityExceeded다.
    uint8 out[32];
    CHECK(writer.Value().IsFinished());
    CHECK(
        writer.Value().TryPush(Text("x"), {}, SecretStreamTag::Message, Span<uint8>(out)).Error() ==
        ErrorCode::InvalidArgument);
    uint8 freshHeader[SecretStreamWriter::HeaderBytes];
    auto fresh = SecretStreamWriter::TryCreate(key, Span<uint8>(freshHeader));
    REQUIRE(fresh);
    CHECK(fresh.Value()
              .TryPush(Text("x"), {}, static_cast<SecretStreamTag>(4), Span<uint8>(out))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(fresh.Value()
              .TryPush(Span<const uint8>(out, 4), {}, SecretStreamTag::Message, Span<uint8>(out))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(fresh.Value()
              .TryPush(Text("x"), {}, SecretStreamTag::Message, Span<uint8>(out, Overhead))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(SecretStreamWriter::TryCreate(key, Span<uint8>(freshHeader).LeftChop(1)).Error() ==
          ErrorCode::InvalidArgument);

    // 받는 쪽 출력 부족은 인증 전 CapacityExceeded이고 상태가 그대로라 다시 열 수 있다.
    auto reader = SecretStreamReader::TryCreate(key, Span<const uint8>(header));
    REQUIRE(reader);
    CHECK(reader.Value()
              .TryPull(Span<const uint8>(first, 3 + Overhead), {}, Span<uint8>(plain, 2))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(reader.Value().TryPull(Span<const uint8>(first, 3 + Overhead), {}, Span<uint8>(plain)));
    CHECK(reader.Value()
              .TryPull(Span<const uint8>(first, 3 + Overhead), {}, Span<uint8>(first, 3))
              .Error() == ErrorCode::InvalidArgument);
}

TEST(CryptoSecretStreamLengthLimitWithoutFatal) {
    // libsodium misuse handler 조건(메시지 > MESSAGEBYTES_MAX)은 부르기 전에 거른다. 길이만 큰
    // Span을 넘기며 내용은 읽히지 않는다(출력은 입력보다 앞 주소에 두어 겹치지 않게 한다).
    const SecretStreamKey key = FixedKey();
    uint8 header[SecretStreamWriter::HeaderBytes];
    auto writer = SecretStreamWriter::TryCreate(key, Span<uint8>(header));
    REQUIRE(writer);
    uint8 memory[64] = {};
    const usize tooLong = static_cast<usize>(SecretStreamMaxMessageBytes) + 1;
    const Span<const uint8> hugeMessage(memory + 32, tooLong);
    CHECK(writer.Value()
              .TryPush(hugeMessage, {}, SecretStreamTag::Message, Span<uint8>(memory, 32))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(writer.Value()
              .TryPush(Span<const uint8>(memory + 32, 1), {}, SecretStreamTag::Message,
                       Span<uint8>(memory, 32))
              .Value() == 1 + Overhead);

    auto reader = SecretStreamReader::TryCreate(key, Span<const uint8>(header));
    REQUIRE(reader);
    const Span<const uint8> hugeSealed(memory + 32, tooLong + Overhead);
    CHECK(reader.Value().TryPull(hugeSealed, {}, Span<uint8>(memory, 32)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(reader.Value().HasFailed());
}

TEST(CryptoSecretStreamKeysAndLifetime) {
    CHECK(SecretStreamKey::TryImport(Span<const uint8>()).Error() == ErrorCode::InvalidArgument);
    SecretStreamKey key = FixedKey();
    uint8 exported[SecretStreamKeyBytes];
    REQUIRE(hazmat::TryExportKey(key, Span<uint8>(exported)));
    CHECK(exported[0] == 0 && exported[31] == 31);
    CHECK(hazmat::TryExportKey(key, Span<uint8>(exported, 16)).Error() ==
          ErrorCode::InvalidArgument);

    // 타입 파생은 결정적이고 context마다 다르다.
    uint8 masterBytes[KdfKeyBytes] = {7};
    auto master = KdfKey::TryImport(Span<const uint8>(masterBytes));
    REQUIRE(master);
    auto derivedA = TryDeriveSecretStreamKey(master.Value(), 1, KdfContext("RkTstStr"));
    auto derivedB = TryDeriveSecretStreamKey(master.Value(), 1, KdfContext("RkTstStr"));
    auto derivedC = TryDeriveSecretStreamKey(master.Value(), 2, KdfContext("RkTstStr"));
    REQUIRE(derivedA && derivedB && derivedC);
    uint8 a[SecretStreamKeyBytes];
    uint8 b[SecretStreamKeyBytes];
    uint8 c[SecretStreamKeyBytes];
    REQUIRE(hazmat::TryExportKey(derivedA.Value(), Span<uint8>(a)));
    REQUIRE(hazmat::TryExportKey(derivedB.Value(), Span<uint8>(b)));
    REQUIRE(hazmat::TryExportKey(derivedC.Value(), Span<uint8>(c)));
    CHECK(Same(Span<const uint8>(a), Span<const uint8>(b)));
    CHECK(!Same(Span<const uint8>(a), Span<const uint8>(c)));
    CHECK(TryDeriveSecretStreamKey(KdfKey(), 1, KdfContext("RkTstStr")).Error() ==
          ErrorCode::StaleHandle);

    // 이동 뒤 원본은 StaleHandle이다.
    uint8 header[SecretStreamWriter::HeaderBytes];
    auto writer = SecretStreamWriter::TryCreate(key, Span<uint8>(header));
    REQUIRE(writer);
    SecretStreamWriter moved = Move(writer.Value());
    uint8 out[32];
    CHECK(
        writer.Value().TryPush(Text("x"), {}, SecretStreamTag::Message, Span<uint8>(out)).Error() ==
        ErrorCode::StaleHandle);
    CHECK(moved.TryPush(Text("x"), {}, SecretStreamTag::Message, Span<uint8>(out)));
    moved.Reset();
    CHECK(moved.IsEmpty());

    auto reader = SecretStreamReader::TryCreate(key, Span<const uint8>(header));
    REQUIRE(reader);
    SecretStreamReader movedReader = Move(reader.Value());
    CHECK(reader.Value().TryFinish().Error() == ErrorCode::StaleHandle);
    CHECK(!movedReader.IsEmpty());

    SecretStreamKey movedKey = Move(key);
    CHECK(key.IsEmpty() && !movedKey.IsEmpty());
    CHECK(SecretStreamWriter::TryCreate(key, Span<uint8>(header)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(SecretStreamReader::TryCreate(key, Span<const uint8>(header)).Error() ==
          ErrorCode::StaleHandle);
}
