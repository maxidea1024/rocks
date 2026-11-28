#include <Compression/Compression.h>
#include <Compression/CompressionStream.h>
#include <Core/Containers/Array.h>
#include <Core/Hash/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Kdf.h>
#include <Crypto/Private/SegmentStreamInternal.h>
#include <Crypto/SegmentStream.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace segment_stream_tests_file {

using namespace rocks::crypto;

// 세그먼트 스트림(Crypto/SegmentStream.h)의 형식·공개 계약 시험이다(Docs/Design/Crypto.ko.md 9.5·
// 9.6·12절, 형식 명세 Docs/Design/CryptoSegmentFormat.ko.md).
// - golden: 고정 salt(모듈 내부 생성 경로 crypto::detail::SegmentStreamFactory)로 만든 벡터 V1–V3이
// 형식
//   명세의 값과 같다. 같은 값을 독립 Python 구현(pyca/cryptography 50.0.2·OpenSSL, hashlib)이 냈다.
//   순차 암호기(출력 창·입력 조각과 무관)와 병렬 writer(역순 봉인)가 같은 byte를 낸다.
// - 왕복: 크기 0·1·S−17·S−16·S−15·2(S−16) 등, 1 byte 출력 창, 무작위 조각, 임의 접근 범위 읽기.
// - 병렬 writer: 여러 스레드가 같은 index를 다투어도 한 번만 봉인한다(AlreadyExists).
// - 압축 결합(9.6절): 압축 → 세그먼트 암호화를 중간 버퍼 하나와 작은 출력 창으로 잇는다.
// 음성 시험(절단·교환·변조·한도)은 SegmentStreamNegativeTests.cpp다.
namespace {

constexpr usize Tag = SegmentTagBytes;

struct HexBytes {
    uint8 bytes[160];
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

bool SameDigest(Span<const uint8> bytes, const char* hex) noexcept {
    const Sha256Digest digest = Sha256::Hash(bytes);
    const HexBytes expected(hex);
    return Same(Span<const uint8>(digest.bytes), expected.View());
}

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

struct TestRandom {
    uint64 state;

    uint64 Next() noexcept {
        state += 0x9e3779b97f4a7c15ull;
        uint64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    usize Range(usize low, usize high) noexcept {
        return low + static_cast<usize>(Next() % (high - low + 1));
    }
};

constexpr KeyId GoldenKeyId = {{0x0123456789abcdefull, 0xfedcba9876543210ull}};

SegmentStreamKey GoldenKey() noexcept {
    uint8 bytes[SegmentStreamKeyBytes];
    for (usize i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<uint8>(i);
    }

    auto key = SegmentStreamKey::TryImport(GoldenKeyId, Span<const uint8>(bytes));
    return Move(key.Value());
}

void GoldenSalt(uint8 (&salt)[32]) noexcept {
    for (usize i = 0; i < sizeof(salt); ++i) {
        salt[i] = static_cast<uint8>(0x40 + i);
    }
}

// 평문 i번째 byte = (31i + 7) mod 256(형식 명세의 golden 평문).
void FillPattern(Array<uint8>& bytes, usize size) noexcept {
    if (!bytes.TryResize(size)) {
        platform::Fatal("SegmentStreamTests: pattern allocation failed");
    }

    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8>(i * 31 + 7);
    }
}

bool AesAvailable() noexcept {
    return TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available;
}

// 입력을 chunk byte씩, 출력을 window byte 창으로 흘려 전체 암호문을 out에 붙인다.
ErrorCode Encrypt(SegmentStreamEncryptor& encryptor, Span<const uint8> plaintext, usize chunk,
                  usize window, Array<uint8>& out) noexcept {
    uint8 buffer[8192];
    const Span<uint8> view(buffer, window < sizeof(buffer) ? window : sizeof(buffer));
    usize offset = 0;
    bool complete = false;
    while (offset < plaintext.Size() || !complete) {
        const usize take = plaintext.Size() - offset < chunk ? plaintext.Size() - offset : chunk;
        auto step = encryptor.TryProcess(plaintext.Slice(offset, take), view);
        if (!step) {
            return step.Error();
        }

        if (!out.TryAppend(Span<const uint8>(buffer, step.Value().produced))) {
            return ErrorCode::OutOfMemory;
        }

        offset += step.Value().consumed;
        complete = step.Value().complete && offset == plaintext.Size();
    }

    for (;;) {
        auto step = encryptor.TryFinish(view);
        if (!step) {
            return step.Error();
        }

        if (!out.TryAppend(Span<const uint8>(buffer, step.Value().produced))) {
            return ErrorCode::OutOfMemory;
        }

        if (step.Value().complete) {
            return ErrorCode::None;
        }
    }
}

// 암호문 전체를 chunk·window로 흘려 평문을 out에 붙이고 TryFinish까지 부른다.
ErrorCode Decrypt(SegmentStreamDecryptor& decryptor, Span<const uint8> ciphertext, usize chunk,
                  usize window, Array<uint8>& out) noexcept {
    uint8 buffer[8192];
    const Span<uint8> view(buffer, window < sizeof(buffer) ? window : sizeof(buffer));
    usize offset = 0;
    bool complete = false;
    while (offset < ciphertext.Size() || !complete) {
        const usize take = ciphertext.Size() - offset < chunk ? ciphertext.Size() - offset : chunk;
        auto step = decryptor.TryProcess(ciphertext.Slice(offset, take), view);
        if (!step) {
            return step.Error();
        }

        if (!out.TryAppend(Span<const uint8>(buffer, step.Value().produced))) {
            return ErrorCode::OutOfMemory;
        }

        offset += step.Value().consumed;
        complete = step.Value().complete && offset == ciphertext.Size();
    }

    for (;;) {
        auto step = decryptor.TryFinish(view);
        if (!step) {
            return step.Error();
        }

        if (!out.TryAppend(Span<const uint8>(buffer, step.Value().produced))) {
            return ErrorCode::OutOfMemory;
        }

        if (step.Value().complete) {
            return ErrorCode::None;
        }
    }
}

// 병렬 writer로 모든 세그먼트를 역순으로 봉인해 전체 암호문을 만든다.
bool SealAll(SegmentStreamParallelWriter& writer, Span<const uint8> header,
             Span<const uint8> plaintext, Array<uint8>& out) noexcept {
    const SegmentLayout& layout = writer.Layout();
    if (!out.TryResize(static_cast<usize>(layout.lastSegmentOffset + layout.lastSegmentBytes))) {
        return false;
    }

    memcpy(out.Data(), header.Data(), SegmentHeaderBytes);
    const usize capacity = static_cast<usize>(layout.segmentBytes) - Tag;
    for (uint64 i = layout.segmentCount; i-- > 0;) {
        const usize offset = static_cast<usize>(i) * capacity;
        const usize bytes = i + 1 == layout.segmentCount ? plaintext.Size() - offset : capacity;
        const usize at = SegmentHeaderBytes + static_cast<usize>(i * layout.segmentBytes);
        auto sealed =
            writer.TrySealSegment(i, plaintext.Slice(offset, bytes), out.View().RightChop(at));
        if (!sealed || sealed.Value() != bytes + Tag) {
            return false;
        }
    }

    return true;
}

// 임의 접근으로 전체 평문을 세그먼트마다 연다.
ErrorCode OpenAll(const SegmentStreamKey& key, Span<const uint8> associatedData,
                  Span<const uint8> ciphertext, Array<uint8>& out) noexcept {
    auto info = TryPeekSegmentHeader(ciphertext.Left(SegmentHeaderBytes));
    if (!info) {
        return info.Error();
    }

    auto layout = TryGetSegmentLayout(info.Value().Params(), ciphertext.Size());
    if (!layout) {
        return layout.Error();
    }

    const SegmentLayout& found = layout.Value();
    auto reader = SegmentStreamRandomAccess::TryOpen(
        key, associatedData, ciphertext.Left(SegmentHeaderBytes),
        ciphertext.Slice(static_cast<usize>(found.lastSegmentOffset),
                         static_cast<usize>(found.lastSegmentBytes)),
        ciphertext.Size());
    if (!reader) {
        return reader.Error();
    }

    if (!out.TryResize(static_cast<usize>(reader.Value().PlaintextBytes()))) {
        return ErrorCode::OutOfMemory;
    }

    const usize capacity = static_cast<usize>(found.segmentBytes) - Tag;
    for (uint64 i = 0; i < found.segmentCount; ++i) {
        const usize at = SegmentHeaderBytes + static_cast<usize>(i * found.segmentBytes);
        const usize bytes = i + 1 == found.segmentCount ? static_cast<usize>(found.lastSegmentBytes)
                                                        : static_cast<usize>(found.segmentBytes);
        auto opened = reader.Value().TryOpenSegment(i, ciphertext.Slice(at, bytes),
                                                    out.View().RightChop(i * capacity));
        if (!opened) {
            return opened.Error();
        }
    }

    return ErrorCode::None;
}

struct GoldenVector {
    const char* name;
    SegmentStreamAlgorithm algorithm;
    const char* associatedData;
    usize plaintextBytes;
    const char* header;
    const char* tags[2];
    const char* sha256;
};

// Docs/Design/CryptoSegmentFormat.ko.md 5절의 벡터다. 키 00 01 .. 1f, 키 ID
// 0123456789abcdef-fedcba9876543210, salt 40 41 .. 5f, log2 S = 12.
constexpr GoldenVector GoldenVectors[] = {
    {"V1",
     SegmentStreamAlgorithm::ChaCha20Poly1305,
     "",
     0,
     "5253454701010c000123456789abcdeffedcba9876543210404142434445464748494a4b4c4d4e4f5051525354"
     "55565758595a5b5c5d5e5f77a63a977eca576fd9d3d645a4358d41d89667114daab63804817cbe09eae763",
     {"ba16bc44eff93960faf1d5a7cd419495", nullptr},
     "879f9ed25bf61c0dd6c5db7efc59ef4dbf0ea2e8c0fa8389b3c467a8843e61b8"},
    {"V2",
     SegmentStreamAlgorithm::ChaCha20Poly1305,
     "rocks segment golden",
     5000,
     "5253454701010c000123456789abcdeffedcba9876543210404142434445464748494a4b4c4d4e4f5051525354"
     "55565758595a5b5c5d5e5fd01846f14ffce5928c0ef95bfb3cf0835ef96a5cdd3a74ab4d6800dafc495169",
     {"976de92e42577bf775997f8de9f4751b", "e5821fbfbd3ef3e1aa7974d395f64a4a"},
     "4f32a3cbc5f77ac29b5916bb568f75778e15467c67469532eb0f670c47304150"},
    {"V3",
     SegmentStreamAlgorithm::Aes256Gcm,
     "rocks segment golden",
     8160,
     "5253454701020c000123456789abcdeffedcba9876543210404142434445464748494a4b4c4d4e4f5051525354"
     "55565758595a5b5c5d5e5f990f2f666b31ef3ea56b4f8b25288841702bfb0a560875007f0c702d516debab",
     {"c0b7dacaff2aff41012a04e47dc6ea4d", "830ec24514f984190a62bbf6e2cc4a7d"},
     "bbd90ee3e25d07749c611c00f2ebbc46167bfb378d2698d984d62c549ae51141"},
};

constexpr const char* GoldenV1Full =
    "5253454701010c000123456789abcdeffedcba9876543210404142434445464748494a4b4c4d4e4f505152535455"
    "565758595a5b5c5d5e5f77a63a977eca576fd9d3d645a4358d41d89667114daab63804817cbe09eae763ba16bc44"
    "eff93960faf1d5a7cd419495";

} // namespace

TEST(CryptoSegmentStreamGolden) {
    SystemAllocator allocator;
    const SegmentStreamKey key = GoldenKey();
    uint8 salt[32];
    GoldenSalt(salt);
    for (const GoldenVector& vector : GoldenVectors) {
        const SegmentStreamParams params{vector.algorithm, 12};
        const Span<const uint8> ad = Text(vector.associatedData);
        Array<uint8> plaintext(allocator);
        FillPattern(plaintext, vector.plaintextBytes);
        auto encryptor = crypto::detail::SegmentStreamFactory::TryCreateEncryptor(
            key, params, ad, Span<const uint8>(salt), GetSecretAllocator());
        if (vector.algorithm == SegmentStreamAlgorithm::Aes256Gcm && !AesAvailable()) {
            CHECK(encryptor.Error() == ErrorCode::Unsupported);
            printf("CryptoSegmentStreamGolden: %s skipped (no AES hardware)\n", vector.name);
            continue;
        }

        REQUIRE(encryptor);
        Array<uint8> ciphertext(allocator);
        REQUIRE(Encrypt(encryptor.Value(), plaintext.View(), 1 << 20, 1 << 20, ciphertext) ==
                ErrorCode::None);
        const HexBytes header(vector.header);
        CHECK(Same(ciphertext.View().Left(SegmentHeaderBytes), header.View()));
        CHECK(SameDigest(ciphertext.View(), vector.sha256));
        auto layout = TryGetSegmentLayout(params, ciphertext.Size());
        REQUIRE(layout);
        for (uint64 i = 0; i < layout.Value().segmentCount; ++i) {
            const usize end = i + 1 == layout.Value().segmentCount
                                  ? ciphertext.Size()
                                  : SegmentHeaderBytes + static_cast<usize>((i + 1) * 4096);
            const HexBytes tag(vector.tags[i]);
            CHECK(Same(ciphertext.View().Slice(end - Tag, Tag), tag.View()));
        }

        if (vector.plaintextBytes == 0) {
            const HexBytes full(GoldenV1Full);
            CHECK(Same(ciphertext.View(), full.View()));
        }

        // 입력 조각·출력 창이 달라도 같은 byte다(1 byte 출력 창 포함).
        auto narrow = crypto::detail::SegmentStreamFactory::TryCreateEncryptor(
            key, params, ad, Span<const uint8>(salt), allocator);
        REQUIRE(narrow);
        Array<uint8> narrowed(allocator);
        REQUIRE(Encrypt(narrow.Value(), plaintext.View(), 7, 1, narrowed) == ErrorCode::None);
        CHECK(Same(narrowed.View(), ciphertext.View()));

        // 병렬 writer도 같은 byte를 낸다.
        uint8 parallelHeader[SegmentHeaderBytes];
        auto writer = crypto::detail::SegmentStreamFactory::TryCreateParallelWriter(
            key, params, ad, plaintext.Size(), Span<const uint8>(salt), Span<uint8>(parallelHeader),
            allocator);
        REQUIRE(writer);
        Array<uint8> parallel(allocator);
        REQUIRE(
            SealAll(writer.Value(), Span<const uint8>(parallelHeader), plaintext.View(), parallel));
        CHECK(Same(parallel.View(), ciphertext.View()));

        // 순차(1 byte 창)·임의 접근 복호가 원래 평문을 낸다.
        auto decryptor = SegmentStreamDecryptor::TryCreate(key, ad);
        REQUIRE(decryptor);
        Array<uint8> decrypted(allocator);
        CHECK(Decrypt(decryptor.Value(), ciphertext.View(), 5, 1, decrypted) == ErrorCode::None);
        CHECK(Same(decrypted.View(), plaintext.View()));
        Array<uint8> opened(allocator);
        CHECK(OpenAll(key, ad, ciphertext.View(), opened) == ErrorCode::None);
        CHECK(Same(opened.View(), plaintext.View()));
    }
}

TEST(CryptoSegmentStreamRoundTripSizes) {
    SystemAllocator allocator;
    TestRandom random{0x5e9};
    auto id = TryNewKeyId();
    REQUIRE(id);
    auto key = SegmentStreamKey::TryGenerate(id.Value());
    REQUIRE(key);
    const SegmentStreamAlgorithm algorithms[2] = {SegmentStreamAlgorithm::ChaCha20Poly1305,
                                                  SegmentStreamAlgorithm::Aes256Gcm};
    constexpr usize S = 4096;
    const usize sizes[] = {
        0, 1, S - 17, S - 16, S - 15, 2 * (S - 16), 2 * (S - 16) + 1, 3 * (S - 16) - 1, 20000};
    for (SegmentStreamAlgorithm algorithm : algorithms) {
        if (algorithm == SegmentStreamAlgorithm::Aes256Gcm && !AesAvailable()) {
            CHECK(SegmentStreamEncryptor::TryCreate(key.Value(), {algorithm, 12}, {}).Error() ==
                  ErrorCode::Unsupported);
            continue;
        }

        const SegmentStreamParams params{algorithm, 12};
        for (usize size : sizes) {
            Array<uint8> plaintext(allocator);
            FillPattern(plaintext, size);
            auto encryptor = SegmentStreamEncryptor::TryCreate(key.Value(), params, Text("ad"));
            REQUIRE(encryptor);
            Array<uint8> ciphertext(allocator);
            REQUIRE(Encrypt(encryptor.Value(), plaintext.View(), random.Range(1, 9000),
                            random.Range(1, 9000), ciphertext) == ErrorCode::None);
            auto expectedBytes = TryGetSegmentStreamCiphertextBytes(params, size);
            REQUIRE(expectedBytes);
            CHECK(ciphertext.Size() == expectedBytes.Value());

            auto decryptor = SegmentStreamDecryptor::TryCreate(key.Value(), Text("ad"));
            REQUIRE(decryptor);
            Array<uint8> decrypted(allocator);
            CHECK(Decrypt(decryptor.Value(), ciphertext.View(), random.Range(1, 9000),
                          random.Range(1, 9000), decrypted) == ErrorCode::None);
            CHECK(Same(decrypted.View(), plaintext.View()));

            Array<uint8> opened(allocator);
            CHECK(OpenAll(key.Value(), Text("ad"), ciphertext.View(), opened) == ErrorCode::None);
            CHECK(Same(opened.View(), plaintext.View()));

            uint8 header[SegmentHeaderBytes];
            auto writer = SegmentStreamParallelWriter::TryCreate(key.Value(), params, Text("ad"),
                                                                 size, Span<uint8>(header));
            REQUIRE(writer);
            Array<uint8> parallel(allocator);
            REQUIRE(SealAll(writer.Value(), Span<const uint8>(header), plaintext.View(), parallel));
            CHECK(parallel.Size() == ciphertext.Size());
            Array<uint8> reopened(allocator);
            CHECK(OpenAll(key.Value(), Text("ad"), parallel.View(), reopened) == ErrorCode::None);
            CHECK(Same(reopened.View(), plaintext.View()));
        }
    }
}

TEST(CryptoSegmentStreamRandomAccessRanges) {
    SystemAllocator allocator;
    const SegmentStreamKey key = GoldenKey();
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    constexpr usize Capacity = 4096 - Tag;
    Array<uint8> plaintext(allocator);
    FillPattern(plaintext, 3 * Capacity + 100);
    auto encryptor = SegmentStreamEncryptor::TryCreate(key, params, {});
    REQUIRE(encryptor);
    Array<uint8> ciphertext(allocator);
    REQUIRE(Encrypt(encryptor.Value(), plaintext.View(), 1000, 1000, ciphertext) ==
            ErrorCode::None);
    auto layout = TryGetSegmentLayout(params, ciphertext.Size());
    REQUIRE(layout);
    CHECK(layout.Value().segmentCount == 4 && layout.Value().lastSegmentBytes == 100 + Tag &&
          layout.Value().plaintextBytes == plaintext.Size());
    auto reader = SegmentStreamRandomAccess::TryOpen(
        key, {}, ciphertext.View().Left(SegmentHeaderBytes),
        ciphertext.View().RightChop(static_cast<usize>(layout.Value().lastSegmentOffset)),
        ciphertext.Size());
    REQUIRE(reader);
    CHECK(reader.Value().PlaintextBytes() == plaintext.Size() &&
          reader.Value().SegmentCount() == 4);

    // 범위 계산: 세그먼트 경계를 걸친 범위, 마지막 세그먼트까지의 범위, 빈 범위.
    auto middle = reader.Value().TryMapPlaintextRange(Capacity - 10, 20);
    REQUIRE(middle);
    CHECK(middle.Value().firstSegment == 0 && middle.Value().segmentCount == 2 &&
          middle.Value().ciphertextOffset == SegmentHeaderBytes &&
          middle.Value().ciphertextBytes == 2 * 4096 &&
          middle.Value().firstSegmentOffset == Capacity - 10);
    auto tail = reader.Value().TryMapPlaintextRange(2 * Capacity + 5, Capacity + 95);
    REQUIRE(tail);
    CHECK(tail.Value().firstSegment == 2 && tail.Value().segmentCount == 2 &&
          tail.Value().ciphertextOffset == SegmentHeaderBytes + 2 * 4096 &&
          tail.Value().ciphertextBytes == 4096 + 100 + Tag && tail.Value().firstSegmentOffset == 5);
    auto empty = reader.Value().TryMapPlaintextRange(plaintext.Size(), 0);
    REQUIRE(empty);
    CHECK(empty.Value().segmentCount == 0);
    CHECK(reader.Value().TryMapPlaintextRange(plaintext.Size(), 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(reader.Value().TryMapPlaintextRange(~uint64{0}, 2).Error() == ErrorCode::InvalidArgument);

    // 매핑한 구간만 읽어 범위 평문을 꺼낸다.
    TestRandom random{77};
    for (usize trial = 0; trial < 50; ++trial) {
        const usize offset = random.Range(0, plaintext.Size() - 1);
        const usize bytes = random.Range(1, plaintext.Size() - offset);
        auto range = reader.Value().TryMapPlaintextRange(offset, bytes);
        REQUIRE(range);
        Array<uint8> collected(allocator);
        uint64 at = range.Value().ciphertextOffset;
        for (uint64 i = 0; i < range.Value().segmentCount; ++i) {
            const uint64 index = range.Value().firstSegment + i;
            const usize segmentBytes = index + 1 == layout.Value().segmentCount
                                           ? static_cast<usize>(layout.Value().lastSegmentBytes)
                                           : 4096;
            uint8 plain[4096];
            auto opened = reader.Value().TryOpenSegment(
                index, ciphertext.View().Slice(static_cast<usize>(at), segmentBytes),
                Span<uint8>(plain));
            REQUIRE(opened);
            REQUIRE(collected.TryAppend(Span<const uint8>(plain, opened.Value())));
            at += segmentBytes;
        }

        CHECK(at == range.Value().ciphertextOffset + range.Value().ciphertextBytes);
        CHECK(Same(
            collected.View().Slice(static_cast<usize>(range.Value().firstSegmentOffset), bytes),
            plaintext.View().Slice(offset, bytes)));
    }

    // 제자리 복호는 허용되고, 작은 출력·범위 밖 index·다른 겹침은 쓰기 전에 거부한다.
    uint8 segment[4096];
    memcpy(segment, ciphertext.Data() + SegmentHeaderBytes, sizeof(segment));
    uint8 small[16];
    CHECK(
        reader.Value().TryOpenSegment(0, Span<const uint8>(segment), Span<uint8>(small)).Error() ==
        ErrorCode::CapacityExceeded);
    CHECK(reader.Value()
              .TryOpenSegment(4, Span<const uint8>(segment), Span<uint8>(segment))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(reader.Value()
              .TryOpenSegment(0, Span<const uint8>(segment), Span<uint8>(segment + 1, 4080))
              .Error() == ErrorCode::InvalidArgument);
    auto inPlace =
        reader.Value().TryOpenSegment(0, Span<const uint8>(segment), Span<uint8>(segment));
    REQUIRE(inPlace);
    CHECK(Same(Span<const uint8>(segment, Capacity), plaintext.View().Left(Capacity)));
}

namespace {

struct RaceContext {
    SegmentStreamParallelWriter* writer;
    const uint8* plaintext;
    uint8* output;
    usize segments;
    usize sealed = 0;
    usize duplicates = 0;
    usize other = 0;
};

void RaceSeal(void* context) noexcept {
    auto& race = *static_cast<RaceContext*>(context);
    constexpr usize Capacity = 4096 - Tag;
    for (usize i = 0; i < race.segments; ++i) {
        const usize at = SegmentHeaderBytes + i * 4096;
        auto result = race.writer->TrySealSegment(
            i, Span<const uint8>(race.plaintext + i * Capacity, Capacity),
            Span<uint8>(race.output + at, 4096));
        if (result) {
            ++race.sealed;
        } else if (result.Error() == ErrorCode::AlreadyExists) {
            ++race.duplicates;
        } else {
            ++race.other;
        }
    }
}

} // namespace

TEST(CryptoSegmentStreamParallelWriterClaims) {
    SystemAllocator allocator;
    const SegmentStreamKey key = GoldenKey();
    const SegmentStreamParams params{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    constexpr usize Capacity = 4096 - Tag;
    constexpr usize Segments = 64;
    Array<uint8> plaintext(allocator);
    FillPattern(plaintext, Segments * Capacity);
    uint8 header[SegmentHeaderBytes];
    auto writer = SegmentStreamParallelWriter::TryCreate(key, params, Text("race"),
                                                         plaintext.Size(), Span<uint8>(header));
    REQUIRE(writer);
    Array<uint8> ciphertext(allocator);
    REQUIRE(ciphertext.TryResize(SegmentHeaderBytes + Segments * 4096));
    memcpy(ciphertext.Data(), header, SegmentHeaderBytes);

    // 같은 index를 네 스레드가 동시에 봉인하려 해도 index마다 한 번만 성공한다. 모두 같은 평문을
    // 봉인하므로 같은 자리에 같은 byte를 쓴다.
    RaceContext contexts[4];
    platform::Thread threads[4];
    for (usize i = 0; i < 4; ++i) {
        contexts[i] = RaceContext{&writer.Value(), plaintext.Data(), ciphertext.Data(), Segments};
    }

    // 출력은 claim에 성공한 스레드만 쓰므로(claim 뒤에만 봉인) 한 자리에 한 스레드만 쓴다.
    for (usize i = 0; i < 4; ++i) {
        if (threads[i].Start(RaceSeal, &contexts[i]) != platform::ThreadStatus::Success) {
            platform::Fatal("SegmentStreamTests: race thread failed to start");
        }
    }

    for (platform::Thread& thread : threads) {
        if (thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("SegmentStreamTests: race thread failed to join");
        }
    }

    usize sealed = 0;
    usize duplicates = 0;
    for (const RaceContext& context : contexts) {
        sealed += context.sealed;
        duplicates += context.duplicates;
        CHECK(context.other == 0);
    }

    CHECK(sealed == Segments && duplicates == 3 * Segments);
    Array<uint8> opened(allocator);
    CHECK(OpenAll(key, Text("race"), ciphertext.View(), opened) == ErrorCode::None);
    CHECK(Same(opened.View(), plaintext.View()));

    // 다시 봉인은 AlreadyExists, 길이 오류·범위 밖·작은 출력은 claim하지 않는다.
    uint8 out[4096];
    CHECK(writer.Value()
              .TrySealSegment(0, plaintext.View().Left(Capacity), Span<uint8>(out))
              .Error() == ErrorCode::AlreadyExists);
    uint8 fresh[SegmentHeaderBytes];
    auto second = SegmentStreamParallelWriter::TryCreate(key, params, {}, 2 * Capacity + 1,
                                                         Span<uint8>(fresh));
    REQUIRE(second);
    CHECK(second.Value().Layout().segmentCount == 3);
    CHECK(second.Value()
              .TrySealSegment(0, plaintext.View().Left(Capacity - 1), Span<uint8>(out))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(second.Value().TrySealSegment(3, plaintext.View().Left(1), Span<uint8>(out)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(second.Value()
              .TrySealSegment(0, plaintext.View().Left(Capacity), Span<uint8>(out, 4095))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(second.Value().TrySealSegment(0, plaintext.View().Left(Capacity), Span<uint8>(out)));
    CHECK(second.Value().TrySealSegment(2, plaintext.View().Left(1), Span<uint8>(out)).Value() ==
          1 + Tag);

    // 제자리 봉인은 허용하고 그 밖의 겹침은 거부한다.
    uint8 inPlace[4096];
    memcpy(inPlace, plaintext.Data(), Capacity);
    CHECK(second.Value()
              .TrySealSegment(1, Span<const uint8>(inPlace + 1, Capacity), Span<uint8>(inPlace))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(second.Value().TrySealSegment(1, Span<const uint8>(inPlace, Capacity),
                                        Span<uint8>(inPlace)));
}

TEST(CryptoSegmentStreamHeaderAndLayout) {
    const SegmentStreamKey key = GoldenKey();
    const HexBytes header(GoldenVectors[0].header);
    auto info = TryPeekSegmentHeader(header.View());
    REQUIRE(info);
    CHECK(info.Value().algorithm == SegmentStreamAlgorithm::ChaCha20Poly1305 &&
          info.Value().segmentSizeLog2 == 12 && info.Value().keyId == GoldenKeyId);

    // byte 순서대로 판정한다.
    uint8 bytes[SegmentHeaderBytes];
    const auto peek = [&](usize index, uint8 value, usize size) {
        memcpy(bytes, header.View().Data(), SegmentHeaderBytes);
        bytes[index] = value;
        return TryPeekSegmentHeader(Span<const uint8>(bytes, size)).Error();
    };
    CHECK(peek(0, 'X', SegmentHeaderBytes) == ErrorCode::AuthenticationFailed);
    CHECK(peek(4, 2, SegmentHeaderBytes) == ErrorCode::Unsupported);
    CHECK(peek(4, 2, 5) == ErrorCode::Unsupported);
    CHECK(peek(4, 1, 4) == ErrorCode::AuthenticationFailed);
    CHECK(peek(4, 1, SegmentHeaderBytes - 1) == ErrorCode::AuthenticationFailed);
    CHECK(peek(5, 3, SegmentHeaderBytes) == ErrorCode::Unsupported);
    CHECK(peek(5, 0, SegmentHeaderBytes) == ErrorCode::Unsupported);
    CHECK(peek(6, 11, SegmentHeaderBytes) == ErrorCode::AuthenticationFailed);
    CHECK(peek(6, 25, SegmentHeaderBytes) == ErrorCode::AuthenticationFailed);
    CHECK(peek(7, 1, SegmentHeaderBytes) == ErrorCode::AuthenticationFailed);
    memcpy(bytes, header.View().Data(), SegmentHeaderBytes);
    memset(bytes + 8, 0, 16);
    CHECK(TryPeekSegmentHeader(Span<const uint8>(bytes)).Error() ==
          ErrorCode::AuthenticationFailed);

    // 크기 산술.
    const SegmentStreamParams chacha{SegmentStreamAlgorithm::ChaCha20Poly1305, 12};
    const SegmentStreamParams aes{SegmentStreamAlgorithm::Aes256Gcm, 12};
    CHECK(TryGetSegmentLayout(chacha, 88 + 15).Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryGetSegmentLayout(chacha, 88 + 16).Value().plaintextBytes == 0);
    CHECK(TryGetSegmentLayout(chacha, 88 + 4096).Value().segmentCount == 1);
    CHECK(TryGetSegmentLayout(chacha, 88 + 4096 + 16).Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryGetSegmentLayout(chacha, 88 + 4096 + 17).Value().plaintextBytes == 4081);
    CHECK(TryGetSegmentLayout({chacha.algorithm, 11}, 200).Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetSegmentLayout({static_cast<SegmentStreamAlgorithm>(3), 12}, 200).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryGetSegmentStreamMaxPlaintextBytes(chacha).Value() == (uint64{1} << 32) * 4080);
    CHECK(TryGetSegmentStreamMaxPlaintextBytes(aes).Value() ==
          (uint64{1} << 40) - (uint64{1} << 32) - 16);
    CHECK(TryGetSegmentStreamMaxPlaintextBytes({aes.algorithm, 16}).Value() ==
          (uint64{1} << 40) - (uint64{1} << 28) - 16);
    CHECK(TryGetSegmentStreamCiphertextBytes(chacha, 0).Value() == 88 + 16);
    CHECK(TryGetSegmentStreamCiphertextBytes(chacha, 4080).Value() == 88 + 4096);
    CHECK(TryGetSegmentStreamCiphertextBytes(chacha, 4081).Value() == 88 + 4096 + 17);
    CHECK(TryGetSegmentStreamCiphertextBytes(chacha, (uint64{1} << 32) * 4080 + 1).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(TryGetSegmentStreamCiphertextBytes({chacha.algorithm, 25}, 1).Error() ==
          ErrorCode::InvalidArgument);
    (void)key;
}

TEST(CryptoSegmentStreamKeysAndLifetime) {
    SystemAllocator allocator;
    CHECK(SegmentStreamKey::TryGenerate(KeyId()).Error() == ErrorCode::InvalidArgument);
    uint8 bytes[SegmentStreamKeyBytes] = {};
    CHECK(SegmentStreamKey::TryImport(GoldenKeyId, Span<const uint8>(bytes, 31)).Error() ==
          ErrorCode::InvalidArgument);
    SegmentStreamKey key = GoldenKey();
    CHECK(key.Id() == GoldenKeyId);
    uint8 exported[SegmentStreamKeyBytes];
    REQUIRE(hazmat::TryExportKey(key, Span<uint8>(exported)));
    CHECK(exported[0] == 0 && exported[31] == 31);

    uint8 masterBytes[KdfKeyBytes] = {9};
    auto master = KdfKey::TryImport(Span<const uint8>(masterBytes));
    REQUIRE(master);
    auto derived =
        TryDeriveSegmentStreamKey(master.Value(), 3, KdfContext("RkTstSeg"), GoldenKeyId);
    REQUIRE(derived);
    CHECK(derived.Value().Id() == GoldenKeyId);
    CHECK(TryDeriveSegmentStreamKey(master.Value(), 3, KdfContext("RkTstSeg"), KeyId()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryDeriveSegmentStreamKey(KdfKey(), 3, KdfContext("RkTstSeg"), GoldenKeyId).Error() ==
          ErrorCode::StaleHandle);

    // 범위 밖 매개변수는 InvalidArgument다.
    CHECK(SegmentStreamEncryptor::TryCreate(key, {SegmentStreamAlgorithm::ChaCha20Poly1305, 25}, {})
              .Error() == ErrorCode::InvalidArgument);
    CHECK(SegmentStreamEncryptor::TryCreate(key, {static_cast<SegmentStreamAlgorithm>(0), 12}, {})
              .Error() == ErrorCode::InvalidArgument);
    uint8 header[SegmentHeaderBytes];
    CHECK(SegmentStreamParallelWriter::TryCreate(key, {}, {}, 1, Span<uint8>(header, 87)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(SegmentStreamParallelWriter::TryCreate(key,
                                                 {SegmentStreamAlgorithm::ChaCha20Poly1305, 12}, {},
                                                 (uint64{1} << 32) * 4080 + 1, Span<uint8>(header))
              .Error() == ErrorCode::CapacityExceeded);

    // 이동 뒤 원본은 StaleHandle, Reset 뒤는 빈 객체다.
    auto encryptor = SegmentStreamEncryptor::TryCreate(key, {}, {}, allocator);
    REQUIRE(encryptor);
    SegmentStreamEncryptor moved = Move(encryptor.Value());
    uint8 out[128];
    CHECK(encryptor.Value().TryProcess({}, Span<uint8>(out)).Error() == ErrorCode::StaleHandle);
    CHECK(encryptor.Value().TryFinish(Span<uint8>(out)).Error() == ErrorCode::StaleHandle);
    CHECK(moved.TryProcess(Span<const uint8>(out, 4), Span<uint8>(out + 4, 4)));
    CHECK(moved.TryProcess(Span<const uint8>(out, 4), Span<uint8>(out + 2, 4)).Error() ==
          ErrorCode::InvalidArgument);
    moved.Reset();
    CHECK(moved.IsEmpty());

    auto decryptor = SegmentStreamDecryptor::TryCreate(key, {}, allocator);
    REQUIRE(decryptor);
    SegmentStreamDecryptor movedDecryptor = Move(decryptor.Value());
    CHECK(decryptor.Value().TryFinish(Span<uint8>(out)).Error() == ErrorCode::StaleHandle);
    CHECK(!movedDecryptor.IsEmpty());

    auto writer = SegmentStreamParallelWriter::TryCreate(key, {}, {}, 10, Span<uint8>(header));
    REQUIRE(writer);
    SegmentStreamParallelWriter movedWriter = Move(writer.Value());
    CHECK(writer.Value()
              .TrySealSegment(0, Span<const uint8>(out, 10), Span<uint8>(out + 10, 26))
              .Error() == ErrorCode::StaleHandle);
    CHECK(movedWriter.TrySealSegment(0, Span<const uint8>(out, 10), Span<uint8>(out + 10, 26)));

    SegmentStreamRandomAccess emptyReader;
    CHECK(emptyReader.TryMapPlaintextRange(0, 0).Error() == ErrorCode::StaleHandle);
    CHECK(emptyReader.TryOpenSegment(0, {}, {}).Error() == ErrorCode::StaleHandle);

    SegmentStreamKey movedKey = Move(key);
    CHECK(key.IsEmpty() && !movedKey.IsEmpty());
    CHECK(SegmentStreamEncryptor::TryCreate(key, {}, {}).Error() == ErrorCode::StaleHandle);
    CHECK(SegmentStreamDecryptor::TryCreate(key, {}).Error() == ErrorCode::StaleHandle);
    CHECK(SegmentStreamParallelWriter::TryCreate(key, {}, {}, 1, Span<uint8>(header)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(SegmentStreamRandomAccess::TryOpen(key, {}, {}, {}, 0).Error() == ErrorCode::StaleHandle);
}

TEST(CryptoSegmentStreamWithCompression) {
    // 9.6절: 압축 다음 암호화. 두 스트림이 같은 StreamProgress 역압 규약을 쓰므로 중간 버퍼 하나와
    // 작은 출력 창으로 잇는다. 복호 다음 해제로 원래 입력이 돌아온다.
    SystemAllocator allocator;
    const SegmentStreamKey key = GoldenKey();
    Array<uint8> input(allocator);
    REQUIRE(input.TryResize(30000));
    for (usize i = 0; i < input.Size(); ++i) {
        input[i] = static_cast<uint8>("rocks segment stream "[i % 21]);
    }

    auto settings = CompressionSettings::TryDefault(CompressionFormat::Lz4Frame);
    REQUIRE(settings);
    auto compress = CompressStream::TryCreate(settings.Value(), allocator);
    REQUIRE(compress);
    auto encryptor =
        SegmentStreamEncryptor::TryCreate(key, {SegmentStreamAlgorithm::ChaCha20Poly1305, 12}, {});
    REQUIRE(encryptor);

    uint8 stage[300];
    uint8 window[97];
    Array<uint8> sealed(allocator);
    // 압축 출력(stage)을 모두 암호화로 흘린다. 암호화 출력은 작은 창으로 받는다.
    const auto sealStage = [&](usize produced) {
        Span<const uint8> sealing = Span<const uint8>(stage).Left(produced);
        while (!sealing.IsEmpty()) {
            auto step = encryptor.Value().TryProcess(sealing, Span<uint8>(window));
            if (!step || !sealed.TryAppend(Span<const uint8>(window, step.Value().produced))) {
                return false;
            }

            sealing = sealing.RightChop(step.Value().consumed);
        }

        return true;
    };

    Span<const uint8> pending = input.View();
    while (!pending.IsEmpty()) {
        auto packed = compress.Value().TryProcess(pending, Span<uint8>(stage));
        REQUIRE(packed);
        pending = pending.RightChop(packed.Value().consumed);
        REQUIRE(sealStage(packed.Value().produced));
    }

    for (;;) {
        auto packed = compress.Value().TryFinish(Span<uint8>(stage));
        REQUIRE(packed);
        REQUIRE(sealStage(packed.Value().produced));
        if (packed.Value().complete) {
            break;
        }
    }

    for (;;) {
        auto step = encryptor.Value().TryFinish(Span<uint8>(window));
        REQUIRE(step);
        REQUIRE(sealed.TryAppend(Span<const uint8>(window, step.Value().produced)));
        if (step.Value().complete) {
            break;
        }
    }

    auto decryptor = SegmentStreamDecryptor::TryCreate(key, {});
    REQUIRE(decryptor);
    Array<uint8> packedBack(allocator);
    REQUIRE(Decrypt(decryptor.Value(), sealed.View(), 333, 129, packedBack) == ErrorCode::None);
    DecompressStreamLimits limits;
    limits.maxOutputBytes = input.Size();
    auto decompress = DecompressStream::TryCreate(CompressionFormat::Lz4Frame, limits, allocator);
    REQUIRE(decompress);
    Array<uint8> restored(allocator);
    REQUIRE(decompress.Value().TryWriteAppend(packedBack.View(), restored));
    CHECK(decompress.Value().TryFinish());
    CHECK(Same(restored.View(), input.View()));
}
} // namespace segment_stream_tests_file
