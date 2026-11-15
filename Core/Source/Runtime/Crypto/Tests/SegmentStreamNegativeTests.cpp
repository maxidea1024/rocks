#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Private/SegmentStreamTestAccess.h>
#include <Crypto/SegmentStream.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 세그먼트 스트림 음성 시험이다(Docs/Design/Crypto.ko.md 12절): 모든 세그먼트 경계와 중간에서의
// 절단, 세그먼트 교환·중복·삭제, 다른 스트림 세그먼트 이식, 헤더·AD 변조(commitment가 세그먼트를
// 열기 전에 거부), last 표시 위조·뒤에 덧붙이기, 잘못된 키, Finish 뒤 입력, 실패 출력 소거와
// sticky, 세그먼트 수 2^32·AES 2^40 byte 경계(시험 접근점으로 위치를 옮긴다).
namespace {

constexpr usize Tag = SegmentTagBytes;
constexpr usize S = 4096;
constexpr usize Capacity = S - Tag;
constexpr KeyId NegativeKeyId = {{0x1111222233334444ull, 0x5555666677778888ull}};

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

SegmentStreamKey MakeKey(uint8 fill, KeyId id = NegativeKeyId) noexcept {
    uint8 bytes[SegmentStreamKeyBytes];
    memset(bytes, fill, sizeof(bytes));
    auto key = SegmentStreamKey::TryImport(id, Span<const uint8>(bytes));
    return Move(key.Value());
}

// 평문 size byte를 봉인한 전체 암호문이다(출력 창 넉넉히).
void Seal(const SegmentStreamKey& key, const SegmentStreamParams& params,
          Span<const uint8> associatedData, usize size, Array<uint8>& out) noexcept {
    SystemAllocator allocator;
    Array<uint8> plaintext(allocator);
    if (!plaintext.TryResize(size)) {
        platform::Fatal("SegmentStreamNegativeTests: allocation failed");
    }

    for (usize i = 0; i < size; ++i) {
        plaintext[i] = static_cast<uint8>(i * 13 + 1);
    }

    auto encryptor = SegmentStreamEncryptor::TryCreate(key, params, associatedData);
    auto total = TryGetSegmentStreamCiphertextBytes(params, size);
    if (!encryptor || !total || !out.TryResize(static_cast<usize>(total.Value()))) {
        platform::Fatal("SegmentStreamNegativeTests: seal setup failed");
    }

    auto step = encryptor.Value().TryProcess(plaintext.View(), out.View());
    if (!step || step.Value().consumed != size) {
        platform::Fatal("SegmentStreamNegativeTests: seal failed");
    }

    auto finish = encryptor.Value().TryFinish(out.View().RightChop(step.Value().produced));
    if (!finish || !finish.Value().complete ||
        step.Value().produced + finish.Value().produced != out.Size()) {
        platform::Fatal("SegmentStreamNegativeTests: finish failed");
    }
}

// 순차 복호 결과다. 실패하면 그 오류, 성공하면 None이다. 실패한 호출의 출력이 0인지도 본다.
ErrorCode DecryptStream(const SegmentStreamKey& key, Span<const uint8> associatedData,
                        Span<const uint8> ciphertext, bool& outputZeroed) noexcept {
    outputZeroed = true;
    auto decryptor = SegmentStreamDecryptor::TryCreate(key, associatedData);
    if (!decryptor) {
        return decryptor.Error();
    }

    uint8 window[5000];
    usize offset = 0;
    while (offset < ciphertext.Size()) {
        memset(window, 0x5a, sizeof(window));
        auto step = decryptor.Value().TryProcess(ciphertext.RightChop(offset), Span<uint8>(window));
        if (!step) {
            outputZeroed = IsAllZero(Span<const uint8>(window));
            return step.Error();
        }

        offset += step.Value().consumed;
    }

    for (;;) {
        memset(window, 0x5a, sizeof(window));
        auto step = decryptor.Value().TryFinish(Span<uint8>(window));
        if (!step) {
            outputZeroed = IsAllZero(Span<const uint8>(window));
            return step.Error();
        }

        if (step.Value().complete) {
            return ErrorCode::None;
        }
    }
}

// 임의 접근 TryOpen 결과다(마지막 세그먼트를 레이아웃대로 잘라 넘긴다).
ErrorCode OpenRandom(const SegmentStreamKey& key, Span<const uint8> associatedData,
                     Span<const uint8> ciphertext) noexcept {
    if (ciphertext.Size() < SegmentHeaderBytes) {
        return SegmentStreamRandomAccess::TryOpen(key, associatedData, ciphertext, {},
                                                  ciphertext.Size())
            .Error();
    }

    auto info = TryPeekSegmentHeader(ciphertext.Left(SegmentHeaderBytes));
    if (!info) {
        return info.Error();
    }

    auto layout = TryGetSegmentLayout(info.Value().Params(), ciphertext.Size());
    if (!layout) {
        return layout.Error();
    }

    auto reader = SegmentStreamRandomAccess::TryOpen(
        key, associatedData, ciphertext.Left(SegmentHeaderBytes),
        ciphertext.RightChop(static_cast<usize>(layout.Value().lastSegmentOffset)),
        ciphertext.Size());
    return reader ? ErrorCode::None : reader.Error();
}

ErrorCode Decrypt(const SegmentStreamKey& key, Span<const uint8> ad,
                  Span<const uint8> ciphertext) noexcept {
    bool zeroed = false;
    const ErrorCode error = DecryptStream(key, ad, ciphertext, zeroed);
    return error != ErrorCode::None && !zeroed ? ErrorCode::Busy : error; // Busy = 출력이 남았다
}

} // namespace

TEST(CryptoSegmentStreamTruncation) {
    SystemAllocator allocator;
    const SegmentStreamKey key = MakeKey(1);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    Array<uint8> ciphertext(allocator);
    Seal(key, params, Text("ad"), 3 * Capacity + 100, ciphertext);
    CHECK(Decrypt(key, Text("ad"), ciphertext.View()) == ErrorCode::None);
    CHECK(OpenRandom(key, Text("ad"), ciphertext.View()) == ErrorCode::None);

    // 헤더 안, 모든 세그먼트 경계, 각 세그먼트 중간에서 자른다. 경계에서 자르면 새로 끝이 된
    // 세그먼트를 last = 1로 열게 되어 실패한다(last 표시 위조와 같다).
    const usize cuts[] = {0,
                          1,
                          5,
                          SegmentHeaderBytes - 1,
                          SegmentHeaderBytes,
                          SegmentHeaderBytes + 1,
                          SegmentHeaderBytes + Tag,
                          SegmentHeaderBytes + S / 2,
                          SegmentHeaderBytes + S,
                          SegmentHeaderBytes + S + Tag,
                          SegmentHeaderBytes + S + Tag + 1,
                          SegmentHeaderBytes + S + S / 2,
                          SegmentHeaderBytes + 2 * S,
                          SegmentHeaderBytes + 2 * S + 777,
                          SegmentHeaderBytes + 3 * S,
                          SegmentHeaderBytes + 3 * S + 50,
                          ciphertext.Size() - 1};
    for (usize cut : cuts) {
        const Span<const uint8> truncated = ciphertext.View().Left(cut);
        CHECK(Decrypt(key, Text("ad"), truncated) == ErrorCode::AuthenticationFailed);
        CHECK(OpenRandom(key, Text("ad"), truncated) == ErrorCode::AuthenticationFailed);
    }

    // 뒤에 덧붙이면(byte 하나든 세그먼트 하나든) 거부한다.
    Array<uint8> extended(allocator);
    REQUIRE(extended.TryAppend(ciphertext.View()));
    REQUIRE(extended.TryPushBack(0));
    CHECK(Decrypt(key, Text("ad"), extended.View()) == ErrorCode::AuthenticationFailed);
    CHECK(OpenRandom(key, Text("ad"), extended.View()) == ErrorCode::AuthenticationFailed);

    // 마지막이 꽉 찬 스트림도 경계 절단과 덧붙이기를 거부한다.
    Array<uint8> full(allocator);
    Seal(key, params, Text("ad"), 2 * Capacity, full);
    CHECK(Decrypt(key, Text("ad"), full.View()) == ErrorCode::None);
    CHECK(Decrypt(key, Text("ad"), full.View().Left(SegmentHeaderBytes + S)) ==
          ErrorCode::AuthenticationFailed);
    REQUIRE(full.TryAppend(ciphertext.View().Slice(SegmentHeaderBytes, S)));
    CHECK(Decrypt(key, Text("ad"), full.View()) == ErrorCode::AuthenticationFailed);
}

TEST(CryptoSegmentStreamReorderAndTransplant) {
    SystemAllocator allocator;
    const SegmentStreamKey key = MakeKey(2);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    Array<uint8> ciphertext(allocator);
    Seal(key, params, {}, 3 * Capacity + 100, ciphertext);
    const auto segment = [&](Array<uint8>& stream, usize index) {
        return stream.View().Slice(SegmentHeaderBytes + index * S, S);
    };

    // 교환(0 ↔ 1), 중복(1을 2 자리에), 삭제(1을 빼고 당김)는 모두 거부한다.
    Array<uint8> swapped(allocator);
    REQUIRE(swapped.TryAppend(ciphertext.View()));
    memcpy(segment(swapped, 0).Data(), segment(ciphertext, 1).Data(), S);
    memcpy(segment(swapped, 1).Data(), segment(ciphertext, 0).Data(), S);
    CHECK(Decrypt(key, {}, swapped.View()) == ErrorCode::AuthenticationFailed);

    Array<uint8> duplicated(allocator);
    REQUIRE(duplicated.TryAppend(ciphertext.View()));
    memcpy(segment(duplicated, 2).Data(), segment(ciphertext, 1).Data(), S);
    CHECK(Decrypt(key, {}, duplicated.View()) == ErrorCode::AuthenticationFailed);

    Array<uint8> deleted(allocator);
    REQUIRE(deleted.TryAppend(ciphertext.View().Left(SegmentHeaderBytes + S)));
    REQUIRE(deleted.TryAppend(ciphertext.View().RightChop(SegmentHeaderBytes + 2 * S)));
    CHECK(Decrypt(key, {}, deleted.View()) == ErrorCode::AuthenticationFailed);
    // 삭제하면 마지막 세그먼트의 번호가 바뀌므로 임의 접근도 열 때(마지막 세그먼트 검증) 거부한다.
    CHECK(OpenRandom(key, {}, deleted.View()) == ErrorCode::AuthenticationFailed);

    // 임의 접근에서 다른 번호 자리의 세그먼트(교환·중복)는 그 세그먼트를 열 때 거부한다.
    auto layout = TryGetSegmentLayout(params, ciphertext.Size());
    REQUIRE(layout);
    auto reader = SegmentStreamRandomAccess::TryOpen(
        key, {}, ciphertext.View().Left(SegmentHeaderBytes),
        ciphertext.View().RightChop(static_cast<usize>(layout.Value().lastSegmentOffset)),
        ciphertext.Size());
    REQUIRE(reader);
    uint8 plain[S];
    CHECK(reader.Value().TryOpenSegment(1, segment(ciphertext, 2), Span<uint8>(plain)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(reader.Value().TryOpenSegment(1, segment(ciphertext, 1), Span<uint8>(plain)));

    // 같은 키·매개변수의 다른 스트림(다른 salt) 세그먼트를 이식하면 거부한다.
    Array<uint8> other(allocator);
    Seal(key, params, {}, 3 * Capacity + 100, other);
    Array<uint8> transplanted(allocator);
    REQUIRE(transplanted.TryAppend(ciphertext.View()));
    memcpy(segment(transplanted, 1).Data(), segment(other, 1).Data(), S);
    CHECK(Decrypt(key, {}, transplanted.View()) == ErrorCode::AuthenticationFailed);
    // 다른 스트림의 헤더로 바꾸면 commitment는 맞지만 세그먼트가 다른 키라 거부한다.
    Array<uint8> otherHeader(allocator);
    REQUIRE(otherHeader.TryAppend(other.View().Left(SegmentHeaderBytes)));
    REQUIRE(otherHeader.TryAppend(ciphertext.View().RightChop(SegmentHeaderBytes)));
    CHECK(Decrypt(key, {}, otherHeader.View()) == ErrorCode::AuthenticationFailed);
    CHECK(OpenRandom(key, {}, otherHeader.View()) == ErrorCode::AuthenticationFailed);
}

TEST(CryptoSegmentStreamTamperAndWrongKey) {
    SystemAllocator allocator;
    const SegmentStreamKey key = MakeKey(3);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    Array<uint8> ciphertext(allocator);
    Seal(key, params, Text("context"), 2 * Capacity + 9, ciphertext);

    // 헤더 byte마다 bit 하나를 뒤집는다. 버전(4)·알고리즘(5)은 Unsupported일 수 있고, 나머지는
    // AuthenticationFailed다. 헤더만 넣어도 세그먼트를 열기 전에 거부된다.
    for (usize position = 0; position < SegmentHeaderBytes; ++position) {
        Array<uint8> tampered(allocator);
        REQUIRE(tampered.TryAppend(ciphertext.View()));
        tampered[position] ^= 0x01;
        const ErrorCode expected = position == 4 || position == 5 ? ErrorCode::Unsupported
                                                                  : ErrorCode::AuthenticationFailed;
        CHECK(OpenRandom(key, Text("context"), tampered.View()) == expected);
        auto decryptor = SegmentStreamDecryptor::TryCreate(key, Text("context"));
        REQUIRE(decryptor);
        uint8 window[64];
        auto step = decryptor.Value().TryProcess(tampered.View().Left(SegmentHeaderBytes),
                                                 Span<uint8>(window));
        CHECK(!step && step.Error() == expected);
    }

    // 다른 AD, 다른 키(같은 ID), 다른 키 ID는 commitment·키 ID 확인에서 거부된다.
    CHECK(Decrypt(key, Text("contexT"), ciphertext.View()) == ErrorCode::AuthenticationFailed);
    CHECK(OpenRandom(key, Text("contexT"), ciphertext.View()) == ErrorCode::AuthenticationFailed);
    const SegmentStreamKey wrongBytes = MakeKey(4);
    CHECK(Decrypt(wrongBytes, Text("context"), ciphertext.View()) ==
          ErrorCode::AuthenticationFailed);
    CHECK(OpenRandom(wrongBytes, Text("context"), ciphertext.View()) ==
          ErrorCode::AuthenticationFailed);
    const SegmentStreamKey wrongId = MakeKey(3, KeyId{{1, 2}});
    CHECK(Decrypt(wrongId, Text("context"), ciphertext.View()) == ErrorCode::AuthenticationFailed);

    // 세그먼트 byte를 뒤집으면 그 세그먼트에서 실패하고 출력 전체를 지운다(Decrypt가 확인).
    const usize positions[] = {SegmentHeaderBytes,         SegmentHeaderBytes + 100,
                               SegmentHeaderBytes + S - 1, SegmentHeaderBytes + S + 5,
                               SegmentHeaderBytes + 2 * S, ciphertext.Size() - 1};
    for (usize position : positions) {
        Array<uint8> tampered(allocator);
        REQUIRE(tampered.TryAppend(ciphertext.View()));
        tampered[position] ^= 0x80;
        CHECK(Decrypt(key, Text("context"), tampered.View()) == ErrorCode::AuthenticationFailed);
    }

    // 실패는 sticky다. 올바른 입력을 더 넣어도, Finish를 불러도 같은 오류다.
    Array<uint8> tampered(allocator);
    REQUIRE(tampered.TryAppend(ciphertext.View()));
    tampered[SegmentHeaderBytes + 3] ^= 0x01;
    auto decryptor = SegmentStreamDecryptor::TryCreate(key, Text("context"));
    REQUIRE(decryptor);
    uint8 window[S];
    CHECK(decryptor.Value()
              .TryProcess(tampered.View().Left(SegmentHeaderBytes + S + 1), Span<uint8>(window))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(decryptor.Value().TryProcess(ciphertext.View().Left(1), Span<uint8>(window)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(decryptor.Value().TryFinish(Span<uint8>(window)).Error() ==
          ErrorCode::AuthenticationFailed);
    decryptor.Value().Reset();
    CHECK(decryptor.Value().IsEmpty());

    // 임의 접근: 길이가 틀린 세그먼트·변조는 AuthenticationFailed이고 출력을 지운다. 상태가
    // 없으므로 다른 세그먼트는 계속 열린다.
    auto info = TryPeekSegmentHeader(ciphertext.View().Left(SegmentHeaderBytes));
    REQUIRE(info);
    auto layout = TryGetSegmentLayout(info.Value().Params(), ciphertext.Size());
    REQUIRE(layout);
    auto reader = SegmentStreamRandomAccess::TryOpen(
        key, Text("context"), ciphertext.View().Left(SegmentHeaderBytes),
        ciphertext.View().RightChop(static_cast<usize>(layout.Value().lastSegmentOffset)),
        ciphertext.Size());
    REQUIRE(reader);
    memset(window, 0x5a, sizeof(window));
    CHECK(reader.Value()
              .TryOpenSegment(0, ciphertext.View().Slice(SegmentHeaderBytes, S - 1),
                              Span<uint8>(window))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(IsAllZero(Span<const uint8>(window)));
    memset(window, 0x5a, sizeof(window));
    CHECK(reader.Value()
              .TryOpenSegment(1, tampered.View().Slice(SegmentHeaderBytes, S), Span<uint8>(window))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(IsAllZero(Span<const uint8>(window)));
    CHECK(reader.Value().TryOpenSegment(1, ciphertext.View().Slice(SegmentHeaderBytes + S, S),
                                        Span<uint8>(window)));
    // 마지막 세그먼트 길이가 레이아웃과 다르면 TryOpen이 거부한다.
    CHECK(SegmentStreamRandomAccess::TryOpen(
              key, Text("context"), ciphertext.View().Left(SegmentHeaderBytes),
              ciphertext.View().RightChop(static_cast<usize>(layout.Value().lastSegmentOffset) + 1),
              ciphertext.Size())
              .Error() == ErrorCode::AuthenticationFailed);
}

TEST(CryptoSegmentStreamPhaseRules) {
    SystemAllocator allocator;
    const SegmentStreamKey key = MakeKey(5);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    Array<uint8> ciphertext(allocator);
    Seal(key, params, {}, 10, ciphertext);

    // Finish 뒤 입력은 InvalidArgument(상태 불변)이고, 끝난 뒤 Finish는 {0, 0, true}다.
    auto decryptor = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(decryptor);
    uint8 window[64];
    REQUIRE(decryptor.Value().TryProcess(ciphertext.View(), Span<uint8>(window)));
    auto finished = decryptor.Value().TryFinish(Span<uint8>(window));
    REQUIRE(finished && finished.Value().complete && finished.Value().produced == 10);
    CHECK(decryptor.Value().TryProcess(ciphertext.View().Left(1), Span<uint8>(window)).Error() ==
          ErrorCode::InvalidArgument);
    auto again = decryptor.Value().TryFinish(Span<uint8>(window));
    CHECK(again && again.Value().complete && again.Value().produced == 0);
    CHECK(decryptor.Value()
              .TryProcess(ciphertext.View(), Span<uint8>(const_cast<uint8*>(ciphertext.Data()), 4))
              .Error() == ErrorCode::InvalidArgument);

    auto encryptor = SegmentStreamEncryptor::TryCreate(key, params, {});
    REQUIRE(encryptor);
    REQUIRE(encryptor.Value().TryFinish(Span<uint8>(window)));
    CHECK(encryptor.Value().TryProcess(Text("x"), Span<uint8>(window)).Error() ==
          ErrorCode::InvalidArgument);

    // 1 byte 출력 창: 마지막 세그먼트 평문도 TryFinish에서 1 byte씩 나온다.
    auto narrow = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(narrow);
    usize offset = 0;
    usize produced = 0;
    while (offset < ciphertext.Size()) {
        auto step =
            narrow.Value().TryProcess(ciphertext.View().RightChop(offset), Span<uint8>(window, 1));
        REQUIRE(step && step.Value().produced == 0);
        offset += step.Value().consumed;
    }

    for (;;) {
        auto step = narrow.Value().TryFinish(Span<uint8>(window, 1));
        REQUIRE(step && step.Value().produced <= 1);
        produced += step.Value().produced;
        if (step.Value().complete) {
            break;
        }
    }

    CHECK(produced == 10);
}

TEST(CryptoSegmentStreamSegmentCountLimit) {
    // 세그먼트 2^32개 경계를 시험 접근점으로 위치를 옮겨 확인한다(ChaCha20-Poly1305, S = 4 KiB).
    SystemAllocator allocator;
    const SegmentStreamKey key = MakeKey(6);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    const uint64 maxPlaintext = TryGetSegmentStreamMaxPlaintextBytes(params).Value();
    CHECK(maxPlaintext == SegmentStreamMaxSegments * Capacity);

    auto encryptor = SegmentStreamEncryptor::TryCreate(key, params, {});
    REQUIRE(encryptor);
    Array<uint8> stream(allocator);
    REQUIRE(stream.TryResize(SegmentHeaderBytes + 2 * S));
    auto header = encryptor.Value().TryProcess({}, stream.View().Left(SegmentHeaderBytes));
    REQUIRE(header && header.Value().produced == SegmentHeaderBytes);
    const uint64 start = SegmentStreamMaxSegments - 2;
    REQUIRE(crypto::detail::SegmentStreamTestAccess::SetEncryptorPosition(encryptor.Value(), start,
                                                                          start * Capacity));
    Array<uint8> plaintext(allocator);
    REQUIRE(plaintext.TryResize(2 * Capacity + 1));
    memset(plaintext.Data(), 0x42, plaintext.Size());
    // 상한 + 1은 읽기 전에 CapacityExceeded(상태 불변), 정확히 상한까지는 받는다.
    CHECK(encryptor.Value()
              .TryProcess(plaintext.View(), stream.View().RightChop(SegmentHeaderBytes))
              .Error() == ErrorCode::CapacityExceeded);
    auto body = encryptor.Value().TryProcess(plaintext.View().Left(2 * Capacity),
                                             stream.View().RightChop(SegmentHeaderBytes));
    REQUIRE(body && body.Value().consumed == 2 * Capacity);
    CHECK(encryptor.Value().TryProcess(plaintext.View().Left(1), {}).Error() ==
          ErrorCode::CapacityExceeded);
    auto finish = encryptor.Value().TryFinish(
        stream.View().RightChop(SegmentHeaderBytes + body.Value().produced));
    REQUIRE(finish && finish.Value().complete);
    REQUIRE(body.Value().produced + finish.Value().produced == 2 * S);

    // 세그먼트 2^32 − 2(마지막 아님)와 2^32 − 1(마지막, 꽉 참)을 같은 위치의 복호기가 연다.
    auto decryptor = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(decryptor);
    uint8 window[S];
    REQUIRE(
        decryptor.Value().TryProcess(stream.View().Left(SegmentHeaderBytes), Span<uint8>(window)));
    REQUIRE(crypto::detail::SegmentStreamTestAccess::SetDecryptorPosition(decryptor.Value(), start,
                                                                          start * S));
    usize offset = SegmentHeaderBytes;
    usize produced = 0;
    while (offset < stream.Size()) {
        auto step =
            decryptor.Value().TryProcess(stream.View().RightChop(offset), Span<uint8>(window));
        REQUIRE(step);
        offset += step.Value().consumed;
        produced += step.Value().produced;
    }

    for (;;) {
        auto step = decryptor.Value().TryFinish(Span<uint8>(window));
        REQUIRE(step);
        produced += step.Value().produced;
        if (step.Value().complete) {
            break;
        }
    }

    CHECK(produced == 2 * Capacity);
    CHECK(window[0] == 0x42);

    // 2^32 − 1번 세그먼트 뒤에 byte가 더 오면 세그먼트가 2^32개를 넘으므로 열기 전에 거부한다.
    auto beyond = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(beyond);
    REQUIRE(beyond.Value().TryProcess(stream.View().Left(SegmentHeaderBytes), Span<uint8>(window)));
    REQUIRE(crypto::detail::SegmentStreamTestAccess::SetDecryptorPosition(
        beyond.Value(), SegmentStreamMaxSegments - 1, (SegmentStreamMaxSegments - 1) * S));
    REQUIRE(
        beyond.Value().TryProcess(stream.View().Slice(SegmentHeaderBytes, S), Span<uint8>(window)));
    CHECK(beyond.Value().TryProcess(stream.View().Left(1), Span<uint8>(window)).Error() ==
          ErrorCode::AuthenticationFailed);

    // 임의 접근의 크기 산술도 같은 경계다(2^32개 수용, 2^32 + 1개 거부).
    const uint64 atLimit = SegmentHeaderBytes + SegmentStreamMaxSegments * S;
    CHECK(TryGetSegmentLayout(params, atLimit).Value().segmentCount == SegmentStreamMaxSegments);
    CHECK(TryGetSegmentLayout(params, atLimit + Tag + 1).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoSegmentStreamAesBodyLimit) {
    const SegmentStreamParams params{SegmentStreamAlgorithm::Aes256Gcm, 12};
    // 크기 산술은 HW와 무관하다. 세그먼트 암호문 합 2^40 − 16은 받고, 16 byte 더 크면 거부한다.
    const uint64 atLimit = SegmentHeaderBytes + SegmentStreamAesMaxBodyBytes;
    auto layout = TryGetSegmentLayout(params, atLimit);
    REQUIRE(layout);
    CHECK(layout.Value().plaintextBytes == TryGetSegmentStreamMaxPlaintextBytes(params).Value());
    CHECK(TryGetSegmentLayout(params, atLimit + 1).Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryGetSegmentStreamCiphertextBytes(params, layout.Value().plaintextBytes).Value() ==
          atLimit);
    CHECK(TryGetSegmentStreamCiphertextBytes(params, layout.Value().plaintextBytes + 1).Error() ==
          ErrorCode::CapacityExceeded);

    const SegmentStreamKey key = MakeKey(7);
    if (!TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available) {
        CHECK(SegmentStreamEncryptor::TryCreate(key, params, {}).Error() == ErrorCode::Unsupported);
        return;
    }

    // 암호기: 평문 상한 직전까지 옮긴 뒤 상한 −1·상한·상한 +1을 확인한다.
    SystemAllocator allocator;
    const uint64 maxPlaintext = layout.Value().plaintextBytes;
    const uint64 fullSegments = maxPlaintext / Capacity; // 꽉 찬 세그먼트 수(마지막은 S − 32)
    auto encryptor = SegmentStreamEncryptor::TryCreate(key, params, {});
    REQUIRE(encryptor);
    uint8 header[SegmentHeaderBytes];
    REQUIRE(encryptor.Value().TryProcess({}, Span<uint8>(header)));
    REQUIRE(crypto::detail::SegmentStreamTestAccess::SetEncryptorPosition(
        encryptor.Value(), fullSegments, fullSegments * Capacity));
    const usize remaining = static_cast<usize>(maxPlaintext - fullSegments * Capacity);
    CHECK(remaining == S - 2 * Tag);
    Array<uint8> plaintext(allocator);
    REQUIRE(plaintext.TryResize(remaining + 1));
    memset(plaintext.Data(), 0x24, plaintext.Size());
    uint8 out[S];
    CHECK(encryptor.Value().TryProcess(plaintext.View(), Span<uint8>(out)).Error() ==
          ErrorCode::CapacityExceeded);
    REQUIRE(encryptor.Value().TryProcess(plaintext.View().Left(remaining), Span<uint8>(out)));
    auto finish = encryptor.Value().TryFinish(Span<uint8>(out));
    REQUIRE(finish && finish.Value().complete && finish.Value().produced == remaining + Tag);

    // 복호기: 같은 위치에서 마지막 세그먼트를 열면 합이 정확히 상한이다. 16 byte 더 큰 위치면
    // 거부한다.
    auto decryptor = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(decryptor);
    uint8 window[S];
    REQUIRE(decryptor.Value().TryProcess(Span<const uint8>(header), Span<uint8>(window)));
    REQUIRE(crypto::detail::SegmentStreamTestAccess::SetDecryptorPosition(
        decryptor.Value(), fullSegments, fullSegments * S));
    REQUIRE(
        decryptor.Value().TryProcess(Span<const uint8>(out, remaining + Tag), Span<uint8>(window)));
    auto done = decryptor.Value().TryFinish(Span<uint8>(window));
    REQUIRE(done && done.Value().complete && done.Value().produced == remaining);

    auto over = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(over);
    REQUIRE(over.Value().TryProcess(Span<const uint8>(header), Span<uint8>(window)));
    REQUIRE(crypto::detail::SegmentStreamTestAccess::SetDecryptorPosition(
        over.Value(), fullSegments, fullSegments * S + Tag));
    REQUIRE(over.Value().TryProcess(Span<const uint8>(out, remaining + Tag), Span<uint8>(window)));
    CHECK(over.Value().TryFinish(Span<uint8>(window)).Error() == ErrorCode::AuthenticationFailed);
}
