#include "CompressionDictionaryTestSupport.h"

#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/CompressionDictionary.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

// 압축 사전의 수명·공유·동시성·오류 분류 시험이다(Docs/Design/Compression.ko.md 11절). 읽기 문맥이
// 사전과 무관하게 여러 reader에 쓰이는 동안 해제된 사전을 가리키지 않는지, 여러 스레드가 한 사전을
// 함께 쓰는지, 실패 분류와 누수를 본다.
namespace rocks::test {

using namespace dictionary_test;

namespace {

// 같은 크기·정렬 요청에 방금 해제한 주소를 그대로 돌려준다. 사전 상태 주소를 재사용시켜 주소
// 비교로 사전을 식별하는 실수를 드러낸다. 다른 스레드에서 써도 된다.
class RecyclingAllocator final : public Allocator {
public:
    explicit RecyclingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    ~RecyclingAllocator() noexcept override {
        for (uint32 index = 0; index < count_; ++index) {
            (void)inner_.Deallocate(cache_[index].base);
        }
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        platform::LockGuard lock(gate_);
        for (uint32 index = count_; index > 0; --index) {
            Block& block = cache_[index - 1];
            if (block.bytes == bytes && block.alignment == alignment) {
                void* user = block.user;
                block = cache_[--count_];
                return Result<void*>::Success(user);
            }
        }

        // 사용자 주소 앞에 크기·정렬·머리 길이를 기록한다.
        const usize offset = alignment > Header ? alignment : Header;
        auto allocation = inner_.Allocate(bytes + offset, offset, tag);
        if (!allocation) {
            return allocation;
        }

        auto* user = static_cast<uint8*>(allocation.Value()) + offset;
        reinterpret_cast<usize*>(user)[-3] = bytes;
        reinterpret_cast<usize*>(user)[-2] = alignment;
        reinterpret_cast<usize*>(user)[-1] = offset;
        return Result<void*>::Success(user);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }

        auto* user = static_cast<uint8*>(address);
        const usize bytes = reinterpret_cast<usize*>(user)[-3];
        const usize alignment = reinterpret_cast<usize*>(user)[-2];
        const usize offset = reinterpret_cast<usize*>(user)[-1];
        platform::LockGuard lock(gate_);
        if (count_ < Capacity) {
            cache_[count_++] = Block{user, user - offset, bytes, alignment};
            return Result<void>::Success();
        }

        return inner_.Deallocate(user - offset);
    }

private:
    struct Block {
        void* user;
        void* base;
        usize bytes;
        usize alignment;
    };

    static constexpr usize Header = 32;
    static constexpr uint32 Capacity = 256;

    Allocator& inner_;
    platform::Mutex gate_;
    Block cache_[Capacity] = {};
    uint32 count_ = 0;
};

// Zstd 블록 컨테이너를 만든다. dictionary가 nullptr이면 같은 수준(3)의 사전 없는 설정이다.
void MakeContainer(const CompressDictionary* dictionary, Span<const uint8> source,
                   uint32 blockSizeLog2, Array<uint8>& out, Allocator& allocator) {
    REQUIRE(out.TryResize(0));
    auto settings = dictionary != nullptr
                        ? CompressedBlocksSettings::TryMake(*dictionary, blockSizeLog2)
                        : CompressedBlocksSettings::TryMake(Settings(CompressionFormat::Zstd, 3),
                                                            blockSizeLog2);
    REQUIRE(settings);
    REQUIRE(TryCompressBlocksAppend(settings.Value(), source, out, allocator));
}

// [offset, offset + bytes)를 읽어 원본과 비교한다. 스레드에서도 부르므로 REQUIRE 대신 bool이다.
bool ReadMatches(const CompressedBlocksReader& reader, Span<const uint8> container,
                 Span<const uint8> original, CompressedBlocksReadContext& context, uint64 offset,
                 usize bytes, Array<uint8>& scratch) {
    if (!scratch.TryResize(bytes)) {
        return false;
    }

    auto read = reader.TryRead(offset, Span<uint8>(scratch.Data(), bytes), container, context);
    return read && Same(scratch, Slice(original, static_cast<usize>(offset), bytes));
}

// 블록 표 CRC를 다시 계산한다(표·헤더를 고친 뒤 뒤쪽 검사를 시험할 때).
void Reseal(Array<uint8>& container) {
    const uint32 count = LoadLittleEndian<uint32>(container.Data() + 16);
    uint32 crc = Crc32(Slice(View(container), 0, 44));
    crc = Crc32(Slice(View(container), CompressedBlocksHeaderBytes, usize{8} * count), crc);
    StoreLittleEndian<uint32>(container.Data() + 44, crc);
}

} // namespace

// 한 읽기 문맥을 reader·사전이 바뀌는 동안 계속 쓴다. 사전 할당기가 해제한 주소를 그대로 돌려주므로
// 새 사전 상태가 이전 사전의 주소를 받는다. 문맥이 주소 비교로 사전을 건너뛰거나 해제된 DDict를
// 가리키면 결과가 틀린다. 사전 없는 reader와 이동된 reader도 섞는다.
TEST(CompressionDictionary_ContextSurvivesReaderAndDictionaryTurnover) {
    SystemAllocator system;
    {
        RecyclingAllocator pool(system);
        Array<uint8> original(system);
        MakeRecords(original, 150000, 5);
        Array<uint8> bytesB(system);
        DictionaryWithId(bytesB, 0x2B2B2B2Bu);

        Array<uint8> containerA(system);
        Array<uint8> containerB(system);
        Array<uint8> plain(system);
        {
            CompressDictionary encodingA = MakeCompress(DictionaryA(), 3, system);
            CompressDictionary encodingB = MakeCompress(View(bytesB), 3, system);
            MakeContainer(&encodingA, View(original), 12, containerA, system);
            MakeContainer(&encodingB, View(original), 12, containerB, system);
            MakeContainer(nullptr, View(original), 12, plain, system);
        }

        auto context = CompressedBlocksReadContext::TryCreate(CompressionFormat::Zstd, 12, system);
        REQUIRE(context);
        auto plainReader = CompressedBlocksReader::TryOpen(View(plain), system);
        REQUIRE(plainReader);

        constexpr uint32 Rounds = 64;
        const void* previous = nullptr;
        uint32 reused = 0;
        Array<uint8> scratch(system);
        for (uint32 round = 0; round < Rounds; ++round) {
            // A·B를 번갈아 연다. 사전 handle은 reader만 가진다.
            const bool useA = (round & 1) == 0;
            const Span<const uint8> container = useA ? View(containerA) : View(containerB);
            CompressedBlocksReader reader;
            {
                DecompressDictionary decoding =
                    MakeDecompress(useA ? DictionaryA() : View(bytesB), pool);
                REQUIRE(decoding.IsValid());
                reused += &decoding.Digest() == previous ? 1 : 0;
                previous = &decoding.Digest();
                const DecompressDictionary candidates[] = {decoding};
                auto opened = CompressedBlocksReader::TryOpen(container, candidates, pool);
                REQUIRE(opened);
                reader = Move(opened.Value());
            }

            // 블록 일부(문맥 버퍼), 블록 하나 전체, 여러 블록, 사전 없는 reader.
            CompressedBlocksReadContext& ctx = context.Value();
            CHECK(ReadMatches(reader, container, View(original), ctx, 1000 + round * 37, 900,
                              scratch));
            CHECK(ReadMatches(reader, container, View(original), ctx, 4096 * 3, 4096, scratch));
            CHECK(
                ReadMatches(reader, container, View(original), ctx, 4096 * 5 + 11, 20000, scratch));
            CHECK(ReadMatches(plainReader.Value(), View(plain), View(original), ctx,
                              7 + round * 101, 5000, scratch));

            // 이동된 reader는 닫혀 StaleHandle이고, 옮겨 받은 reader가 사전을 가진다.
            CompressedBlocksReader moved(Move(reader));
            CHECK(!reader.IsOpen());
            CHECK(!reader.UsesDictionary());
            CHECK(reader.TryRead(0, Span<uint8>(scratch.Data(), 1), container, ctx).Error() ==
                  ErrorCode::StaleHandle);
            CHECK(moved.UsesDictionary());
            CHECK(ReadMatches(moved, container, View(original), ctx, 4096 * 9 + 5, 300, scratch));
        }

        // 시험이 실제로 주소 재사용을 거쳤다.
        CHECK(reused >= Rounds / 2);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

struct ThreadShared {
    const CompressDictionary* encoding;
    const DecompressDictionary* decoding;
    const CompressedBlocksReader* reader;
    Span<const uint8> container;
    Span<const uint8> original;
    Allocator* allocator;
    uint32 failures;
};

struct ThreadArguments {
    ThreadShared* shared;
    uint32 index;
};

// 스레드 하나의 일이다. 공유 사전으로 문맥을 만들어 압축·해제하고, 공유 reader를 자기 읽기 문맥으로
// 읽는다. 실패 수를 원자적으로 더한다.
void ShareDictionaries(void* context) noexcept {
    auto* arguments = static_cast<ThreadArguments*>(context);
    ThreadShared& shared = *arguments->shared;
    Allocator& allocator = *shared.allocator;
    const uint32 index = arguments->index;
    uint32 failures = 0;
    {
        auto readContext = CompressedBlocksReadContext::TryCreate(*shared.reader, allocator);
        auto compressor = Compressor::TryCreate(*shared.encoding, allocator);
        auto decompressor = Decompressor::TryCreate(*shared.decoding, allocator);
        if (!readContext || !compressor || !decompressor) {
            platform::AtomicFetchAdd(&shared.failures, 1000u);
            return;
        }

        Array<uint8> input(allocator);
        Array<uint8> packed(allocator);
        Array<uint8> again(allocator);
        Array<uint8> restored(allocator);
        Array<uint8> scratch(allocator);
        for (uint32 round = 0; round < 60; ++round) {
            MakeRecords(input, 300 + (index * 977 + round * 131) % 9000, index * 100 + round);
            (void)packed.TryResize(0);
            (void)again.TryResize(0);
            (void)restored.TryResize(0);

            // handle 복사본(일회성 문맥)과 오래 쓰는 문맥이 같은 byte를 만든다.
            const CompressDictionary copy = *shared.encoding;
            if (!TryCompressAppend(copy, View(input), packed) ||
                !compressor.Value().TryCompressAppend(View(input), again) ||
                !Same(again, View(packed))) {
                ++failures;
                continue;
            }

            if (!decompressor.Value().TryDecompressAppend(View(packed), input.Size(), restored) ||
                !Same(restored, View(input))) {
                ++failures;
            }

            const uint64 offset =
                (uint64{index} * 7919 + round * 104729) % (shared.original.Size() - 6000);
            if (!ReadMatches(*shared.reader, shared.container, shared.original, readContext.Value(),
                             offset, 5000, scratch)) {
                ++failures;
            }
        }
    }

    platform::AtomicFetchAdd(&shared.failures, failures);
}

} // namespace

// 여러 스레드가 한 CDict·DDict·reader를 동시에 쓴다. scheduler 병렬 읽기·압축도 순차와 같다. 마지막
// handle이 놓이면 사전 메모리가 모두 반환된다.
TEST(CompressionDictionary_ThreadsShareDictionaries) {
    SystemAllocator system;
    CountingAllocator dictionaryMemory(system);
    {
        Array<uint8> original(system);
        MakeRecords(original, 400000, 9);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 5, dictionaryMemory);
        Array<uint8> container(system);
        MakeContainer(&encoding, View(original), 13, container, system);
        CompressedBlocksReader reader;
        {
            DecompressDictionary decoding = MakeDecompress(DictionaryA(), dictionaryMemory);
            const DecompressDictionary candidates[] = {decoding};
            auto opened = CompressedBlocksReader::TryOpen(View(container), candidates, system);
            REQUIRE(opened);
            reader = Move(opened.Value());

            ThreadShared shared{&encoding,      &decoding, &reader, View(container),
                                View(original), &system,   0};
            constexpr uint32 Count = 8;
            ThreadArguments arguments[Count];
            platform::Thread threads[Count];
            uint32 started = 0;
            for (; started < Count; ++started) {
                arguments[started] = ThreadArguments{&shared, started};
                if (threads[started].Start(&ShareDictionaries, &arguments[started]) !=
                    platform::ThreadStatus::Success) {
                    break;
                }
            }

            for (uint32 index = 0; index < started; ++index) {
                CHECK(threads[index].Join() == platform::ThreadStatus::Success);
            }

            CHECK(started == Count);
            CHECK(platform::AtomicLoad(&shared.failures) == 0);
        }

        WorkerScheduler scheduler(system);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 64}));
        {
            Array<uint8> whole(system);
            REQUIRE(whole.TryResize(original.Size()));
            for (uint32 round = 0; round < 4; ++round) {
                REQUIRE(reader.TryRead(scheduler, 0, Span<uint8>(whole.Data(), whole.Size()),
                                       View(container), 0, system));
                CHECK(Same(whole, View(original)));
            }

            auto settings = CompressedBlocksSettings::TryMake(encoding, 13);
            REQUIRE(settings);
            Array<uint8> parallel(system);
            for (uint32 round = 0; round < 2; ++round) {
                REQUIRE(parallel.TryResize(0));
                REQUIRE(TryCompressBlocksAppend(scheduler, settings.Value(), View(original),
                                                parallel, system));
                CHECK(Same(parallel, View(container)));
            }
        }

        REQUIRE(scheduler.Shutdown());
    }

    CHECK(dictionaryMemory.Current() == 0);
    CHECK(system.Stats().liveAllocations == 0);
}

// 압축 크기가 1MiB를 넘는 블록은 입력 분할 때문에 zstd 스트리밍 경로(입력 버퍼 할당)로 풀린다. 그
// 경로로 사전 블록을 푼 뒤 같은 문맥으로 사전 없는 컨테이너를 읽는다.
TEST(CompressionDictionary_LargeDictionaryBlocksThenPlain) {
    SystemAllocator system;
    {
        Array<uint8> original(system);
        MakeRecords(original, usize{6} << 20, 77);
        // 짝수 byte를 난수로 바꿔 압축 크기를 키운다.
        uint64 state = 0x1234567ull;
        for (usize index = 0; index < original.Size(); index += 2) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            original[index] = static_cast<uint8>(state >> 40);
        }

        CompressDictionary encoding = MakeCompress(DictionaryA(), 1, system);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), system);
        constexpr uint32 BlockLog2 = 22;
        constexpr usize Block = usize{1} << BlockLog2;
        Array<uint8> container(system);
        MakeContainer(&encoding, View(original), BlockLog2, container, system);
        Array<uint8> plain(system);
        MakeContainer(nullptr, View(original), BlockLog2, plain, system);
        const uint32 firstStored =
            LoadLittleEndian<uint32>(container.Data() + CompressedBlocksHeaderBytes);
        CHECK(firstStored > (usize{1} << 20));
        CHECK(firstStored < Block);

        const DecompressDictionary candidates[] = {decoding};
        auto reader = CompressedBlocksReader::TryOpen(View(container), candidates, system);
        auto plainReader = CompressedBlocksReader::TryOpen(View(plain), system);
        REQUIRE(reader);
        REQUIRE(plainReader);
        auto context =
            CompressedBlocksReadContext::TryCreate(CompressionFormat::Zstd, BlockLog2, system);
        REQUIRE(context);
        CompressedBlocksReadContext& ctx = context.Value();
        Array<uint8> scratch(system);
        for (uint32 round = 0; round < 2; ++round) {
            CHECK(ReadMatches(reader.Value(), View(container), View(original), ctx, 0, Block,
                              scratch));
            CHECK(ReadMatches(plainReader.Value(), View(plain), View(original), ctx, 0, Block,
                              scratch));
            CHECK(ReadMatches(reader.Value(), View(container), View(original), ctx, Block + 5, 1000,
                              scratch));
            CHECK(ReadMatches(plainReader.Value(), View(plain), View(original), ctx, Block + 5,
                              1000, scratch));
        }
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 사전 컨테이너를 여는 경로·읽기 문맥·읽기·writer의 k번째 할당을 실패시킨다. 결과는
// OutOfMemory뿐이고 새지 않으며, 실패한 블록은 다시 줄 수 있다. NFailAllocator는 원자적이지 않아
// 단일 스레드 경로만 쓴다.
TEST(CompressionDictionary_DictionaryBlocksOutOfMemory) {
    SystemAllocator system;
    {
        Array<uint8> original(system);
        MakeRecords(original, 70000, 3);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 3, system);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), system);
        Array<uint8> container(system);
        MakeContainer(&encoding, View(original), 12, container, system);
        const DecompressDictionary candidates[] = {decoding};

        NFailAllocator failing(system);
        uint32 failures = 0;
        for (uint64 attempt = 1; attempt <= 40; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto reader = CompressedBlocksReader::TryOpen(View(container), candidates, failing);
            if (!reader) {
                CHECK(reader.Error() == ErrorCode::OutOfMemory);
                ++failures;
                continue;
            }

            auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), failing);
            if (!context) {
                CHECK(context.Error() == ErrorCode::OutOfMemory);
                ++failures;
                continue;
            }

            Array<uint8> out(system);
            REQUIRE(out.TryResize(30000));
            auto read = reader.Value().TryRead(1234, Span<uint8>(out.Data(), out.Size()),
                                               View(container), context.Value());
            if (!read) {
                CHECK(read.Error() == ErrorCode::OutOfMemory);
                ++failures;
                continue;
            }

            CHECK(Same(out, Slice(View(original), 1234, 30000)));
        }

        failing.DisableFailure();
        CHECK(failures > 0);

        // writer: 코덱 문맥 생성이 실패해도 상태가 그대로라 같은 블록을 다시 준다.
        auto settings = CompressedBlocksSettings::TryMake(encoding, 12);
        REQUIRE(settings);
        for (uint64 attempt = 1; attempt <= 12; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto writer = CompressedBlocksWriter::TryBegin(settings.Value(), 8192, failing);
            if (!writer) {
                CHECK(writer.Error() == ErrorCode::OutOfMemory);
                continue;
            }

            Array<uint8> payload(system);
            auto first = writer.Value().TryAppendBlock(Slice(View(original), 0, 4096), payload);
            if (!first) {
                CHECK(first.Error() == ErrorCode::OutOfMemory);
                CHECK(payload.Size() == 0);
                failing.DisableFailure();
                CHECK(writer.Value().TryAppendBlock(Slice(View(original), 0, 4096), payload));
            }
        }

        failing.DisableFailure();
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 사전 불일치의 오류 분류다. raw 사전 문맥은 사전 ID가 있는 frame을, 형식 사전 문맥은 사전 ID 없는
// frame(raw 사전으로 만든 frame 포함)을 Unsupported로 거부한다. 사전 컨테이너의 블록을 사전 ID
// 없는 frame으로 바꾸고 CRC를 다시 맞추면 형식 위반(InvalidArgument)이며, 같은 문맥으로 다음 블록은
// 정상으로 읽힌다.
TEST(CompressionDictionary_ErrorClassification) {
    SystemAllocator system;
    {
        Array<uint8> original(system);
        MakeRecords(original, 20000, 11);
        Array<uint8> raw(system);
        MakeRecords(raw, 4096, 1);
        CompressDictionary encodingA = MakeCompress(DictionaryA(), 3, system);
        DecompressDictionary decodingA = MakeDecompress(DictionaryA(), system);
        CompressDictionary encodingRaw = MakeCompress(View(raw), 3, system);
        DecompressDictionary decodingRaw = MakeDecompress(View(raw), system);

        Array<uint8> withA(system);
        REQUIRE(TryCompressAppend(encodingA, View(original), withA));
        Array<uint8> withRaw(system);
        REQUIRE(TryCompressAppend(encodingRaw, View(original), withRaw));
        Array<uint8> out(system);
        auto decodeError = [&](const DecompressDictionary& dictionary, const Array<uint8>& packed) {
            (void)out.TryResize(0);
            auto result = TryDecompressAppend(dictionary, View(packed), 1 << 20, out);
            return result ? ErrorCode::None : result.Error();
        };

        CHECK(decodeError(decodingRaw, withA) == ErrorCode::Unsupported);
        CHECK(decodeError(decodingA, withRaw) == ErrorCode::Unsupported);
        CHECK(decodeError(decodingRaw, withRaw) == ErrorCode::None);
        CHECK(Same(out, View(original)));

        // 블록 0을 같은 원본의 사전 없는 frame으로 바꾼다(원본보다 작아야 압축 블록이다).
        Array<uint8> container(system);
        MakeContainer(&encodingA, View(original), 12, container, system);
        const uint32 count = LoadLittleEndian<uint32>(container.Data() + 16);
        const usize payload = CompressedBlocksHeaderBytes + usize{8} * count;
        const uint32 oldSize =
            LoadLittleEndian<uint32>(container.Data() + CompressedBlocksHeaderBytes);
        Array<uint8> plainFrame(system);
        REQUIRE(TryCompressAppend(Settings(CompressionFormat::Zstd, 19),
                                  Slice(View(original), 0, 4096), plainFrame));
        REQUIRE(plainFrame.Size() < 4096);
        CHECK(TryGetZstdFrameDictionaryId(View(plainFrame)).Value() == 0);

        Array<uint8> forged(system);
        REQUIRE(forged.TryAppend(Slice(View(container), 0, payload)));
        REQUIRE(forged.TryAppend(View(plainFrame)));
        REQUIRE(forged.TryAppend(
            Slice(View(container), payload + oldSize, container.Size() - payload - oldSize)));
        StoreLittleEndian<uint32>(forged.Data() + CompressedBlocksHeaderBytes,
                                  static_cast<uint32>(plainFrame.Size()));
        StoreLittleEndian<uint32>(forged.Data() + CompressedBlocksHeaderBytes + 4,
                                  Crc32(View(plainFrame)));
        const uint64 payloadSize =
            LoadLittleEndian<uint64>(forged.Data() + 24) - oldSize + plainFrame.Size();
        StoreLittleEndian<uint64>(forged.Data() + 24, payloadSize);
        Reseal(forged);

        const DecompressDictionary candidates[] = {decodingA};
        auto reader = CompressedBlocksReader::TryOpen(View(forged), candidates, system);
        REQUIRE(reader);
        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), system);
        REQUIRE(context);
        Array<uint8> target(system);
        REQUIRE(target.TryResize(4096));
        CHECK(reader.Value()
                  .TryRead(0, Span<uint8>(target.Data(), 4096), View(forged), context.Value())
                  .Error() == ErrorCode::InvalidArgument);
        REQUIRE(reader.Value().TryRead(4096, Span<uint8>(target.Data(), 4096), View(forged),
                                       context.Value()));
        CHECK(Same(target, Slice(View(original), 4096, 4096)));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 파일 흐름: 헤더 → 인덱스만으로 사전 후보와 함께 열고, 필요한 저장 byte만 읽는다.
TEST(CompressionDictionary_IndexOnlyOpenWithDictionary) {
    SystemAllocator system;
    {
        Array<uint8> original(system);
        MakeRecords(original, 50000, 2);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 3, system);
        DecompressDictionary decoding = MakeDecompress(DictionaryA(), system);
        Array<uint8> container(system);
        MakeContainer(&encoding, View(original), 12, container, system);

        auto indexBytes = CompressedBlocksReader::TryGetIndexBytes(
            Slice(View(container), 0, CompressedBlocksHeaderBytes), container.Size());
        REQUIRE(indexBytes);
        const DecompressDictionary candidates[] = {decoding};
        auto reader = CompressedBlocksReader::TryOpen(Slice(View(container), 0, indexBytes.Value()),
                                                      container.Size(), candidates, system);
        REQUIRE(reader);
        CHECK(reader.Value().UsesDictionary());
        auto where = reader.Value().TryLocate(10000, 5000);
        REQUIRE(where);
        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), system);
        REQUIRE(context);

        const usize storedOffset = static_cast<usize>(where.Value().storedOffset);
        const usize storedBytes = static_cast<usize>(where.Value().storedBytes);
        Array<uint8> target(system);
        REQUIRE(target.TryResize(5000));
        REQUIRE(reader.Value().TryRead(10000, Span<uint8>(target.Data(), 5000),
                                       Slice(View(container), storedOffset, storedBytes),
                                       where.Value().storedOffset, context.Value()));
        CHECK(Same(target, Slice(View(original), 10000, 5000)));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 사전을 ASan이 격리하는 system 할당기로 만들고 reader·사전을 버린 뒤 같은 문맥으로 사전 없는
// 컨테이너를 읽는다. 해제 경로가 문맥에서 사전을 떼지 않으면 ASan 빌드에서 heap-use-after-free다.
TEST(CompressionDictionary_ContextAfterDictionaryFreedAsan) {
    SystemAllocator system;
    {
        Array<uint8> original(system);
        MakeRecords(original, 60000, 8);
        Array<uint8> container(system);
        Array<uint8> plain(system);
        {
            CompressDictionary encoding = MakeCompress(DictionaryA(), 3, system);
            MakeContainer(&encoding, View(original), 12, container, system);
            MakeContainer(nullptr, View(original), 12, plain, system);
        }

        auto context = CompressedBlocksReadContext::TryCreate(CompressionFormat::Zstd, 12, system);
        REQUIRE(context);
        auto plainReader = CompressedBlocksReader::TryOpen(View(plain), system);
        REQUIRE(plainReader);
        Array<uint8> scratch(system);
        for (uint32 round = 0; round < 8; ++round) {
            {
                DecompressDictionary decoding = MakeDecompress(DictionaryA(), system);
                const DecompressDictionary candidates[] = {decoding};
                auto reader = CompressedBlocksReader::TryOpen(View(container), candidates, system);
                REQUIRE(reader);
                CHECK(ReadMatches(reader.Value(), View(container), View(original), context.Value(),
                                  100 + round, 9000, scratch));
            }

            CHECK(ReadMatches(plainReader.Value(), View(plain), View(original), context.Value(),
                              200 + round, 9000, scratch));
        }
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 회귀: 이동된 사전 블록 설정도 사전을 유지한다(이동은 복사다). 이동 생성·이동 대입 양쪽 모두 원래
// 설정과 같은 byte의 사전 컨테이너를 만든다.
TEST(CompressionDictionary_MovedSettingsKeepDictionary) {
    SystemAllocator system;
    {
        Array<uint8> original(system);
        MakeRecords(original, 40000, 12);
        CompressDictionary encoding = MakeCompress(DictionaryA(), 3, system);
        auto settings = CompressedBlocksSettings::TryMake(encoding, 12);
        REQUIRE(settings);
        Array<uint8> expected(system);
        REQUIRE(TryCompressBlocksAppend(settings.Value(), View(original), expected, system));
        CHECK(LoadLittleEndian<uint32>(expected.Data() + 20) == 1);

        auto compressWith = [&](const CompressedBlocksSettings& used) {
            Array<uint8> packed(system);
            auto written = TryCompressBlocksAppend(used, View(original), packed, system);
            return written && Same(packed, View(expected));
        };

        CompressedBlocksSettings taken = Move(settings.Value());
        CHECK(settings.Value().UsesDictionary());
        CHECK(taken.UsesDictionary());
        CHECK(compressWith(settings.Value()));
        CHECK(compressWith(taken));

        auto assigned = CompressedBlocksSettings::TryMake(Settings(CompressionFormat::Zstd, 3), 12);
        REQUIRE(assigned);
        CHECK(!assigned.Value().UsesDictionary());
        assigned.Value() = Move(taken);
        CHECK(taken.UsesDictionary());
        CHECK(assigned.Value().UsesDictionary());
        CHECK(compressWith(taken));
        CHECK(compressWith(assigned.Value()));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 회귀: 구현 상한(2^31)을 넘는 window를 선언한 frame 헤더는 손상이 아니라 Unsupported다(RFC 8878은
// 허용한다). 헤더가 덜 오면 InvalidArgument다.
TEST(CompressionDictionary_FrameIdOversizedWindow) {
    // magic, FHD 0(크기·사전 ID 없음, single segment 아님), window descriptor 0xB0 = windowLog 32.
    static constexpr uint8 Oversized[] = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0xB0};
    CHECK(TryGetZstdFrameDictionaryId(Bytes(Oversized)).Error() == ErrorCode::Unsupported);
    CHECK(TryGetZstdFrameDictionaryId(Slice(Bytes(Oversized), 0, 3)).Error() ==
          ErrorCode::InvalidArgument);

    // 상한 안의 가장 큰 window(0xA8 = windowLog 31)는 사전 ID 0이다.
    static constexpr uint8 Widest[] = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0xA8};
    CHECK(TryGetZstdFrameDictionaryId(Bytes(Widest)).Value() == 0);
}

} // namespace rocks::test
