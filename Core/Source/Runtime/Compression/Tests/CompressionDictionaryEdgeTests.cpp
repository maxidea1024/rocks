#include "CompressionDictionaryTestSupport.h"

#include <Base/Endian.h>
#include <Compression/AsyncCompression.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/CompressionDictionary.h>
#include <Compression/CompressionStream.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <Task/WorkerScheduler.h>

// 압축 사전(Docs/Design/Compression.ko.md 11절)의 경계 시험이다. 스트림 헤더 분할·skippable
// frame, 사전 ID 필드가 0인 frame, 일회성 해제의 frame 순회, 비동기 Span 해제, 수준별 사전, 블록
// 컨테이너의 쓰기 경로·부분 읽기·사전 불일치·이전 형식을 본다.
namespace rocks::test {

using namespace compression;
using namespace dictionary_test;

namespace {

// 레코드와 난수(압축 불가) 구간을 번갈아 이어 붙인다. 블록 컨테이너가 압축에 실패한 블록을 raw로
// 저장한 뒤 같은 문맥으로 다음 블록을 압축하는 경로를 함께 거친다.
void MakeMixedRecords(Array<uint8>& out, usize bytes) {
    Array<uint8> part(out.GetAllocator());
    REQUIRE(out.TryResize(0));
    for (uint32 index = 0; out.Size() < bytes; ++index) {
        const usize remaining = bytes - out.Size();
        const usize wanted = 50000 + usize{index % 5} * 9001;
        const usize size = wanted < remaining ? wanted : remaining;
        if (index % 2 == 0) {
            MakeRecords(part, size, 100 + index);
        } else {
            Fill(part, size, Pattern::Random, 0x9e3779b97f4a7c15ull + index);
        }

        REQUIRE(part.Size() == size);
        REQUIRE(out.TryAppend(View(part)));
    }
}

// 스트림 해제기에 input을 step byte씩 넣고 끝을 확인한다. 첫 오류를 돌려준다.
ErrorCode DecodeInSteps(DecompressStream& decoder, Span<const uint8> input, usize step,
                        Array<uint8>& out) {
    for (usize offset = 0; offset < input.Size(); offset += step) {
        const usize piece = input.Size() - offset < step ? input.Size() - offset : step;
        auto written = decoder.TryWriteAppend(Slice(input, offset, piece), out);
        if (!written) {
            return written.Error();
        }
    }

    auto finished = decoder.TryFinish();
    return finished ? ErrorCode::None : finished.Error();
}

// 헤더·표를 고친 컨테이너의 indexCrc32를 다시 계산한다(CRC 뒤의 검사를 시험할 때).
void ResealIndex(Array<uint8>& container) {
    const uint32 count = LoadLittleEndian<uint32>(container.Data() + 16);
    uint32 crc = Crc32(Slice(View(container), 0, 44));
    crc = Crc32(Slice(View(container), CompressedBlocksHeaderBytes, usize{8} * count), crc);
    StoreLittleEndian<uint32>(container.Data() + 44, crc);
}

// writer로 블록을 하나씩 압축해 컨테이너(인덱스 + payload)를 만든다.
void WriteBlocks(const CompressedBlocksSettings& settings, Span<const uint8> source,
                 Array<uint8>& out, Allocator& allocator) {
    auto writer = CompressedBlocksWriter::TryBegin(settings, source.Size(), allocator);
    REQUIRE(writer);
    Array<uint8> payload(allocator);
    const usize blockSize = settings.BlockSize();
    for (usize offset = 0; offset < source.Size(); offset += blockSize) {
        const usize size = source.Size() - offset < blockSize ? source.Size() - offset : blockSize;
        REQUIRE(writer.Value().TryAppendBlock(Slice(source, offset, size), payload));
    }

    REQUIRE(out.TryResize(writer.Value().IndexBytes()));
    REQUIRE(writer.Value().TryFinishIndex(Span<uint8>(out.Data(), out.Size())));
    REQUIRE(out.TryAppend(View(payload)));
}

} // namespace

// 스트림 해제: frame 헤더가 호출 사이에서 나뉘어도(1byte씩 포함) skippable frame을 사이에 둔 사전
// frame을 모두 풀고, 사전 ID 없는 frame은 앞 frame을 다 내보낸 뒤 거부한다.
TEST(CompressionDictionary_StreamSplitHeadersAndSkippableFrames) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 9000, 3);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 3, allocator);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        Array<uint8> withA(allocator);
        CompressWith(encoding, View(original), withA, allocator);
        Array<uint8> plain(allocator);
        REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 3), View(original), plain));
        const uint8 skippable[] = {0x50, 0x2A, 0x4D, 0x18, 3, 0, 0, 0, 7, 7, 7};

        // [skippable][A][skippable][A]
        Array<uint8> joined(allocator);
        REQUIRE(joined.TryAppend(Bytes(skippable)));
        REQUIRE(joined.TryAppend(View(withA)));
        REQUIRE(joined.TryAppend(Bytes(skippable)));
        REQUIRE(joined.TryAppend(View(withA)));
        Array<uint8> twice(allocator);
        REQUIRE(twice.TryAppend(View(original)));
        REQUIRE(twice.TryAppend(View(original)));
        const usize steps[] = {1, 2, 3, 5, 7, 13};
        for (const usize step : steps) {
            auto decoder =
                DecompressStream::TryCreate(decoding, {.maxOutputBytes = twice.Size()}, allocator);
            REQUIRE(decoder);
            Array<uint8> out(allocator);
            CHECK(DecodeInSteps(decoder.Value(), View(joined), step, out) == ErrorCode::None);
            CHECK(Same(out, View(twice)));
        }

        // [A][사전 ID 없는 frame]을 1byte씩: A는 모두 나오고 둘째 frame 헤더에서 Unsupported다.
        Array<uint8> mixed(allocator);
        REQUIRE(mixed.TryAppend(View(withA)));
        REQUIRE(mixed.TryAppend(View(plain)));
        auto decoder =
            DecompressStream::TryCreate(decoding, {.maxOutputBytes = twice.Size()}, allocator);
        REQUIRE(decoder);
        Array<uint8> out(allocator);
        CHECK(DecodeInSteps(decoder.Value(), View(mixed), 1, out) == ErrorCode::Unsupported);
        CHECK(Same(out, View(original)));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 스트림 해제는 사전 ID 검사를 window 검사보다 먼저 한다. 사전 ID 없는 frame은 window가 호출자
// 상한을 넘어도 Unsupported이고(상한 설정에 따라 오류가 바뀌지 않는다), 사전 ID가 맞는 frame만
// window 상한(CapacityExceeded)을 본다.
TEST(CompressionDictionary_StreamChecksIdBeforeWindow) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 9000, 5);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 19, allocator);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);

        // 큰 크기 힌트로 window 8MiB(수준 19)인 frame을 만든다(원본 크기 미기록).
        auto frameWithLargeWindow = [&](CompressStream stream, Array<uint8>& packed) {
            REQUIRE(stream.TryWriteAppend(View(original), packed));
            REQUIRE(stream.TryFinishAppend(packed));
        };
        Array<uint8> plain(allocator);
        auto plainStream = CompressStream::TryCreate(Settings(CompressionFormat::Zstd, 19),
                                                     allocator, uint64{64} << 20);
        REQUIRE(plainStream);
        frameWithLargeWindow(Move(plainStream.Value()), plain);
        Array<uint8> withA(allocator);
        auto dictionaryStream = CompressStream::TryCreate(encoding, allocator, uint64{64} << 20);
        REQUIRE(dictionaryStream);
        frameWithLargeWindow(Move(dictionaryStream.Value()), withA);

        auto decodeError = [&](const DecompressDictionary* dictionary, const Array<uint8>& packed,
                               uint32 maxWindowLog) {
            const DecompressStreamLimits limits{.maxOutputBytes = original.Size(),
                                                .maxWindowLog = maxWindowLog};
            auto decoder =
                dictionary != nullptr
                    ? DecompressStream::TryCreate(*dictionary, limits, allocator)
                    : DecompressStream::TryCreate(CompressionFormat::Zstd, limits, allocator);
            if (!decoder) {
                return decoder.Error();
            }

            Array<uint8> out(allocator);
            return DecodeInSteps(decoder.Value(), View(packed), 4096, out);
        };
        // 사전 없는 해제기에서는 window 상한 초과다(상한 20 < window 23).
        CHECK(decodeError(nullptr, plain, 20) == ErrorCode::CapacityExceeded);
        CHECK(decodeError(nullptr, plain, 23) == ErrorCode::None);
        // 형식 사전 해제기는 상한과 무관하게 사전 ID 없는 frame을 Unsupported로 거부한다.
        CHECK(decodeError(&decoding, plain, 20) == ErrorCode::Unsupported);
        CHECK(decodeError(&decoding, plain, 23) == ErrorCode::Unsupported);
        // 사전 ID가 맞는 frame은 window 상한을 본다.
        CHECK(decodeError(&decoding, withA, 20) == ErrorCode::CapacityExceeded);
        CHECK(decodeError(&decoding, withA, 23) == ErrorCode::None);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사전 ID 필드가 있지만 값이 0인 frame은 사전 ID 없는 frame과 같다. 사전 없는 해제와 raw 사전은
// 풀고, 형식 사전 문맥은 일회성·문맥·스트림 모두 Unsupported로 거부한다.
TEST(CompressionDictionary_RejectsExplicitZeroIdField) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 9000, 7);
        Array<uint8> plain(allocator);
        REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 3), View(original), plain));

        // frame 헤더 설명자(offset 4)의 사전 ID flag를 1byte로 바꾸고 window 설명자 뒤에 0을
        // 넣는다.
        const uint8 descriptor = plain[4];
        REQUIRE((descriptor & 0x03) == 0);
        const usize idAt = (descriptor & 0x20) != 0 ? 5 : 6;
        Array<uint8> zeroId(allocator);
        REQUIRE(zeroId.TryAppend(Slice(View(plain), 0, idAt)));
        zeroId[4] = static_cast<uint8>(descriptor | 0x01);
        REQUIRE(zeroId.TryPushBack(0));
        REQUIRE(zeroId.TryAppend(Slice(View(plain), idAt, plain.Size() - idAt)));
        CHECK(TryGetZstdFrameDictionaryId(View(zeroId)).Value() == 0);

        Array<uint8> restored(allocator);
        REQUIRE(
            TryDecompressAppend(CompressionFormat::Zstd, View(zeroId), original.Size(), restored));
        CHECK(Same(restored, View(original)));
        Array<uint8> rawBytes(allocator);
        DictionaryBytes(rawBytes, Dictionary::Raw4K);
        DecompressDictionary raw = MakeDecompress(View(rawBytes), allocator);
        REQUIRE(restored.TryResize(0));
        REQUIRE(TryDecompressAppend(raw, View(zeroId), original.Size(), restored));
        CHECK(Same(restored, View(original)));

        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        CHECK(TryDecompressAppend(decoding, View(zeroId), original.Size(), restored).Error() ==
              ErrorCode::Unsupported);
        auto decompressor = Decompressor::TryCreate(decoding, allocator);
        REQUIRE(decompressor);
        REQUIRE(restored.TryResize(original.Size()));
        CHECK(decompressor.Value()
                  .TryDecompress(View(zeroId), Span<uint8>(restored.Data(), restored.Size()))
                  .Error() == ErrorCode::Unsupported);
        auto decoder =
            DecompressStream::TryCreate(decoding, {.maxOutputBytes = original.Size()}, allocator);
        REQUIRE(decoder);
        Array<uint8> streamed(allocator);
        CHECK(DecodeInSteps(decoder.Value(), View(zeroId), 1, streamed) == ErrorCode::Unsupported);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 일회성 해제는 frame을 모두 훑어 확인한다. 원본 크기를 기록하지 않은 frame(스트림) 뒤의 사전 ID
// 없는 frame도 거부하고, 사전 frame 뒤의 잘린 꼬리는 손상(InvalidArgument)이다.
TEST(CompressionDictionary_OneShotWalksEveryFrame) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 5000, 9);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 5, allocator);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        Array<uint8> withA(allocator);
        CompressWith(encoding, View(original), withA, allocator);
        Array<uint8> streamed(allocator);
        auto stream = CompressStream::TryCreate(encoding, allocator);
        REQUIRE(stream);
        REQUIRE(stream.Value().TryWriteAppend(View(original), streamed));
        REQUIRE(stream.Value().TryFinishAppend(streamed));
        Array<uint8> plain(allocator);
        REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 3), View(original), plain));

        // [A][스트림 A(원본 크기 미기록)]는 풀리고 뒤에 사전 ID 없는 frame이 붙으면 Unsupported다.
        Array<uint8> chain(allocator);
        REQUIRE(chain.TryAppend(View(withA)));
        REQUIRE(chain.TryAppend(View(streamed)));
        auto decompressor = Decompressor::TryCreate(decoding, allocator);
        REQUIRE(decompressor);
        Array<uint8> restored(allocator);
        REQUIRE(decompressor.Value().TryDecompressAppend(View(chain), 1 << 20, restored));
        CHECK(restored.Size() == 2 * original.Size());
        CHECK(Same(Slice(View(restored), original.Size(), original.Size()), View(original)));
        REQUIRE(chain.TryAppend(View(plain)));
        REQUIRE(restored.TryResize(0));
        CHECK(decompressor.Value().TryDecompressAppend(View(chain), 1 << 20, restored).Error() ==
              ErrorCode::Unsupported);
        CHECK(restored.Size() == 0);

        // 사전 frame 뒤에 다음 frame의 magic 3byte·skippable 헤더 5byte만 있으면 손상이다.
        const uint8 skippableHead[] = {0x50, 0x2A, 0x4D, 0x18, 3};
        const Span<const uint8> tails[] = {Slice(View(plain), 0, 3), Bytes(skippableHead)};
        for (const Span<const uint8>& tail : tails) {
            Array<uint8> truncated(allocator);
            REQUIRE(truncated.TryAppend(View(withA)));
            REQUIRE(truncated.TryAppend(tail));
            CHECK(TryDecompressAppend(decoding, View(truncated), 1 << 20, restored).Error() ==
                  ErrorCode::InvalidArgument);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 비동기 Span 해제(TrySubmitDecompress의 destination 변형)에 사전을 쓴다. 제출 뒤 호출자 handle을
// 놓아도 작업이 사전을 유지하고, 사전 ID 없는 frame은 Unsupported다.
TEST(CompressionDictionary_AsyncDecompressIntoSpan) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            Array<uint8> original(allocator);
            MakeRecords(original, 20000, 11);
            Array<uint8> withA(allocator);
            CompressWith(MakeCompress(DictionaryA(), 3, allocator), View(original), withA,
                         allocator);
            Array<uint8> plain(allocator);
            REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 3), View(original), plain));
            Array<uint8> target(allocator);
            Array<uint8> other(allocator);
            REQUIRE(target.TryResize(original.Size()));
            REQUIRE(other.TryResize(original.Size()));

            CompressionTask decoded;
            CompressionTask rejected;
            CompressionTask unsubmitted;
            {
                DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
                REQUIRE(TrySubmitDecompress(scheduler, decoded, decoding, View(withA),
                                            Span<uint8>(target.Data(), target.Size()), allocator));
                REQUIRE(TrySubmitDecompress(scheduler, rejected, decoding, View(plain),
                                            Span<uint8>(other.Data(), other.Size()), allocator));
                const DecompressDictionary empty;
                CHECK(TrySubmitDecompress(scheduler, unsubmitted, empty, View(plain),
                                          Span<uint8>(other.Data(), other.Size()), allocator)
                          .Error() == ErrorCode::StaleHandle);
            }

            auto written = decoded.TryWaitResult();
            REQUIRE(written);
            CHECK(written.Value() == original.Size());
            CHECK(Same(target, View(original)));
            CHECK(rejected.TryWaitResult().Error() == ErrorCode::Unsupported);
        }

        REQUIRE(scheduler.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 수준은 압축 사전에 고정된다. 같은 사전 byte라도 수준 1과 19는 다른 CDict·다른 frame이고, 해제
// 사전 하나로 둘 다 풀린다. 같은 수준으로 다시 만들면 같은 byte다.
TEST(CompressionDictionary_LevelIsPartOfDictionary) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 20000, 13);
        CompressDictionary fast = MakeCompress(DictionaryA(), 1, allocator);
        CompressDictionary best = MakeCompress(DictionaryA(), 19, allocator);
        CHECK(fast.Settings().Level() == 1);
        CHECK(best.Settings().Level() == 19);
        CHECK(fast.Id() == best.Id());
        CHECK(fast.Digest() == best.Digest());
        CHECK(fast.MemoryBytes() != best.MemoryBytes());

        Array<uint8> packedFast(allocator);
        Array<uint8> packedBest(allocator);
        CompressWith(fast, View(original), packedFast, allocator);
        CompressWith(best, View(original), packedBest, allocator);
        CHECK(!Same(packedFast, View(packedBest)));
        CHECK(packedBest.Size() < packedFast.Size());

        Array<uint8> again(allocator);
        CompressWith(MakeCompress(DictionaryA(), 19, allocator), View(original), again, allocator);
        CHECK(Same(again, View(packedBest)));

        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        const Array<uint8>* const packedFrames[] = {&packedFast, &packedBest};
        for (const Array<uint8>* packed : packedFrames) {
            Array<uint8> restored(allocator);
            REQUIRE(TryDecompressAppend(decoding, View(*packed), original.Size(), restored));
            CHECK(Same(restored, View(original)));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 블록 컨테이너의 writer·일회성·병렬 압축이 사전 블록에서도 같은 byte다. 압축 불가 구간(raw 저장)과
// 사전 재적재 크기의 블록(256KiB, 사전 A의 6배 이상), raw 사전을 포함한다.
TEST(CompressionDictionary_BlockWriterPathsAgree) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 128}));
        {
            // 병렬 압축은 1MiB 이상·블록 5개 이상일 때만 나눈다.
            Array<uint8> original(allocator);
            MakeMixedRecords(original, (usize{1} << 20) + 100000);
            Array<uint8> rawBytes(allocator);
            DictionaryBytes(rawBytes, Dictionary::Raw4K);
            struct Case {
                Span<const uint8> dictionary;
                int32 level;
                uint32 blockSizeLog2;
            };
            const Case cases[] = {
                {DictionaryA(), 3, 12},
                {DictionaryA(), 3, 18},
                {View(rawBytes), 1, 14},
            };
            for (const Case& item : cases) {
                CompressDictionary encoding = MakeCompress(item.dictionary, item.level, allocator);
                auto settings = CompressedBlocksSettings::TryMake(encoding, item.blockSizeLog2);
                REQUIRE(settings);
                Array<uint8> oneShot(allocator);
                REQUIRE(
                    TryCompressBlocksAppend(settings.Value(), View(original), oneShot, allocator));
                Array<uint8> written(allocator);
                WriteBlocks(settings.Value(), View(original), written, allocator);
                CHECK(Same(written, View(oneShot)));
                Array<uint8> parallel(allocator);
                REQUIRE(TryCompressBlocksAppend(scheduler, settings.Value(), View(original),
                                                parallel, allocator));
                CHECK(Same(parallel, View(oneShot)));

                // 64KiB보다 작은 블록이면 난수 구간만 담은 블록(raw 저장)과 압축된 블록이 모두
                // 있다(난수 구간은 50KB 이상이다). 256KiB 블록은 모두 레코드를 포함해 압축된다.
                const uint32 count = LoadLittleEndian<uint32>(oneShot.Data() + 16);
                uint32 rawBlocks = 0;
                for (uint32 block = 0; block < count; ++block) {
                    const uint32 stored = LoadLittleEndian<uint32>(
                        oneShot.Data() + CompressedBlocksHeaderBytes + usize{8} * block);
                    const usize rawSize =
                        block + 1 < count ? usize{1} << item.blockSizeLog2
                                          : original.Size() - (usize{block} << item.blockSizeLog2);
                    rawBlocks += stored == rawSize ? 1 : 0;
                }

                CHECK(item.blockSizeLog2 >= 16 || rawBlocks > 0);
                CHECK(rawBlocks < count);

                const DecompressDictionary candidates[] = {
                    MakeDecompress(item.dictionary, allocator)};
                auto reader = CompressedBlocksReader::TryOpen(View(oneShot), candidates, allocator);
                REQUIRE(reader);
                Array<uint8> restored(allocator);
                REQUIRE(restored.TryResize(original.Size()));
                REQUIRE(reader.Value().TryRead(scheduler, 0,
                                               Span<uint8>(restored.Data(), restored.Size()),
                                               View(oneShot), 0, allocator));
                CHECK(Same(restored, View(original)));
            }
        }

        REQUIRE(scheduler.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사전 컨테이너의 인덱스 크기 조회(후보 없이), 양 끝이 블록 일부인 순차·병렬 읽기, 해제에 실패한
// 뒤의 읽기 문맥 재사용(사전이 떼어져 사전 없는 컨테이너를 그대로 읽는다).
TEST(CompressionDictionary_BlockPartialReadsAndIndexBytes) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 128}));
        {
            Array<uint8> original(allocator);
            MakeRecords(original, (usize{5} << 18) + 4321, 17);
            CompressDictionary encoding = MakeCompress(DictionaryA(), 3, allocator);
            auto settings = CompressedBlocksSettings::TryMake(encoding, 14);
            REQUIRE(settings);
            Array<uint8> container(allocator);
            REQUIRE(
                TryCompressBlocksAppend(settings.Value(), View(original), container, allocator));
            const uint32 count = LoadLittleEndian<uint32>(container.Data() + 16);
            const usize indexBytes = CompressedBlocksHeaderBytes + usize{8} * count;

            // 헤더만으로 인덱스 크기를 구한다(사전 후보가 필요 없다).
            auto measured = CompressedBlocksReader::TryGetIndexBytes(
                Slice(View(container), 0, CompressedBlocksHeaderBytes), container.Size());
            REQUIRE(measured);
            CHECK(measured.Value() == indexBytes);

            const DecompressDictionary candidates[] = {MakeDecompress(DictionaryA(), allocator)};
            auto reader = CompressedBlocksReader::TryOpen(Slice(View(container), 0, indexBytes),
                                                          container.Size(), candidates, allocator);
            REQUIRE(reader);
            CHECK(reader.Value().UsesDictionary());

            // 병렬 읽기: 앞뒤 끝이 블록 중간이고 1MiB를 넘어 helper가 참여한다.
            const uint64 from = 5000;
            Array<uint8> out(allocator);
            REQUIRE(out.TryResize((usize{1} << 20) + 777));
            REQUIRE(reader.Value().TryRead(scheduler, from, Span<uint8>(out.Data(), out.Size()),
                                           View(container), 0, allocator));
            CHECK(Same(out, Slice(View(original), from, out.Size())));

            // 순차 읽기: 블록 세 개에 걸친 부분 읽기.
            auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
            REQUIRE(context);
            REQUIRE(out.TryResize(40000));
            REQUIRE(reader.Value().TryRead(from, Span<uint8>(out.Data(), out.Size()),
                                           View(container), context.Value()));
            CHECK(Same(out, Slice(View(original), from, out.Size())));

            // 첫 블록의 frame magic을 깨고 CRC 검사 없이 읽으면 해제가 실패한다. 같은 문맥으로
            // 사전 없는 컨테이너와 원래 컨테이너를 다시 읽을 수 있다.
            Array<uint8> corrupt(allocator);
            REQUIRE(corrupt.TryAppend(View(container)));
            REQUIRE(LoadLittleEndian<uint32>(container.Data() + CompressedBlocksHeaderBytes) <
                    (uint32{1} << 14));
            corrupt[indexBytes] ^= 0xFF;
            REQUIRE(out.TryResize(usize{1} << 14));
            CHECK(reader.Value()
                      .TryRead(0, Span<uint8>(out.Data(), out.Size()), View(corrupt),
                               context.Value(), false)
                      .Error() == ErrorCode::InvalidArgument);

            Array<uint8> plain(allocator);
            REQUIRE(TryCompressBlocksAppend(
                CompressedBlocksSettings::TryMake(Settings(CompressionFormat::Zstd, 3), 14).Value(),
                Slice(View(original), 0, 100000), plain, allocator));
            auto plainReader = CompressedBlocksReader::TryOpen(View(plain), allocator);
            REQUIRE(plainReader);
            REQUIRE(plainReader.Value().TryRead(0, Span<uint8>(out.Data(), out.Size()), View(plain),
                                                context.Value()));
            CHECK(Same(out, Slice(View(original), 0, out.Size())));
            REQUIRE(reader.Value().TryRead(0, Span<uint8>(out.Data(), out.Size()), View(container),
                                           context.Value()));
            CHECK(Same(out, Slice(View(original), 0, out.Size())));
        }

        REQUIRE(scheduler.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 블록 컨테이너의 사전 불일치와 이전 형식: 헤더 지문은 A인데 블록이 다른 사전 ID로 압축됐으면
// 읽기가 InvalidArgument, raw 사전 컨테이너는 같은 raw 사전으로만 열리며(다른 raw 사전은
// Unsupported), 헤더 40byte였던 version 1 컨테이너는 Unsupported다.
TEST(CompressionDictionary_BlockDictionaryMismatches) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 64000, 19);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        const DecompressDictionary candidates[] = {decoding};

        // 사전 ID 0x2B2B2B2B로 압축한 블록에 A의 지문을 쓰고 CRC를 다시 맞춘다.
        Array<uint8> otherBytes(allocator);
        DictionaryWithId(otherBytes, 0x2B2B2B2Bu);
        CompressDictionary other = MakeCompress(View(otherBytes), 3, allocator);
        Array<uint8> forged(allocator);
        REQUIRE(TryCompressBlocksAppend(CompressedBlocksSettings::TryMake(other, 12).Value(),
                                        View(original), forged, allocator));
        platform::MemoryCopy(forged.Data() + 32, decoding.Digest().bytes, 8);
        ResealIndex(forged);
        auto forgedReader = CompressedBlocksReader::TryOpen(View(forged), candidates, allocator);
        REQUIRE(forgedReader);
        auto context = CompressedBlocksReadContext::TryCreate(forgedReader.Value(), allocator);
        REQUIRE(context);
        Array<uint8> out(allocator);
        REQUIRE(out.TryResize(4096));
        CHECK(forgedReader.Value()
                  .TryRead(0, Span<uint8>(out.Data(), out.Size()), View(forged), context.Value())
                  .Error() == ErrorCode::InvalidArgument);

        // raw 사전 컨테이너: 블록 frame에 사전 ID가 없어 지문만으로 맞춘다.
        Array<uint8> rawBytes(allocator);
        DictionaryBytes(rawBytes, Dictionary::Raw4K);
        Array<uint8> otherRaw(allocator);
        MakeRecords(otherRaw, 4096, 5);
        Array<uint8> container(allocator);
        REQUIRE(TryCompressBlocksAppend(
            CompressedBlocksSettings::TryMake(MakeCompress(View(rawBytes), 3, allocator), 12)
                .Value(),
            View(original), container, allocator));
        const uint32 firstStored =
            LoadLittleEndian<uint32>(container.Data() + CompressedBlocksHeaderBytes);
        const usize payload = CompressedBlocksHeaderBytes +
                              usize{8} * LoadLittleEndian<uint32>(container.Data() + 16);
        REQUIRE(firstStored < 4096);
        CHECK(TryGetZstdFrameDictionaryId(Slice(View(container), payload, firstStored)).Value() ==
              0);
        const DecompressDictionary wrongRaw[] = {MakeDecompress(View(otherRaw), allocator)};
        CHECK(CompressedBlocksReader::TryOpen(View(container), wrongRaw, allocator).Error() ==
              ErrorCode::Unsupported);
        const DecompressDictionary rightRaw[] = {MakeDecompress(View(rawBytes), allocator)};
        auto rawReader = CompressedBlocksReader::TryOpen(View(container), rightRaw, allocator);
        REQUIRE(rawReader);
        Array<uint8> restored(allocator);
        REQUIRE(restored.TryResize(original.Size()));
        REQUIRE(rawReader.Value().TryRead(0, Span<uint8>(restored.Data(), restored.Size()),
                                          View(container), context.Value()));
        CHECK(Same(restored, View(original)));

        // 이전 형식(version 1): 헤더 40byte = 앞 32byte, reserved 4byte, CRC 4byte.
        Array<uint8> plain(allocator);
        REQUIRE(TryCompressBlocksAppend(
            CompressedBlocksSettings::TryMake(Settings(CompressionFormat::Zstd, 3), 12).Value(),
            View(original), plain, allocator));
        const uint32 count = LoadLittleEndian<uint32>(plain.Data() + 16);
        Array<uint8> old(allocator);
        REQUIRE(old.TryAppend(Slice(View(plain), 0, 32)));
        StoreLittleEndian<uint16>(old.Data() + 4, 1);
        const uint8 zeros[8] = {};
        REQUIRE(old.TryAppend(Bytes(zeros)));
        REQUIRE(old.TryAppend(Slice(View(plain), CompressedBlocksHeaderBytes,
                                    plain.Size() - CompressedBlocksHeaderBytes)));
        uint32 crc = Crc32(Slice(View(old), 0, 36));
        crc = Crc32(Slice(View(old), 40, usize{8} * count), crc);
        StoreLittleEndian<uint32>(old.Data() + 36, crc);
        CHECK(CompressedBlocksReader::TryGetIndexBytes(
                  Slice(View(old), 0, CompressedBlocksHeaderBytes), old.Size())
                  .Error() == ErrorCode::Unsupported);
        CHECK(CompressedBlocksReader::TryOpen(View(old), allocator).Error() ==
              ErrorCode::Unsupported);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
