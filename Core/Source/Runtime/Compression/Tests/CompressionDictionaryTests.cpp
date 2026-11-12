#include "CompressionDictionaryTestSupport.h"

#include <Base/Endian.h>
#include <Compression/AsyncCompression.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/CompressionDictionary.h>
#include <Compression/CompressionStream.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

// 압축 사전(Docs/Design/Compression.ko.md 11절) 시험이다.
namespace rocks::test {

using namespace compression;
using namespace dictionary_test;
using compression_test::DictionaryGolden;
using compression_test::DictionaryGoldens;

TEST(CompressionDictionary_CreateContracts) {
    SystemAllocator allocator;
    {
        // 형식: zstd만. 아는 다른 형식은 Unsupported, 모르는 값은 InvalidArgument.
        Array<uint8> raw(allocator);
        MakeRecords(raw, 4096, 3);
        CHECK(CompressError(View(raw), allocator, CompressionFormat::Lz4) ==
              ErrorCode::Unsupported);
        CHECK(CompressError(View(raw), allocator, CompressionFormat::Gzip) ==
              ErrorCode::Unsupported);
        CHECK(DecompressError(View(raw), allocator, CompressionFormat::Deflate) ==
              ErrorCode::Unsupported);
        CHECK(DecompressError(View(raw), allocator, static_cast<CompressionFormat>(99)) ==
              ErrorCode::InvalidArgument);

        // 크기: 9 ≤ n ≤ 32MiB.
        CHECK(CompressError(Slice(View(raw), 0, 8), allocator) == ErrorCode::InvalidArgument);
        CHECK(DecompressError(Slice(View(raw), 0, 8), allocator) == ErrorCode::InvalidArgument);
        CHECK(CompressError(Slice(View(raw), 0, 9), allocator) == ErrorCode::None);
        CHECK(DecompressError(Slice(View(raw), 0, 9), allocator) == ErrorCode::None);
        CHECK(CompressError(Span<const uint8>(), allocator) == ErrorCode::InvalidArgument);
        Array<uint8> huge(allocator);
        REQUIRE(huge.TryResize(MaxDictionaryBytes + 1));
        CHECK(CompressError(View(huge), allocator) == ErrorCode::InvalidArgument);
        CHECK(DecompressError(View(huge), allocator) == ErrorCode::InvalidArgument);

        // 식별: 형식 사전은 ID가 있고 raw는 0이다. Digest는 사전 byte의 SHA-256이다.
        CompressDictionary formatted = MakeCompress(DictionaryA(), 7, allocator);
        CHECK(formatted.IsValid());
        CHECK(formatted.Id() == DictionaryIdA);
        CHECK(formatted.Size() == sizeof(FormattedDictionaryA));
        CHECK(formatted.Digest() == Sha256::Hash(DictionaryA()));
        CHECK(formatted.Settings().Format() == CompressionFormat::Zstd);
        CHECK(formatted.Settings().Level() == 7);
        CHECK(formatted.MemoryBytes() > formatted.Size());
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        CHECK(decoding.Id() == DictionaryIdA);
        CHECK(decoding.Format() == CompressionFormat::Zstd);
        CHECK(decoding.Digest() == formatted.Digest());
        CHECK(decoding.MemoryBytes() > decoding.Size());
        CHECK(MakeCompress(View(raw), 3, allocator).Id() == 0);
        CHECK(MakeDecompress(View(raw), allocator).Id() == 0);

        // 손상 형식 사전, magic으로 시작하는 raw, 사전 ID 0인 형식 사전은 InvalidArgument다.
        CHECK(CompressError(Slice(DictionaryA(), 0, 40), allocator) == ErrorCode::InvalidArgument);
        CHECK(DecompressError(Slice(DictionaryA(), 0, 40), allocator) ==
              ErrorCode::InvalidArgument);
        Array<uint8> magicRaw(allocator);
        REQUIRE(magicRaw.TryAppend(Slice(DictionaryA(), 0, 4)));
        REQUIRE(magicRaw.TryAppend(Slice(View(raw), 0, 200)));
        CHECK(CompressError(View(magicRaw), allocator) == ErrorCode::InvalidArgument);
        CHECK(DecompressError(View(magicRaw), allocator) == ErrorCode::InvalidArgument);
        Array<uint8> zeroId(allocator);
        DictionaryWithId(zeroId, 0);
        CHECK(CompressError(View(zeroId), allocator) == ErrorCode::InvalidArgument);
        CHECK(DecompressError(View(zeroId), allocator) == ErrorCode::InvalidArgument);

        // handle: 복사는 같은 사전, 이동된 쪽은 빈 handle이다. 빈 handle은 StaleHandle이다.
        CompressDictionary copy = formatted;
        CHECK(copy.Digest() == formatted.Digest());
        CompressDictionary moved = Move(copy);
        CHECK(!copy.IsValid());
        CHECK(moved.IsValid());
        const CompressDictionary empty;
        const DecompressDictionary emptyDecoding;
        Array<uint8> out(allocator);
        uint8 buffer[64] = {};
        CHECK(Compressor::TryCreate(empty, allocator).Error() == ErrorCode::StaleHandle);
        CHECK(Decompressor::TryCreate(emptyDecoding, allocator).Error() == ErrorCode::StaleHandle);
        CHECK(TryCompressAppend(empty, View(raw), out).Error() == ErrorCode::StaleHandle);
        CHECK(TryCompress(empty, View(raw), Span<uint8>(buffer, 64), allocator).Error() ==
              ErrorCode::StaleHandle);
        CHECK(TryDecompress(emptyDecoding, View(raw), Span<uint8>(buffer, 64), allocator).Error() ==
              ErrorCode::StaleHandle);
        CHECK(TryDecompressAppend(emptyDecoding, View(raw), 100, out).Error() ==
              ErrorCode::StaleHandle);
        CHECK(CompressStream::TryCreate(empty, allocator).Error() == ErrorCode::StaleHandle);
        CHECK(DecompressStream::TryCreate(emptyDecoding, {.maxOutputBytes = 100}, allocator)
                  .Error() == ErrorCode::StaleHandle);
        CHECK(CompressedBlocksSettings::TryMake(empty).Error() == ErrorCode::StaleHandle);
        const DecompressDictionary candidates[] = {decoding, emptyDecoding};
        CHECK(CompressedBlocksReader::TryOpen(View(raw), candidates, allocator).Error() ==
              ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사전 문맥이 표준 ZSTD_createCDict(L) 경로와 같은 byte를 만든다(11절 수준 매개변수 정책). 작은
// 입력(사전 표 사용), 큰 입력(그 수준의 전체 표로 사전 재적재), 큰 사전(CDict 표 조정)을 포함한다.
TEST(CompressionDictionary_MatchesStandardLevelSemantics) {
    SystemAllocator allocator;
    {
        Array<uint8> dictionary(allocator);
        Array<uint8> original(allocator);
        Array<uint8> packed(allocator);
        Array<uint8> restored(allocator);
        for (const DictionaryGolden& golden : DictionaryGoldens) {
            DictionaryBytes(dictionary, golden.dictionary);
            MakeRecords(original, golden.inputBytes, 9);
            CompressDictionary encoding = MakeCompress(View(dictionary), golden.level, allocator);
            CompressWith(encoding, View(original), packed, allocator);
            const uint32 crc = Crc32(View(packed));
            if (packed.Size() != golden.outputBytes || crc != golden.crc) {
                printf("dictionary golden mismatch: dictionary %u level %d input %llu -> %llu "
                       "0x%08x\n",
                       static_cast<unsigned>(golden.dictionary), golden.level,
                       static_cast<unsigned long long>(golden.inputBytes),
                       static_cast<unsigned long long>(packed.Size()), crc);
            }
            CHECK(packed.Size() == golden.outputBytes);
            CHECK(crc == golden.crc);

            // 자유 함수도 같은 byte다.
            Array<uint8> once(allocator);
            REQUIRE(TryCompressAppend(encoding, View(original), once));
            CHECK(Same(once, View(packed)));

            DecompressDictionary decoding = MakeDecompress(View(dictionary), allocator);
            REQUIRE(restored.TryResize(0));
            REQUIRE(TryDecompressAppend(decoding, View(packed), golden.inputBytes, restored));
            CHECK(Same(restored, View(original)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 작은 입력에서 사전의 효과와 문맥 재사용 결과가 같은지를 본다.
TEST(CompressionDictionary_ImprovesSmallInputsAndReusesContexts) {
    SystemAllocator allocator;
    {
        CompressDictionary encoding = MakeCompress(DictionaryA(), 3, allocator);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
        auto compressor = Compressor::TryCreate(encoding, allocator);
        auto plain = Compressor::TryCreate(Settings(CompressionFormat::Zstd, 3), allocator);
        auto decompressor = Decompressor::TryCreate(decoding, allocator);
        REQUIRE(compressor);
        REQUIRE(plain);
        REQUIRE(decompressor);

        usize withDictionary = 0;
        usize without = 0;
        Array<uint8> original(allocator);
        Array<uint8> packed(allocator);
        Array<uint8> fresh(allocator);
        Array<uint8> restored(allocator);
        for (uint32 seed = 100; seed < 140; ++seed) {
            MakeRecords(original, 200 + seed * 3, seed);
            REQUIRE(packed.TryResize(0));
            REQUIRE(compressor.Value().TryCompressAppend(View(original), packed));
            withDictionary += packed.Size();
            Array<uint8> plainPacked(allocator);
            REQUIRE(plain.Value().TryCompressAppend(View(original), plainPacked));
            without += plainPacked.Size();

            // 재사용 문맥 = 새 문맥 = 자유 함수.
            CompressWith(encoding, View(original), fresh, allocator);
            CHECK(Same(fresh, View(packed)));

            // 크기를 아는 해제(Span)와 Append.
            REQUIRE(restored.TryResize(original.Size()));
            auto written = decompressor.Value().TryDecompress(
                View(packed), Span<uint8>(restored.Data(), restored.Size()));
            REQUIRE(written);
            CHECK(written.Value() == original.Size());
            CHECK(Same(restored, View(original)));
        }
        printf("dictionary small inputs: %llu bytes with dictionary, %llu without\n",
               static_cast<unsigned long long>(withDictionary),
               static_cast<unsigned long long>(without));
        CHECK(withDictionary * 10 < without * 7);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// frame의 사전 ID 검사와 TryGetZstdFrameDictionaryId.
TEST(CompressionDictionary_FrameIdentityChecks) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 3000, 21);
        CompressDictionary encodingA = MakeCompress(DictionaryA(), 3, allocator);
        DecompressDictionary decodingA = MakeDecompress(DictionaryA(), allocator);
        Array<uint8> otherBytes(allocator);
        DictionaryWithId(otherBytes, 0x2B2B2B2Bu);
        DecompressDictionary decodingB = MakeDecompress(View(otherBytes), allocator);

        Array<uint8> withA(allocator);
        CompressWith(encodingA, View(original), withA, allocator);
        Array<uint8> plain(allocator);
        REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 3), View(original), plain));

        // frame의 사전 ID 조회.
        CHECK(TryGetZstdFrameDictionaryId(View(withA)).Value() == DictionaryIdA);
        CHECK(TryGetZstdFrameDictionaryId(View(plain)).Value() == 0);
        CHECK(TryGetZstdFrameDictionaryId(Slice(View(withA), 0, 3)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TryGetZstdFrameDictionaryId(Text("not a zstd frame")).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TryGetZstdFrameDictionaryId(Span<const uint8>()).Error() ==
              ErrorCode::InvalidArgument);
        const uint8 skippable[] = {0x50, 0x2A, 0x4D, 0x18, 2, 0, 0, 0, 7, 7};
        CHECK(TryGetZstdFrameDictionaryId(Bytes(skippable)).Value() == 0);

        Array<uint8> restored(allocator);
        auto decodeError = [&](const DecompressDictionary* dictionary, const Array<uint8>& packed) {
            CHECK(restored.TryResize(0));
            auto result =
                dictionary != nullptr
                    ? TryDecompressAppend(*dictionary, View(packed), 1 << 20, restored)
                    : TryDecompressAppend(CompressionFormat::Zstd, View(packed), 1 << 20, restored);
            return result ? ErrorCode::None : result.Error();
        };
        CHECK(decodeError(&decodingA, withA) == ErrorCode::None);
        CHECK(Same(restored, View(original)));
        // 사전이 없거나 ID가 다른 문맥: Unsupported.
        CHECK(decodeError(nullptr, withA) == ErrorCode::Unsupported);
        CHECK(decodeError(&decodingB, withA) == ErrorCode::Unsupported);
        // 형식 사전 문맥은 사전 ID 없는 frame을 거부한다(검사 없이 풀면 틀린 결과가 나올 수 있다).
        CHECK(decodeError(&decodingA, plain) == ErrorCode::Unsupported);
        Array<uint8> mixed(allocator);
        REQUIRE(mixed.TryAppend(View(withA)));
        REQUIRE(mixed.TryAppend(View(plain)));
        CHECK(decodeError(&decodingA, mixed) == ErrorCode::Unsupported);
        Array<uint8> twice(allocator);
        REQUIRE(twice.TryAppend(View(withA)));
        REQUIRE(twice.TryAppend(Bytes(skippable)));
        REQUIRE(twice.TryAppend(View(withA)));
        CHECK(decodeError(&decodingA, twice) == ErrorCode::None);
        CHECK(restored.Size() == 2 * original.Size());
        uint8 small[3000];
        CHECK(TryDecompress(decodingA, View(plain), Span<uint8>(small, sizeof(small)), allocator)
                  .Error() == ErrorCode::Unsupported);

        // raw 사전: ID가 없어 frame으로는 구분하지 못한다. 사전 없는 frame은 그대로 풀리고, 다른
        // raw 사전으로 만든 frame은 checksum 불일치(InvalidArgument)로 걸린다.
        Array<uint8> rawOne(allocator);
        Array<uint8> rawTwo(allocator);
        MakeRecords(rawOne, 4096, 3);
        MakeRecords(rawTwo, 4096, 5);
        DecompressDictionary decodingRaw = MakeDecompress(View(rawOne), allocator);
        CHECK(decodeError(&decodingRaw, plain) == ErrorCode::None);
        CHECK(Same(restored, View(original)));
        Array<uint8> withRawTwo(allocator);
        CompressWith(MakeCompress(View(rawTwo), 3, allocator), View(original), withRawTwo,
                     allocator);
        CHECK(TryGetZstdFrameDictionaryId(View(withRawTwo)).Value() == 0);
        CHECK(decodeError(&decodingRaw, withRawTwo) == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CompressionDictionary_StreamsRoundTripAndCheckIds) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        MakeRecords(original, 200000, 31);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 5, allocator);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);

        auto stream = CompressStream::TryCreate(encoding, allocator, original.Size());
        REQUIRE(stream);
        Array<uint8> packed(allocator);
        for (usize offset = 0; offset < original.Size(); offset += 7000) {
            const usize piece =
                original.Size() - offset < 7000 ? original.Size() - offset : usize{7000};
            REQUIRE(stream.Value().TryWriteAppend(Slice(View(original), offset, piece), packed));
        }
        REQUIRE(stream.Value().TryFinishAppend(packed));
        CHECK(TryGetZstdFrameDictionaryId(View(packed)).Value() == DictionaryIdA);

        // 일회성 해제와 스트림 해제가 모두 원본을 돌려준다.
        Array<uint8> restored(allocator);
        REQUIRE(TryDecompressAppend(decoding, View(packed), original.Size(), restored));
        CHECK(Same(restored, View(original)));
        auto decoder =
            DecompressStream::TryCreate(decoding, {.maxOutputBytes = original.Size()}, allocator);
        REQUIRE(decoder);
        Array<uint8> streamed(allocator);
        for (usize offset = 0; offset < packed.Size(); offset += 999) {
            const usize piece = packed.Size() - offset < 999 ? packed.Size() - offset : usize{999};
            REQUIRE(decoder.Value().TryWriteAppend(Slice(View(packed), offset, piece), streamed));
        }
        CHECK(decoder.Value().TryFinish());
        CHECK(Same(streamed, View(original)));

        // Reset 뒤에도 같은 사전이다(같은 조각이면 같은 byte).
        stream.Value().Reset();
        Array<uint8> again(allocator);
        for (usize offset = 0; offset < original.Size(); offset += 7000) {
            const usize piece =
                original.Size() - offset < 7000 ? original.Size() - offset : usize{7000};
            REQUIRE(stream.Value().TryWriteAppend(Slice(View(original), offset, piece), again));
        }
        REQUIRE(stream.Value().TryFinishAppend(again));
        CHECK(Same(again, View(packed)));

        // 스트림 해제도 사전 ID 없는 frame·다른 사전을 거부한다.
        Array<uint8> plain(allocator);
        REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 3),
                                  Slice(View(original), 0, 5000), plain));
        decoder.Value().Reset();
        Array<uint8> sink(allocator);
        CHECK(decoder.Value().TryWriteAppend(View(plain), sink).Error() == ErrorCode::Unsupported);
        auto noDictionary = DecompressStream::TryCreate(
            CompressionFormat::Zstd, {.maxOutputBytes = original.Size()}, allocator);
        REQUIRE(noDictionary);
        CHECK(noDictionary.Value().TryWriteAppend(View(packed), sink).Error() ==
              ErrorCode::Unsupported);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 문맥·스트림·블록 설정·reader가 사전 handle을 가져 수명을 보장한다. 사전 할당기와 문맥 할당기를
// 따로 센다.
TEST(CompressionDictionary_HandlesKeepDictionaryAlive) {
    SystemAllocator system;
    CountingAllocator dictionaryMemory(system);
    {
        Array<uint8> original(system);
        MakeRecords(original, 50000, 41);
        Array<uint8> expected(system);
        Array<uint8> container(system);
        auto compressor = Compressor::TryCreate(MakeCompress(DictionaryA(), 3, dictionaryMemory),
                                                system); // 임시 handle만 있다
        REQUIRE(compressor);
        auto stream =
            CompressStream::TryCreate(MakeCompress(DictionaryA(), 3, dictionaryMemory), system);
        REQUIRE(stream);
        auto settings =
            CompressedBlocksSettings::TryMake(MakeCompress(DictionaryA(), 3, dictionaryMemory), 12);
        REQUIRE(settings);
        CHECK(dictionaryMemory.Current() > 0);

        REQUIRE(compressor.Value().TryCompressAppend(View(original), expected));
        Array<uint8> streamed(system);
        REQUIRE(stream.Value().TryWriteAppend(View(original), streamed));
        REQUIRE(stream.Value().TryFinishAppend(streamed));
        REQUIRE(TryCompressBlocksAppend(settings.Value(), View(original), container, system));

        auto decoder =
            Decompressor::TryCreate(MakeDecompress(DictionaryA(), dictionaryMemory), system);
        REQUIRE(decoder);
        Array<uint8> restored(system);
        REQUIRE(decoder.Value().TryDecompressAppend(View(expected), original.Size(), restored));
        CHECK(Same(restored, View(original)));

        CompressedBlocksReader reader;
        {
            const DecompressDictionary candidates[] = {
                MakeDecompress(DictionaryA(), dictionaryMemory)};
            auto opened = CompressedBlocksReader::TryOpen(View(container), candidates, system);
            REQUIRE(opened);
            reader = Move(opened.Value());
        }
        auto context = CompressedBlocksReadContext::TryCreate(reader, system);
        REQUIRE(context);
        REQUIRE(restored.TryResize(original.Size()));
        REQUIRE(reader.TryRead(0, Span<uint8>(restored.Data(), restored.Size()), View(container),
                               context.Value()));
        CHECK(Same(restored, View(original)));
    }
    CHECK(dictionaryMemory.Current() == 0);
    CHECK(system.Stats().liveAllocations == 0);
}

// 한 사전을 여러 worker가 동시에 쓴다(비동기 제출). 결과는 동기와 같은 byte이고, 제출 뒤 호출자
// handle을 놓아도 작업이 사전을 유지한다.
TEST(CompressionDictionary_AsyncSharingMatchesSync) {
    SystemAllocator allocator;
    CountingAllocator dictionaryMemory(allocator);
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 128}));
        {
            constexpr uint32 Jobs = 24;
            struct Slot {
                explicit Slot(Allocator& allocator) noexcept
                    : original(allocator), expected(allocator), packed(allocator),
                      restored(allocator) {
                }

                Array<uint8> original;
                Array<uint8> expected;
                Array<uint8> packed;
                Array<uint8> restored;
            };
            Array<Slot> slots(allocator);
            REQUIRE(slots.TryReserve(Jobs));
            CompressionTask compressTasks[Jobs];
            CompressionTask decompressTasks[Jobs];
            {
                CompressDictionary encoding = MakeCompress(DictionaryA(), 9, dictionaryMemory);
                DecompressDictionary decoding = MakeDecompress(DictionaryA(), dictionaryMemory);
                for (uint32 job = 0; job < Jobs; ++job) {
                    REQUIRE(slots.TryPushBack(Slot(allocator)));
                    Slot& slot = slots[job];
                    MakeRecords(slot.original, 300 + job * 4111, job + 50);
                    CompressWith(encoding, View(slot.original), slot.expected, allocator);
                }
                for (uint32 job = 0; job < Jobs; ++job) {
                    Slot& slot = slots[job];
                    REQUIRE(TrySubmitCompress(scheduler, compressTasks[job], encoding,
                                              View(slot.original), slot.packed));
                    // 해제 입력은 동기로 만든 결과다(제출 시점에 길이가 고정되어야 한다).
                    REQUIRE(TrySubmitDecompress(scheduler, decompressTasks[job], decoding,
                                                View(slot.expected), slot.original.Size(),
                                                slot.restored));
                }
                CompressDictionary empty;
                DecompressDictionary emptyDecoding;
                CHECK(TrySubmitCompress(scheduler, compressTasks[0], empty, View(slots[0].original),
                                        slots[0].packed)
                          .Error() == ErrorCode::StaleHandle);
                CHECK(TrySubmitDecompress(scheduler, decompressTasks[0], emptyDecoding,
                                          View(slots[0].expected), 1, slots[0].restored)
                          .Error() == ErrorCode::StaleHandle);
            }
            // 호출자 handle은 모두 놓았다. 작업이 가진 복사본으로 끝난다.
            for (uint32 job = 0; job < Jobs; ++job) {
                CHECK(compressTasks[job].TryWaitResult());
                CHECK(decompressTasks[job].TryWaitResult());
                CHECK(Same(slots[job].packed, View(slots[job].expected)));
                CHECK(Same(slots[job].restored, View(slots[job].original)));
            }
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(dictionaryMemory.Current() == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 생성 경로마다 k번째 할당을 실패시킨다. 결과는 항상 OutOfMemory이며 손상(InvalidArgument)으로
// 잘못 분류되지 않고 새지 않는다.
TEST(CompressionDictionary_OutOfMemoryIsClassified) {
    SystemAllocator system;
    {
        NFailAllocator failing(system);
        Array<uint8> raw(system);
        MakeRecords(raw, 4096, 3);
        const Span<const uint8> inputs[] = {DictionaryA(), View(raw)};
        for (const Span<const uint8>& bytes : inputs) {
            failing.DisableFailure();
            REQUIRE(CompressDictionary::TryCreate(Settings(CompressionFormat::Zstd, 19), bytes,
                                                  failing));
            const uint64 compressAttempts = failing.Attempts();
            for (uint64 attempt = 1; attempt <= compressAttempts; ++attempt) {
                failing.FailOnAttempt(attempt);
                auto created = CompressDictionary::TryCreate(Settings(CompressionFormat::Zstd, 19),
                                                             bytes, failing);
                CHECK(!created && created.Error() == ErrorCode::OutOfMemory);
            }

            failing.DisableFailure();
            REQUIRE(DecompressDictionary::TryCreate(CompressionFormat::Zstd, bytes, failing));
            const uint64 decompressAttempts = failing.Attempts();
            for (uint64 attempt = 1; attempt <= decompressAttempts; ++attempt) {
                failing.FailOnAttempt(attempt);
                auto created =
                    DecompressDictionary::TryCreate(CompressionFormat::Zstd, bytes, failing);
                CHECK(!created && created.Error() == ErrorCode::OutOfMemory);
            }
        }

        // 사전 문맥 생성도 OutOfMemory로 실패하고 사전은 그대로 쓸 수 있다.
        CompressDictionary encoding = MakeCompress(DictionaryA(), 3, system);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), system);
        Array<uint8> original(system);
        MakeRecords(original, 20000, 61);
        Array<uint8> expected(system);
        CompressWith(encoding, View(original), expected, system);
        for (uint64 attempt = 1; attempt <= 64; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto compressor = Compressor::TryCreate(encoding, failing);
            if (compressor) {
                Array<uint8> packed(system);
                auto written = compressor.Value().TryCompressAppend(View(original), packed);
                CHECK(written || written.Error() == ErrorCode::OutOfMemory);
                if (written) {
                    CHECK(Same(packed, View(expected)));
                }
            } else {
                CHECK(compressor.Error() == ErrorCode::OutOfMemory);
            }
            auto decompressor = Decompressor::TryCreate(decoding, failing);
            CHECK(decompressor || decompressor.Error() == ErrorCode::OutOfMemory);
            auto stream = CompressStream::TryCreate(encoding, failing);
            CHECK(stream || stream.Error() == ErrorCode::OutOfMemory);
            auto decoder = DecompressStream::TryCreate(decoding, {.maxOutputBytes = 100}, failing);
            CHECK(decoder || decoder.Error() == ErrorCode::OutOfMemory);
        }
        failing.DisableFailure();
    }
    CHECK(system.Stats().liveAllocations == 0);
}

// 블록 컨테이너: 헤더 사전 bit·지문, 후보 선택, 읽기 문맥을 사전 있는·없는 reader에 번갈아 쓰기,
// 병렬 = 순차, 작은 블록의 압축률.
TEST(CompressionDictionary_BlockContainers) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 128}));
        {
            Array<uint8> original(allocator);
            MakeRecords(original, 600000, 71);
            CompressDictionary encoding = MakeCompress(DictionaryA(), 3, allocator);
            DecompressDictionary decoding = MakeDecompress(DictionaryA(), allocator);
            Array<uint8> otherBytes(allocator);
            DictionaryWithId(otherBytes, 0x2B2B2B2Bu);
            DecompressDictionary other = MakeDecompress(View(otherBytes), allocator);

            auto settings = CompressedBlocksSettings::TryMake(encoding, 12);
            REQUIRE(settings);
            CHECK(settings.Value().UsesDictionary());
            CHECK(settings.Value().Compression().Level() == 3);
            Array<uint8> container(allocator);
            REQUIRE(
                TryCompressBlocksAppend(settings.Value(), View(original), container, allocator));
            Array<uint8> parallel(allocator);
            REQUIRE(TryCompressBlocksAppend(scheduler, settings.Value(), View(original), parallel,
                                            allocator));
            CHECK(Same(parallel, View(container)));
            const uint32 crc = Crc32(View(container));
            printf("dictionary blocks: %llu bytes crc 0x%08x\n",
                   static_cast<unsigned long long>(container.Size()), crc);
            // 기준: Windows x64 clang 21.1.6, zstd 1.5.7, 사전 A·수준 3·4KiB 블록.
            CHECK(container.Size() == 126242u);
            CHECK(crc == 0x30ac3eb8u);

            // 헤더: flags bit0, 지문 = 사전 SHA-256 앞 8byte.
            CHECK(LoadLittleEndian<uint32>(container.Data() + 20) == 1);
            CHECK(
                Same(Slice(View(container), 32, 8), Span<const uint8>(encoding.Digest().bytes, 8)));

            // 사전 없는 같은 설정보다 작다.
            Array<uint8> plain(allocator);
            REQUIRE(TryCompressBlocksAppend(
                CompressedBlocksSettings::TryMake(Settings(CompressionFormat::Zstd, 3), 12).Value(),
                View(original), plain, allocator));
            printf("dictionary blocks: %llu bytes with dictionary, %llu without (4KiB blocks)\n",
                   static_cast<unsigned long long>(container.Size()),
                   static_cast<unsigned long long>(plain.Size()));
            CHECK(container.Size() * 10 < plain.Size() * 9);

            // 후보: 없으면·다른 사전뿐이면 Unsupported, 맞는 후보를 고른다.
            CHECK(CompressedBlocksReader::TryOpen(View(container), allocator).Error() ==
                  ErrorCode::Unsupported);
            const DecompressDictionary wrong[] = {other};
            CHECK(CompressedBlocksReader::TryOpen(View(container), wrong, allocator).Error() ==
                  ErrorCode::Unsupported);
            const DecompressDictionary candidates[] = {other, decoding};
            auto reader = CompressedBlocksReader::TryOpen(View(container), candidates, allocator);
            REQUIRE(reader);
            CHECK(reader.Value().UsesDictionary());
            CHECK(reader.Value().DictionaryFingerprint() ==
                  LoadLittleEndian<uint64>(decoding.Digest().bytes));

            // 사전 없는 컨테이너는 후보를 무시한다.
            auto plainReader = CompressedBlocksReader::TryOpen(View(plain), candidates, allocator);
            REQUIRE(plainReader);
            CHECK(!plainReader.Value().UsesDictionary());
            CHECK(plainReader.Value().DictionaryFingerprint() == 0);

            // 한 읽기 문맥을 두 reader에 번갈아 쓴다(문맥은 사전과 무관하다).
            auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
            REQUIRE(context);
            Array<uint8> out(allocator);
            REQUIRE(out.TryResize(10000));
            const uint64 Offsets[] = {0, 4000, 123456, 599000 - 10000, 300001};
            for (const uint64 offset : Offsets) {
                const usize bytes =
                    original.Size() - offset < out.Size() ? original.Size() - offset : out.Size();
                const Span<uint8> target(out.Data(), bytes);
                REQUIRE(reader.Value().TryRead(offset, target, View(container), context.Value()));
                CHECK(Same(target, Slice(View(original), offset, bytes)));
                REQUIRE(plainReader.Value().TryRead(offset, target, View(plain), context.Value()));
                CHECK(Same(target, Slice(View(original), offset, bytes)));
            }

            // 병렬 읽기.
            Array<uint8> whole(allocator);
            REQUIRE(whole.TryResize(original.Size()));
            REQUIRE(reader.Value().TryRead(scheduler, 0, Span<uint8>(whole.Data(), whole.Size()),
                                           View(container), 0, allocator));
            CHECK(Same(whole, View(original)));

            // 지문 byte 손상은 CRC(InvalidArgument), CRC를 다시 맞추면 맞는 사전이
            // 없다(Unsupported).
            Array<uint8> corrupt(allocator);
            REQUIRE(corrupt.TryAppend(View(container)));
            corrupt[35] ^= 0x01;
            CHECK(CompressedBlocksReader::TryOpen(View(corrupt), candidates, allocator).Error() ==
                  ErrorCode::InvalidArgument);
            const uint32 count = LoadLittleEndian<uint32>(corrupt.Data() + 16);
            uint32 resealed = Crc32(Slice(View(corrupt), 0, 44));
            resealed = Crc32(Slice(View(corrupt), CompressedBlocksHeaderBytes, usize{8} * count),
                             resealed);
            StoreLittleEndian<uint32>(corrupt.Data() + 44, resealed);
            CHECK(CompressedBlocksReader::TryOpen(View(corrupt), candidates, allocator).Error() ==
                  ErrorCode::Unsupported);

            // 사전 블록 frame은 사전 ID를 기록한다.
            const uint32 firstStored = LoadLittleEndian<uint32>(container.Data() + 48);
            const usize payload = CompressedBlocksHeaderBytes + usize{8} * count;
            if (firstStored < 4096) {
                CHECK(TryGetZstdFrameDictionaryId(Slice(View(container), payload, firstStored))
                          .Value() == DictionaryIdA);
            }
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
