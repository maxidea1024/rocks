#include <Core/Source/Runtime/Compression/Tests/CompressionDictionaryTestSupport.h>

#include <Compression/Compression.h>
#include <Compression/CompressionDictionary.h>
#include <CompressionTraining/CompressionDictionaryTraining.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Function.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

// zstd 사전 학습(Docs/Design/Compression.ko.md 11.6절) 시험이다.
namespace rocks::test {

using namespace dictionary_test;

namespace {

// 표본 n개를 이어 붙인다. 표본 i는 MakeRecords(100 + (i * 37) % 300, seed + i)다(길이·내용이 다른
// 작은 레코드).
void MakeSamples(Array<uint8>& samples, Array<usize>& sizes, usize count, uint32 seed) {
    REQUIRE(samples.TryResize(0));
    REQUIRE(sizes.TryResize(0));
    Array<uint8> one(samples.GetAllocator());
    for (usize index = 0; index < count; ++index) {
        MakeRecords(one, 100 + (index * 37) % 300, seed + static_cast<uint32>(index));
        REQUIRE(samples.TryAppend(View(one)));
        REQUIRE(sizes.TryPushBack(one.Size()));
    }
}

Span<const usize> SizesView(const Array<usize>& sizes) {
    return Span<const usize>(sizes.Data(), sizes.Size());
}

DictionaryTrainingSettings FixedSettings() {
    DictionaryTrainingSettings settings;
    settings.capacity = usize{8} << 10;
    settings.compressionLevel = 3;
    settings.dictionaryId = 40001;
    settings.segmentSize = 200;
    settings.dmerSize = 8;
    return settings;
}

ErrorCode TrainError(Span<const uint8> samples, Span<const usize> sizes,
                     const DictionaryTrainingSettings& settings, Array<uint8>& output) {
    auto trained = TryTrainCompressionDictionary(samples, sizes, settings, output);
    return trained ? ErrorCode::None : trained.Error();
}

ErrorCode FinalizeError(Span<const uint8> content, Span<const uint8> samples,
                        Span<const usize> sizes, const DictionaryFinalizeSettings& settings,
                        Array<uint8>& output) {
    auto finalized = TryFinalizeCompressionDictionary(content, samples, sizes, settings, output);
    return finalized ? ErrorCode::None : finalized.Error();
}

} // namespace

// finalize는 사전 사용 시험 자료(FormattedDictionaryA, probe가 ZDICT_finalizeDictionary로 만든
// 사전)를 byte 단위로 재현한다(같은 내용·표본·수준·ID).
TEST(CompressionDictionaryTraining_FinalizeReproducesTestDictionary) {
    SystemAllocator allocator;
    {
        Array<uint8> content(allocator);
        MakeRecords(content, 2048, 1);
        Array<uint8> samples(allocator);
        MakeRecords(samples, 65536, 7);
        Array<usize> sizes(allocator);
        for (usize index = 0; index < 256; ++index) {
            REQUIRE(sizes.TryPushBack(256));
        }

        Array<uint8> output(allocator);
        REQUIRE(output.TryAppend(Text("prefix")));
        DictionaryFinalizeSettings settings;
        settings.compressionLevel = 3;
        settings.dictionaryId = DictionaryIdA;
        auto finalized = TryFinalizeCompressionDictionary(View(content), View(samples),
                                                          SizesView(sizes), settings, output);
        REQUIRE(finalized);
        CHECK(finalized.Value() == sizeof(FormattedDictionaryA));
        CHECK(Same(Slice(View(output), 0, 6), Text("prefix")));
        CHECK(Same(Slice(View(output), 6, output.Size() - 6), DictionaryA()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CompressionDictionaryTraining_FinalizeContracts) {
    SystemAllocator allocator;
    {
        Array<uint8> content(allocator);
        MakeRecords(content, 3000, 11);
        Array<uint8> samples(allocator);
        Array<usize> sizes(allocator);
        MakeSamples(samples, sizes, 50, 300);
        Array<uint8> output(allocator);

        // ID 0은 내용에서 유도한다(0이 아니고 직접 줄 수 있는 범위 안).
        DictionaryFinalizeSettings derived;
        REQUIRE(TryFinalizeCompressionDictionary(View(content), View(samples), SizesView(sizes),
                                                 derived, output));
        DecompressDictionary opened = MakeDecompress(View(output), allocator);
        CHECK(opened.Id() >= MinExplicitDictionaryId && opened.Id() <= MaxExplicitDictionaryId);

        // 내용은 자르지 않는다: 결과 = 헤더 + 내용이고 내용이 끝에 그대로 있다.
        CHECK(output.Size() > content.Size());
        CHECK(Same(Slice(View(output), output.Size() - content.Size(), content.Size()),
                   View(content)));

        // 빈 내용·표본 없음도 된다(내용은 8byte로 채운다).
        Array<uint8> empty(allocator);
        CHECK(FinalizeError(Span<const uint8>(), Span<const uint8>(), Span<const usize>(), derived,
                            empty) == ErrorCode::None);
        CHECK(empty.Size() >= 8 + 8);

        // 인자 오류는 output을 바꾸지 않는다.
        const usize before = output.Size();
        DictionaryFinalizeSettings bad = derived;
        const uint32 Reserved[] = {1, MinExplicitDictionaryId - 1, MaxExplicitDictionaryId + 1};
        for (const uint32 id : Reserved) {
            bad.dictionaryId = id;
            CHECK(FinalizeError(View(content), View(samples), SizesView(sizes), bad, output) ==
                  ErrorCode::InvalidArgument);
        }
        bad = derived;
        bad.compressionLevel = 0;
        CHECK(FinalizeError(View(content), View(samples), SizesView(sizes), bad, output) ==
              ErrorCode::InvalidArgument);
        bad.compressionLevel = 20;
        CHECK(FinalizeError(View(content), View(samples), SizesView(sizes), bad, output) ==
              ErrorCode::InvalidArgument);
        CHECK(FinalizeError(View(content), Slice(View(samples), 0, samples.Size() - 1),
                            SizesView(sizes), derived, output) == ErrorCode::InvalidArgument);
        Array<uint8> huge(allocator);
        REQUIRE(huge.TryResize(MaxDictionaryBytes - 255));
        CHECK(FinalizeError(View(huge), View(samples), SizesView(sizes), derived, output) ==
              ErrorCode::InvalidArgument);
        // 내용이 output 저장소 안에 있으면(성장 중 재할당으로 무효) 거부한다.
        CHECK(FinalizeError(Slice(View(output), 0, 100), View(samples), SizesView(sizes), derived,
                            output) == ErrorCode::InvalidArgument);
        CHECK(output.Size() == before);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 표본·설정이면 같은 사전이다. 고정 k·d는 그 값을, 탐색은 후보 중 고른 값을 돌려주며 다시
// 학습하면 같은 byte다.
TEST(CompressionDictionaryTraining_TrainsDeterministically) {
    SystemAllocator allocator;
    {
        Array<uint8> samples(allocator);
        Array<usize> sizes(allocator);
        MakeSamples(samples, sizes, 400, 1000);

        Array<uint8> first(allocator);
        auto trained =
            TryTrainCompressionDictionary(View(samples), SizesView(sizes), FixedSettings(), first);
        REQUIRE(trained);
        CHECK(trained.Value().bytes == first.Size());
        CHECK(trained.Value().segmentSize == 200);
        CHECK(trained.Value().dmerSize == 8);
        CHECK(first.Size() <= FixedSettings().capacity);
        Array<uint8> second(allocator);
        REQUIRE(TryTrainCompressionDictionary(View(samples), SizesView(sizes), FixedSettings(),
                                              second));
        CHECK(Same(second, View(first)));
        const uint32 crc = Crc32(View(first));
        printf("trained dictionary: %llu bytes crc 0x%08x\n",
               static_cast<unsigned long long>(first.Size()), crc);
        // 기준: Windows x64 clang 21.1.6, zstd 1.5.7 fastCover(k 200, d 8, f 20, 수준 3, ID 40001).
        CHECK(first.Size() == 8192u);
        CHECK(crc == 0x5faf596cu);
        CHECK(MakeDecompress(View(first), allocator).Id() == 40001);

        // k 탐색: 고른 값으로 다시 학습하면 같은 사전이다.
        DictionaryTrainingSettings search = FixedSettings();
        search.segmentSize = 0;
        Array<uint8> searched(allocator);
        auto chosen =
            TryTrainCompressionDictionary(View(samples), SizesView(sizes), search, searched);
        REQUIRE(chosen);
        CHECK(chosen.Value().segmentSize >= 50 && chosen.Value().segmentSize <= 2000);
        DictionaryTrainingSettings replay = search;
        replay.segmentSize = chosen.Value().segmentSize;
        Array<uint8> replayed(allocator);
        REQUIRE(TryTrainCompressionDictionary(View(samples), SizesView(sizes), replay, replayed));
        CHECK(Same(replayed, View(searched)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 학습 사전은 학습에 쓰지 않은 같은 종류의 작은 입력에서 사전 없음·같은 크기의 raw 사전보다 작게
// 압축한다.
TEST(CompressionDictionaryTraining_TrainedDictionaryImprovesSmallInputs) {
    SystemAllocator allocator;
    {
        Array<uint8> samples(allocator);
        Array<usize> sizes(allocator);
        MakeSamples(samples, sizes, 400, 1000);
        Array<uint8> trained(allocator);
        REQUIRE(TryTrainCompressionDictionary(View(samples), SizesView(sizes), FixedSettings(),
                                              trained));
        Array<uint8> raw(allocator);
        MakeRecords(raw, trained.Size(), 5000);

        CompressDictionary withTrained = MakeCompress(View(trained), 3, allocator);
        CompressDictionary withRaw = MakeCompress(View(raw), 3, allocator);
        auto plain = Compressor::TryCreate(Settings(CompressionFormat::Zstd, 3), allocator);
        auto trainedCompressor = Compressor::TryCreate(withTrained, allocator);
        auto rawCompressor = Compressor::TryCreate(withRaw, allocator);
        REQUIRE(plain);
        REQUIRE(trainedCompressor);
        REQUIRE(rawCompressor);
        DecompressDictionary decoding = MakeDecompress(View(trained), allocator);

        usize plainBytes = 0;
        usize trainedBytes = 0;
        usize rawBytes = 0;
        Array<uint8> item(allocator);
        Array<uint8> packed(allocator);
        Array<uint8> restored(allocator);
        for (uint32 seed = 7000; seed < 7040; ++seed) {
            MakeRecords(item, 150 + (seed % 7) * 40, seed);
            REQUIRE(packed.TryResize(0));
            REQUIRE(plain.Value().TryCompressAppend(View(item), packed));
            plainBytes += packed.Size();
            REQUIRE(packed.TryResize(0));
            REQUIRE(rawCompressor.Value().TryCompressAppend(View(item), packed));
            rawBytes += packed.Size();
            REQUIRE(packed.TryResize(0));
            REQUIRE(trainedCompressor.Value().TryCompressAppend(View(item), packed));
            trainedBytes += packed.Size();
            REQUIRE(restored.TryResize(0));
            REQUIRE(TryDecompressAppend(decoding, View(packed), item.Size(), restored));
            CHECK(Same(restored, View(item)));
        }
        printf("training: %llu bytes trained, %llu raw, %llu none\n",
               static_cast<unsigned long long>(trainedBytes),
               static_cast<unsigned long long>(rawBytes),
               static_cast<unsigned long long>(plainBytes));
        CHECK(trainedBytes < rawBytes);
        CHECK(trainedBytes * 10 < plainBytes * 7);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 호출 전에 거르는 인자 규칙이다(upstream이 죽거나 원인을 뭉치는 경우 포함). 실패하면 output은
// 그대로다.
TEST(CompressionDictionaryTraining_ArgumentValidation) {
    SystemAllocator allocator;
    {
        Array<uint8> samples(allocator);
        Array<usize> sizes(allocator);
        MakeSamples(samples, sizes, 40, 2000);
        Array<uint8> output(allocator);
        REQUIRE(output.TryAppend(Text("keep")));
        const Span<const uint8> all = View(samples);
        const Span<const usize> allSizes = SizesView(sizes);
        auto with = [](auto change) {
            DictionaryTrainingSettings settings = FixedSettings();
            change(settings);
            return settings;
        };

        // 표본 수 5 미만, 합 8 미만, 합 불일치.
        CHECK(TrainError(all, Span<const usize>(sizes.Data(), 4), FixedSettings(), output) ==
              ErrorCode::InvalidArgument);
        const uint8 tiny[7] = {1, 2, 3, 4, 5, 6, 7};
        const usize tinySizes[] = {1, 1, 1, 2, 2};
        CHECK(TrainError(Span<const uint8>(tiny, 7), Span<const usize>(tinySizes, 5),
                         FixedSettings(), output) == ErrorCode::InvalidArgument);
        CHECK(TrainError(Slice(all, 0, all.Size() - 1), allSizes, FixedSettings(), output) ==
              ErrorCode::InvalidArgument);
        // capacity, 수준, ID, d, k 범위.
        const DictionaryTrainingSettings Bad[] = {
            with([](auto& s) { s.capacity = MinTrainingCapacity - 1; }),
            with([](auto& s) { s.capacity = MaxDictionaryBytes + 1; }),
            with([](auto& s) { s.compressionLevel = 0; }),
            with([](auto& s) { s.compressionLevel = 20; }),
            with([](auto& s) { s.dictionaryId = MinExplicitDictionaryId - 1; }),
            with([](auto& s) { s.dictionaryId = MaxExplicitDictionaryId + 1; }),
            with([](auto& s) { s.dmerSize = 7; }),
            with([](auto& s) { s.dmerSize = 0; }),
            with([](auto& s) { s.segmentSize = 7; }),
            with([](auto& s) {
                s.capacity = 1024;
                s.segmentSize = 1025;
            }),
            with([](auto& s) {
                s.capacity = usize{1} << 20;
                s.segmentSize = 65543;
            }),
        };
        for (const DictionaryTrainingSettings& settings : Bad) {
            CHECK(TrainError(all, allSizes, settings, output) == ErrorCode::InvalidArgument);
        }
        CHECK(Same(output, Text("keep")));
        // 표본이 output 저장소 안에 있으면(성장 중 재할당으로 무효) 거부한다.
        Array<uint8> storage(allocator);
        REQUIRE(storage.TryAppend(Slice(all, 0, 64)));
        const usize eight[] = {8, 8, 8, 8, 8};
        CHECK(TrainError(Slice(View(storage), 0, 40), Span<const usize>(eight, 5), FixedSettings(),
                         storage) == ErrorCode::InvalidArgument);
        CHECK(storage.Size() == 64);

        // k = d, d = 6, 0byte 표본이 섞인 표본은 된다.
        CHECK(TrainError(all, allSizes, with([](auto& s) { s.segmentSize = 8; }), output) ==
              ErrorCode::None);
        CHECK(TrainError(all, allSizes, with([](auto& s) { s.dmerSize = 6; }), output) ==
              ErrorCode::None);
        Array<usize> withEmpty(allocator);
        REQUIRE(withEmpty.TryPushBack(0));
        REQUIRE(withEmpty.TryAppend(allSizes));
        REQUIRE(withEmpty.TryPushBack(0));
        CHECK(TrainError(all, SizesView(withEmpty), FixedSettings(), output) == ErrorCode::None);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경계: 길이 합 overflow·상한(검사가 먼저 실패하므로 byte를 읽지 않는 큰 Span으로 시험한다), 작은
// capacity의 k 탐색 (후보 k = 50만 들어간다), d = 6 결과, 1–7byte 내용 finalize(앞에 0 padding).
TEST(CompressionDictionaryTraining_Boundaries) {
    SystemAllocator allocator;
    {
        Array<uint8> output(allocator);
        static const uint8 Probe[16] = {};
        const usize overflow[] = {~usize{0} - 1, ~usize{0} - 1, 1, 1, 1};
        CHECK(TrainError(Span<const uint8>(Probe, 16), Span<const usize>(overflow, 5),
                         FixedSettings(), output) == ErrorCode::InvalidArgument);
        const usize atLimit[] = {0xFFFFFFFFull - 4, 1, 1, 1, 1};
        CHECK(TrainError(Span<const uint8>(Probe, 0xFFFFFFFFull), Span<const usize>(atLimit, 5),
                         FixedSettings(), output) == ErrorCode::InvalidArgument);
        CHECK(output.Size() == 0);

        Array<uint8> samples(allocator);
        Array<usize> sizes(allocator);
        MakeSamples(samples, sizes, 120, 6000);
        DictionaryTrainingSettings small = FixedSettings();
        small.capacity = 300;
        small.segmentSize = 0;
        auto searched =
            TryTrainCompressionDictionary(View(samples), SizesView(sizes), small, output);
        REQUIRE(searched);
        CHECK(searched.Value().segmentSize == 50);
        CHECK(output.Size() <= 300);
        CHECK(MakeDecompress(View(output), allocator).Id() == 40001);

        DictionaryTrainingSettings six = FixedSettings();
        six.dmerSize = 6;
        Array<uint8> withSix(allocator);
        auto trained = TryTrainCompressionDictionary(View(samples), SizesView(sizes), six, withSix);
        REQUIRE(trained);
        CHECK(trained.Value().dmerSize == 6);

        for (usize length = 1; length <= 7; ++length) {
            Array<uint8> finalized(allocator);
            REQUIRE(TryFinalizeCompressionDictionary(Slice(View(samples), 0, length), View(samples),
                                                     SizesView(sizes), DictionaryFinalizeSettings(),
                                                     finalized));
            // 내용은 8byte로 채워 끝에 둔다(앞은 0).
            CHECK(Same(Slice(View(finalized), finalized.Size() - length, length),
                       Slice(View(samples), 0, length)));
            CHECK(MakeDecompress(View(finalized), allocator).IsValid());
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// output 성장 실패는 OutOfMemory이고 output은 그대로다.
TEST(CompressionDictionaryTraining_OutputOutOfMemory) {
    SystemAllocator system;
    {
        Array<uint8> samples(system);
        Array<usize> sizes(system);
        MakeSamples(samples, sizes, 40, 3000);
        NFailAllocator failing(system);
        Array<uint8> output(failing);
        failing.FailOnAttempt(1);
        auto trained =
            TryTrainCompressionDictionary(View(samples), SizesView(sizes), FixedSettings(), output);
        CHECK(!trained && trained.Error() == ErrorCode::OutOfMemory);
        CHECK(output.Size() == 0);
        failing.FailOnAttempt(1);
        auto finalized = TryFinalizeCompressionDictionary(Slice(View(samples), 0, 1000),
                                                          View(samples), SizesView(sizes),
                                                          DictionaryFinalizeSettings(), output);
        CHECK(!finalized && finalized.Error() == ErrorCode::OutOfMemory);
        CHECK(output.Size() == 0);
        failing.DisableFailure();
    }
    CHECK(system.Stats().liveAllocations == 0);
}

// 여러 worker가 동시에 학습해도(내부 직렬화) 순차 결과와 같다.
TEST(CompressionDictionaryTraining_ConcurrentCallsMatchSequential) {
    SystemAllocator allocator;
    {
        Array<uint8> samples(allocator);
        Array<usize> sizes(allocator);
        MakeSamples(samples, sizes, 200, 4000);
        Array<uint8> expected(allocator);
        REQUIRE(TryTrainCompressionDictionary(View(samples), SizesView(sizes), FixedSettings(),
                                              expected));

        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 16}));
        {
            constexpr usize Jobs = 6;
            struct Slot {
                explicit Slot(Allocator& allocator) noexcept : output(allocator) {
                }

                Array<uint8> output;
                bool ok = false;
            };
            Array<Slot> slots(allocator);
            REQUIRE(slots.TryReserve(Jobs));
            Array<WorkerTaskHandle> handles(allocator);
            for (usize job = 0; job < Jobs; ++job) {
                REQUIRE(slots.TryPushBack(Slot(allocator)));
                Slot* slot = &slots[job];
                const Span<const uint8> input = View(samples);
                const Span<const usize> inputSizes = SizesView(sizes);
                auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                    allocator, [slot, input, inputSizes](WorkerTaskContext&) noexcept {
                        slot->ok = TryTrainCompressionDictionary(input, inputSizes, FixedSettings(),
                                                                 slot->output)
                                       .IsOk();
                    });
                REQUIRE(body);
                auto handle = scheduler.Submit(Move(body.Value()));
                REQUIRE(handle);
                REQUIRE(handles.TryPushBack(Move(handle.Value())));
            }
            for (usize job = 0; job < Jobs; ++job) {
                REQUIRE(handles[job].Wait(platform::InfiniteTimeout));
                CHECK(slots[job].ok);
                CHECK(Same(slots[job].output, View(expected)));
            }
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
