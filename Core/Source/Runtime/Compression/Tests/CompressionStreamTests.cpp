#include "CompressionTestSupport.h"

#include <Compression/CompressionStream.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <stdio.h>

namespace rocks::test {
namespace {

using namespace compression;

constexpr CompressionFormat StreamFormats[] = {CompressionFormat::Zstd, CompressionFormat::Deflate,
                                               CompressionFormat::Zlib, CompressionFormat::Gzip};

usize Smaller(usize first, usize second) {
    return first < second ? first : second;
}

Span<const uint8> Slice(Span<const uint8> data, usize offset, usize size) {
    return Span<const uint8>(data.Data() + offset, size);
}

Span<const uint8> Text(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

template <usize Count>
Span<const uint8> Bytes(const uint8 (&values)[Count]) {
    return Span<const uint8>(values, Count);
}

DecompressStreamLimits Limits(usize maxOutputBytes, uint32 maxWindowLog = 27) {
    DecompressStreamLimits limits;
    limits.maxOutputBytes = maxOutputBytes;
    limits.maxWindowLog = maxWindowLog;
    return limits;
}

// input을 chunk 단위로 붙여 쓰고 스트림을 끝낸다.
bool CompressInChunks(CompressStream& stream, Span<const uint8> input, usize chunk,
                      Array<uint8>& output) {
    for (usize offset = 0; offset < input.Size(); offset += chunk) {
        if (!stream.TryWriteAppend(Slice(input, offset, Smaller(chunk, input.Size() - offset)),
                                   output)) {
            return false;
        }
    }
    return stream.TryFinishAppend(output).IsOk();
}

bool DecompressInChunks(DecompressStream& stream, Span<const uint8> input, usize chunk,
                        Array<uint8>& output) {
    for (usize offset = 0; offset < input.Size(); offset += chunk) {
        if (!stream.TryWriteAppend(Slice(input, offset, Smaller(chunk, input.Size() - offset)),
                                   output)) {
            return false;
        }
    }
    return true;
}

// TryProcess·TryFinish만으로 압축한다. 입력은 inputChunk, 출력 창은 window byte씩이다.
bool CompressWithBackpressure(CompressStream& stream, Span<const uint8> input, usize inputChunk,
                              usize window, Array<uint8>& output) {
    Array<uint8> scratch(output.GetAllocator());
    if (!scratch.TryResize(window)) {
        return false;
    }

    const Span<uint8> target(scratch.Data(), scratch.Size());
    usize offset = 0;
    while (offset < input.Size()) {
        auto step = stream.TryProcess(
            Slice(input, offset, Smaller(inputChunk, input.Size() - offset)), target);
        if (!step || step.Value().produced > window ||
            !output.TryAppend(Span<const uint8>(scratch.Data(), step.Value().produced))) {
            return false;
        }
        offset += step.Value().consumed;
        if (step.Value().consumed == 0 && step.Value().produced == 0) {
            return false;
        }
    }
    while (true) {
        auto step = stream.TryFinish(target);
        if (!step || !output.TryAppend(Span<const uint8>(scratch.Data(), step.Value().produced))) {
            return false;
        }
        if (step.Value().complete) {
            return true;
        }
        if (step.Value().produced == 0) {
            return false;
        }
    }
}

// TryProcess만으로 해제한다. 한 호출의 출력은 window를 넘지 않아야 한다(역압).
bool DecompressWithBackpressure(DecompressStream& stream, Span<const uint8> input, usize inputChunk,
                                usize window, Array<uint8>& output) {
    Array<uint8> scratch(output.GetAllocator());
    if (!scratch.TryResize(window)) {
        return false;
    }

    const Span<uint8> target(scratch.Data(), scratch.Size());
    usize offset = 0;
    while (true) {
        auto step = stream.TryProcess(
            Slice(input, offset, Smaller(inputChunk, input.Size() - offset)), target);
        if (!step || step.Value().produced > window ||
            !output.TryAppend(Span<const uint8>(scratch.Data(), step.Value().produced))) {
            return false;
        }
        offset += step.Value().consumed;
        if (offset == input.Size() && step.Value().complete) {
            return true;
        }
        if (step.Value().consumed == 0 && step.Value().produced == 0) {
            return false;
        }
    }
}

// 일회성 해제기와 스트림 해제기 모두로 확인한다.
bool StreamDecodesTo(CompressionFormat format, Span<const uint8> encoded,
                     Span<const uint8> expected) {
    SystemAllocator allocator;
    bool ok = true;
    {
        Array<uint8> once(allocator);
        auto decoded = TryDecompressAppend(format, encoded, expected.Size() + 1, once);
        ok = ok && decoded && Same(once, expected);

        auto stream = DecompressStream::TryCreate(format, Limits(expected.Size() + 1), allocator);
        Array<uint8> streamed(allocator);
        ok = ok && stream && stream.Value().TryWriteAppend(encoded, streamed) &&
             stream.Value().TryFinish() && stream.Value().IsAtStreamBoundary() &&
             Same(streamed, expected);
    }
    return ok && allocator.Stats().liveAllocations == 0;
}

} // namespace

TEST(CompressionStream_RoundTripAcrossChunkings) {
    SystemAllocator allocator;
    {
        // 분할 단위(1MiB)를 넘는 크기를 포함한다.
        static constexpr usize Sizes[] = {0, 1, 1000, (usize{1} << 20) + 5};
        static constexpr usize Chunks[] = {1, 7, 4096, usize{1} << 20};
        static constexpr Pattern Patterns[] = {Pattern::Text, Pattern::Random};
        for (CompressionFormat format : StreamFormats) {
            for (usize size : Sizes) {
                for (Pattern pattern : Patterns) {
                    Array<uint8> original(allocator);
                    Fill(original, size, pattern, size + 1);
                    for (usize chunk : Chunks) {
                        // 1byte 조각은 작은 입력에서만 잰다(시간).
                        if (chunk < 4096 && size > 4096) {
                            continue;
                        }
                        auto stream = CompressStream::TryCreate(DefaultSettings(format), allocator,
                                                                original.Size());
                        REQUIRE(stream);
                        Array<uint8> packed(allocator);
                        REQUIRE(CompressInChunks(stream.Value(), View(original), chunk, packed));
                        CHECK(StreamDecodesTo(format, View(packed), View(original)));

                        // 해제 조각 나누기와 무관하게 같은 결과다.
                        auto decoder =
                            DecompressStream::TryCreate(format, Limits(size + 1), allocator);
                        REQUIRE(decoder);
                        Array<uint8> restored(allocator);
                        CHECK(DecompressInChunks(decoder.Value(), View(packed), chunk, restored));
                        CHECK(decoder.Value().TryFinish());
                        CHECK(Same(restored, View(original)));
                    }

                    // 일회성 압축 결과도 스트림으로 해제한다.
                    Array<uint8> packedOnce(allocator);
                    REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packedOnce));
                    CHECK(StreamDecodesTo(format, View(packedOnce), View(original)));
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 출력 창이 작으면 오류 없이 일부만 진행한다. 작은 압축 입력이 큰 출력을 한꺼번에 만들지 않는다.
TEST(CompressionStream_BackpressureBoundsEachCall) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 200000, Pattern::Text, 11);
        Array<uint8> zeros(allocator);
        Fill(zeros, usize{8} << 20, Pattern::Zeros);
        static constexpr usize Windows[] = {1, 13, 4096};
        for (CompressionFormat format : StreamFormats) {
            for (usize window : Windows) {
                auto stream = CompressStream::TryCreate(DefaultSettings(format), allocator);
                REQUIRE(stream);
                Array<uint8> packed(allocator);
                REQUIRE(
                    CompressWithBackpressure(stream.Value(), View(original), 5000, window, packed));
                CHECK(StreamDecodesTo(format, View(packed), View(original)));

                auto decoder =
                    DecompressStream::TryCreate(format, Limits(original.Size()), allocator);
                REQUIRE(decoder);
                Array<uint8> restored(allocator);
                CHECK(DecompressWithBackpressure(decoder.Value(), View(packed), 333, window,
                                                 restored));
                CHECK(decoder.Value().TryFinish());
                CHECK(Same(restored, View(original)));
            }

            // 8MiB의 0: 압축본 전체를 한 번에 줘도 출력 창(4KiB)만큼만 나온다.
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(zeros), packed));
            auto decoder = DecompressStream::TryCreate(format, Limits(zeros.Size()), allocator);
            REQUIRE(decoder);
            Array<uint8> window(allocator);
            REQUIRE(window.TryResize(4096));
            auto step = decoder.Value().TryProcess(View(packed), Span<uint8>(window.Data(), 4096));
            REQUIRE(step);
            CHECK(step.Value().produced == 4096);
            CHECK(!step.Value().complete);
            CHECK(!decoder.Value().IsAtStreamBoundary());
            Array<uint8> restored(allocator);
            REQUIRE(restored.TryAppend(Span<const uint8>(window.Data(), 4096)));
            const usize consumed = step.Value().consumed;
            CHECK(decoder.Value().TryWriteAppend(
                Slice(View(packed), consumed, packed.Size() - consumed), restored));
            CHECK(decoder.Value().TryFinish());
            CHECK(Same(restored, View(zeros)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// flush한 지점까지의 byte만으로 그때까지의 입력을 모두 해제할 수 있다(네트워크 메시지 경계).
TEST(CompressionStream_FlushMakesPrefixDecodable) {
    SystemAllocator allocator;
    {
        const Span<const uint8> first = Text("first message over the stream");
        const Span<const uint8> second = Text("second message, sent after a sync flush");
        for (CompressionFormat format : StreamFormats) {
            auto stream = CompressStream::TryCreate(DefaultSettings(format), allocator);
            REQUIRE(stream);
            auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            Array<uint8> packed(allocator);
            Array<uint8> restored(allocator);

            REQUIRE(stream.Value().TryWriteAppend(first, packed));
            REQUIRE(stream.Value().TryFlushAppend(packed));
            // 이미 flush했으면 다시 flush해도 성공한다.
            REQUIRE(stream.Value().TryFlushAppend(packed));
            REQUIRE(decoder.Value().TryWriteAppend(View(packed), restored));
            CHECK(Same(restored, first));
            CHECK(!decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);

            const usize sent = packed.Size();
            REQUIRE(stream.Value().TryWriteAppend(second, packed));
            REQUIRE(stream.Value().TryFinishAppend(packed));
            REQUIRE(decoder.Value().TryWriteAppend(Slice(View(packed), sent, packed.Size() - sent),
                                                   restored));
            CHECK(decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish());
            CHECK(restored.Size() == first.Size() + second.Size());
            CHECK(Same(Span<const uint8>(restored.Data() + first.Size(), second.Size()), second));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 미완료 flush·finish 중에는 같은 요청만 받고, 거부는 상태를 바꾸지 않는다.
TEST(CompressionStream_PhaseRules) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 20000, Pattern::Random, 17);
        uint8 one[1] = {};
        const Span<uint8> tiny(one, 1);
        for (CompressionFormat format : StreamFormats) {
            auto created = CompressStream::TryCreate(DefaultSettings(format), allocator);
            REQUIRE(created);
            CompressStream& stream = created.Value();
            Array<uint8> packed(allocator);

            // 출력 공간이 0이면 진행하지 않고 단계도 그대로다.
            auto empty = stream.TryFlush(Span<uint8>());
            CHECK(empty && empty.Value().produced == 0 && !empty.Value().complete);
            auto nothing = stream.TryProcess(Span<const uint8>(), Span<uint8>());
            CHECK(nothing && nothing.Value().complete);
            auto blocked = stream.TryProcess(View(original), Span<uint8>());
            CHECK(blocked && blocked.Value().consumed == 0 && !blocked.Value().complete);

            REQUIRE(stream.TryWriteAppend(View(original), packed));
            auto flushing = stream.TryFlush(tiny);
            REQUIRE(flushing);
            CHECK(!flushing.Value().complete);
            REQUIRE(packed.TryAppend(Span<const uint8>(one, flushing.Value().produced)));
            CHECK(stream.TryProcess(View(original), tiny).Error() == ErrorCode::InvalidArgument);
            CHECK(stream.TryFinish(tiny).Error() == ErrorCode::InvalidArgument);
            CHECK(stream.TryFinishAppend(packed).Error() == ErrorCode::InvalidArgument);
            REQUIRE(stream.TryFlushAppend(packed));

            auto finishing = stream.TryFinish(tiny);
            REQUIRE(finishing);
            REQUIRE(packed.TryAppend(Span<const uint8>(one, finishing.Value().produced)));
            if (!finishing.Value().complete) {
                CHECK(stream.TryProcess(View(original), tiny).Error() ==
                      ErrorCode::InvalidArgument);
                CHECK(stream.TryFlush(tiny).Error() == ErrorCode::InvalidArgument);
                REQUIRE(stream.TryFinishAppend(packed));
            }
            // 거부된 TryProcess는 입력을 소비하지 않았다.
            CHECK(StreamDecodesTo(format, View(packed), View(original)));

            // 끝난 뒤: Finish는 빈 완료, Process·Flush는 거부다.
            auto again = stream.TryFinish(tiny);
            CHECK(again && again.Value().complete && again.Value().produced == 0);
            CHECK(stream.TryFinishAppend(packed).Value() == 0);
            CHECK(stream.TryProcess(View(original), tiny).Error() == ErrorCode::InvalidArgument);
            CHECK(stream.TryWriteAppend(View(original), packed).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(stream.TryFlushAppend(packed).Error() == ErrorCode::InvalidArgument);

            // Reset 뒤 같은 호출 순서는 새 객체와 같은 byte다.
            stream.Reset();
            Array<uint8> reused(allocator);
            REQUIRE(CompressInChunks(stream, View(original), 3000, reused));
            auto fresh = CompressStream::TryCreate(DefaultSettings(format), allocator);
            REQUIRE(fresh);
            Array<uint8> expected(allocator);
            REQUIRE(CompressInChunks(fresh.Value(), View(original), 3000, expected));
            CHECK(Same(reused, View(expected)));
            CHECK(StreamDecodesTo(format, View(reused), View(original)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CompressionStream_ArgumentAndHandleContracts) {
    SystemAllocator allocator;
    {
        CHECK(
            CompressStream::TryCreate(DefaultSettings(CompressionFormat::Lz4), allocator).Error() ==
            ErrorCode::Unsupported);
        CHECK(DecompressStream::TryCreate(CompressionFormat::Lz4, Limits(1), allocator).Error() ==
              ErrorCode::Unsupported);
        CHECK(DecompressStream::TryCreate(static_cast<CompressionFormat>(99), Limits(1), allocator)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(DecompressStream::TryCreate(CompressionFormat::Zstd, Limits(0), allocator).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(DecompressStream::TryCreate(CompressionFormat::Zstd, Limits(1, MinZstdWindowLog - 1),
                                          allocator)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(DecompressStream::TryCreate(CompressionFormat::Gzip, Limits(1, MaxZstdWindowLog + 1),
                                          allocator)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(DecompressStream::TryCreate(CompressionFormat::Zstd, Limits(1, MinZstdWindowLog),
                                          allocator));
        CHECK(DecompressStream::TryCreate(CompressionFormat::Zstd, Limits(1, MaxZstdWindowLog),
                                          allocator));

        Array<uint8> original(allocator);
        Fill(original, 5000, Pattern::Text, 23);
        for (CompressionFormat format : StreamFormats) {
            auto stream = CompressStream::TryCreate(DefaultSettings(format), allocator);
            REQUIRE(stream);

            // 입력·출력이 겹치면 거부하고 상태는 그대로다.
            Array<uint8> buffer(allocator);
            REQUIRE(buffer.TryAppend(View(original)));
            CHECK(stream.Value()
                      .TryProcess(View(buffer), Span<uint8>(buffer.Data() + 10, 100))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(stream.Value().TryWriteAppend(Slice(View(buffer), 0, 10), buffer).Error() ==
                  ErrorCode::InvalidArgument);
            Array<uint8> packed(allocator);
            REQUIRE(CompressInChunks(stream.Value(), View(original), 1000, packed));

            auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            CHECK(decoder.Value().TryWriteAppend(Slice(View(packed), 0, 4), packed).Error() ==
                  ErrorCode::InvalidArgument);
            Array<uint8> restored(allocator);
            REQUIRE(decoder.Value().TryWriteAppend(View(packed), restored));
            CHECK(decoder.Value().TryFinish());
            CHECK(Same(restored, View(original)));

            // 이동된 객체는 StaleHandle이고 Reset은 아무것도 하지 않는다.
            CompressStream moved = Move(stream.Value());
            CHECK(stream.Value().TryFinishAppend(packed).Error() == ErrorCode::StaleHandle);
            CHECK(stream.Value().TryFlush(Span<uint8>()).Error() == ErrorCode::StaleHandle);
            stream.Value().Reset();
            moved.Reset();
            CHECK(moved.TryWriteAppend(View(original), packed));

            DecompressStream taken = Move(decoder.Value());
            CHECK(decoder.Value().TryWriteAppend(View(packed), restored).Error() ==
                  ErrorCode::StaleHandle);
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::StaleHandle);
            CHECK(!decoder.Value().IsAtStreamBoundary());
            decoder.Value().Reset();
            CHECK(taken.TryFinish());
        }

        CompressStream empty;
        CHECK(empty.TryProcess(Span<const uint8>(), Span<uint8>()).Error() ==
              ErrorCode::StaleHandle);
        DecompressStream emptyDecoder;
        CHECK(emptyDecoder.TryFinish().Error() == ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 상한은 Create·Reset 이후 모든 frame·member 출력의 누계이며 정확히 상한이면 성공한다.
TEST(CompressionStream_OutputLimitIsCumulativeAndSticky) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 300000, Pattern::Text, 29);
        const usize size = original.Size();
        for (CompressionFormat format : StreamFormats) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));

            auto exact = DecompressStream::TryCreate(format, Limits(size), allocator);
            REQUIRE(exact);
            Array<uint8> restored(allocator);
            CHECK(exact.Value().TryWriteAppend(View(packed), restored));
            CHECK(exact.Value().TryFinish());

            auto tight = DecompressStream::TryCreate(format, Limits(size - 1), allocator);
            REQUIRE(tight);
            Array<uint8> output(allocator);
            REQUIRE(output.TryAppend(Text("keep")));
            CHECK(tight.Value().TryWriteAppend(View(packed), output).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(output.Size() == 4);
            // sticky: Reset 전까지 같은 오류다.
            CHECK(tight.Value().TryWriteAppend(Span<const uint8>(), output).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(tight.Value().TryFinish().Error() == ErrorCode::CapacityExceeded);
            CHECK(!tight.Value().IsAtStreamBoundary());
            // 큰 출력 창을 주는 TryProcess도 상한에서 멈춘다.
            tight.Value().Reset();
            Array<uint8> large(allocator);
            REQUIRE(large.TryResize(size * 2));
            CHECK(tight.Value()
                      .TryProcess(View(packed), Span<uint8>(large.Data(), large.Size()))
                      .Error() == ErrorCode::CapacityExceeded);
            tight.Value().Reset();
            CHECK(tight.Value().TryWriteAppend(Slice(View(packed), 0, 10), output));

            // 두 번째 frame·member는 누계로 센다. Deflate·Zlib은 스트림 뒤 byte가 손상이다.
            Array<uint8> twice(allocator);
            REQUIRE(twice.TryAppend(View(packed)));
            REQUIRE(twice.TryAppend(View(packed)));
            auto both = DecompressStream::TryCreate(format, Limits(size * 2), allocator);
            auto short2 = DecompressStream::TryCreate(format, Limits(size * 2 - 1), allocator);
            REQUIRE(both && short2);
            Array<uint8> joined(allocator);
            Array<uint8> clipped(allocator);
            const bool concatenates =
                format == CompressionFormat::Zstd || format == CompressionFormat::Gzip;
            if (concatenates) {
                CHECK(both.Value().TryWriteAppend(View(twice), joined));
                CHECK(both.Value().TryFinish());
                CHECK(joined.Size() == size * 2);
                CHECK(short2.Value().TryWriteAppend(View(twice), clipped).Error() ==
                      ErrorCode::CapacityExceeded);
                // Reset은 누계를 지운다.
                both.Value().Reset();
                joined.Clear();
                CHECK(both.Value().TryWriteAppend(View(twice), joined));
                CHECK(joined.Size() == size * 2);
            } else {
                CHECK(both.Value().TryWriteAppend(View(twice), joined).Error() ==
                      ErrorCode::InvalidArgument);
                CHECK(both.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 압축 폭탄: 출력 상한과 zstd window 상한이 작업 메모리를 묶는다.
TEST(CompressionStream_BombsStayBounded) {
    SystemAllocator system;
    CountingAllocator counting(system);
    constexpr usize Limit = usize{1} << 20;
    Array<uint8> zeros(system);
    Fill(zeros, usize{64} << 20, Pattern::Zeros);
    for (CompressionFormat format : StreamFormats) {
        Array<uint8> packed(system);
        REQUIRE(TryCompressAppend(Settings(format, format == CompressionFormat::Zstd ? 19 : 9),
                                  View(zeros), packed));
        counting.ResetPeak();
        {
            auto decoder = DecompressStream::TryCreate(format, Limits(Limit, 20), counting);
            REQUIRE(decoder);
            Array<uint8> output(counting);
            const ErrorCode error = decoder.Value().TryWriteAppend(View(packed), output).Error();
            // zstd 수준 19 frame은 window가 2^20보다 커서 해제 전에 거부한다.
            CHECK(error == ErrorCode::CapacityExceeded);
            CHECK(output.Size() == 0);
        }
        CHECK(counting.Peak() < Limit * 2);
    }
    CHECK(counting.Current() == 0);
}

// zstd window 검사는 frame 시작마다 하며 입력 조각 나누기와 무관하다.
TEST(CompressionStream_ZstdWindowLimit) {
    SystemAllocator allocator;
    {
        // 크기 미기록, window 2^21(WD 0x58)·2^31(WD 0xa8), raw 블록 'A' 하나.
        static constexpr uint8 Wide21[] = {0x28, 0xb5, 0x2f, 0xfd, 0x00,
                                           0x58, 0x09, 0x00, 0x00, 'A'};
        static constexpr uint8 Wide31[] = {0x28, 0xb5, 0x2f, 0xfd, 0x00,
                                           0xa8, 0x09, 0x00, 0x00, 'A'};
        // window 2^32(WD 0xb0)·2^31 × 9/8(WD 0xa9, windowLog 31 + 가수부): 구현 상한(2^31) 밖이다.
        static constexpr uint8 TooWide[] = {0x28, 0xb5, 0x2f, 0xfd, 0x00,
                                            0xb0, 0x09, 0x00, 0x00, 'A'};
        static constexpr uint8 Mantissa[] = {0x28, 0xb5, 0x2f, 0xfd, 0x00,
                                             0xa9, 0x09, 0x00, 0x00, 'A'};
        // single segment, 원본 크기 2^40 선언: window = 원본 크기라 구현 상한 밖이다.
        static constexpr uint8 Huge[] = {0x28, 0xb5, 0x2f, 0xfd, 0xe0, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x00, 'A'};
        // single segment, 원본 크기 100인데 마지막 RLE 블록이 1000byte를 선언한다(손상).
        static constexpr uint8 Overrun[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20,
                                            0x64, 0x43, 0x1f, 0x00, 0x41};
        struct Case {
            Span<const uint8> frame;
            uint32 maxWindowLog;
            ErrorCode expected;
        };
        const Case Cases[] = {
            {Bytes(Wide21), 20, ErrorCode::CapacityExceeded},
            {Bytes(Wide21), 21, ErrorCode::None},
            {Bytes(Wide31), 27, ErrorCode::CapacityExceeded},
            {Bytes(TooWide), 31, ErrorCode::Unsupported},
            {Bytes(Mantissa), 31, ErrorCode::Unsupported},
            {Bytes(Huge), 31, ErrorCode::Unsupported},
            {Bytes(Overrun), 27, ErrorCode::InvalidArgument},
        };
        for (const Case& item : Cases) {
            const usize Chunkings[] = {1, 3, item.frame.Size()};
            for (usize chunk : Chunkings) {
                auto decoder = DecompressStream::TryCreate(
                    CompressionFormat::Zstd, Limits(1 << 20, item.maxWindowLog), allocator);
                REQUIRE(decoder);
                Array<uint8> output(allocator);
                ErrorCode error = ErrorCode::None;
                for (usize offset = 0; offset < item.frame.Size() && error == ErrorCode::None;
                     offset += chunk) {
                    auto step = decoder.Value().TryWriteAppend(
                        Slice(item.frame, offset, Smaller(chunk, item.frame.Size() - offset)),
                        output);
                    error = step ? ErrorCode::None : step.Error();
                }
                CHECK(error == item.expected);
                if (item.expected == ErrorCode::None) {
                    CHECK(decoder.Value().TryFinish());
                    CHECK(output.Size() == 1 && output[0] == 'A');
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경계·절단·스트림 뒤 byte·skippable frame.
TEST(CompressionStream_BoundariesAndTrailingBytes) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 40000, Pattern::Text, 31);
        for (CompressionFormat format : StreamFormats) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));

            auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            CHECK(!decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);

            // 마지막 byte 전까지는 절단이다. 마지막 byte로 경계에 닿는다.
            Array<uint8> restored(allocator);
            REQUIRE(decoder.Value().TryWriteAppend(Slice(View(packed), 0, packed.Size() - 1),
                                                   restored));
            CHECK(!decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
            REQUIRE(decoder.Value().TryWriteAppend(Slice(View(packed), packed.Size() - 1, 1),
                                                   restored));
            CHECK(decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish());
            CHECK(Same(restored, View(original)));

            // 스트림 뒤 byte.
            static constexpr uint8 Junk[] = {'x', 'y', 'z'};
            static constexpr uint8 GzipMagic[] = {0x1f};
            const ErrorCode junk = decoder.Value().TryWriteAppend(Bytes(Junk), restored).Error();
            CHECK(junk == ErrorCode::InvalidArgument);
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
            if (format == CompressionFormat::Gzip) {
                // magic 첫 byte만 오면 다음 member를 기다린다(경계가 아니다).
                decoder.Value().Reset();
                Array<uint8> next(allocator);
                REQUIRE(decoder.Value().TryWriteAppend(View(packed), next));
                REQUIRE(decoder.Value().TryWriteAppend(Bytes(GzipMagic), next));
                CHECK(!decoder.Value().IsAtStreamBoundary());
                CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
            }
        }

        // zstd skippable frame(magic 0x184D2A50): 출력 없이 경계다. skippable만 받아도 Finish는
        // 성공이다.
        static constexpr uint8 Skippable[] = {0x50, 0x2a, 0x4d, 0x18, 0x03, 0x00,
                                              0x00, 0x00, 1,    2,    3};
        static constexpr uint8 EmptySkippable[] = {0x5f, 0x2a, 0x4d, 0x18, 0x00, 0x00, 0x00, 0x00};
        const Span<const uint8> Frames[] = {Bytes(Skippable), Bytes(EmptySkippable)};
        for (Span<const uint8> frame : Frames) {
            const usize Chunkings[] = {1, frame.Size()};
            for (usize chunk : Chunkings) {
                auto decoder = DecompressStream::TryCreate(CompressionFormat::Zstd, Limits(1 << 20),
                                                           allocator);
                REQUIRE(decoder);
                Array<uint8> output(allocator);
                REQUIRE(DecompressInChunks(decoder.Value(), frame, chunk, output));
                CHECK(output.Size() == 0);
                CHECK(decoder.Value().IsAtStreamBoundary());
                CHECK(decoder.Value().TryFinish());

                Array<uint8> packed(allocator);
                REQUIRE(TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), View(original),
                                          packed));
                REQUIRE(DecompressInChunks(decoder.Value(), View(packed), 4096, output));
                REQUIRE(DecompressInChunks(decoder.Value(), frame, chunk, output));
                CHECK(decoder.Value().TryFinish());
                CHECK(Same(output, View(original)));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 코덱·출력 성장 할당 실패는 sticky이며 Append 길이를 되돌린다. Reset 뒤에는 다시 성공한다.
TEST(CompressionStream_OutOfMemoryIsStickyAndRecoverable) {
    SystemAllocator system;
    Array<uint8> original(system);
    Fill(original, 150000, Pattern::Text, 37);
    for (CompressionFormat format : StreamFormats) {
        Array<uint8> packed(system);
        REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));
        NFailAllocator failing(system);

        // 생성 실패는 누수 없이 OutOfMemory다.
        bool created = false;
        for (uint64 attempt = 1; attempt <= 16 && !created; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto stream = CompressStream::TryCreate(DefaultSettings(format), failing);
            auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), failing);
            created = stream && decoder;
            CHECK(created || (stream ? decoder.Error() : stream.Error()) == ErrorCode::OutOfMemory);
        }
        CHECK(created);
        CHECK(system.Stats().liveAllocations == 2);

        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            {
                failing.DisableFailure();
                auto stream = CompressStream::TryCreate(DefaultSettings(format), failing);
                REQUIRE(stream);
                Array<uint8> output(failing);
                REQUIRE(output.TryAppend(Text("base")));
                failing.FailOnAttempt(attempt);
                auto written = stream.Value().TryWriteAppend(View(original), output);
                const usize afterWrite = output.Size();
                auto finished = written ? stream.Value().TryFinishAppend(output)
                                        : Result<usize>::Failure(written.Error());
                succeeded = finished.IsOk();
                CHECK(!succeeded || failing.Attempts() < attempt);
                if (!succeeded) {
                    CHECK(finished.Error() == ErrorCode::OutOfMemory);
                    CHECK(output.Size() == (written ? afterWrite : 4));
                    CHECK(stream.Value().TryFinishAppend(output).Error() == ErrorCode::OutOfMemory);
                    failing.DisableFailure();
                    stream.Value().Reset();
                    REQUIRE(output.TryResize(4));
                    REQUIRE(stream.Value().TryWriteAppend(View(original), output));
                    REQUIRE(stream.Value().TryFinishAppend(output));
                }
                CHECK(StreamDecodesTo(format, Slice(View(output), 4, output.Size() - 4),
                                      View(original)));
            }
            CHECK(system.Stats().liveAllocations == 2);
        }
        CHECK(succeeded);

        succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            {
                failing.DisableFailure();
                auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), failing);
                REQUIRE(decoder);
                Array<uint8> output(failing);
                REQUIRE(output.TryAppend(Text("base")));
                failing.FailOnAttempt(attempt);
                auto written = decoder.Value().TryWriteAppend(View(packed), output);
                succeeded = written.IsOk();
                CHECK(!succeeded || failing.Attempts() < attempt);
                if (!succeeded) {
                    CHECK(written.Error() == ErrorCode::OutOfMemory);
                    CHECK(output.Size() == 4);
                    CHECK(decoder.Value().TryFinish().Error() == ErrorCode::OutOfMemory);
                    failing.DisableFailure();
                    decoder.Value().Reset();
                    REQUIRE(decoder.Value().TryWriteAppend(View(packed), output));
                }
                CHECK(decoder.Value().TryFinish());
                CHECK(Same(Slice(View(output), 4, output.Size() - 4), View(original)));
            }
            CHECK(system.Stats().liveAllocations == 2);
        }
        CHECK(succeeded);
    }
}

// 같은 호출 순서면 플랫폼과 무관하게 같은 byte다(64bit little-endian 대상).
TEST(CompressionStream_DeterministicOutputGoldens) {
    struct Golden {
        CompressionFormat format;
        usize size;
        uint32 crc;
    };
    static constexpr Golden Goldens[] = {
        // 기준: Windows x64 clang 21.1.6, zstd 1.5.7·zlib 1.4, 기본 수준, 4099byte 조각 + 중간
        // flush.
        {CompressionFormat::Zstd, 847499, 0xad593300u},
        {CompressionFormat::Deflate, 885075, 0xd3d963d4u},
        {CompressionFormat::Zlib, 885081, 0xe9386ea6u},
        {CompressionFormat::Gzip, 885093, 0x4da3a361u},
    };
    SystemAllocator allocator;
    Array<uint8> original(allocator);
    Fill(original, (usize{3} << 19) + 11, Pattern::Text, 41);
    const usize half = original.Size() / 2;
    for (const Golden& golden : Goldens) {
        auto stream = CompressStream::TryCreate(DefaultSettings(golden.format), allocator);
        REQUIRE(stream);
        Array<uint8> packed(allocator);
        for (usize offset = 0; offset < half; offset += 4099) {
            REQUIRE(stream.Value().TryWriteAppend(
                Slice(View(original), offset, Smaller(4099, half - offset)), packed));
        }
        REQUIRE(stream.Value().TryFlushAppend(packed));
        REQUIRE(CompressInChunks(
            stream.Value(), Slice(View(original), half, original.Size() - half), 4099, packed));
        CHECK(StreamDecodesTo(golden.format, View(packed), View(original)));
        const uint32 crc = Crc32(View(packed));
        if (packed.Size() != golden.size || crc != golden.crc) {
            printf("compression stream golden mismatch: format %u size %llu crc 0x%08x\n",
                   static_cast<unsigned>(golden.format),
                   static_cast<unsigned long long>(packed.Size()), crc);
        }
        CHECK(packed.Size() == golden.size);
        CHECK(crc == golden.crc);
    }
}

// 출력 창이 아주 작아도 flush·finish가 유한한 호출 안에 끝난다(zlib flush marker 반복 회귀).
TEST(CompressionStream_TinyWindowsTerminate) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 30000, Pattern::Text, 43);
        for (CompressionFormat format : StreamFormats) {
            for (usize window = 1; window <= 8; ++window) {
                auto stream = CompressStream::TryCreate(DefaultSettings(format), allocator);
                REQUIRE(stream);
                Array<uint8> packed(allocator);
                uint8 buffer[8] = {};
                const Span<uint8> target(buffer, window);
                bool ok = true;
                for (usize round = 0; round < 2 && ok; ++round) {
                    const Span<const uint8> half =
                        Slice(View(original), round * 15000, original.Size() / 2);
                    ok = stream.Value().TryWriteAppend(half, packed).IsOk();
                    // 처음은 flush, 다음은 finish를 작은 창으로 complete까지 반복한다.
                    bool complete = false;
                    for (usize call = 0; call < 200000 && ok && !complete; ++call) {
                        auto step = round == 0 ? stream.Value().TryFlush(target)
                                               : stream.Value().TryFinish(target);
                        ok = step &&
                             packed.TryAppend(Span<const uint8>(buffer, step.Value().produced));
                        complete = ok && step.Value().complete;
                    }
                    ok = ok && complete;
                }
                CHECK(ok);
                CHECK(StreamDecodesTo(format, View(packed), View(original)));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 분할 단위(1MiB)를 넘는 창, 상한에 정확히 닿는 TryProcess, 상한 직후의 빈 frame·member.
TEST(CompressionStream_LargeWindowsAndLimitEdges) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, (usize{3} << 20) + 5, Pattern::Text, 47);
        const usize size = original.Size();
        Array<uint8> window(allocator);
        REQUIRE(window.TryResize(size + 1));
        const Span<uint8> big(window.Data(), window.Size());
        for (CompressionFormat format : StreamFormats) {
            // 압축: 큰 창 하나로 Process·Flush·Finish.
            auto stream = CompressStream::TryCreate(DefaultSettings(format), allocator);
            REQUIRE(stream);
            Array<uint8> packed(allocator);
            auto processed = stream.Value().TryProcess(View(original), big);
            REQUIRE(processed);
            CHECK(processed.Value().complete && processed.Value().consumed == size);
            REQUIRE(packed.TryAppend(Span<const uint8>(window.Data(), processed.Value().produced)));
            auto flushed = stream.Value().TryFlush(big);
            REQUIRE(flushed && flushed.Value().complete);
            REQUIRE(packed.TryAppend(Span<const uint8>(window.Data(), flushed.Value().produced)));
            auto finished = stream.Value().TryFinish(big);
            REQUIRE(finished && finished.Value().complete);
            REQUIRE(packed.TryAppend(Span<const uint8>(window.Data(), finished.Value().produced)));
            CHECK(StreamDecodesTo(format, View(packed), View(original)));

            // 해제: 허용량보다 큰 창으로 정확히 상한에 닿으면 성공이다.
            auto exact = DecompressStream::TryCreate(format, Limits(size), allocator);
            REQUIRE(exact);
            auto decoded = exact.Value().TryProcess(View(packed), big);
            REQUIRE(decoded);
            CHECK(decoded.Value().consumed == packed.Size() && decoded.Value().complete);
            CHECK(decoded.Value().produced == size);
            CHECK(Same(Span<const uint8>(window.Data(), size), View(original)));
            CHECK(exact.Value().TryFinish());

            // 상한에 닿은 뒤 빈 frame·member는 성공, 1byte라도 더 나오면 CapacityExceeded다.
            if (format == CompressionFormat::Zstd || format == CompressionFormat::Gzip) {
                Array<uint8> empty(allocator);
                REQUIRE(TryCompressAppend(DefaultSettings(format), Span<const uint8>(), empty));
                Array<uint8> tail(allocator);
                CHECK(exact.Value().TryWriteAppend(View(empty), tail));
                CHECK(tail.Size() == 0 && exact.Value().TryFinish());
                Array<uint8> one(allocator);
                REQUIRE(TryCompressAppend(DefaultSettings(format), Text("x"), one));
                CHECK(exact.Value().TryWriteAppend(View(one), tail).Error() ==
                      ErrorCode::CapacityExceeded);
                CHECK(tail.Size() == 0);
            }

            // 1MiB를 여러 번 늘린 뒤 상한 초과로 실패해도 길이를 되돌린다.
            auto tight = DecompressStream::TryCreate(format, Limits(size - 1), allocator);
            REQUIRE(tight);
            Array<uint8> output(allocator);
            REQUIRE(output.TryAppend(Text("keep")));
            CHECK(tight.Value().TryWriteAppend(View(packed), output).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(output.Size() == 4 && output.Capacity() >= size - 1);

            // 상한 SIZE_MAX도 overflow 없이 동작한다.
            auto unlimited =
                DecompressStream::TryCreate(format, Limits(static_cast<usize>(-1)), allocator);
            REQUIRE(unlimited);
            Array<uint8> restored(allocator);
            CHECK(unlimited.Value().TryWriteAppend(View(packed), restored));
            CHECK(unlimited.Value().TryFinish() && Same(restored, View(original)));

            // 이동 대입은 대상의 상태를 해제하고 원본을 넘겨받는다.
            DecompressStream assigned;
            assigned = Move(unlimited.Value());
            CHECK(assigned.TryFinish());
            assigned = Move(exact.Value());
            CHECK(!exact.Value().IsAtStreamBoundary());
        }

        // 출력 0으로 부른 TryFinish는 단계를 바꾸지 않는다(이어서 TryProcess를 받는다).
        auto stream =
            CompressStream::TryCreate(DefaultSettings(CompressionFormat::Gzip), allocator);
        REQUIRE(stream);
        auto empty = stream.Value().TryFinish(Span<uint8>());
        CHECK(empty && !empty.Value().complete);
        Array<uint8> packed(allocator);
        CHECK(stream.Value().TryWriteAppend(Text("after empty finish"), packed));
        CHECK(stream.Value().TryFinishAppend(packed));
        CHECK(StreamDecodesTo(CompressionFormat::Gzip, View(packed), Text("after empty finish")));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 판정이 출력 창 크기에 따라 달라지지 않는다: Deflate·Zlib 뒤 byte, 스트림이 창 끝에서 끝나는 경우.
TEST(CompressionStream_OutcomeIndependentOfWindow) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 5000, Pattern::Text, 53);
        Array<uint8> window(allocator);
        REQUIRE(window.TryResize(original.Size()));
        static constexpr CompressionFormat Ended[] = {CompressionFormat::Deflate,
                                                      CompressionFormat::Zlib};
        for (CompressionFormat format : Ended) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));
            REQUIRE(packed.TryAppend(Text("!")));
            auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            // 창이 정확히 원본 크기여도 뒤 byte를 이 호출에서 거부한다.
            CHECK(decoder.Value()
                      .TryProcess(View(packed), Span<uint8>(window.Data(), window.Size()))
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
        }

        for (CompressionFormat format : StreamFormats) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));
            auto decoder = DecompressStream::TryCreate(format, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            auto step =
                decoder.Value().TryProcess(View(packed), Span<uint8>(window.Data(), window.Size()));
            REQUIRE(step);
            // 원본 크기와 같은 창: 스트림이 창 끝에서 끝나도 complete이고 경계다.
            CHECK(step.Value().consumed == packed.Size() && step.Value().complete);
            CHECK(decoder.Value().IsAtStreamBoundary());
            CHECK(Same(Span<const uint8>(window.Data(), window.Size()), View(original)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// zstd ultra 수준(20–22): 일회성과 크기 힌트를 준 스트림이 왕복하고, 스트림 frame은 기본 window
// 상한(2^27)으로 풀린다. 크기 힌트가 매개변수를 입력에 맞춰 줄여 작업 메모리가 작다(힌트 없는 큰
// 스트림은 수백 MiB).
TEST(CompressionStream_ZstdUltraLevelsRoundTrip) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 300000, Pattern::Text, 91);
        for (int32 level = 20; level <= 22; ++level) {
            const CompressionSettings settings = Settings(CompressionFormat::Zstd, level);
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(settings, View(original), packed));
            Array<uint8> restored(allocator);
            REQUIRE(TryDecompressAppend(CompressionFormat::Zstd, View(packed), original.Size(),
                                        restored));
            CHECK(Same(restored, View(original)));

            auto stream = CompressStream::TryCreate(settings, allocator, original.Size());
            REQUIRE(stream);
            Array<uint8> streamed(allocator);
            REQUIRE(stream.Value().TryWriteAppend(View(original), streamed));
            REQUIRE(stream.Value().TryFinishAppend(streamed));
            auto decoder = DecompressStream::TryCreate(
                CompressionFormat::Zstd, {.maxOutputBytes = original.Size()}, allocator);
            REQUIRE(decoder);
            Array<uint8> decoded(allocator);
            REQUIRE(decoder.Value().TryWriteAppend(View(streamed), decoded));
            CHECK(decoder.Value().TryFinish());
            CHECK(Same(decoded, View(original)));
        }
        CHECK(CompressionSettings::TryMake(CompressionFormat::Zstd, 22));
        CHECK(CompressionSettings::TryMake(CompressionFormat::Zstd, 23).Error() ==
              ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
