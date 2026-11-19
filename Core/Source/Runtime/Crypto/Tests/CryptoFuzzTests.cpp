#include <Base/Endian.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Crypto/Aead.h>
#include <Crypto/Channel.h>
#include <Crypto/KeyId.h>
#include <Crypto/Private/ChannelInternal.h>
#include <Crypto/SecretStream.h>
#include <Crypto/SegmentStream.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace crypto_fuzz_tests_file {

using namespace rocks::crypto;

// 고정 seed 변형 fuzz다(Docs/Design/Crypto.ko.md 12절 fuzz). coverage 기반 fuzzer(libFuzzer)는 빌드
// 기반(모듈 sanitizer 인자)이 없어 아직 쓰지 않고, 시험 묶음 안에서 매번 같은 변형 입력을 돌린다.
// 공격자가 정하는 입력(세그먼트 헤더·전체 크기·암호문, secretstream 메시지, 보안 채널 handshake
// 메시지·datagram, Wycheproof JSON)이
// 정해진 오류로만 거부되고(Fatal·InvalidArgument 없음), 실패한 출력이 지워지며, 배치 계산이 형식의
// 식과 같은지 본다.
namespace {

struct FuzzRandom {
    uint64 state;

    uint64 Next() noexcept {
        state += 0x9e3779b97f4a7c15ull;
        uint64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    usize Below(usize bound) noexcept {
        return bound == 0 ? 0 : static_cast<usize>(Next() % bound);
    }
};

Span<const uint8> Bytes(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

bool AllZero(Span<const uint8> bytes) noexcept {
    for (uint8 byte : bytes) {
        if (byte != 0) {
            return false;
        }
    }

    return true;
}

// source를 복사해 한 번 변형한다: bit 뒤집기, byte 바꾸기, 자르기, 늘리기, 구간 복제(세그먼트
// 교환과 중복을 흉내 낸다). 결과가 원본과 같을 수도 있으므로 호출자가 비교한다.
bool Mutate(FuzzRandom& random, Span<const uint8> source, Array<uint8>& out) noexcept {
    out.Clear();
    if (!out.TryAppend(source)) {
        return false;
    }

    const usize size = out.Size();
    switch (random.Below(5)) {
    case 0:
        if (size != 0) {
            out[random.Below(size)] ^= static_cast<uint8>(1u << random.Below(8));
        }

        return true;
    case 1:
        if (size != 0) {
            out[random.Below(size)] = static_cast<uint8>(random.Next());
        }

        return true;
    case 2:
        return static_cast<bool>(out.TryResize(random.Below(size + 1)));
    case 3: {
        const usize extra = 1 + random.Below(64);
        if (!out.TryResize(size + extra)) {
            return false;
        }

        for (usize index = size; index < size + extra; ++index) {
            out[index] = static_cast<uint8>(random.Next());
        }

        return true;
    }
    default:
        if (size >= 2) {
            const usize from = random.Below(size);
            const usize to = random.Below(size);
            const usize count = 1 + random.Below(size - (from > to ? from : to));
            memmove(out.Data() + to, out.Data() + from, count);
        }

        return true;
    }
}

bool AesAvailable() noexcept {
    return TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available;
}

// plaintextBytes 크기의 평문을 봉인한 전체 암호문을 만든다.
bool Seal(const SegmentStreamKey& key, const SegmentStreamParams& params, usize plaintextBytes,
          Array<uint8>& plaintext, Array<uint8>& ciphertext) noexcept {
    if (!plaintext.TryResize(plaintextBytes)) {
        return false;
    }

    for (usize index = 0; index < plaintextBytes; ++index) {
        plaintext[index] = static_cast<uint8>(index * 31 + 7);
    }

    auto total = TryGetSegmentStreamCiphertextBytes(params, plaintextBytes);
    auto encryptor = SegmentStreamEncryptor::TryCreate(key, params, Bytes("fuzz"));
    if (!total || !encryptor || !ciphertext.TryResize(static_cast<usize>(total.Value()))) {
        return false;
    }

    auto processed = encryptor.Value().TryProcess(plaintext.View(), ciphertext.View());
    if (!processed || processed.Value().consumed != plaintextBytes) {
        return false;
    }

    const usize head = processed.Value().produced;
    auto finished = encryptor.Value().TryFinish(ciphertext.View().RightChop(head));
    return finished && head + finished.Value().produced == ciphertext.Size();
}

} // namespace

// 세그먼트 헤더 읽기: 어떤 byte·길이든 성공(알려진 매개변수)이거나
// AuthenticationFailed·Unsupported다.
TEST(CryptoFuzzSegmentHeader) {
    SystemAllocator allocator;
    auto id = TryNewKeyId();
    REQUIRE(id);
    auto key = SegmentStreamKey::TryGenerate(id.Value());
    REQUIRE(key);
    Array<uint8> plaintext(allocator);
    Array<uint8> ciphertext(allocator);
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    REQUIRE(Seal(key.Value(), params, 100, plaintext, ciphertext));
    const Span<const uint8> header = ciphertext.View().Left(SegmentHeaderBytes);
    REQUIRE(TryPeekSegmentHeader(header));

    FuzzRandom random{0xf022};
    Array<uint8> mutated(allocator);
    usize accepted = 0;
    for (usize iteration = 0; iteration < 20000; ++iteration) {
        REQUIRE(Mutate(random, header, mutated));
        auto info = TryPeekSegmentHeader(mutated.View());
        if (info) {
            ++accepted;
            CHECK(mutated.Size() == SegmentHeaderBytes);
            CHECK(info.Value().segmentSizeLog2 >= SegmentSizeLog2Min &&
                  info.Value().segmentSizeLog2 <= SegmentSizeLog2Max);
            CHECK(info.Value().algorithm == SegmentStreamAlgorithm::ChaCha20Poly1305 ||
                  info.Value().algorithm == SegmentStreamAlgorithm::Aes256Gcm);
        } else {
            CHECK(info.Error() == ErrorCode::AuthenticationFailed ||
                  info.Error() == ErrorCode::Unsupported);
        }
    }

    // salt·commitment byte를 바꾼 헤더는 읽기에서 통과해야 한다(값 확인은 commitment 몫이다).
    CHECK(accepted != 0);
}

// 전체 크기에서 정하는 배치: 공격자가 정하는 크기는 성공이거나 AuthenticationFailed이고, 성공하면
// 형식 명세의 식과 같다.
TEST(CryptoFuzzSegmentLayout) {
    FuzzRandom random{0x1a70};
    const uint64 edges[] = {0,
                            1,
                            SegmentHeaderBytes,
                            SegmentHeaderBytes + 15,
                            SegmentHeaderBytes + 16,
                            SegmentHeaderBytes + 17,
                            uint64{1} << 32,
                            uint64{1} << 40,
                            (uint64{1} << 40) + SegmentHeaderBytes,
                            uint64{1} << 56,
                            ~uint64{0} - 1,
                            ~uint64{0}};
    for (usize iteration = 0; iteration < 50000; ++iteration) {
        const SegmentStreamParams params{
            (iteration & 1) != 0 ? SegmentStreamAlgorithm::Aes256Gcm
                                 : SegmentStreamAlgorithm::ChaCha20Poly1305,
            static_cast<uint8>(SegmentSizeLog2Min +
                               random.Below(SegmentSizeLog2Max - SegmentSizeLog2Min + 1))};
        const uint64 S = uint64{1} << params.segmentSizeLog2;
        uint64 total = 0;
        switch (random.Below(4)) {
        case 0:
            total = edges[random.Below(sizeof(edges) / sizeof(edges[0]))];
            break;
        case 1:
            total = SegmentHeaderBytes + random.Below(8) * S + random.Below(40);
            break;
        case 2:
            total = random.Next() >> random.Below(64);
            break;
        default:
            total =
                SegmentHeaderBytes + (uint64{1} << 32) * S - random.Below(64) + random.Below(64);
            break;
        }

        auto layout = TryGetSegmentLayout(params, total);
        if (!layout) {
            CHECK(layout.Error() == ErrorCode::AuthenticationFailed);
            continue;
        }

        const SegmentLayout& value = layout.Value();
        const uint64 n = value.segmentCount;
        CHECK(n >= 1 && n <= SegmentStreamMaxSegments);
        CHECK(value.segmentBytes == S);
        CHECK(value.lastSegmentOffset == SegmentHeaderBytes + (n - 1) * S);
        CHECK(value.lastSegmentBytes >= SegmentTagBytes && value.lastSegmentBytes <= S);
        CHECK(n == 1 || value.lastSegmentBytes > SegmentTagBytes);
        CHECK(value.lastSegmentOffset + value.lastSegmentBytes == total);
        CHECK(value.plaintextBytes ==
              (n - 1) * (S - SegmentTagBytes) + (value.lastSegmentBytes - SegmentTagBytes));
        if (params.algorithm == SegmentStreamAlgorithm::Aes256Gcm) {
            CHECK(total - SegmentHeaderBytes <= SegmentStreamAesMaxBodyBytes);
        }
    }
}

// 임의 접근 범위 계산: 평문 범위는 성공이거나(덮는 세그먼트·암호문 구간이 식과 같다)
// InvalidArgument다.
TEST(CryptoFuzzRandomAccessRanges) {
    SystemAllocator allocator;
    auto id = TryNewKeyId();
    REQUIRE(id);
    auto key = SegmentStreamKey::TryGenerate(id.Value());
    REQUIRE(key);
    FuzzRandom random{0x4a9e};
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    const uint64 S = uint64{1} << params.segmentSizeLog2;
    const uint64 chunk = S - SegmentTagBytes;
    for (usize stream = 0; stream < 24; ++stream) {
        Array<uint8> plaintext(allocator);
        Array<uint8> ciphertext(allocator);
        REQUIRE(Seal(key.Value(), params, random.Below(5 * chunk + 3), plaintext, ciphertext));
        auto layout = TryGetSegmentLayout(params, ciphertext.Size());
        REQUIRE(layout);
        auto access = SegmentStreamRandomAccess::TryOpen(
            key.Value(), Bytes("fuzz"), ciphertext.View().Left(SegmentHeaderBytes),
            ciphertext.View().Slice(static_cast<usize>(layout.Value().lastSegmentOffset),
                                    static_cast<usize>(layout.Value().lastSegmentBytes)),
            ciphertext.Size());
        REQUIRE(access);
        const uint64 plain = access.Value().PlaintextBytes();
        CHECK(plain == plaintext.Size());
        for (usize query = 0; query < 2000; ++query) {
            const uint64 offset =
                random.Below(3) == 0 ? random.Next() : random.Below(static_cast<usize>(plain) + 2);
            const uint64 bytes =
                random.Below(3) == 0 ? random.Next() : random.Below(static_cast<usize>(plain) + 2);
            auto range = access.Value().TryMapPlaintextRange(offset, bytes);
            const bool valid = offset <= plain && bytes <= plain - offset;
            if (!valid) {
                CHECK(!range && range.Error() == ErrorCode::InvalidArgument);
                continue;
            }

            REQUIRE(range);
            const SegmentRange& value = range.Value();
            if (bytes == 0) {
                CHECK(value.segmentCount == 0 && value.ciphertextBytes == 0);
                continue;
            }

            const uint64 first = offset / chunk;
            const uint64 last = (offset + bytes - 1) / chunk;
            CHECK(value.firstSegment == first);
            CHECK(value.segmentCount == last - first + 1);
            CHECK(value.firstSegmentOffset == offset % chunk);
            CHECK(value.ciphertextOffset == SegmentHeaderBytes + first * S);
            const uint64 end = last + 1 == layout.Value().segmentCount
                                   ? ciphertext.Size()
                                   : SegmentHeaderBytes + (last + 1) * S;
            CHECK(value.ciphertextBytes == end - value.ciphertextOffset);
        }
    }
}

// 세그먼트 복호: 변형한 암호문은 원본과 같지 않으면 AuthenticationFailed·Unsupported로만 거부되고,
// 그 호출의 출력은 지워진다. 순차 복호기와 임의 접근 열기를 함께 본다.
TEST(CryptoFuzzSegmentDecrypt) {
    SystemAllocator allocator;
    auto id = TryNewKeyId();
    REQUIRE(id);
    auto key = SegmentStreamKey::TryGenerate(id.Value());
    REQUIRE(key);
    FuzzRandom random{0xdec0};
    for (usize algorithmIndex = 0; algorithmIndex < 2; ++algorithmIndex) {
        const SegmentStreamAlgorithm algorithm = algorithmIndex == 0
                                                     ? SegmentStreamAlgorithm::ChaCha20Poly1305
                                                     : SegmentStreamAlgorithm::Aes256Gcm;
        if (algorithm == SegmentStreamAlgorithm::Aes256Gcm && !AesAvailable()) {
            continue;
        }

        const SegmentStreamParams params{algorithm, 12};
        const usize chunk = (usize{1} << params.segmentSizeLog2) - SegmentTagBytes;
        Array<uint8> plaintext(allocator);
        Array<uint8> ciphertext(allocator);
        REQUIRE(Seal(key.Value(), params, 2 * chunk + 100, plaintext, ciphertext));
        Array<uint8> mutated(allocator);
        Array<uint8> output(allocator);
        REQUIRE(output.TryResize(ciphertext.Size() + 128));
        for (usize iteration = 0; iteration < 600; ++iteration) {
            REQUIRE(Mutate(random, ciphertext.View(), mutated));
            const bool changed = !Same(mutated.View(), ciphertext.View());
            auto decryptor = SegmentStreamDecryptor::TryCreate(key.Value(), Bytes("fuzz"));
            REQUIRE(decryptor);
            ErrorCode error = ErrorCode::None;
            usize produced = 0;
            auto processed = decryptor.Value().TryProcess(mutated.View(), output.View());
            if (!processed) {
                error = processed.Error();
                CHECK(AllZero(output.View()));
            } else {
                produced = processed.Value().produced;
                auto finished = decryptor.Value().TryFinish(output.View().RightChop(produced));
                if (!finished) {
                    error = finished.Error();
                    CHECK(AllZero(output.View().RightChop(produced)));
                } else {
                    produced += finished.Value().produced;
                }
            }

            if (changed) {
                CHECK(error == ErrorCode::AuthenticationFailed || error == ErrorCode::Unsupported);
            } else {
                CHECK(error == ErrorCode::None &&
                      Same(output.View().Left(produced), plaintext.View()));
            }

            for (uint8& byte : output.View()) {
                byte = 0;
            }

            // 임의 접근 열기는 변형한 전체 크기로 배치를 다시 계산해 헤더·마지막 세그먼트를
            // 확인한다.
            if (mutated.Size() < SegmentHeaderBytes) {
                continue;
            }

            auto layout = TryGetSegmentLayout(params, mutated.Size());
            if (!layout) {
                CHECK(layout.Error() == ErrorCode::AuthenticationFailed);
                continue;
            }

            auto access = SegmentStreamRandomAccess::TryOpen(
                key.Value(), Bytes("fuzz"), mutated.View().Left(SegmentHeaderBytes),
                mutated.View().Slice(static_cast<usize>(layout.Value().lastSegmentOffset),
                                     static_cast<usize>(layout.Value().lastSegmentBytes)),
                mutated.Size());
            if (!access) {
                CHECK(access.Error() == ErrorCode::AuthenticationFailed ||
                      access.Error() == ErrorCode::Unsupported);
                continue;
            }

            // 열렸다면 세그먼트마다 검증한다. 변형이 있었다면 어느 세그먼트든 실패해야 한다.
            bool allOpened = true;
            for (uint64 index = 0; index < access.Value().SegmentCount(); ++index) {
                const SegmentLayout& value = access.Value().Layout();
                const uint64 offset = SegmentHeaderBytes + index * value.segmentBytes;
                const uint64 bytes =
                    index + 1 == value.segmentCount ? value.lastSegmentBytes : value.segmentBytes;
                auto opened = access.Value().TryOpenSegment(
                    index,
                    mutated.View().Slice(static_cast<usize>(offset), static_cast<usize>(bytes)),
                    output.View());
                if (!opened) {
                    CHECK(opened.Error() == ErrorCode::AuthenticationFailed);
                    CHECK(AllZero(output.View()));
                    allOpened = false;
                }
            }

            CHECK(allOpened != changed);
        }
    }
}

// secretstream reader: 변형한 메시지는 AuthenticationFailed로만 거부되고 출력이 지워지며 이후
// 호출도 같은 오류다(sticky). 무작위 길이의 쓰레기 메시지도 같다.
TEST(CryptoFuzzSecretStreamReader) {
    SystemAllocator allocator;
    auto key = SecretStreamKey::TryGenerate();
    REQUIRE(key);
    uint8 header[SecretStreamWriter::HeaderBytes];
    auto writer = SecretStreamWriter::TryCreate(key.Value(), Span<uint8>(header));
    REQUIRE(writer);
    constexpr usize MessageCount = 3;
    const usize sizes[MessageCount] = {0, 40, 300};
    Array<uint8> sealed[MessageCount] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                         Array<uint8>(allocator)};
    for (usize index = 0; index < MessageCount; ++index) {
        uint8 message[300];
        for (usize position = 0; position < sizes[index]; ++position) {
            message[position] = static_cast<uint8>(position + index);
        }

        REQUIRE(sealed[index].TryResize(sizes[index] + SecretStreamWriter::OverheadBytes));
        auto pushed = writer.Value().TryPush(Span<const uint8>(message, sizes[index]), Bytes("ad"),
                                             index + 1 == MessageCount ? SecretStreamTag::Final
                                                                       : SecretStreamTag::Message,
                                             sealed[index].View());
        REQUIRE(pushed && pushed.Value() == sealed[index].Size());
    }

    FuzzRandom random{0x5ec7};
    Array<uint8> mutated(allocator);
    uint8 output[1024];
    for (usize iteration = 0; iteration < 3000; ++iteration) {
        const usize target = random.Below(MessageCount);
        const bool garbage = random.Below(8) == 0;
        if (garbage) {
            REQUIRE(mutated.TryResize(random.Below(400)));
            for (uint8& byte : mutated.View()) {
                byte = static_cast<uint8>(random.Next());
            }
        } else {
            REQUIRE(Mutate(random, sealed[target].View(), mutated));
        }

        auto reader = SecretStreamReader::TryCreate(key.Value(), Span<const uint8>(header));
        REQUIRE(reader);
        bool failed = false;
        for (usize index = 0; index < MessageCount; ++index) {
            const Span<const uint8> input =
                index == target ? Span<const uint8>(mutated.View()) : sealed[index].View();
            memset(output, 0xa5, sizeof(output));
            auto pulled = reader.Value().TryPull(input, Bytes("ad"), Span<uint8>(output));
            if (!pulled) {
                CHECK(pulled.Error() == ErrorCode::AuthenticationFailed);
                CHECK(AllZero(Span<const uint8>(output)));
                failed = true;
                break;
            }
        }

        const bool changed = garbage || !Same(mutated.View(), sealed[target].View());
        CHECK(failed == changed);
        if (failed) {
            CHECK(reader.Value().TryFinish().Error() == ErrorCode::AuthenticationFailed);
        } else {
            CHECK(reader.Value().TryFinish());
        }
    }
}

// Wycheproof 파일을 읽는 JSON 파서: 변형한 문서는 성공하거나 오류를 돌려줄 뿐이다(멈추거나 범위
// 밖을 읽지 않는다. Asan 구성에서 함께 확인된다).
TEST(CryptoFuzzWycheproofJson) {
    SystemAllocator allocator;
    const char* seed =
        "{\"algorithm\":\"AES-GCM\",\"numberOfTests\":2,\"testGroups\":[{\"keySize\":128,\"tests\":"
        "[{\"tcId\":1,\"key\":\"00112233\",\"iv\":\"aabb\",\"msg\":\"\",\"flags\":[\"Ktv\"],"
        "\"result\":\"valid\"},{\"tcId\":2,\"comment\":\"\\u00e9\\n\",\"result\":\"invalid\","
        "\"x\":[1.5e3,-0,true,false,null]}]}]}";
    const Span<const uint8> source = Bytes(seed);
    JsonDocument original(allocator);
    REQUIRE(original.Parse(StringView(seed)).code == JsonErrorCode::None);

    FuzzRandom random{0x150e};
    Array<uint8> mutated(allocator);
    usize parsed = 0;
    for (usize iteration = 0; iteration < 20000; ++iteration) {
        REQUIRE(Mutate(random, source, mutated));
        // 변형을 몇 번 겹쳐 구조가 크게 깨진 입력도 만든다.
        for (usize extra = random.Below(3); extra != 0; --extra) {
            Array<uint8> again(allocator);
            REQUIRE(again.TryAppend(mutated.View()));
            REQUIRE(Mutate(random, again.View(), mutated));
        }

        JsonDocument document(allocator);
        const JsonError error = document.Parse(
            StringView(reinterpret_cast<const char*>(mutated.Data()), mutated.Size()));
        if (error.code == JsonErrorCode::None) {
            ++parsed;
        }
    }

    CHECK(parsed != 0);
}

namespace {

// 고정 키 보안 채널: 서버 비밀키 01..20, ephemeral 21..40(클라이언트)·41..60(서버)이다. 메시지가
// 실행마다 같아 변형 위치와 결과가 seed로 재현된다.
struct FixedChannel {
    uint8 serverSecret[ChannelKeyBytes];
    uint8 clientEphemeral[ChannelKeyBytes];
    uint8 serverEphemeral[ChannelKeyBytes];
    ChannelServerKey serverKey;

    FixedChannel() noexcept {
        for (usize index = 0; index < ChannelKeyBytes; ++index) {
            serverSecret[index] = static_cast<uint8>(0x01 + index);
            clientEphemeral[index] = static_cast<uint8>(0x21 + index);
            serverEphemeral[index] = static_cast<uint8>(0x41 + index);
        }

        KeyId id;
        id.value.high = 1;
        id.value.low = 2;
        auto key = ChannelServerKey::TryImport(id, Span<const uint8>(serverSecret));
        if (key) {
            serverKey = Move(key.Value());
        }
    }

    Result<NoiseHandshake> TryInitiator() const noexcept {
        return NoiseHandshake::TryCreateInitiator(serverKey.PublicKey(), Bytes("fuzz"));
    }

    Result<NoiseHandshake> TryResponder() const noexcept {
        return NoiseHandshake::TryCreateResponder(serverKey, Bytes("fuzz"));
    }

    // 클라이언트 첫 메시지(payload "hello")를 쓴다. 실패하면 0이다.
    usize WriteHello(NoiseHandshake& initiator, Span<uint8> message) const noexcept {
        auto written = crypto::detail::ChannelFactory::TryWriteMessage(
            initiator, Span<const uint8>(clientEphemeral), Bytes("hello"), message);
        return written ? written.Value() : 0;
    }

    // 서버 둘째 메시지(payload "welcome")를 쓴다. 실패하면 0이다.
    usize WriteWelcome(NoiseHandshake& responder, Span<uint8> message) const noexcept {
        auto written = crypto::detail::ChannelFactory::TryWriteMessage(
            responder, Span<const uint8>(serverEphemeral), Bytes("welcome"), message);
        return written ? written.Value() : 0;
    }
};

} // namespace

// handshake 메시지 읽기: 변형한 첫 메시지(서버가 읽음)와 둘째 메시지(클라이언트가 읽음)는 원본과
// 같을 때만 받고, 나머지는 AuthenticationFailed(payload 소거, Failed)로만 거부된다.
TEST(CryptoFuzzNoiseHandshakeRead) {
    SystemAllocator allocator;
    const FixedChannel channel;
    REQUIRE(!channel.serverKey.IsEmpty());
    uint8 hello[64];
    uint8 welcome[64];
    uint8 payload[256];
    auto initiator = channel.TryInitiator();
    auto responder = channel.TryResponder();
    REQUIRE(initiator && responder);
    const usize helloBytes = channel.WriteHello(initiator.Value(), Span<uint8>(hello));
    REQUIRE(helloBytes != 0);
    REQUIRE(responder.Value().TryReadMessage(Span<const uint8>(hello).Left(helloBytes),
                                             Span<uint8>(payload)));
    const usize welcomeBytes = channel.WriteWelcome(responder.Value(), Span<uint8>(welcome));
    REQUIRE(welcomeBytes != 0);

    FuzzRandom random{0x401e};
    Array<uint8> mutated(allocator);
    uint8 scratch[64];
    usize accepted = 0;
    for (usize iteration = 0; iteration < 1500; ++iteration) {
        const bool second = random.Below(2) == 1;
        const Span<const uint8> source = second ? Span<const uint8>(welcome).Left(welcomeBytes)
                                                : Span<const uint8>(hello).Left(helloBytes);
        const bool garbage = random.Below(8) == 0;
        if (garbage) {
            REQUIRE(mutated.TryResize(random.Below(200)));
            for (uint8& byte : mutated.View()) {
                byte = static_cast<uint8>(random.Next());
            }
        } else {
            REQUIRE(Mutate(random, source, mutated));
        }

        auto reader = second ? channel.TryInitiator() : channel.TryResponder();
        REQUIRE(reader);
        if (second) {
            REQUIRE(channel.WriteHello(reader.Value(), Span<uint8>(scratch)) == helloBytes);
        }

        memset(payload, 0xa5, sizeof(payload));
        auto read = reader.Value().TryReadMessage(mutated.View(), Span<uint8>(payload));
        const bool changed = garbage || !Same(mutated.View(), source);
        if (read) {
            ++accepted;
            CHECK(!changed);
            CHECK(reader.Value().NextStep() ==
                  (second ? NoiseStep::Split : NoiseStep::WriteMessage));
        } else {
            CHECK(read.Error() == ErrorCode::AuthenticationFailed);
            CHECK(AllZero(Span<const uint8>(payload)));
            CHECK(changed);
            // 서버는 실패하면 버리고, 클라이언트는 읽기 전으로 돌아가 원래 둘째 메시지를 읽을 수
            // 있다.
            if (second) {
                CHECK(reader.Value().NextStep() == NoiseStep::ReadMessage);
                CHECK(reader.Value().TryReadMessage(source, Span<uint8>(payload)).Value() ==
                      welcomeBytes - NoiseMessageOverheadBytes);
            } else {
                CHECK(reader.Value().NextStep() == NoiseStep::Failed);
            }
        }
    }

    CHECK(accepted != 0);
}

// datagram 열기: counter·header·봉인 중 하나를 변형하면 AuthenticationFailed(payload 소거)로만
// 거부되고, 거부된 시도는 창을 바꾸지 않아 원래 datagram이 그 뒤에 열리며, 그다음 재전송은
// 거부된다.
TEST(CryptoFuzzDatagramOpen) {
    SystemAllocator allocator;
    const FixedChannel channel;
    REQUIRE(!channel.serverKey.IsEmpty());
    auto initiator = channel.TryInitiator();
    auto responder = channel.TryResponder();
    REQUIRE(initiator && responder);
    uint8 message[64];
    uint8 payload[512];
    const usize helloBytes = channel.WriteHello(initiator.Value(), Span<uint8>(message));
    REQUIRE(helloBytes != 0);
    REQUIRE(responder.Value().TryReadMessage(Span<const uint8>(message).Left(helloBytes),
                                             Span<uint8>(payload)));
    const usize welcomeBytes = channel.WriteWelcome(responder.Value(), Span<uint8>(message));
    REQUIRE(welcomeBytes != 0);
    REQUIRE(initiator.Value().TryReadMessage(Span<const uint8>(message).Left(welcomeBytes),
                                             Span<uint8>(payload)));
    auto client = initiator.Value().TrySplit();
    auto server = responder.Value().TrySplit();
    REQUIRE(client && server);
    DatagramSealer& sealer = client.Value().send;
    DatagramOpener& opener = server.Value().receive;

    FuzzRandom random{0xd47a};
    Array<uint8> mutated(allocator);
    Array<uint8> mutatedHeader(allocator);
    uint8 plain[300];
    uint8 sealed[512];
    uint8 header[13] = {4, 7, 0, 0, 0};
    for (usize iteration = 0; iteration < 3000; ++iteration) {
        const usize size = random.Below(sizeof(plain));
        for (usize index = 0; index < size; ++index) {
            plain[index] = static_cast<uint8>(index * 13 + iteration);
        }

        StoreLittleEndian<uint64>(header + 5, sealer.NextCounter());
        auto counter = sealer.TrySeal(Span<const uint8>(header), Span<const uint8>(plain, size),
                                      Span<uint8>(sealed));
        REQUIRE(counter);
        const Span<const uint8> original = Span<const uint8>(sealed).Left(size + 16);

        uint64 openCounter = counter.Value();
        mutatedHeader.Clear();
        mutated.Clear();
        REQUIRE(mutatedHeader.TryAppend(Span<const uint8>(header)) && mutated.TryAppend(original));
        switch (random.Below(3)) {
        case 0:
            REQUIRE(Mutate(random, original, mutated));
            break;
        case 1:
            REQUIRE(Mutate(random, Span<const uint8>(header), mutatedHeader));
            break;
        default:
            switch (random.Below(3)) {
            case 0:
                openCounter ^= uint64{1} << random.Below(64);
                break;
            case 1:
                openCounter = counter.Value() - 3 + random.Below(7);
                break;
            default:
                openCounter = random.Next();
                break;
            }

            break;
        }

        const bool changed = openCounter != counter.Value() ||
                             !Same(mutatedHeader.View(), Span<const uint8>(header)) ||
                             !Same(mutated.View(), original);
        memset(payload, 0xa5, sizeof(payload));
        auto opened =
            opener.TryOpen(openCounter, mutatedHeader.View(), mutated.View(), Span<uint8>(payload));
        if (!changed) {
            CHECK(opened && opened.Value() == size);
            CHECK(Same(Span<const uint8>(payload).Left(size), Span<const uint8>(plain, size)));
        } else {
            CHECK(opened.Error() == ErrorCode::AuthenticationFailed);
            CHECK(AllZero(Span<const uint8>(payload)));
            auto retry = opener.TryOpen(counter.Value(), Span<const uint8>(header), original,
                                        Span<uint8>(payload));
            CHECK(retry && retry.Value() == size);
        }

        CHECK(
            opener
                .TryOpen(counter.Value(), Span<const uint8>(header), original, Span<uint8>(payload))
                .Error() == ErrorCode::AuthenticationFailed);
    }

    CHECK(!opener.IsExhausted());
    CHECK(opener.AuthenticationFailures() < 3000);
}
} // namespace crypto_fuzz_tests_file
