#include <AssetNugget/NuggetFile.h>
#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>
#include <Core/Memory/SystemAllocator.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// `.rnug` v2 파일 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 6): 쓰기 계획·붙이기·열기
// 왕복(작은 값·1 MiB 이상 값의 조각 해시 표·Rcbk 값), 머리·Record byte 변조 위치마다 거부, 값·표
// 변조, 잘림·남는 byte, 다른 버전 Unsupported, 값 읽기·해제 검증(RawHash·IndexHash·Codec 계약),
// 계획 입력 거부, 범위 reader 경계, 할당 실패.
using namespace rocks;
namespace nugget_file_tests_file {

namespace {

Sha256Digest Digest(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

// 결정적 byte 열이다.
Array<uint8> Pattern(Allocator& allocator, usize size, uint8 seed) noexcept {
    Array<uint8> bytes(allocator);
    CHECK(bytes.TryResize(size));
    for (usize index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8>(seed + index * 31 + (index >> 8));
    }

    return bytes;
}

NuggetStoredValue IdentityValue(uint32 id, Span<const uint8> raw) noexcept {
    NuggetStoredValue value;
    value.id = id;
    value.rawSize = raw.Size();
    value.rawHash = Sha256::Hash(raw);
    value.stored = raw;
    return value;
}

// raw를 RCBK(Zstd)로 압축한 저장 byte와 그 값이다.
NuggetStoredValue RcbkValue(Allocator& allocator, uint32 id, Span<const uint8> raw,
                            Array<uint8>& stored) noexcept {
    auto compression = CompressionSettings::TryDefault(CompressionFormat::Zstd);
    CHECK(compression);
    auto settings = CompressedBlocksSettings::TryMake(compression.Value(), 16);
    CHECK(settings);
    CHECK(TryCompressBlocksAppend(settings.Value(), raw, stored, allocator));
    auto reader = CompressedBlocksReader::TryOpen(stored.View(), allocator);
    CHECK(reader);
    NuggetStoredValue value;
    value.id = id;
    value.encoding = NuggetEncoding::Rcbk;
    value.rawSize = raw.Size();
    value.rawHash = Sha256::Hash(raw);
    value.indexLength = reader.Value().IndexBytes();
    value.indexHash = Sha256::Hash(stored.View().Slice(0, reader.Value().IndexBytes()));
    value.stored = stored.View();
    return value;
}

struct Built {
    explicit Built(Allocator& allocator) noexcept
        : small(Pattern(allocator, 37, 1)), large(Pattern(allocator, NuggetChunkBytes + 5, 2)),
          packedRaw(Pattern(allocator, 200000, 3)), packed(allocator), file(allocator) {
        const NuggetStoredValue values[] = {IdentityValue(0, small.View()),
                                            IdentityValue(3, large.View()),
                                            RcbkValue(allocator, 9, packedRaw.View(), packed)};
        NuggetInput input(allocator);
        CHECK(input.bucket.TryAssign("scc"));
        input.key = Digest(5);
        input.digest = Digest(6);
        const NuggetInput inputs[] = {Move(input)};
        const uint8 meta[] = {4, 5};
        auto plan = PlanNuggetFile(allocator, NuggetBucketTexture, Digest(1), values, inputs, meta);
        CHECK(plan);
        if (plan) {
            fileBytes = plan.Value().fileBytes;
            recordBytes = plan.Value().head.Size() - NuggetFileHeadBytes;
            CHECK(AppendNuggetFile(plan.Value(), values, file));
        }
    }

    Array<uint8> small;
    Array<uint8> large;
    Array<uint8> packedRaw;
    Array<uint8> packed;
    Array<uint8> file;
    uint64 fileBytes = 0;
    usize recordBytes = 0;
};

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

} // namespace

// 왕복: 계획 크기 = 파일 크기, 연 Record(행 해시·표 해시·storedSize), digest, 값마다 읽기·해제가
// 원본과 같다(Identity·1 MiB 이상·Rcbk), 머리 golden(magic·버전·Record 크기·Record SHA-256).
TEST(NuggetFile_RoundTrip) {
    SystemAllocator allocator;
    Built built(allocator);
    REQUIRE(built.file.Size() == built.fileBytes);
    const Span<const uint8> file = built.file.View();
    CHECK(file[0] == 'R' && file[1] == 'N' && file[2] == 'U' && file[3] == 'G' &&
          LoadLittleEndian<uint32>(file.Data() + 4) == NuggetFileVersion &&
          LoadLittleEndian<uint64>(file.Data() + 8) == built.recordBytes);
    const Sha256Digest recordHash =
        Sha256::Hash(file.Slice(NuggetFileHeadBytes, built.recordBytes));
    CHECK(Same(file.Slice(16, Sha256Digest::Bytes), recordHash.bytes));

    NuggetMemoryReader reader(file);
    auto view = OpenNuggetFile(allocator, reader);
    REQUIRE(view);
    const NuggetRecord& record = view.Value().record;
    REQUIRE(record.values.Size() == 3);
    CHECK(record.bucket.View() == NuggetBucketTexture && record.key == Digest(1) &&
          record.inputs.Size() == 1 && record.meta.Size() == 2);
    CHECK(view.Value().digest == NuggetRecordDigest(record));
    CHECK(record.values[0].chunkTableHash == Sha256Digest{} &&
          !(record.values[1].chunkTableHash == Sha256Digest{}) &&
          view.Value().tables[1].Size() == 2 * Sha256Digest::Bytes &&
          view.Value().tables[0].IsEmpty());
    const Span<const uint8> raws[] = {built.small.View(), built.large.View(),
                                      built.packedRaw.View()};
    for (usize index = 0; index < 3; ++index) {
        Array<uint8> stored(allocator);
        REQUIRE(ReadNuggetValue(view.Value(), reader, index, stored));
        Array<uint8> raw(allocator);
        REQUIRE(DecodeNuggetValue(allocator, record.values[index], stored.View(), raw));
        CHECK(Same(raw.View(), raws[index]));
    }

    CHECK(record.values[2].storedSize < built.packedRaw.Size());
    // 값 없는 파일도 된다.
    auto empty = PlanNuggetFile(allocator, "shader", Digest(2), {}, {}, {});
    REQUIRE(empty);
    Array<uint8> emptyFile(allocator);
    REQUIRE(AppendNuggetFile(empty.Value(), {}, emptyFile));
    NuggetMemoryReader emptyReader(emptyFile.View());
    auto emptyView = OpenNuggetFile(allocator, emptyReader);
    REQUIRE(emptyView);
    CHECK(emptyView.Value().record.values.IsEmpty());
}

// 머리·Record byte를 하나씩 뒤집으면 모두 거부한다(magic·Record 크기·Record SHA-256·Record 본문).
// 버전 byte는 Unsupported, 그 밖은 InvalidArgument다.
TEST(NuggetFile_HeadAndRecordTampering) {
    SystemAllocator allocator;
    Built built(allocator);
    const usize span = NuggetFileHeadBytes + built.recordBytes;
    for (usize at = 0; at < span; ++at) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(built.file.View()));
        copy[at] ^= 0x01;
        NuggetMemoryReader reader(copy.View());
        auto view = OpenNuggetFile(allocator, reader);
        const bool version = at >= 4 && at < 8;
        CHECK(!view &&
              view.Error() == (version ? ErrorCode::Unsupported : ErrorCode::InvalidArgument));
    }
}

// 값 byte 변조는 그 값 읽기만 거부한다(ContentHash), 조각 해시 표 변조는 열기를 거부한다, 잘림·남는
// byte·머리보다 짧은 파일은 거부한다, Record 크기가 1 MiB를 넘으면 거부한다.
TEST(NuggetFile_ValueTableAndSizeTampering) {
    SystemAllocator allocator;
    Built built(allocator);
    NuggetMemoryReader clean(built.file.View());
    auto view = OpenNuggetFile(allocator, clean);
    REQUIRE(view);
    // 값마다 첫 byte·마지막 byte.
    for (usize index = 0; index < 3; ++index) {
        const uint64 offset = view.Value().offsets[index];
        const uint64 size = view.Value().record.values[index].storedSize;
        const uint64 positions[] = {offset, offset + size - 1};
        for (uint64 at : positions) {
            Array<uint8> copy(allocator);
            REQUIRE(copy.TryAppend(built.file.View()));
            copy[static_cast<usize>(at)] ^= 0x80;
            NuggetMemoryReader reader(copy.View());
            auto opened = OpenNuggetFile(allocator, reader);
            REQUIRE(opened);
            for (usize other = 0; other < 3; ++other) {
                Array<uint8> stored(allocator);
                auto read = ReadNuggetValue(opened.Value(), reader, other, stored);
                CHECK(other == index ? read.Error() == ErrorCode::InvalidArgument
                                     : static_cast<bool>(read));
                // 흘려 읽는 검증도 같은 판정이다.
                uint8 buffer[4096];
                auto verified = VerifyNuggetValue(opened.Value(), reader, other, buffer);
                CHECK(other == index ? verified.Error() == ErrorCode::InvalidArgument
                                     : static_cast<bool>(verified));
            }
        }
    }

    // 조각 해시 표(파일 끝 64 byte).
    const usize tableAt = built.file.Size() - 2 * Sha256Digest::Bytes;
    for (usize at = tableAt; at < built.file.Size(); at += 13) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(built.file.View()));
        copy[at] ^= 0x01;
        NuggetMemoryReader reader(copy.View());
        CHECK(OpenNuggetFile(allocator, reader).Error() == ErrorCode::InvalidArgument);
    }

    // 잘림·남는 byte·머리보다 짧음.
    const usize sizes[] = {built.file.Size() - 1, NuggetFileHeadBytes - 1, NuggetFileHeadBytes,
                           NuggetFileHeadBytes + built.recordBytes};
    for (usize size : sizes) {
        NuggetMemoryReader reader(built.file.View().Slice(0, size));
        CHECK(OpenNuggetFile(allocator, reader).Error() == ErrorCode::InvalidArgument);
    }

    Array<uint8> longer(allocator);
    REQUIRE(longer.TryAppend(built.file.View()) && longer.TryPushBack(0));
    NuggetMemoryReader extra(longer.View());
    CHECK(OpenNuggetFile(allocator, extra).Error() == ErrorCode::InvalidArgument);

    // Record 크기 칸이 1 MiB를 넘음.
    Array<uint8> huge(allocator);
    REQUIRE(huge.TryAppend(built.file.View()));
    StoreLittleEndian<uint64>(huge.Data() + 8, NuggetRecordMaxBytes + 1);
    NuggetMemoryReader hugeReader(huge.View());
    CHECK(OpenNuggetFile(allocator, hugeReader).Error() == ErrorCode::InvalidArgument);
}

// 해제 검증: 크기 불일치·RawHash 불일치·Rcbk 색인 해시·색인 길이·원본 크기·손상 블록, Codec은
// Unsupported, 실패하면 out은 원래 길이다. 값 읽기 index 범위 밖.
TEST(NuggetFile_DecodeChecks) {
    SystemAllocator allocator;
    Built built(allocator);
    NuggetMemoryReader reader(built.file.View());
    auto view = OpenNuggetFile(allocator, reader);
    REQUIRE(view);
    Array<uint8> stored(allocator);
    CHECK(ReadNuggetValue(view.Value(), reader, 3, stored).Error() == ErrorCode::InvalidArgument);

    const NuggetValueRow identity = view.Value().record.values[0];
    Array<uint8> out(allocator);
    REQUIRE(out.TryPushBack(42));
    CHECK(DecodeNuggetValue(allocator, identity, built.small.View().Slice(0, 36), out).Error() ==
          ErrorCode::InvalidArgument);
    NuggetValueRow wrongHash = identity;
    wrongHash.rawHash.bytes[0] ^= 1;
    CHECK(DecodeNuggetValue(allocator, wrongHash, built.small.View(), out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.Size() == 1);
    // 정규 형식을 거치지 않은 행: Identity인데 rawSize가 저장 크기와 다르다.
    NuggetValueRow sizeMismatch = identity;
    sizeMismatch.rawSize = identity.storedSize + 1;
    CHECK(DecodeNuggetValue(allocator, sizeMismatch, built.small.View(), out).Error() ==
          ErrorCode::InvalidArgument);
    NuggetValueRow codec = identity;
    codec.encoding = NuggetEncoding::Codec;
    CHECK(DecodeNuggetValue(allocator, codec, built.small.View(), out).Error() ==
          ErrorCode::Unsupported);

    const NuggetValueRow rcbk = view.Value().record.values[2];
    const Span<const uint8> packed = built.packed.View();
    NuggetValueRow badIndexHash = rcbk;
    badIndexHash.indexHash.bytes[0] ^= 1;
    CHECK(DecodeNuggetValue(allocator, badIndexHash, packed, out).Error() ==
          ErrorCode::InvalidArgument);
    NuggetValueRow badIndexLength = rcbk;
    badIndexLength.indexLength = rcbk.storedSize + 1;
    CHECK(DecodeNuggetValue(allocator, badIndexLength, packed, out).Error() ==
          ErrorCode::InvalidArgument);
    NuggetValueRow shorterIndex = rcbk;
    shorterIndex.indexLength = rcbk.indexLength - 8;
    shorterIndex.indexHash =
        Sha256::Hash(packed.Slice(0, static_cast<usize>(shorterIndex.indexLength)));
    CHECK(DecodeNuggetValue(allocator, shorterIndex, packed, out).Error() ==
          ErrorCode::InvalidArgument);
    NuggetValueRow wrongRaw = rcbk;
    wrongRaw.rawSize = rcbk.rawSize - 1;
    CHECK(DecodeNuggetValue(allocator, wrongRaw, packed, out).Error() ==
          ErrorCode::InvalidArgument);
    NuggetValueRow wrongRawHash = rcbk;
    wrongRawHash.rawHash.bytes[0] ^= 1;
    CHECK(DecodeNuggetValue(allocator, wrongRawHash, packed, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.Size() == 1);
    // 손상된 블록(색인은 그대로 — 블록 CRC가 잡는다)·손상된 헤더.
    Array<uint8> broken(allocator);
    REQUIRE(broken.TryAppend(packed));
    broken[broken.Size() - 1] ^= 0xff;
    CHECK(DecodeNuggetValue(allocator, rcbk, broken.View(), out).Error() ==
          ErrorCode::InvalidArgument);
    Array<uint8> badHeader(allocator);
    REQUIRE(badHeader.TryAppend(packed));
    badHeader[0] ^= 0xff;
    NuggetValueRow headerRow = rcbk;
    headerRow.indexHash =
        Sha256::Hash(badHeader.View().Slice(0, static_cast<usize>(rcbk.indexLength)));
    CHECK(DecodeNuggetValue(allocator, headerRow, badHeader.View(), out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.Size() == 1);
}

// 계획 입력 거부(정규 형식 위반: 순서·bucket·Identity 크기 불일치), 붙이기 입력 불일치, 범위 reader
// 경계.
TEST(NuggetFile_PlanAndReaderRules) {
    SystemAllocator allocator;
    const Array<uint8> raw = Pattern(allocator, 8, 1);
    const NuggetStoredValue backwards[] = {IdentityValue(2, raw.View()),
                                           IdentityValue(1, raw.View())};
    CHECK(PlanNuggetFile(allocator, "texture", Digest(1), backwards, {}, {}).Error() ==
          ErrorCode::InvalidArgument);
    const NuggetStoredValue one[] = {IdentityValue(1, raw.View())};
    CHECK(PlanNuggetFile(allocator, "Texture", Digest(1), one, {}, {}).Error() ==
          ErrorCode::InvalidArgument);
    NuggetStoredValue mismatch = IdentityValue(1, raw.View());
    mismatch.rawSize = 9;
    const NuggetStoredValue mismatched[] = {mismatch};
    CHECK(PlanNuggetFile(allocator, "texture", Digest(1), mismatched, {}, {}).Error() ==
          ErrorCode::InvalidArgument);

    auto plan = PlanNuggetFile(allocator, "texture", Digest(1), one, {}, {});
    REQUIRE(plan);
    Array<uint8> out(allocator);
    CHECK(AppendNuggetFile(plan.Value(), {}, out).Error() == ErrorCode::InvalidArgument);
    const NuggetStoredValue shorter[] = {IdentityValue(1, raw.View().Slice(0, 7))};
    CHECK(AppendNuggetFile(plan.Value(), shorter, out).Error() == ErrorCode::InvalidArgument);
    CHECK(out.IsEmpty());

    const uint8 bytes[] = {1, 2, 3};
    NuggetMemoryReader reader(bytes);
    uint8 two[2];
    CHECK(reader.ReadAt(1, two) && two[0] == 2 && two[1] == 3);
    CHECK(reader.ReadAt(2, two).Error() == ErrorCode::InvalidArgument);
    CHECK(reader.ReadAt(4, Span<uint8>()).Error() == ErrorCode::InvalidArgument);
    CHECK(reader.ReadAt(3, Span<uint8>()) && reader.Size() == 3);
}

// 할당 실패(계획·붙이기·열기·값 읽기·해제): 모두 OutOfMemory이고 실패 없는 결과는 기준과 같다.
TEST(NuggetFile_AllocationFailures) {
    SystemAllocator system;
    Built built(system);
    const NuggetStoredValue values[] = {IdentityValue(0, built.small.View()),
                                        IdentityValue(3, built.large.View())};
    NuggetInput input(system);
    REQUIRE(input.bucket.TryAssign("scc"));
    const NuggetInput inputs[] = {Move(input)};
    Array<uint8> expected(system);
    {
        auto plan = PlanNuggetFile(system, "texture", Digest(1), values, inputs, {});
        REQUIRE(plan && AppendNuggetFile(plan.Value(), values, expected));
    }

    test::NFailAllocator failing(system);
    uint32 failures = 0;
    auto loop = [&](auto&& body) noexcept {
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            const bool ok = body();
            const bool failed = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!failed) {
                CHECK(ok);
                return;
            }

            ++failures;
        }
    };
    loop([&]() noexcept {
        auto plan = PlanNuggetFile(failing, "texture", Digest(1), values, inputs, {});
        if (!plan) {
            CHECK(plan.Error() == ErrorCode::OutOfMemory);
            return false;
        }

        Array<uint8> file(failing);
        auto appended = AppendNuggetFile(plan.Value(), values, file);
        CHECK(appended || appended.Error() == ErrorCode::OutOfMemory);
        return static_cast<bool>(appended) && Same(file.View(), expected.View());
    });
    loop([&]() noexcept {
        NuggetMemoryReader reader(built.file.View());
        auto view = OpenNuggetFile(failing, reader);
        if (!view) {
            CHECK(view.Error() == ErrorCode::OutOfMemory);
            return false;
        }

        for (usize index = 0; index < 3; ++index) {
            Array<uint8> stored(failing);
            auto read = ReadNuggetValue(view.Value(), reader, index, stored);
            if (!read) {
                CHECK(read.Error() == ErrorCode::OutOfMemory);
                return false;
            }

            Array<uint8> raw(failing);
            auto decoded =
                DecodeNuggetValue(failing, view.Value().record.values[index], stored.View(), raw);
            if (!decoded) {
                CHECK(decoded.Error() == ErrorCode::OutOfMemory);
                return false;
            }
        }

        return true;
    });
    CHECK(failures > 2);
}

namespace {

// n번째(1부터) ReadAt이 실패하는 범위 reader다.
class FailingReader final : public NuggetRangeReader {
public:
    explicit FailingReader(Span<const uint8> bytes) noexcept : inner_(bytes) {
    }

    uint64 Size() const noexcept override {
        return inner_.Size();
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override {
        ++calls;
        if (failAt != 0 && calls == failAt) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return inner_.ReadAt(offset, out);
    }

    uint32 calls = 0;
    uint32 failAt = 0;

private:
    NuggetMemoryReader inner_;
};

} // namespace

// 흘려 읽는 값 검증: 버퍼 크기(1 byte·값보다 작음·같음·큼)와 상관없이 정상 값은 통과, 빈 값(크기
// 0)도 통과, index 범위 밖·빈 버퍼 InvalidArgument, 읽기 IO 오류는 그 조각(첫·중간·마지막)마다
// 그대로 돌려준다(손상과 구분).
TEST(NuggetFile_VerifyValue) {
    SystemAllocator allocator;
    Built built(allocator);
    NuggetMemoryReader reader(built.file.View());
    auto view = OpenNuggetFile(allocator, reader);
    REQUIRE(view);
    Array<uint8> buffer(allocator);
    REQUIRE(buffer.TryResize(NuggetChunkBytes + 64));
    const Span<uint8> storage = buffer.View();
    for (usize index = 0; index < 3; ++index) {
        const usize stored = static_cast<usize>(view.Value().record.values[index].storedSize);
        const usize sizes[] = {1, 7, stored - 1, stored, stored + 1};
        for (usize size : sizes) {
            REQUIRE(size <= storage.Size());
            CHECK(VerifyNuggetValue(view.Value(), reader, index, storage.Slice(0, size)));
        }
    }

    CHECK(VerifyNuggetValue(view.Value(), reader, 3, storage).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(VerifyNuggetValue(view.Value(), reader, 0, Span<uint8>()).Error() ==
          ErrorCode::InvalidArgument);

    // 빈 값.
    const NuggetStoredValue empty[] = {IdentityValue(2, Span<const uint8>())};
    auto plan = PlanNuggetFile(allocator, NuggetBucketTexture, Digest(3), empty, {}, {});
    REQUIRE(plan);
    Array<uint8> emptyFile(allocator);
    REQUIRE(AppendNuggetFile(plan.Value(), empty, emptyFile));
    NuggetMemoryReader emptyReader(emptyFile.View());
    auto emptyView = OpenNuggetFile(allocator, emptyReader);
    REQUIRE(emptyView);
    CHECK(VerifyNuggetValue(emptyView.Value(), emptyReader, 0, storage));

    // 값 1(1 MiB + 5)을 4096 byte씩: 조각마다 읽기 실패를 주입한다.
    FailingReader failing(built.file.View());
    auto failingView = OpenNuggetFile(allocator, failing);
    REQUIRE(failingView);
    const uint32 opened = failing.calls;
    const usize step = 4096;
    const uint64 large = failingView.Value().record.values[1].storedSize;
    const uint32 chunks = static_cast<uint32>((large + step - 1) / step);
    const uint32 failures[] = {1, chunks / 2, chunks};
    for (uint32 failAt : failures) {
        failing.calls = opened;
        failing.failAt = opened + failAt;
        CHECK(VerifyNuggetValue(failingView.Value(), failing, 1, storage.Slice(0, step)).Error() ==
              ErrorCode::Busy);
    }

    failing.failAt = 0;
    failing.calls = opened;
    CHECK(VerifyNuggetValue(failingView.Value(), failing, 1, storage.Slice(0, step)));
    CHECK(failing.calls == opened + chunks);
}

// 읽기 IO 오류는 그대로 돌려준다(머리·Record·조각 표·값 — 손상과 구분), 해시가 맞는 깨진 Record는
// 손상이다.
TEST(NuggetFile_ReadErrorsAndHashedGarbage) {
    SystemAllocator allocator;
    Built built(allocator);
    uint32 calls = 0;
    {
        FailingReader reader(built.file.View());
        REQUIRE(OpenNuggetFile(allocator, reader));
        calls = reader.calls;
    }

    REQUIRE(calls >= 3);
    for (uint32 failAt = 1; failAt <= calls; ++failAt) {
        FailingReader reader(built.file.View());
        reader.failAt = failAt;
        CHECK(OpenNuggetFile(allocator, reader).Error() == ErrorCode::Busy);
    }

    FailingReader reader(built.file.View());
    auto view = OpenNuggetFile(allocator, reader);
    REQUIRE(view);
    reader.failAt = reader.calls + 1;
    Array<uint8> stored(allocator);
    CHECK(ReadNuggetValue(view.Value(), reader, 0, stored).Error() == ErrorCode::Busy);

    // 더 새 Record schema 버전은 손상이 아니라 Unsupported다(파일 버전은 같다 — 지우지 않는다).
    {
        RecordBuilder newer(allocator, NuggetRecordSchema, NuggetRecordSchemaVersion + 1);
        auto newerBytes = newer.Build();
        REQUIRE(newerBytes);
        const Sha256Digest newerHash = Sha256::Hash(newerBytes.Value().View());
        Array<uint8> newerFile(allocator);
        REQUIRE(newerFile.TryAppend(built.file.View().Slice(0, 8)));
        uint8 newerSize[8];
        StoreLittleEndian<uint64>(newerSize, newerBytes.Value().Size());
        REQUIRE(newerFile.TryAppend(newerSize) && newerFile.TryAppend(newerHash.bytes) &&
                newerFile.TryAppend(newerBytes.Value().View()));
        NuggetMemoryReader newerReader(newerFile.View());
        CHECK(OpenNuggetFile(allocator, newerReader).Error() == ErrorCode::Unsupported);
    }

    // 머리의 SHA-256은 맞지만 Record가 아닌 byte.
    const uint8 garbage[] = {9, 9, 9, 9};
    const Sha256Digest hash = Sha256::Hash(garbage);
    Array<uint8> file(allocator);
    REQUIRE(file.TryAppend(built.file.View().Slice(0, 8)));
    uint8 size[8];
    StoreLittleEndian<uint64>(size, sizeof(garbage));
    REQUIRE(file.TryAppend(size) && file.TryAppend(hash.bytes) && file.TryAppend(garbage));
    NuggetMemoryReader garbageReader(file.View());
    CHECK(OpenNuggetFile(allocator, garbageReader).Error() == ErrorCode::InvalidArgument);
}

} // namespace nugget_file_tests_file
