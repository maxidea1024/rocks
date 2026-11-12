#include "CompressionTestSupport.h"

#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <stdio.h>

namespace rocks::test {
namespace {

using namespace compression;

constexpr CompressionFormat BlockFormats[] = {CompressionFormat::Lz4, CompressionFormat::Zstd,
                                              CompressionFormat::Deflate};

CompressedBlocksSettings BlockSettings(CompressionFormat format, uint32 blockSizeLog2,
                                       int32 level = -1000) {
    const CompressionSettings compression =
        level == -1000 ? DefaultSettings(format) : Settings(format, level);
    return CompressedBlocksSettings::TryMake(compression, blockSizeLog2).Value();
}

Span<const uint8> Text(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

Span<const uint8> Slice(Span<const uint8> data, usize offset, usize size) {
    return Span<const uint8>(data.Data() + offset, size);
}

uint32 StoredSize(const Array<uint8>& container, uint32 block) {
    return LoadLittleEndian<uint32>(container.Data() + CompressedBlocksHeaderBytes + 8 * block);
}

// 헤더·표를 고친 뒤 indexCrc32를 다시 계산한다(헤더 검사보다 뒤의 검사를 시험할 때).
void Reseal(Array<uint8>& container) {
    const uint32 count = LoadLittleEndian<uint32>(container.Data() + 16);
    uint32 crc = Crc32(Span<const uint8>(container.Data(), 44));
    crc = Crc32(Span<const uint8>(container.Data() + CompressedBlocksHeaderBytes, usize{8} * count),
                crc);
    StoreLittleEndian<uint32>(container.Data() + 44, crc);
}

ErrorCode OpenError(const Array<uint8>& container) {
    SystemAllocator allocator;
    auto reader = CompressedBlocksReader::TryOpen(View(container), allocator);
    return reader ? ErrorCode::None : reader.Error();
}

// 컨테이너 전체를 해제해 기대값과 비교하고 임의 범위 몇 개도 읽는다.
bool ReadsBack(const Array<uint8>& container, Span<const uint8> expected) {
    SystemAllocator allocator;
    bool ok = true;
    {
        auto reader = CompressedBlocksReader::TryOpen(View(container), allocator);
        if (!reader) {
            return false;
        }
        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
        Array<uint8> restored(allocator);
        ok = context && reader.Value().RawSize() == expected.Size() &&
             restored.TryResize(expected.Size()) &&
             reader.Value().TryRead(0, Span<uint8>(restored.Data(), restored.Size()),
                                    View(container), context.Value()) &&
             Same(restored, expected) && reader.Value().TryVerify(View(container), 0);

        // 블록 경계를 걸치거나 블록 안에 있는 범위다.
        const usize size = expected.Size();
        const usize ranges[][2] = {{0, size / 3},
                                   {size / 3, size / 2},
                                   {size / 2, 1},
                                   {size - size / 7, size / 7},
                                   {size / 5, 4097}};
        for (const auto& range : ranges) {
            const usize offset = range[0];
            const usize bytes = offset + range[1] <= size ? range[1] : size - offset;
            Array<uint8> part(allocator);
            ok = ok && part.TryResize(bytes) &&
                 reader.Value().TryRead(offset, Span<uint8>(part.Data(), bytes), View(container),
                                        context.Value()) &&
                 Same(part, Slice(expected, offset, bytes));
        }
    }
    return ok && allocator.Stats().liveAllocations == 0;
}

} // namespace

TEST(CompressedBlocks_SettingsAndBound) {
    CHECK(CompressedBlocksSettings::TryMake(DefaultSettings(CompressionFormat::Zlib)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CompressedBlocksSettings::TryMake(DefaultSettings(CompressionFormat::Gzip)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CompressedBlocksSettings::TryMake(DefaultSettings(CompressionFormat::Zstd), 11).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CompressedBlocksSettings::TryMake(DefaultSettings(CompressionFormat::Zstd), 25).Error() ==
          ErrorCode::InvalidArgument);
    auto settings = CompressedBlocksSettings::TryMake(DefaultSettings(CompressionFormat::Lz4));
    REQUIRE(settings);
    CHECK(settings.Value().BlockSizeLog2() == DefaultCompressedBlockSizeLog2);
    CHECK(settings.Value().BlockSize() == usize{1} << DefaultCompressedBlockSizeLog2);

    CHECK(TryGetCompressedBlocksBound(12, 0).Value() == 48);
    CHECK(TryGetCompressedBlocksBound(12, 4096).Value() == 48 + 8 + 4096);
    CHECK(TryGetCompressedBlocksBound(12, 4097).Value() == 48 + 16 + 4097);
    CHECK(TryGetCompressedBlocksBound(11, 1).Error() == ErrorCode::InvalidArgument);
    // 블록 수가 u32를 넘는다.
    CHECK(TryGetCompressedBlocksBound(12, uint64{1} << 44).Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetCompressedBlocksBound(24, ~uint64{0} - 8).Error() == ErrorCode::InvalidArgument);
}

TEST(CompressedBlocks_RoundTripAcrossCodecsAndSizes) {
    SystemAllocator allocator;
    {
        static constexpr usize Sizes[] = {0, 1, 4095, 4096, 4097, 3 * 4096 + 5, 300000};
        static constexpr Pattern Patterns[] = {Pattern::Text, Pattern::Random, Pattern::Zeros};
        static constexpr uint32 Logs[] = {12, 16};
        for (CompressionFormat format : BlockFormats) {
            for (uint32 blockSizeLog2 : Logs) {
                for (usize size : Sizes) {
                    for (Pattern pattern : Patterns) {
                        Array<uint8> original(allocator);
                        Fill(original, size, pattern, size + blockSizeLog2);
                        Array<uint8> container(allocator);
                        REQUIRE(container.TryAppend(Text("pre")));
                        auto written =
                            TryCompressBlocksAppend(BlockSettings(format, blockSizeLog2),
                                                    View(original), container, allocator);
                        REQUIRE(written);
                        CHECK(written.Value() == container.Size() - 3);
                        CHECK(written.Value() <=
                              TryGetCompressedBlocksBound(blockSizeLog2, size).Value());

                        Array<uint8> body(allocator);
                        REQUIRE(body.TryAppend(Slice(View(container), 3, written.Value())));
                        CHECK(ReadsBack(body, View(original)));
                    }
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 압축이 원본보다 작지 않은 블록은 raw로 저장한다(정규형). Deflate 수준 0은 항상 raw다.
TEST(CompressedBlocks_RawFallbackIsCanonical) {
    SystemAllocator allocator;
    {
        Array<uint8> random(allocator);
        Fill(random, 5 * 4096 + 100, Pattern::Random, 3);
        Array<uint8> text(allocator);
        Fill(text, 5 * 4096 + 100, Pattern::Text, 3);
        for (CompressionFormat format : BlockFormats) {
            Array<uint8> container(allocator);
            REQUIRE(TryCompressBlocksAppend(BlockSettings(format, 12), View(random), container,
                                            allocator));
            for (uint32 block = 0; block < 6; ++block) {
                CHECK(StoredSize(container, block) == (block < 5 ? 4096u : 100u));
            }
            CHECK(ReadsBack(container, View(random)));

            Array<uint8> packed(allocator);
            REQUIRE(
                TryCompressBlocksAppend(BlockSettings(format, 12), View(text), packed, allocator));
            for (uint32 block = 0; block < 5; ++block) {
                CHECK(StoredSize(packed, block) < 4096u);
            }
        }

        Array<uint8> stored(allocator);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(CompressionFormat::Deflate, 12, 0),
                                        View(text), stored, allocator));
        CHECK(StoredSize(stored, 0) == 4096u && StoredSize(stored, 5) == 100u);
        CHECK(ReadsBack(stored, View(text)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// writer로 블록을 흘려 쓴 결과는 한 번에 압축한 컨테이너와 같다.
TEST(CompressedBlocks_WriterMatchesOneShot) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 7 * 4096 + 33, Pattern::Text, 9);
        const auto settings = BlockSettings(CompressionFormat::Zstd, 12);
        Array<uint8> expected(allocator);
        REQUIRE(TryCompressBlocksAppend(settings, View(original), expected, allocator));

        auto writer = CompressedBlocksWriter::TryBegin(settings, original.Size(), allocator);
        REQUIRE(writer);
        CHECK(writer.Value().BlockCount() == 8);
        CHECK(writer.Value().IndexBytes() == 48 + 8 * 8);
        Array<uint8> payload(allocator);
        // 크기가 다르거나 겹치면 거부하고 상태는 그대로다.
        CHECK(writer.Value().TryAppendBlock(Slice(View(original), 0, 4095), payload).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(payload.TryAppend(Slice(View(original), 0, 4096)));
        CHECK(writer.Value().TryAppendBlock(View(payload), payload).Error() ==
              ErrorCode::InvalidArgument);
        payload.Clear();
        for (usize offset = 0; offset < original.Size(); offset += 4096) {
            const usize bytes = original.Size() - offset < 4096 ? original.Size() - offset : 4096;
            Array<uint8> block(allocator);
            REQUIRE(writer.Value().TryAppendBlock(Slice(View(original), offset, bytes), block));
            REQUIRE(payload.TryAppend(View(block)));
            if (offset == 0) {
                Array<uint8> index(allocator);
                REQUIRE(index.TryResize(writer.Value().IndexBytes()));
                CHECK(writer.Value()
                          .TryFinishIndex(Span<uint8>(index.Data(), index.Size()))
                          .Error() == ErrorCode::InvalidArgument);
            }
        }
        CHECK(writer.Value().TryAppendBlock(Slice(View(original), 0, 33), payload).Error() ==
              ErrorCode::InvalidArgument);

        Array<uint8> assembled(allocator);
        REQUIRE(assembled.TryResize(writer.Value().IndexBytes()));
        CHECK(writer.Value()
                  .TryFinishIndex(Span<uint8>(assembled.Data(), assembled.Size() - 1))
                  .Error() == ErrorCode::InvalidArgument);
        REQUIRE(writer.Value().TryFinishIndex(Span<uint8>(assembled.Data(), assembled.Size())));
        REQUIRE(assembled.TryAppend(View(payload)));
        CHECK(Same(assembled, View(expected)));

        CompressedBlocksWriter moved = Move(writer.Value());
        CHECK(writer.Value().TryAppendBlock(View(original), payload).Error() ==
              ErrorCode::StaleHandle);
        CHECK(writer.Value().IndexBytes() == 0);
        CHECK(moved.BlockCount() == 8);

        // 빈 원본은 헤더만이다.
        auto empty = CompressedBlocksWriter::TryBegin(settings, 0, allocator);
        REQUIRE(empty);
        uint8 header[CompressedBlocksHeaderBytes] = {};
        REQUIRE(empty.Value().TryFinishIndex(Span<uint8>(header, sizeof(header))));
        Array<uint8> emptyContainer(allocator);
        REQUIRE(TryCompressBlocksAppend(settings, Span<const uint8>(), emptyContainer, allocator));
        CHECK(Same(emptyContainer, Span<const uint8>(header, sizeof(header))));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 헤더 필드 하나씩을 망가뜨린다. 헤더 검사는 indexCrc32보다 먼저다.
TEST(CompressedBlocks_HeaderValidation) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 3 * 4096 + 7, Pattern::Text, 13);
        Array<uint8> good(allocator);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(CompressionFormat::Lz4, 12), View(original),
                                        good, allocator));
        CHECK(OpenError(good) == ErrorCode::None);

        struct Mutation {
            usize offset;
            uint8 value;
            ErrorCode expected;
        };
        const Mutation Mutations[] = {
            {0, 'X', ErrorCode::InvalidArgument},   // magic
            {4, 1, ErrorCode::Unsupported},         // version(이전 40byte 헤더의 1)
            {4, 3, ErrorCode::Unsupported},         // version(모르는 값)
            {6, 0, ErrorCode::InvalidArgument},     // codec 0
            {6, 4, ErrorCode::Unsupported},         // 모르는 codec
            {7, 11, ErrorCode::InvalidArgument},    // blockSizeLog2
            {7, 13, ErrorCode::InvalidArgument},    // 블록 수 불일치
            {8, 0xff, ErrorCode::InvalidArgument},  // rawSize
            {16, 9, ErrorCode::InvalidArgument},    // blockCount
            {20, 1, ErrorCode::Unsupported},        // 사전 bit + Lz4(지원하지 않는 조합)
            {20, 2, ErrorCode::Unsupported},        // 모르는 flags bit
            {23, 0x80, ErrorCode::Unsupported},     // 모르는 flags bit(최상위)
            {24, 0, ErrorCode::InvalidArgument},    // payloadSize
            {32, 1, ErrorCode::InvalidArgument},    // 사전 bit 없이 지문
            {39, 1, ErrorCode::InvalidArgument},    // 사전 bit 없이 지문(마지막 byte)
            {40, 1, ErrorCode::InvalidArgument},    // reserved
            {44, 0x55, ErrorCode::InvalidArgument}, // indexCrc32
            {52, 0x55, ErrorCode::InvalidArgument}, // 표(CRC 불일치)
        };
        for (const Mutation& mutation : Mutations) {
            Array<uint8> bad(allocator);
            REQUIRE(bad.TryAppend(View(good)));
            bad[mutation.offset] = mutation.value;
            CHECK(OpenError(bad) == mutation.expected);
        }

        // 표 CRC를 다시 맞춰도 저장 크기 규칙이 틀리면 거부한다: 0, 원본 초과, 합 불일치.
        const uint32 first = StoredSize(good, 0);
        const uint32 second = StoredSize(good, 1);
        const uint32 Sizes[][2] = {
            {0, first + second}, {4097, first + second - 4097}, {first + 1, second}};
        for (const auto& sizes : Sizes) {
            Array<uint8> bad(allocator);
            REQUIRE(bad.TryAppend(View(good)));
            StoreLittleEndian<uint32>(bad.Data() + 48, sizes[0]);
            StoreLittleEndian<uint32>(bad.Data() + 56, sizes[1]);
            Reseal(bad);
            CHECK(OpenError(bad) == ErrorCode::InvalidArgument);
        }

        // 컨테이너 길이가 다르면(절단·뒤 byte) 거부한다.
        Array<uint8> truncated(allocator);
        REQUIRE(truncated.TryAppend(Slice(View(good), 0, good.Size() - 1)));
        CHECK(OpenError(truncated) == ErrorCode::InvalidArgument);
        Array<uint8> trailing(allocator);
        REQUIRE(trailing.TryAppend(View(good)));
        REQUIRE(trailing.TryAppend(Text("x")));
        CHECK(OpenError(trailing) == ErrorCode::InvalidArgument);
        CHECK(OpenError(Array<uint8>(allocator)) == ErrorCode::InvalidArgument);

        // 헤더만으로 인덱스 크기를 구한다. 거짓 블록 수로 큰 할당을 유도할 수 없다.
        CHECK(CompressedBlocksReader::TryGetIndexBytes(Slice(View(good), 0, 48), good.Size())
                  .Value() == 48 + 8 * 4);
        Array<uint8> huge(allocator);
        REQUIRE(huge.TryAppend(Slice(View(good), 0, 48)));
        StoreLittleEndian<uint64>(huge.Data() + 8, uint64{0xffffffff} << 12);
        StoreLittleEndian<uint32>(huge.Data() + 16, 0xffffffffu);
        StoreLittleEndian<uint64>(huge.Data() + 24, 0xffffffffull);
        CHECK(CompressedBlocksReader::TryGetIndexBytes(View(huge), huge.Size()).Error() ==
              ErrorCode::InvalidArgument);
        CountingAllocator counting(allocator);
        CHECK(CompressedBlocksReader::TryOpen(View(huge), counting).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(counting.Allocations() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 저장 byte 손상은 CRC로 해제 전에 잡는다. stored 범위는 다시 계산해 확인한다.
TEST(CompressedBlocks_IntegrityAndStoredRanges) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 6 * 4096 + 200, Pattern::Text, 17);
        for (CompressionFormat format : BlockFormats) {
            Array<uint8> container(allocator);
            REQUIRE(TryCompressBlocksAppend(BlockSettings(format, 12), View(original), container,
                                            allocator));
            auto opened = CompressedBlocksReader::TryOpen(View(container), allocator);
            REQUIRE(opened);
            const CompressedBlocksReader& reader = opened.Value();
            auto created = CompressedBlocksReadContext::TryCreate(reader, allocator);
            REQUIRE(created);
            CompressedBlocksReadContext& context = created.Value();

            // 파일처럼 필요한 저장 byte만 준다.
            const uint64 offset = 4096 + 100;
            Array<uint8> part(allocator);
            REQUIRE(part.TryResize(2 * 4096));
            auto where = reader.TryLocate(offset, part.Size());
            REQUIRE(where);
            CHECK(where.Value().firstBlock == 1 && where.Value().blockCount == 3);
            const Span<const uint8> stored =
                Slice(View(container), where.Value().storedOffset, where.Value().storedBytes);
            CHECK(reader.TryRead(offset, Span<uint8>(part.Data(), part.Size()), stored,
                                 where.Value().storedOffset, context));
            CHECK(Same(part, Slice(View(original), offset, part.Size())));
            // 한 byte 모자란 stored·잘못된 기준 위치는 거부한다.
            CHECK(reader
                      .TryRead(offset, Span<uint8>(part.Data(), part.Size()),
                               Slice(stored, 0, stored.Size() - 1), where.Value().storedOffset,
                               context)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(reader
                      .TryRead(offset, Span<uint8>(part.Data(), part.Size()), stored,
                               where.Value().storedOffset + 1, context)
                      .Error() == ErrorCode::InvalidArgument);

            // 범위 밖·0byte·겹침.
            uint8 one = 0;
            CHECK(reader.TryRead(original.Size(), Span<uint8>(&one, 1), View(container), context)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(reader.TryRead(original.Size(), Span<uint8>(), View(container), context));
            CHECK(reader.TryLocate(original.Size() - 1, 2).Error() == ErrorCode::InvalidArgument);
            CHECK(reader.TryLocate(5, 0).Value().blockCount == 0);
            CHECK(
                reader.TryRead(0, Span<uint8>(container.Data() + 50, 10), View(container), context)
                    .Error() == ErrorCode::InvalidArgument);

            // 둘째 블록 저장 byte를 손상시킨다.
            Array<uint8> corrupt(allocator);
            REQUIRE(corrupt.TryAppend(View(container)));
            corrupt[reader.IndexBytes() + StoredSize(container, 0) + 1] ^= 0x40;
            CHECK(reader.TryVerify(View(corrupt), 0).Error() == ErrorCode::InvalidArgument);
            CHECK(reader.TryVerify(Slice(View(container), 0, container.Size() - 1), 0).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(reader.TryVerify(View(container), 0));
            Array<uint8> whole(allocator);
            REQUIRE(whole.TryResize(original.Size()));
            context.Invalidate();
            CHECK(reader.TryRead(0, Span<uint8>(whole.Data(), whole.Size()), View(corrupt), context)
                      .Error() == ErrorCode::InvalidArgument);
            // 첫 블록만 읽으면 손상과 무관하다.
            CHECK(reader.TryRead(0, Span<uint8>(whole.Data(), 4096), View(corrupt), context));
            // 검증을 끄면 손상을 모를 수 있다(코덱 오류가 나거나 틀린 byte가 나온다).
            (void)reader.TryRead(4096, Span<uint8>(whole.Data(), 4096), View(corrupt), context,
                                 false);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 블록 일부만 걸치는 읽기는 문맥에 블록을 캐시한다. 문맥의 형식·블록 크기를 확인한다.
TEST(CompressedBlocks_ReadContextCacheAndCompatibility) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 4 * 4096, Pattern::Text, 19);
        Array<uint8> container(allocator);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(CompressionFormat::Zstd, 12), View(original),
                                        container, allocator));
        auto reader = CompressedBlocksReader::TryOpen(View(container), allocator);
        REQUIRE(reader);
        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
        REQUIRE(context);

        uint8 small[16] = {};
        REQUIRE(reader.Value().TryRead(4096 + 10, Span<uint8>(small, 16), View(container),
                                       context.Value()));
        // 같은 블록의 다음 읽기는 캐시를 쓴다: 저장 byte가 바뀌어도 이전 해제 결과를 돌려준다.
        Array<uint8> changed(allocator);
        REQUIRE(changed.TryAppend(View(container)));
        changed[reader.Value().IndexBytes() + StoredSize(container, 0) + 2] ^= 0x10;
        CHECK(reader.Value().TryRead(4096 + 40, Span<uint8>(small, 16), View(changed),
                                     context.Value()));
        CHECK(Same(Span<const uint8>(small, 16), Slice(View(original), 4096 + 40, 16)));
        context.Value().Invalidate();
        CHECK(reader.Value()
                  .TryRead(4096 + 40, Span<uint8>(small, 16), View(changed), context.Value())
                  .Error() == ErrorCode::InvalidArgument);

        // 형식이 다르거나 블록 크기가 작은 문맥은 거부한다. 큰 블록 문맥은 된다.
        auto lz4 = CompressedBlocksReadContext::TryCreate(CompressionFormat::Lz4, 12, allocator);
        auto tiny = CompressedBlocksReadContext::TryCreate(CompressionFormat::Zstd, 12, allocator);
        auto large = CompressedBlocksReadContext::TryCreate(CompressionFormat::Zstd, 16, allocator);
        REQUIRE(lz4 && tiny && large);
        CHECK(reader.Value()
                  .TryRead(0, Span<uint8>(small, 16), View(container), lz4.Value())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(reader.Value().TryRead(0, Span<uint8>(small, 16), View(container), large.Value()));
        CHECK(CompressedBlocksReadContext::TryCreate(CompressionFormat::Gzip, 12, allocator)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(CompressedBlocksReadContext::TryCreate(CompressionFormat::Zstd, 25, allocator)
                  .Error() == ErrorCode::InvalidArgument);

        Array<uint8> bigger(allocator);
        Fill(bigger, 3 * 65536, Pattern::Text, 21);
        Array<uint8> wide(allocator);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(CompressionFormat::Zstd, 16), View(bigger),
                                        wide, allocator));
        auto wideReader = CompressedBlocksReader::TryOpen(View(wide), allocator);
        REQUIRE(wideReader);
        CHECK(wideReader.Value()
                  .TryRead(0, Span<uint8>(small, 16), View(wide), tiny.Value())
                  .Error() == ErrorCode::InvalidArgument);
        // 문맥을 다른 reader와 번갈아 써도 캐시가 섞이지 않는다.
        REQUIRE(wideReader.Value().TryRead(10, Span<uint8>(small, 16), View(wide), large.Value()));
        CHECK(Same(Span<const uint8>(small, 16), Slice(View(bigger), 10, 16)));
        REQUIRE(reader.Value().TryRead(10, Span<uint8>(small, 16), View(container), large.Value()));
        CHECK(Same(Span<const uint8>(small, 16), Slice(View(original), 10, 16)));

        // 이동된 reader·문맥은 StaleHandle이다.
        CompressedBlocksReader moved = Move(reader.Value());
        CHECK(reader.Value().TryLocate(0, 1).Error() == ErrorCode::StaleHandle);
        CHECK(CompressedBlocksReadContext::TryCreate(reader.Value(), allocator).Error() ==
              ErrorCode::StaleHandle);
        CompressedBlocksReadContext movedContext = Move(context.Value());
        CHECK(moved.TryRead(0, Span<uint8>(small, 16), View(container), context.Value()).Error() ==
              ErrorCode::StaleHandle);
        CHECK(moved.TryRead(0, Span<uint8>(small, 16), View(container), movedContext));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 병렬 압축은 순차와 같은 byte다. 제출 거부·종료된 scheduler에서도 호출 스레드가 끝낸다.
TEST(CompressedBlocks_ParallelMatchesSequential) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, (usize{5} << 20) + 12345, Pattern::Text, 23);
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 64}));
        for (CompressionFormat format : BlockFormats) {
            const auto settings = BlockSettings(format, 14);
            Array<uint8> sequential(allocator);
            REQUIRE(TryCompressBlocksAppend(settings, View(original), sequential, allocator));
            Array<uint8> parallel(allocator);
            REQUIRE(parallel.TryAppend(Text("pre")));
            auto written =
                TryCompressBlocksAppend(scheduler, settings, View(original), parallel, allocator);
            REQUIRE(written);
            CHECK(written.Value() == sequential.Size());
            CHECK(Same(Slice(View(parallel), 3, parallel.Size() - 3), View(sequential)));

            // 병렬 읽기: 전체, 블록 경계를 걸치는 범위, 작은 범위.
            auto reader = CompressedBlocksReader::TryOpen(View(sequential), allocator);
            REQUIRE(reader);
            const usize ranges[][2] = {{0, original.Size()}, {1000, 3 << 20}, {16384 + 5, 100}};
            for (const auto& range : ranges) {
                Array<uint8> restored(allocator);
                REQUIRE(restored.TryResize(range[1]));
                CHECK(reader.Value().TryRead(scheduler, range[0],
                                             Span<uint8>(restored.Data(), restored.Size()),
                                             View(sequential), 0, allocator));
                CHECK(Same(restored, Slice(View(original), range[0], range[1])));
            }

            // 손상은 가장 앞 블록의 오류다(결정적).
            Array<uint8> corrupt(allocator);
            REQUIRE(corrupt.TryAppend(View(sequential)));
            corrupt[corrupt.Size() - 1] ^= 1;
            corrupt[reader.Value().IndexBytes() + 3] ^= 1;
            Array<uint8> restored(allocator);
            REQUIRE(restored.TryResize(original.Size()));
            CHECK(reader.Value()
                      .TryRead(scheduler, 0, Span<uint8>(restored.Data(), restored.Size()),
                               View(corrupt), 0, allocator)
                      .Error() == ErrorCode::InvalidArgument);
        }

        // worker 안에서 부르면 WrongThread다.
        ErrorCode inside = ErrorCode::None;
        const auto settings = BlockSettings(CompressionFormat::Lz4, 14);
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [&](WorkerTaskContext&) noexcept {
                Array<uint8> output(allocator);
                auto result =
                    TryCompressBlocksAppend(scheduler, settings, View(original), output, allocator);
                inside = result ? ErrorCode::None : result.Error();
            });
        REQUIRE(body);
        auto handle = scheduler.Submit(Move(body.Value()));
        REQUIRE(handle);
        REQUIRE(handle.Value().Wait());
        CHECK(inside == ErrorCode::WrongThread);
        REQUIRE(scheduler.Shutdown());

        // 종료된 scheduler(제출 거부)에서도 결과가 같다.
        Array<uint8> sequential(allocator);
        REQUIRE(TryCompressBlocksAppend(settings, View(original), sequential, allocator));
        Array<uint8> fallback(allocator);
        REQUIRE(TryCompressBlocksAppend(scheduler, settings, View(original), fallback, allocator));
        CHECK(Same(fallback, View(sequential)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패는 output 길이를 되돌리고 누수가 없다.
TEST(CompressedBlocks_OutOfMemoryRollsBack) {
    SystemAllocator system;
    Array<uint8> original(system);
    Fill(original, 9 * 4096 + 77, Pattern::Text, 29);
    for (CompressionFormat format : BlockFormats) {
        const auto settings = BlockSettings(format, 12);
        Array<uint8> expected(system);
        REQUIRE(TryCompressBlocksAppend(settings, View(original), expected, system));
        NFailAllocator failing(system);
        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            {
                Array<uint8> output(failing);
                failing.DisableFailure();
                REQUIRE(output.TryAppend(Text("base")));
                failing.FailOnAttempt(attempt);
                auto written = TryCompressBlocksAppend(settings, View(original), output, failing);
                succeeded = written.IsOk();
                CHECK(!succeeded || failing.Attempts() < attempt);
                CHECK(succeeded ? Same(Slice(View(output), 4, output.Size() - 4), View(expected))
                                : output.Size() == 4 && written.Error() == ErrorCode::OutOfMemory);
            }
            CHECK(system.Stats().liveAllocations == 2);
        }
        CHECK(succeeded);

        succeeded = false;
        for (uint64 attempt = 1; attempt <= 16 && !succeeded; ++attempt) {
            {
                failing.FailOnAttempt(attempt);
                auto reader = CompressedBlocksReader::TryOpen(View(expected), failing);
                auto context = reader
                                   ? CompressedBlocksReadContext::TryCreate(reader.Value(), failing)
                                   : Result<CompressedBlocksReadContext>::Failure(reader.Error());
                succeeded = context.IsOk();
                CHECK(succeeded || context.Error() == ErrorCode::OutOfMemory);
                if (succeeded) {
                    failing.DisableFailure();
                    Array<uint8> restored(system);
                    REQUIRE(restored.TryResize(original.Size()));
                    CHECK(reader.Value().TryRead(0, Span<uint8>(restored.Data(), restored.Size()),
                                                 View(expected), context.Value()));
                    CHECK(Same(restored, View(original)));
                }
            }
            CHECK(system.Stats().liveAllocations == 2);
        }
        CHECK(succeeded);
    }
}

// 같은 입력·설정이면 플랫폼과 무관하게 같은 byte다(64bit little-endian 대상).
TEST(CompressedBlocks_DeterministicOutputGoldens) {
    struct Golden {
        CompressionFormat format;
        usize size;
        uint32 crc;
    };
    static constexpr Golden Goldens[] = {
        // 기준: Windows x64 clang 21.1.6, LZ4 1.11.1·zstd 1.5.7·zlib 1.4, 기본 수준, 64KiB 블록,
        // RCBK v2(헤더 48byte).
        {CompressionFormat::Lz4, 1405524, 0x8193c4edu},
        {CompressionFormat::Zstd, 820607, 0xa64ce078u},
        {CompressionFormat::Deflate, 900484, 0x5ada0e61u},
    };
    SystemAllocator allocator;
    Array<uint8> original(allocator);
    Fill(original, (usize{3} << 19) + 11, Pattern::Text, 31);
    for (const Golden& golden : Goldens) {
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(golden.format, 16), View(original), packed,
                                        allocator));
        const uint32 crc = Crc32(View(packed));
        if (packed.Size() != golden.size || crc != golden.crc) {
            printf("compressed blocks golden mismatch: format %u size %llu crc 0x%08x\n",
                   static_cast<unsigned>(golden.format),
                   static_cast<unsigned long long>(packed.Size()), crc);
        }
        CHECK(packed.Size() == golden.size);
        CHECK(crc == golden.crc);
    }
}

namespace {

template <usize Count>
Span<const uint8> Bytes(const uint8 (&values)[Count]) {
    return Span<const uint8>(values, Count);
}

// 해제한 마지막 블록을 보관했다가 같은 크기 요청에 다시 준다. 주소 재사용을 결정적으로 만든다.
class ReusingAllocator final : public Allocator {
public:
    explicit ReusingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    ~ReusingAllocator() noexcept override {
        if (held_ != nullptr) {
            (void)inner_.Deallocate(static_cast<uint8*>(held_) - Header);
        }
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (held_ != nullptr && heldBytes_ == bytes && alignment <= Header) {
            void* reused = held_;
            held_ = nullptr;
            return Result<void*>::Success(reused);
        }
        auto allocation = inner_.Allocate(bytes + Header, Header, tag);
        if (!allocation) {
            return allocation;
        }
        auto* user = static_cast<uint8*>(allocation.Value()) + Header;
        reinterpret_cast<usize*>(user)[-1] = bytes;
        return Result<void*>::Success(user);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }
        if (held_ == nullptr) {
            held_ = address;
            heldBytes_ = reinterpret_cast<usize*>(address)[-1];
            return Result<void>::Success();
        }
        return inner_.Deallocate(static_cast<uint8*>(address) - Header);
    }

private:
    static constexpr usize Header = 16;

    Allocator& inner_;
    void* held_ = nullptr;
    usize heldBytes_ = 0;
};

// 블록 저장 byte를 그대로 받아 컨테이너를 만든다(코덱 ID·블록 크기·원본 크기를 직접 정한다).
Array<uint8> Handmade(uint8 codec, uint32 blockSizeLog2, uint64 rawSize,
                      const Span<const uint8>* blocks, usize count, Allocator& allocator) {
    Array<uint8> container(allocator);
    usize payload = 0;
    for (usize index = 0; index < count; ++index) {
        payload += blocks[index].Size();
    }
    (void)container.TryResize(48 + 8 * count);
    platform::MemoryCopy(container.Data(), "RCBK", 4);
    StoreLittleEndian<uint16>(container.Data() + 4, 2);
    container[6] = codec;
    container[7] = static_cast<uint8>(blockSizeLog2);
    StoreLittleEndian<uint64>(container.Data() + 8, rawSize);
    StoreLittleEndian<uint32>(container.Data() + 16, static_cast<uint32>(count));
    StoreLittleEndian<uint32>(container.Data() + 20, 0);
    StoreLittleEndian<uint64>(container.Data() + 24, payload);
    StoreLittleEndian<uint64>(container.Data() + 32, 0);
    StoreLittleEndian<uint32>(container.Data() + 40, 0);
    for (usize index = 0; index < count; ++index) {
        StoreLittleEndian<uint32>(container.Data() + 48 + 8 * index,
                                  static_cast<uint32>(blocks[index].Size()));
        StoreLittleEndian<uint32>(container.Data() + 52 + 8 * index, Crc32(blocks[index]));
    }
    for (usize index = 0; index < count; ++index) {
        (void)container.TryAppend(blocks[index]);
    }
    Reseal(container);
    return container;
}

ErrorCode ReadError(const Array<uint8>& container, usize offset, usize bytes) {
    SystemAllocator allocator;
    auto reader = CompressedBlocksReader::TryOpen(View(container), allocator);
    if (!reader) {
        return reader.Error();
    }
    auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
    Array<uint8> out(allocator);
    if (!context || !out.TryResize(bytes)) {
        return ErrorCode::OutOfMemory;
    }
    auto read = reader.Value().TryRead(offset, Span<uint8>(out.Data(), bytes), View(container),
                                       context.Value());
    return read ? ErrorCode::None : read.Error();
}

} // namespace

// 문맥 캐시는 reader를 닫고 같은 주소에 새 reader가 열려도 섞이지 않는다. verify 없이 채운 캐시는
// verify 읽기에서 확인한다. 캐시 hit여도 stored 범위를 확인한다.
TEST(CompressedBlocks_ReadContextCacheIsolation) {
    SystemAllocator system;
    {
        ReusingAllocator reusing(system);
        auto context = CompressedBlocksReadContext::TryCreate(CompressionFormat::Lz4, 12, system);
        REQUIRE(context);
        uint8 out[160] = {};

        // C: raw 블록 'Z' × 4096을 부분 읽어 버퍼를 'Z'로 채운다.
        Array<uint8> zeds(system);
        Fill(zeds, 4096, Pattern::Zeros);
        for (usize index = 0; index < zeds.Size(); ++index) {
            zeds[index] = 'Z';
        }
        const Span<const uint8> zedBlock = View(zeds);
        Array<uint8> c = Handmade(1, 12, 4096, &zedBlock, 1, system);
        {
            auto reader = CompressedBlocksReader::TryOpen(View(c), reusing);
            REQUIRE(reader);
            REQUIRE(reader.Value().TryRead(0, Span<uint8>(out, 10), View(c), context.Value()));
        }

        // A: 원본 100byte raw 블록 S(0xff × 100)를 부분 읽고 닫는다.
        Array<uint8> fill(system);
        Fill(fill, 100, Pattern::Zeros);
        for (usize index = 0; index < fill.Size(); ++index) {
            fill[index] = 0xff;
        }
        const Span<const uint8> sBlock = View(fill);
        Array<uint8> a = Handmade(1, 12, 100, &sBlock, 1, system);
        {
            auto reader = CompressedBlocksReader::TryOpen(View(a), reusing);
            REQUIRE(reader);
            REQUIRE(reader.Value().TryRead(0, Span<uint8>(out, 50), View(a), context.Value()));
        }

        // B: 표와 payload가 A와 같지만 원본이 200byte라 S는 LZ4 압축 데이터로 해석되어야
        // 한다(손상). 같은 주소에 열려도 A의 캐시를 쓰지 않는다.
        Array<uint8> b = Handmade(1, 12, 200, &sBlock, 1, system);
        auto reader = CompressedBlocksReader::TryOpen(View(b), reusing);
        REQUIRE(reader);
        CHECK(reader.Value().TryRead(0, Span<uint8>(out, 150), View(b), context.Value()).Error() ==
              ErrorCode::InvalidArgument);
    }
    {
        Array<uint8> random(system);
        Fill(random, 4196, Pattern::Random, 61);
        Array<uint8> container(system);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(CompressionFormat::Lz4, 12), View(random),
                                        container, system));
        REQUIRE(StoredSize(container, 0) == 4096u);
        auto reader = CompressedBlocksReader::TryOpen(View(container), system);
        REQUIRE(reader);
        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), system);
        REQUIRE(context);
        uint8 out[16] = {};

        // 캐시 hit여도 stored가 블록을 덮지 않으면 거부한다.
        REQUIRE(reader.Value().TryRead(4096 + 10, Span<uint8>(out, 16), View(container),
                                       context.Value()));
        CHECK(reader.Value()
                  .TryRead(4096 + 20, Span<uint8>(out, 16),
                           Slice(View(container), 0, container.Size() - 1), context.Value())
                  .Error() == ErrorCode::InvalidArgument);

        // verify 없이 손상 블록을 캐시한 뒤 verify 읽기는 CRC를 확인한다.
        Array<uint8> corrupt(system);
        REQUIRE(corrupt.TryAppend(View(container)));
        corrupt[reader.Value().IndexBytes() + 5] ^= 0x01;
        context.Value().Invalidate();
        CHECK(
            reader.Value().TryRead(0, Span<uint8>(out, 10), View(corrupt), context.Value(), false));
        CHECK(reader.Value()
                  .TryRead(2, Span<uint8>(out, 10), View(corrupt), context.Value())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(reader.Value().TryRead(2, Span<uint8>(out, 10), View(container), context.Value()));
        CHECK(Same(Span<const uint8>(out, 10), Slice(View(random), 2, 10)));
    }
    CHECK(system.Stats().liveAllocations == 0);
}

// CRC는 맞지만 코덱 데이터가 블록 원본 크기와 맞지 않거나 사전을 요구하면 InvalidArgument다.
TEST(CompressedBlocks_CodecDataMustMatchBlock) {
    SystemAllocator allocator;
    {
        // LZ4: literal 5byte("hello")만 있는 block. 원본 4096을 선언했다.
        static constexpr uint8 Lz4Short[] = {0x50, 'h', 'e', 'l', 'l', 'o'};
        // Deflate: 마지막 stored block 5byte.
        static constexpr uint8 DeflateShort[] = {0x01, 0x05, 0x00, 0xfa, 0xff,
                                                 'h',  'e',  'l',  'l',  'o'};
        // zstd: dictID 7을 요구하는 frame(RLE 'D' × 4096, 원본 크기 4096).
        static constexpr uint8 ZstdDict[] = {0x28, 0xb5, 0x2f, 0xfd, 0x61, 0x07,
                                             0x00, 0x0f, 0x03, 0x80, 0x00, 0x44};
        // zstd: 원본 크기 100을 기록한 frame(RLE). 블록은 4096을 선언했다.
        static constexpr uint8 ZstdLie[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20,
                                            0x64, 0x23, 0x03, 0x00, 0x44};
        struct Case {
            uint8 codec;
            Span<const uint8> block;
        };
        const Case Cases[] = {{1, Bytes(Lz4Short)},
                              {3, Bytes(DeflateShort)},
                              {2, Bytes(ZstdDict)},
                              {2, Bytes(ZstdLie)}};
        for (const Case& item : Cases) {
            Array<uint8> container = Handmade(item.codec, 12, 4096, &item.block, 1, allocator);
            CHECK(OpenError(container) == ErrorCode::None);
            CHECK(ReadError(container, 0, 4096) == ErrorCode::InvalidArgument);
            CHECK(ReadError(container, 100, 10) == ErrorCode::InvalidArgument);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 헤더 규칙을 TryGetIndexBytes로 따로 확인하고, 인덱스만 먼저 읽는 파일 흐름을 시험한다.
TEST(CompressedBlocks_HeaderRulesAndFileFlow) {
    SystemAllocator allocator;
    {
        auto header = [](uint64 rawSize, uint32 count, uint64 payload) {
            uint8 bytes[48] = {'R', 'C', 'B', 'K', 2, 0, 1, 12};
            StoreLittleEndian<uint64>(bytes + 8, rawSize);
            StoreLittleEndian<uint32>(bytes + 16, count);
            StoreLittleEndian<uint64>(bytes + 24, payload);
            struct Bytes48 {
                uint8 data[48];
            } result{};
            platform::MemoryCopy(result.data, bytes, 48);
            return result;
        };
        auto indexBytes = [](const auto& bytes, uint64 containerBytes) {
            return CompressedBlocksReader::TryGetIndexBytes(Span<const uint8>(bytes.data, 48),
                                                            containerBytes);
        };
        // payload < 블록 수, payload > rawSize(전체 길이는 맞춘다).
        CHECK(indexBytes(header(8192, 2, 1), 48 + 16 + 1).Error() == ErrorCode::InvalidArgument);
        CHECK(indexBytes(header(10, 1, 11), 48 + 8 + 11).Error() == ErrorCode::InvalidArgument);
        // 48byte 미만.
        const auto good = header(10, 1, 5);
        CHECK(indexBytes(good, 48 + 8 + 5).Value() == 56);
        CHECK(CompressedBlocksReader::TryGetIndexBytes(Span<const uint8>(good.data, 47), 61)
                  .Error() == ErrorCode::InvalidArgument);
        // 최대 블록 수(2^32 − 1)는 길이만 맞으면 된다. rawSize 2^64 − 1은 블록 수가 u32를 넘는다.
        const uint64 maxCount = 0xffffffffull;
        CHECK(
            indexBytes(header(maxCount << 12, 0xffffffffu, maxCount), 48 + 9 * maxCount).Value() ==
            48 + 8 * maxCount);
        CHECK(indexBytes(header(~uint64{0}, 0xffffffffu, maxCount), 48 + 9 * maxCount).Error() ==
              ErrorCode::InvalidArgument);

        // 파일 흐름: 헤더 → 인덱스만 → TryLocate → 필요한 저장 byte만 읽는다.
        Array<uint8> original(allocator);
        Fill(original, 3 * 4096 + 5, Pattern::Text, 67);
        Array<uint8> container(allocator);
        REQUIRE(TryCompressBlocksAppend(BlockSettings(CompressionFormat::Deflate, 12),
                                        View(original), container, allocator));
        auto index = CompressedBlocksReader::TryGetIndexBytes(Slice(View(container), 0, 48),
                                                              container.Size());
        REQUIRE(index);
        CHECK(CompressedBlocksReader::TryOpen(Slice(View(container), 0, index.Value() - 1),
                                              container.Size(), allocator)
                  .Error() == ErrorCode::InvalidArgument);
        auto reader = CompressedBlocksReader::TryOpen(Slice(View(container), 0, index.Value()),
                                                      container.Size(), allocator);
        REQUIRE(reader);
        CHECK(reader.Value().ContainerBytes() == container.Size());
        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
        REQUIRE(context);

        // 블록 경계·마지막 짧은 블록에서 TryLocate가 맞고, 같은 범위를 그 저장 byte만으로 읽는다.
        const uint64 size = original.Size();
        const uint64 Ranges[][2] = {{4096, 4096}, {4095, 2}, {8191, 1}, {12288, 5}, {12292, 1}};
        for (const auto& range : Ranges) {
            auto where = reader.Value().TryLocate(range[0], range[1]);
            REQUIRE(where);
            const uint32 first = static_cast<uint32>(range[0] >> 12);
            const uint32 last = static_cast<uint32>((range[0] + range[1] - 1) >> 12);
            CHECK(where.Value().firstBlock == first &&
                  where.Value().blockCount == last - first + 1);
            uint64 offset = index.Value();
            for (uint32 block = 0; block < first; ++block) {
                offset += StoredSize(container, block);
            }
            uint64 bytes = 0;
            for (uint32 block = first; block <= last; ++block) {
                bytes += StoredSize(container, block);
            }
            CHECK(where.Value().storedOffset == offset && where.Value().storedBytes == bytes);
            Array<uint8> part(allocator);
            REQUIRE(part.TryResize(static_cast<usize>(range[1])));
            CHECK(reader.Value().TryRead(
                range[0], Span<uint8>(part.Data(), part.Size()),
                Slice(View(container), static_cast<usize>(offset), static_cast<usize>(bytes)),
                offset, context.Value()));
            CHECK(Same(part, Slice(View(original), static_cast<usize>(range[0]),
                                   static_cast<usize>(range[1]))));
        }
        CHECK(reader.Value().TryLocate(size, 0));
        CHECK(reader.Value().TryLocate(size + 1, 0).Error() == ErrorCode::InvalidArgument);
        uint8 one = 0;
        CHECK(reader.Value()
                  .TryRead(0, Span<uint8>(&one, 1), View(container), ~uint64{0}, context.Value())
                  .Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 호출 스레드가 job을 다 끝내면, 유일한 worker가 다른 일로 막혀 있어도 남은 helper를 회수하고
// 반환한다(예전에는 worker가 풀릴 때까지 기다렸다).
TEST(CompressedBlocks_ParallelReturnsWhileWorkersBusy) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, usize{3} << 20, Pattern::Text, 71);
        const auto settings = BlockSettings(CompressionFormat::Lz4, 14);
        Array<uint8> expected(allocator);
        REQUIRE(TryCompressBlocksAppend(settings, View(original), expected, allocator));
        auto reader = CompressedBlocksReader::TryOpen(View(expected), allocator);
        REQUIRE(reader);

        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 1, .maxInFlight = 64}));
        platform::ManualResetEvent running;
        platform::ManualResetEvent release;
        // 막힌 worker는 최대 10초 뒤 스스로 풀린다(결함이 다시 생겨도 시험이 멈추지 않는다).
        auto blocker = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [&](WorkerTaskContext&) noexcept {
                running.Signal();
                (void)release.Wait(10000);
            });
        REQUIRE(blocker);
        auto blocked = scheduler.Submit(Move(blocker.Value()));
        REQUIRE(blocked);
        REQUIRE(running.Wait(10000) == platform::WaitResult::Signaled);

        const uint64 start = platform::MonotonicNanoseconds();
        Array<uint8> parallel(allocator);
        CHECK(TryCompressBlocksAppend(scheduler, settings, View(original), parallel, allocator));
        Array<uint8> restored(allocator);
        REQUIRE(restored.TryResize(original.Size()));
        CHECK(reader.Value().TryRead(scheduler, 0, Span<uint8>(restored.Data(), restored.Size()),
                                     View(expected), 0, allocator));
        const uint64 elapsed = platform::MonotonicNanoseconds() - start;
        release.Signal();
        CHECK(elapsed < uint64{5000000000});
        CHECK(Same(parallel, View(expected)));
        CHECK(Same(restored, View(original)));
        REQUIRE(blocked.Value().Wait());
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// scheduler 형태(worker 1개·maxInFlight가 helper보다 작음·시작하지 않음)와 수준·데이터에 관계없이
// 병렬 결과는 순차와 같다. 병렬 읽기는 순차 기준 경계(블록 4·5개)와 job당 블록 하나(1MiB 블록)도
// 맞다.
TEST(CompressedBlocks_ParallelShapesLevelsAndBoundaries) {
    SystemAllocator allocator;
    {
        Array<uint8> mixed(allocator);
        Fill(mixed, (usize{3} << 19) + 77, Pattern::Text, 73);
        Array<uint8> noise(allocator);
        Fill(noise, usize{1} << 19, Pattern::Random, 79);
        for (usize index = 0; index < noise.Size(); index += 2) {
            mixed[index] = noise[index]; // 앞 512KiB는 반쯤 압축되지 않는다
        }

        struct Shape {
            uint32 workers;
            uint32 maxInFlight;
            bool start;
        };
        static constexpr Shape Shapes[] = {
            {1, 1, true}, {4, 2, true}, {8, 64, true}, {4, 16, false}};
        struct Level {
            CompressionFormat format;
            int32 level;
        };
        static constexpr Level Levels[] = {
            {CompressionFormat::Lz4, 1},     {CompressionFormat::Lz4, 12},
            {CompressionFormat::Zstd, 1},    {CompressionFormat::Zstd, 12},
            {CompressionFormat::Deflate, 0}, {CompressionFormat::Deflate, 9}};
        for (const Shape& shape : Shapes) {
            WorkerScheduler scheduler(allocator);
            if (shape.start) {
                REQUIRE(scheduler.Start(
                    {.workerCount = shape.workers, .maxInFlight = shape.maxInFlight}));
            }
            for (const Level& level : Levels) {
                const auto settings = BlockSettings(level.format, 14, level.level);
                Array<uint8> sequential(allocator);
                REQUIRE(TryCompressBlocksAppend(settings, View(mixed), sequential, allocator));
                Array<uint8> parallel(allocator);
                REQUIRE(
                    TryCompressBlocksAppend(scheduler, settings, View(mixed), parallel, allocator));
                CHECK(Same(parallel, View(sequential)));
            }
            if (shape.start) {
                REQUIRE(scheduler.Shutdown());
            }
        }

        // 병렬 읽기 경계: 256KiB 블록 4개(순차)·5개(병렬), 1MiB 블록 6개(job당 블록 하나).
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 8, .maxInFlight = 64}));
        struct Case {
            uint32 blockSizeLog2;
            usize size;
        };
        const Case Cases[] = {
            {18, usize{4} << 18}, {18, (usize{5} << 18) + 3}, {20, (usize{5} << 20) + 9}};
        for (const Case& item : Cases) {
            Array<uint8> original(allocator);
            Fill(original, item.size, Pattern::Text, item.size);
            Array<uint8> container(allocator);
            REQUIRE(TryCompressBlocksAppend(
                scheduler, BlockSettings(CompressionFormat::Zstd, item.blockSizeLog2, 1),
                View(original), container, allocator));
            auto reader = CompressedBlocksReader::TryOpen(View(container), allocator);
            REQUIRE(reader);
            const usize blockSize = usize{1} << item.blockSizeLog2;
            const usize Ranges[][2] = {
                {0, item.size}, {7, item.size - 14}, {blockSize - 1, blockSize + 2}};
            for (const auto& range : Ranges) {
                Array<uint8> restored(allocator);
                REQUIRE(restored.TryResize(range[1]));
                CHECK(reader.Value().TryRead(scheduler, range[0],
                                             Span<uint8>(restored.Data(), restored.Size()),
                                             View(container), 0, allocator));
                CHECK(Same(restored, Slice(View(original), range[0], range[1])));
            }
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// writer 경계: 빈 원본에 블록을 주면 InvalidArgument, 살아 있는 writer에 이동 대입해도 누수가 없다.
// Deflate 수준 0은 코덱 문맥을 만들지 않는다.
TEST(CompressedBlocks_WriterEdges) {
    SystemAllocator system;
    {
        const auto settings = BlockSettings(CompressionFormat::Zstd, 12);
        auto empty = CompressedBlocksWriter::TryBegin(settings, 0, system);
        REQUIRE(empty);
        Array<uint8> payload(system);
        CHECK(empty.Value().TryAppendBlock(Text("x"), payload).Error() ==
              ErrorCode::InvalidArgument);

        auto first = CompressedBlocksWriter::TryBegin(settings, 8192, system);
        auto second = CompressedBlocksWriter::TryBegin(settings, 4096, system);
        REQUIRE(first && second);
        Array<uint8> block(system);
        Fill(block, 4096, Pattern::Text, 83);
        REQUIRE(first.Value().TryAppendBlock(View(block), payload));
        first.Value() = Move(second.Value());
        CHECK(first.Value().BlockCount() == 1);
        CHECK(second.Value().TryAppendBlock(View(block), payload).Error() ==
              ErrorCode::StaleHandle);

        CountingAllocator counting(system);
        auto stored = CompressedBlocksWriter::TryBegin(
            BlockSettings(CompressionFormat::Deflate, 12, 0), 4096, counting);
        REQUIRE(stored);
        const usize afterBegin = counting.Current();
        Array<uint8> raw(system);
        CHECK(stored.Value().TryAppendBlock(View(block), raw).Value() == 4096);
        CHECK(counting.Current() == afterBegin);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

} // namespace rocks::test
