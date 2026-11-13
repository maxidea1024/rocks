#include "CompressionTestSupport.h"

#include <Compression/AsyncCompression.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/CompressionCodec.h>
#include <Compression/CompressionDictionary.h>
#include <Compression/CompressionStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <stdio.h>

// 재시도 사이 취소를 결정적으로 시험하려고 내부 경계(취소 probe)를 쓴다. 시험은 저장소 루트를
// include 경로로 쓴다(AsyncCompressionTests와 같은 방식).
#include <Core/Source/Runtime/Compression/Private/CompressionInternal.h>

// 사용자 등록 형식(CompressionCodec.h) 시험이다. 등록 표는 프로세스 전역이고 시험은 이름순으로
// 한 프로세스에서 실행되므로, 각 시험은 ScopedCodec으로 등록해 REQUIRE로 일찍 끝나도 해제한다.
// 해제 성공이 곧 사용권 누수가 없다는 확인이다.
namespace rocks::test {
namespace {

using namespace compression;
using Body = Function<void(WorkerTaskContext&)>;

constexpr CompressionLevelRange TestLevels{1, 9, 5};

// 등록하고, 소멸할 때 아직 등록돼 있으면 해제해 성공을 확인한다. codec은 이 객체보다 먼저
// 선언한다(나중에 소멸한다).
class ScopedCodec {
public:
    ScopedCodec(const char* name, const CompressionCodec& codec,
                CompressionLevelRange levels = TestLevels, uint32 version = 1) {
        auto registered = TryRegisterCompressionCodec({StringView(name), version, levels}, codec);
        CHECK(registered);
        if (registered) {
            format_ = registered.Value();
            held_ = true;
        }
    }

    ~ScopedCodec() {
        if (held_) {
            CHECK(TryUnregisterCompressionCodec(format_));
        }
    }

    ScopedCodec(const ScopedCodec&) = delete;
    ScopedCodec& operator=(const ScopedCodec&) = delete;

    CompressionFormat Format() const {
        return format_;
    }

    // 해제를 시도한다. 성공하면 소멸자는 더 해제하지 않는다.
    Result<void> Unregister() {
        auto removed = TryUnregisterCompressionCodec(format_);
        if (removed) {
            held_ = false;
        }
        return removed;
    }

private:
    CompressionFormat format_ = CompressionFormat::Lz4;
    bool held_ = false;
};

// 시험 코덱이 scratch를 실제로 쓰게 하는 작업 메모리 대여다(OOM 경로 시험용).
Result<void> BorrowScratch(Allocator& scratch) noexcept {
    auto work = scratch.Allocate(64, 16);
    if (!work) {
        return Result<void>::Failure(work.Error());
    }
    return scratch.Deallocate(work.Value());
}

// 첫 byte에 수준을 기록하고 나머지는 0x5A와 XOR한다. 출력 = 입력 + 1byte.
class XorCodec final : public CompressionCodec {
public:
    Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept override {
        if (sourceBytes == static_cast<usize>(-1)) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }
        return Result<usize>::Success(sourceBytes + 1);
    }

    Result<usize> TryCompress(int32 level, Span<const uint8> source, Span<uint8> destination,
                              Allocator& scratch) const noexcept override {
        auto borrowed = BorrowScratch(scratch);
        if (!borrowed) {
            return Result<usize>::Failure(borrowed.Error());
        }
        if (destination.Size() < source.Size() + 1) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }

        destination[0] = static_cast<uint8>(level);
        for (usize index = 0; index < source.Size(); ++index) {
            destination[index + 1] = static_cast<uint8>(source[index] ^ 0x5A);
        }
        return Result<usize>::Success(source.Size() + 1);
    }

    Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination,
                                Allocator& scratch) const noexcept override {
        auto borrowed = BorrowScratch(scratch);
        if (!borrowed) {
            return Result<usize>::Failure(borrowed.Error());
        }
        if (destination.Size() < source.Size() - 1) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }

        for (usize index = 1; index < source.Size(); ++index) {
            destination[index - 1] = static_cast<uint8>(source[index] ^ 0x5A);
        }
        return Result<usize>::Success(source.Size() - 1);
    }
};

// (반복 수 1–255, 값) 쌍의 run-length 부호다. 해제 호출 수를 센다(단일 스레드 시험에서만 읽는다).
class RleCodec final : public CompressionCodec {
public:
    mutable uint32 decompressCalls = 0;

    Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept override {
        if (sourceBytes > static_cast<usize>(-1) / 2) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }
        return Result<usize>::Success(sourceBytes * 2);
    }

    Result<usize> TryCompress(int32, Span<const uint8> source, Span<uint8> destination,
                              Allocator&) const noexcept override {
        usize written = 0;
        usize index = 0;
        while (index < source.Size()) {
            usize run = 1;
            while (index + run < source.Size() && run < 255 &&
                   source[index + run] == source[index]) {
                ++run;
            }
            if (written + 2 > destination.Size()) {
                return Result<usize>::Failure(ErrorCode::CapacityExceeded);
            }
            destination[written] = static_cast<uint8>(run);
            destination[written + 1] = source[index];
            written += 2;
            index += run;
        }
        return Result<usize>::Success(written);
    }

    Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination,
                                Allocator&) const noexcept override {
        ++decompressCalls;
        if (source.Size() % 2 != 0) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        usize written = 0;
        for (usize index = 0; index < source.Size(); index += 2) {
            const usize run = source[index];
            if (run == 0) {
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }
            if (run > destination.Size() - written) {
                return Result<usize>::Failure(ErrorCode::CapacityExceeded);
            }
            for (usize copy = 0; copy < run; ++copy) {
                destination[written + copy] = source[index + 1];
            }
            written += run;
        }
        return Result<usize>::Success(written);
    }
};

// 반환 계약을 어기는 코덱이다. 시험이 mode·error를 호출 사이에 바꾼다(단일 스레드).
class MisbehavingCodec final : public CompressionCodec {
public:
    enum class Mode : uint8 {
        OversizedResult, // 압축·해제가 destination보다 큰 크기를 돌려준다
        CapacityAtBound, // 압축이 bound 이상의 출력에서도 CapacityExceeded다
        CompressError,   // 압축이 error를 돌려준다
        DecompressError, // 해제가 error를 돌려준다
        BoundError       // bound가 error를 돌려준다
    };

    mutable Mode mode = Mode::OversizedResult;
    mutable ErrorCode error = ErrorCode::None;

    Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept override {
        if (mode == Mode::BoundError) {
            return Result<usize>::Failure(error);
        }
        return Result<usize>::Success(sourceBytes + 8);
    }

    Result<usize> TryCompress(int32, Span<const uint8>, Span<uint8> destination,
                              Allocator&) const noexcept override {
        switch (mode) {
        case Mode::OversizedResult:
            return Result<usize>::Success(destination.Size() + 1);
        case Mode::CapacityAtBound:
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        case Mode::CompressError:
            return Result<usize>::Failure(error);
        case Mode::DecompressError:
        case Mode::BoundError:
            break;
        }
        return Result<usize>::Success(0);
    }

    Result<usize> TryDecompress(Span<const uint8>, Span<uint8> destination,
                                Allocator&) const noexcept override {
        if (mode == Mode::OversizedResult) {
            return Result<usize>::Success(destination.Size() + 1);
        }
        if (mode == Mode::DecompressError) {
            return Result<usize>::Failure(error);
        }
        return Result<usize>::Success(0);
    }
};

// 압축 호출 안에서 진입을 알리고 release까지 기다린다(실행 중인 사용권 시험용). 출력은 복사다.
class BlockingCodec final : public CompressionCodec {
public:
    mutable platform::ManualResetEvent entered;
    mutable platform::ManualResetEvent release;

    Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept override {
        return Result<usize>::Success(sourceBytes);
    }

    Result<usize> TryCompress(int32, Span<const uint8> source, Span<uint8> destination,
                              Allocator&) const noexcept override {
        entered.Signal();
        (void)release.Wait(platform::InfiniteTimeout);
        return Copy(source, destination);
    }

    Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination,
                                Allocator&) const noexcept override {
        return Copy(source, destination);
    }

private:
    static Result<usize> Copy(Span<const uint8> source, Span<uint8> destination) noexcept {
        if (destination.Size() < source.Size()) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }
        for (usize index = 0; index < source.Size(); ++index) {
            destination[index] = source[index];
        }
        return Result<usize>::Success(source.Size());
    }
};

// N번째 확인부터 취소를 보고하는 probe다.
struct CountdownProbe {
    uint32 remaining;

    static bool Check(void* raw) noexcept {
        auto& self = *static_cast<CountdownProbe*>(raw);
        if (self.remaining == 0) {
            return true;
        }
        --self.remaining;
        return false;
    }
};

Span<const uint8> Text(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

} // namespace

// 등록 표: 이름 규칙·수준 범위 위반, 중복 이름, 17번째 등록(해제 중인 칸 포함), 형식 값의 단조
// 증가와 재사용 금지.
TEST(CompressionCodec_RegistrationRules) {
    SystemAllocator allocator;
    XorCodec codec;
    static constexpr const char* BadNames[] = {
        "",
        "kraken",
        "Oodle.kraken",
        ".kraken",
        "oodle.",
        "oodle..kraken",
        "oodle.kra ken",
        "oodle.kraken!",
        "abcdefghijklmnop.qrstuvwxyz012345", // 33byte
    };
    for (const char* name : BadNames) {
        CHECK(TryRegisterCompressionCodec({StringView(name), 1, TestLevels}, codec).Error() ==
              ErrorCode::InvalidArgument);
    }
    CHECK(TryRegisterCompressionCodec({StringView("test.inverted"), 1, {5, 9, 4}}, codec).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryRegisterCompressionCodec({StringView("test.inverted"), 1, {1, 4, 5}}, codec).Error() ==
          ErrorCode::InvalidArgument);

    // 32byte·여러 조각·'_'·'-'·숫자는 받는다.
    ScopedCodec longest("abcdefghijklmnop.qrstuvwxyz01234", codec);
    ScopedCodec dotted("studio.rle-v2.fast_1", codec);
    CHECK(static_cast<uint32>(longest.Format()) >= 0x100);
    CHECK(static_cast<uint32>(dotted.Format()) > static_cast<uint32>(longest.Format()));
    CHECK(TryRegisterCompressionCodec({StringView("studio.rle-v2.fast_1"), 2, TestLevels}, codec)
              .Error() == ErrorCode::AlreadyExists);

    // 나머지 14칸을 채우면 17번째는 CapacityExceeded다.
    char names[MaxCompressionCodecs][16] = {};
    CompressionFormat filled[MaxCompressionCodecs] = {};
    for (uint32 index = 2; index < MaxCompressionCodecs; ++index) {
        snprintf(names[index], sizeof(names[index]), "test.fill%u", index);
        auto registered =
            TryRegisterCompressionCodec({StringView(names[index]), 1, TestLevels}, codec);
        CHECK(registered);
        filled[index] = registered ? registered.Value() : CompressionFormat::Lz4;
    }
    CHECK(
        TryRegisterCompressionCodec({StringView("test.overflow"), 1, TestLevels}, codec).Error() ==
        ErrorCode::CapacityExceeded);

    // 해제 중인 칸도 칸을 차지한다: 문맥이 사용권을 쥔 형식을 해제하면 Busy이고 아직 17번째는
    // 들어가지 않는다.
    {
        auto compressor = Compressor::TryCreate(DefaultSettings(filled[2]), allocator);
        CHECK(compressor);
        CHECK(TryUnregisterCompressionCodec(filled[2]).Error() == ErrorCode::Busy);
        CHECK(TryRegisterCompressionCodec({StringView("test.overflow"), 1, TestLevels}, codec)
                  .Error() == ErrorCode::CapacityExceeded);
    }
    CHECK(TryUnregisterCompressionCodec(filled[2]));
    ScopedCodec extra("test.overflow", codec);

    // 해제한 값은 다시 쓰지 않는다. 같은 이름을 다시 등록하면 더 큰 새 값이다.
    const CompressionFormat old = dotted.Format();
    CHECK(dotted.Unregister());
    CHECK(TryUnregisterCompressionCodec(old).Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetCompressionLevelRange(old).Error() == ErrorCode::InvalidArgument);
    ScopedCodec again("studio.rle-v2.fast_1", codec);
    CHECK(static_cast<uint32>(again.Format()) > static_cast<uint32>(extra.Format()));
    CHECK(TryGetCompressionLevelRange(old).Error() == ErrorCode::InvalidArgument);

    // 내장·모르는 값의 해제는 InvalidArgument다.
    CHECK(TryUnregisterCompressionCodec(CompressionFormat::Zstd).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryUnregisterCompressionCodec(static_cast<CompressionFormat>(99)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryUnregisterCompressionCodec(static_cast<CompressionFormat>(0xFFFFFFFF)).Error() ==
          ErrorCode::InvalidArgument);
    for (uint32 index = 3; index < MaxCompressionCodecs; ++index) {
        CHECK(TryUnregisterCompressionCodec(filled[index]));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 형식 이름·revision: 내장 이름 golden과 이름 ↔ 값 왕복, 사용자 등록 형식의 정보, 이름 문법.
TEST(CompressionCodec_FormatInfoAndLookup) {
    struct Builtin {
        CompressionFormat format;
        const char* name;
        bool compresses;
    };
    static constexpr Builtin Builtins[] = {{CompressionFormat::Lz4, "lz4", true},
                                           {CompressionFormat::Zstd, "zstd", true},
                                           {CompressionFormat::Deflate, "deflate", true},
                                           {CompressionFormat::Zlib, "zlib", true},
                                           {CompressionFormat::Gzip, "gzip", true},
                                           {CompressionFormat::Lz4Frame, "lz4frame", true},
                                           {CompressionFormat::Brotli, "brotli", false}};
    for (const Builtin& builtin : Builtins) {
        auto info = TryGetCompressionFormatInfo(builtin.format);
        REQUIRE(info);
        CHECK(info.Value().Name() == StringView(builtin.name));
        CHECK(info.Value().revision == CompressionOutputRevision);
        CHECK(!info.Value().plugin);
        CHECK(info.Value().compresses == builtin.compresses);
        auto found = TryFindCompressionFormat(info.Value().Name());
        CHECK(found && found.Value() == builtin.format);
    }
    CHECK(TryGetCompressionFormatInfo(static_cast<CompressionFormat>(7)).Error() ==
          ErrorCode::InvalidArgument);
    // 발급될 수 없는 사용자 형식 값이다(다른 시험의 등록 이력과 무관하다).
    CHECK(TryGetCompressionFormatInfo(static_cast<CompressionFormat>(0xFFFFFFFF)).Error() ==
          ErrorCode::InvalidArgument);

    // 이름 문법 위반(대문자 포함)은 InvalidArgument, 문법에 맞지만 없는 이름은 NotFound다.
    CHECK(TryFindCompressionFormat(StringView("")).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFindCompressionFormat(StringView("Zstd")).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFindCompressionFormat(StringView("zstd.")).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFindCompressionFormat(StringView("zstd2")).Error() == ErrorCode::NotFound);
    CHECK(TryFindCompressionFormat(StringView("test.absent")).Error() == ErrorCode::NotFound);

    XorCodec codec;
    ScopedCodec registered("test.info", codec, {0, 3, 2}, 42);
    const CompressionFormat format = registered.Format();
    auto info = TryGetCompressionFormatInfo(format);
    REQUIRE(info);
    CHECK(info.Value().Name() == StringView("test.info"));
    CHECK(info.Value().revision == 42);
    CHECK(info.Value().plugin);
    CHECK(info.Value().compresses);
    auto found = TryFindCompressionFormat(StringView("test.info"));
    CHECK(found && found.Value() == format);
    auto range = TryGetCompressionLevelRange(format);
    CHECK(range && range.Value().minimum == 0 && range.Value().maximum == 3 &&
          range.Value().defaultLevel == 2);

    CHECK(registered.Unregister());
    CHECK(TryGetCompressionFormatInfo(format).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFindCompressionFormat(StringView("test.info")).Error() == ErrorCode::NotFound);
}

// 내장 형식과 같은 API로 왕복한다: 일회성 Span·Append, Compressor·Decompressor의 Span·Append
// (같은 byte), bound, 수준 범위 검증, 크기를 아는 해제의 경계.
TEST(CompressionCodec_RoundTripThroughPublicApi) {
    SystemAllocator allocator;
    XorCodec codec;
    ScopedCodec registered("test.xor", codec);
    const CompressionFormat format = registered.Format();
    Array<uint8> original(allocator);
    Fill(original, 70000, Pattern::Random, 5);

    CHECK(CompressionSettings::TryMake(format, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(CompressionSettings::TryMake(format, 10).Error() == ErrorCode::InvalidArgument);
    const CompressionSettings settings = Settings(format, 7);
    CHECK(DefaultSettings(format).Level() == 5);
    CHECK(TryGetCompressBound(format, 10).Value() == 11);
    CHECK(TryGetCompressBound(format, static_cast<usize>(-1)).Error() == ErrorCode::Overflow);

    Array<uint8> packed(allocator);
    static constexpr uint8 Base[] = {'k', 'e', 'e', 'p'};
    REQUIRE(packed.TryAppend(Span<const uint8>(Base, 4)));
    auto appended = TryCompressAppend(settings, View(original), packed);
    REQUIRE(appended);
    CHECK(appended.Value() == original.Size() + 1);
    CHECK(packed.Size() == 4 + original.Size() + 1 && packed[4] == 7);
    const Span<const uint8> frame(packed.Data() + 4, appended.Value());

    Array<uint8> spanOut(allocator);
    REQUIRE(spanOut.TryResize(original.Size() + 1));
    const Span<uint8> spanTarget(spanOut.Data(), spanOut.Size());
    auto spanWritten = TryCompress(settings, View(original), spanTarget, allocator);
    CHECK(spanWritten && Same(Span<const uint8>(spanOut.Data(), spanWritten.Value()), frame));
    CHECK(TryCompress(settings, View(original), Span<uint8>(spanOut.Data(), original.Size()),
                      allocator)
              .Error() == ErrorCode::CapacityExceeded);

    {
        auto compressor = Compressor::TryCreate(settings, allocator);
        REQUIRE(compressor);
        Array<uint8> again(allocator);
        CHECK(compressor.Value().TryCompressAppend(View(original), again));
        CHECK(Same(again, frame));
        auto viaSpan = compressor.Value().TryCompress(View(original), spanTarget);
        CHECK(viaSpan && Same(Span<const uint8>(spanOut.Data(), viaSpan.Value()), frame));
    }

    // 크기를 아는 해제: 정확한 크기는 성공, 1byte 모자라면 CapacityExceeded다.
    Array<uint8> restored(allocator);
    REQUIRE(restored.TryResize(original.Size()));
    const Span<uint8> exactTarget(restored.Data(), restored.Size());
    auto exact = TryDecompress(format, frame, exactTarget, allocator);
    CHECK(exact && exact.Value() == original.Size() && Same(restored, View(original)));
    CHECK(TryDecompress(format, frame, Span<uint8>(restored.Data(), original.Size() - 1), allocator)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(TryDecompress(format, Span<const uint8>(), Span<uint8>(restored.Data(), 1), allocator)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryDecompress(format, frame, Span<uint8>(const_cast<uint8*>(frame.Data()), 4), allocator)
              .Error() == ErrorCode::InvalidArgument);

    Array<uint8> unpacked(allocator);
    auto unpackedBytes = TryDecompressAppend(format, frame, original.Size(), unpacked);
    CHECK(unpackedBytes && Same(unpacked, View(original)));

    {
        auto decompressor = Decompressor::TryCreate(format, allocator);
        REQUIRE(decompressor);
        Array<uint8> viaContext(allocator);
        CHECK(decompressor.Value().TryDecompressAppend(frame, original.Size(), viaContext));
        CHECK(Same(viaContext, View(original)));
        auto viaSpan = decompressor.Value().TryDecompress(frame, exactTarget);
        CHECK(viaSpan && viaSpan.Value() == original.Size() && Same(restored, View(original)));
    }

    // 빈 입력 압축은 수준 byte 하나다.
    Array<uint8> empty(allocator);
    auto emptyBytes = TryCompressAppend(settings, Span<const uint8>(), empty);
    CHECK(emptyBytes && emptyBytes.Value() == 1);
}

// 크기를 모르는 해제의 두 배 재시도: 64KiB와 입력 × 4 중 큰 값(입력 × 4가 상한을 넘으면 상한)에서
// 시작해 두 배씩, 상한에서 멈춘다. 실패하면 output 길이를 되돌리고, 재시도 사이에 취소를 확인한다.
TEST(CompressionCodec_DecompressAppendDoublingBoundaries) {
    SystemAllocator allocator;
    RleCodec codec;
    ScopedCodec registered("test.rle", codec);
    const CompressionFormat format = registered.Format();
    static constexpr uint8 Base[] = {'b', 'a', 's', 'e'};

    struct Case {
        usize rawBytes;
        usize maxOutputBytes;
        uint32 calls;
        bool succeeds;
    };
    static constexpr usize KiB = 1024;
    static constexpr Case Cases[] = {
        {64 * KiB, 64 * KiB, 1, true},      // 첫 크기에 정확히 맞는다
        {64 * KiB + 1, 1 << 20, 2, true},   // 한 번 늘린다
        {1 << 20, 1 << 20, 5, true},        // 64→128→256→512KiB→1MiB(상한)
        {1 << 20, (1 << 20) - 1, 5, false}, // 상한까지 늘려도 모자라다
        {64 * KiB + 1, 64 * KiB, 1, false}, // 첫 크기가 이미 상한이다
        {100, 99, 1, false},                // 상한이 첫 크기보다 작다
    };
    for (const Case& c : Cases) {
        Array<uint8> original(allocator);
        Fill(original, c.rawBytes, Pattern::Zeros);
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));

        Array<uint8> output(allocator);
        REQUIRE(output.TryAppend(Span<const uint8>(Base, 4)));
        codec.decompressCalls = 0;
        auto result = TryDecompressAppend(format, View(packed), c.maxOutputBytes, output);
        CHECK(codec.decompressCalls == c.calls);
        if (c.succeeds) {
            CHECK(result && result.Value() == c.rawBytes);
            CHECK(output.Size() == 4 + c.rawBytes &&
                  Same(Span<const uint8>(output.Data() + 4, c.rawBytes), View(original)));
        } else {
            CHECK(result.Error() == ErrorCode::CapacityExceeded);
            CHECK(output.Size() == 4 && Same(output, Span<const uint8>(Base, 4)));
        }
    }

    // 압축이 잘 안 되는 입력: 입력 × 4가 상한 이하면 입력 × 4에서, 상한을 넘으면 상한에서 시작해
    // 한 번에 끝난다.
    Array<uint8> noisy(allocator);
    Fill(noisy, 100000, Pattern::Random, 3);
    Array<uint8> packed(allocator);
    REQUIRE(TryCompressAppend(DefaultSettings(format), View(noisy), packed));
    REQUIRE(packed.Size() > 125000);
    static constexpr usize NoisyLimits[] = {usize{1} << 20, 500000};
    for (const usize maxOutputBytes : NoisyLimits) {
        Array<uint8> output(allocator);
        codec.decompressCalls = 0;
        CHECK(TryDecompressAppend(format, View(packed), maxOutputBytes, output));
        CHECK(codec.decompressCalls == 1 && Same(output, View(noisy)));
    }

    // 손상(홀수 길이)은 재시도 없이 InvalidArgument다.
    Array<uint8> output(allocator);
    codec.decompressCalls = 0;
    CHECK(
        TryDecompressAppend(format, Span<const uint8>(packed.Data(), 3), 1 << 20, output).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(codec.decompressCalls == 1);

    // 재시도 사이 취소: 첫 호출(64KiB)이 모자란 뒤 두 번째 호출 전에 취소를 관측한다.
    Array<uint8> zeros(allocator);
    Fill(zeros, 1 << 20, Pattern::Zeros);
    Array<uint8> zeroPacked(allocator);
    REQUIRE(TryCompressAppend(DefaultSettings(format), View(zeros), zeroPacked));
    for (uint32 allowed = 0; allowed < 3; ++allowed) {
        Array<uint8> cancelled(allocator);
        REQUIRE(cancelled.TryAppend(Span<const uint8>(Base, 4)));
        CountdownProbe countdown{allowed};
        detail::CompressionProbe probe{&CountdownProbe::Check, &countdown};
        codec.decompressCalls = 0;
        auto result = detail::DecompressAppendOnce(format, View(zeroPacked), 1 << 20, cancelled,
                                                   allocator, probe);
        CHECK(result.Error() == ErrorCode::Cancelled);
        CHECK(codec.decompressCalls == allowed);
        CHECK(cancelled.Size() == 4 && Same(cancelled, Span<const uint8>(Base, 4)));
    }
    CHECK(TryDecompressAppend(format, View(zeroPacked), 1 << 20, output));
}

// 반환 계약을 어긴 코덱은 DependencyFailed다. Append는 실패하면 길이를 되돌린다.
TEST(CompressionCodec_ContractViolationsBecomeDependencyFailed) {
    SystemAllocator allocator;
    MisbehavingCodec codec;
    ScopedCodec registered("test.misbehaving", codec);
    const CompressionFormat format = registered.Format();
    const CompressionSettings settings = DefaultSettings(format);
    const Span<const uint8> source = Text("contract");
    uint8 fixed[64] = {};
    Array<uint8> output(allocator);
    static constexpr uint8 Base[] = {'b', 'a', 's', 'e'};
    REQUIRE(output.TryAppend(Span<const uint8>(Base, 4)));

    // 반환 크기가 destination보다 크다.
    codec.mode = MisbehavingCodec::Mode::OversizedResult;
    CHECK(TryCompress(settings, source, Span<uint8>(fixed, 64), allocator).Error() ==
          ErrorCode::DependencyFailed);
    CHECK(TryCompressAppend(settings, source, output).Error() == ErrorCode::DependencyFailed);
    CHECK(output.Size() == 4);
    CHECK(TryDecompress(format, source, Span<uint8>(fixed, 64), allocator).Error() ==
          ErrorCode::DependencyFailed);
    CHECK(TryDecompressAppend(format, source, 1 << 20, output).Error() ==
          ErrorCode::DependencyFailed);
    CHECK(output.Size() == 4 && Same(output, Span<const uint8>(Base, 4)));

    // bound 이상의 출력에서 CapacityExceeded는 위반이고, bound보다 작으면 그대로 전한다.
    codec.mode = MisbehavingCodec::Mode::CapacityAtBound;
    CHECK(TryCompress(settings, source, Span<uint8>(fixed, source.Size() + 8), allocator).Error() ==
          ErrorCode::DependencyFailed);
    CHECK(TryCompress(settings, source, Span<uint8>(fixed, source.Size() + 7), allocator).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(TryCompressAppend(settings, source, output).Error() == ErrorCode::DependencyFailed);
    CHECK(output.Size() == 4);

    // 허용 목록 밖의 오류. Unsupported는 해제에서만 허용이다. Cancelled도 위반이다(코덱은 취소를
    // 모른다).
    struct ErrorCase {
        ErrorCode reported;
        ErrorCode compress;
        ErrorCode decompress;
    };
    static constexpr ErrorCase Errors[] = {
        {ErrorCode::InvalidArgument, ErrorCode::InvalidArgument, ErrorCode::InvalidArgument},
        {ErrorCode::OutOfMemory, ErrorCode::OutOfMemory, ErrorCode::OutOfMemory},
        {ErrorCode::Overflow, ErrorCode::Overflow, ErrorCode::Overflow},
        {ErrorCode::DependencyFailed, ErrorCode::DependencyFailed, ErrorCode::DependencyFailed},
        {ErrorCode::Unsupported, ErrorCode::DependencyFailed, ErrorCode::Unsupported},
        {ErrorCode::Cancelled, ErrorCode::DependencyFailed, ErrorCode::DependencyFailed},
        {ErrorCode::NotFound, ErrorCode::DependencyFailed, ErrorCode::DependencyFailed},
        {ErrorCode::Busy, ErrorCode::DependencyFailed, ErrorCode::DependencyFailed},
    };
    for (const ErrorCase& e : Errors) {
        codec.error = e.reported;
        codec.mode = MisbehavingCodec::Mode::CompressError;
        CHECK(TryCompress(settings, source, Span<uint8>(fixed, 64), allocator).Error() ==
              e.compress);
        CHECK(TryCompressAppend(settings, source, output).Error() == e.compress);
        codec.mode = MisbehavingCodec::Mode::DecompressError;
        CHECK(TryDecompress(format, source, Span<uint8>(fixed, 64), allocator).Error() ==
              e.decompress);
        CHECK(TryDecompressAppend(format, source, 1 << 20, output).Error() == e.decompress);
        CHECK(output.Size() == 4);
    }

    // bound 오류는 InvalidArgument·Overflow만 허용이다.
    codec.mode = MisbehavingCodec::Mode::BoundError;
    codec.error = ErrorCode::Overflow;
    CHECK(TryGetCompressBound(format, 1).Error() == ErrorCode::Overflow);
    codec.error = ErrorCode::OutOfMemory;
    CHECK(TryGetCompressBound(format, 1).Error() == ErrorCode::DependencyFailed);
    CHECK(TryCompressAppend(settings, source, output).Error() == ErrorCode::DependencyFailed);
    CHECK(output.Size() == 4);
}

// 지원하지 않는 표면: 스트림·사전 Unsupported, 블록 컨테이너(영속 codec ID) InvalidArgument.
TEST(CompressionCodec_UnsupportedSurfacesRejectPluginFormats) {
    SystemAllocator allocator;
    XorCodec codec;
    ScopedCodec registered("test.surfaces", codec);
    const CompressionFormat format = registered.Format();
    const CompressionSettings settings = DefaultSettings(format);
    uint8 dictionary[64] = {};
    for (uint32 index = 0; index < 64; ++index) {
        dictionary[index] = static_cast<uint8>(index * 7);
    }
    const Span<const uint8> dictionaryBytes(dictionary, 64);

    CHECK(CompressStream::TryCreate(settings, allocator).Error() == ErrorCode::Unsupported);
    CHECK(DecompressStream::TryCreate(format, DecompressStreamLimits{1 << 20}, allocator).Error() ==
          ErrorCode::Unsupported);
    CHECK(CompressDictionary::TryCreate(settings, dictionaryBytes, allocator).Error() ==
          ErrorCode::Unsupported);
    CHECK(DecompressDictionary::TryCreate(format, dictionaryBytes, allocator).Error() ==
          ErrorCode::Unsupported);
    CHECK(CompressedBlocksSettings::TryMake(settings).Error() == ErrorCode::InvalidArgument);
    CHECK(CompressedBlocksReadContext::TryCreate(format, 16, allocator).Error() ==
          ErrorCode::InvalidArgument);

    // 해제한 값은 모르는 형식 값과 같다(이미 만든 설정도 마찬가지다).
    CHECK(registered.Unregister());
    CHECK(CompressStream::TryCreate(settings, allocator).Error() == ErrorCode::InvalidArgument);
    CHECK(DecompressStream::TryCreate(format, DecompressStreamLimits{1 << 20}, allocator).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CompressDictionary::TryCreate(settings, dictionaryBytes, allocator).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DecompressDictionary::TryCreate(format, dictionaryBytes, allocator).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사용권: 살아 있는 문맥이 있으면 해제는 Busy이고 새 사용은 막히지만 기존 문맥은 계속
// 동작한다. 문맥이 사라진 뒤 다시 부르면 성공한다.
TEST(CompressionCodec_UnregisterIsBusyWhileContextsLive) {
    SystemAllocator allocator;
    XorCodec codec;
    ScopedCodec registered("test.lease", codec);
    const CompressionFormat format = registered.Format();
    const CompressionSettings settings = DefaultSettings(format);
    const Span<const uint8> source = Text("lease held by contexts");
    {
        Array<uint8> frame(allocator);
        REQUIRE(TryCompressAppend(settings, source, frame));
        auto compressor = Compressor::TryCreate(settings, allocator);
        auto decompressor = Decompressor::TryCreate(format, allocator);
        REQUIRE(compressor && decompressor);

        CHECK(TryUnregisterCompressionCodec(format).Error() == ErrorCode::Busy);
        // 해제가 시작된 뒤의 새 사용은 모두 모르는 값처럼 거부된다(이미 만든 설정 포함).
        uint8 fixed[64] = {};
        Array<uint8> output(allocator);
        CHECK(CompressionSettings::TryMake(format, 5).Error() == ErrorCode::InvalidArgument);
        CHECK(CompressionSettings::TryDefault(format).Error() == ErrorCode::InvalidArgument);
        CHECK(TryCompressAppend(settings, source, output).Error() == ErrorCode::InvalidArgument);
        CHECK(TryCompress(settings, source, Span<uint8>(fixed, 64), allocator).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TryDecompress(format, View(frame), Span<uint8>(fixed, 64), allocator).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TryDecompressAppend(format, View(frame), 1024, output).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(output.Size() == 0);
        CHECK(TryGetCompressBound(format, 1).Error() == ErrorCode::InvalidArgument);
        CHECK(TryGetCompressionLevelRange(format).Error() == ErrorCode::InvalidArgument);
        CHECK(Compressor::TryCreate(settings, allocator).Error() == ErrorCode::InvalidArgument);
        CHECK(Decompressor::TryCreate(format, allocator).Error() == ErrorCode::InvalidArgument);
        CHECK(TryGetCompressionFormatInfo(format).Error() == ErrorCode::InvalidArgument);
        CHECK(TryFindCompressionFormat(StringView("test.lease")).Error() == ErrorCode::NotFound);
        // 해제 중인 이름은 아직 칸을 차지한다.
        CHECK(
            TryRegisterCompressionCodec({StringView("test.lease"), 1, TestLevels}, codec).Error() ==
            ErrorCode::AlreadyExists);

        // 기존 문맥은 끝까지 쓸 수 있다.
        Array<uint8> packed(allocator);
        CHECK(compressor.Value().TryCompressAppend(source, packed));
        Array<uint8> restored(allocator);
        CHECK(decompressor.Value().TryDecompressAppend(View(packed), 1024, restored));
        CHECK(Same(restored, source));

        // 압축 문맥을 놓아도 해제 문맥이 남아 있으면 아직 Busy다.
        compressor.Value() = Compressor();
        CHECK(TryUnregisterCompressionCodec(format).Error() == ErrorCode::Busy);
    }
    CHECK(registered.Unregister());
    CHECK(TryUnregisterCompressionCodec(format).Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실행 중인 코덱 호출도 사용권을 쥔다: 비동기 작업과 다른 스레드의 일회성 호출이 코덱 안에 있는
// 동안 해제는 Busy, 새 제출은 InvalidArgument이고, 호출이 끝나면 해제가 성공한다.
TEST(CompressionCodec_UnregisterIsBusyWhileCodecRuns) {
    SystemAllocator allocator;
    BlockingCodec codec;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            const Span<const uint8> source = Text("running codec holds the lease");
            Array<uint8> packed(allocator);
            Array<uint8> rejected(allocator);
            CompressionTask running;
            CompressionTask refused;
            {
                ScopedCodec registered("test.blocking", codec);
                const CompressionSettings settings = DefaultSettings(registered.Format());
                REQUIRE(TrySubmitCompress(scheduler, running, settings, source, packed));
                CHECK(codec.entered.Wait(platform::InfiniteTimeout) ==
                      platform::WaitResult::Signaled);
                CHECK(TryUnregisterCompressionCodec(registered.Format()).Error() ==
                      ErrorCode::Busy);
                CHECK(TrySubmitCompress(scheduler, refused, settings, source, rejected).Error() ==
                      ErrorCode::InvalidArgument);
                codec.release.Signal();
                auto result = running.TryWaitResult();
                CHECK(result && Same(packed, source));
                CHECK(registered.Unregister());
            }

            // 다른 스레드(worker body)의 일회성 호출이 코덱 안에 있는 동안.
            codec.entered.Reset();
            codec.release.Reset();
            ScopedCodec registered("test.blocking", codec);
            const CompressionFormat format = registered.Format();
            ErrorCode oneShot = ErrorCode::Busy;
            auto body = Body::TryCreate(
                allocator, [format, source, &oneShot, &allocator](WorkerTaskContext&) noexcept {
                    Array<uint8> output(allocator);
                    auto written = TryCompressAppend(DefaultSettings(format), source, output);
                    oneShot = written && Same(output, source) ? ErrorCode::None
                              : written                       ? ErrorCode::DependencyFailed
                                                              : written.Error();
                });
            REQUIRE(body);
            auto handle = scheduler.Submit(Move(body.Value()));
            REQUIRE(handle);
            CHECK(codec.entered.Wait(platform::InfiniteTimeout) == platform::WaitResult::Signaled);
            CHECK(TryUnregisterCompressionCodec(format).Error() == ErrorCode::Busy);
            codec.release.Signal();
            CHECK(handle.Value().Wait(platform::InfiniteTimeout).Value() ==
                  platform::WaitResult::Signaled);
            CHECK(oneShot == ErrorCode::None);
            CHECK(registered.Unregister());
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 비동기: 대기 중인 작업이 사용권을 쥐어 해제는 Busy, 새 제출은 InvalidArgument다. 대기 중이던
// 작업은 끝까지 실행되고(동기와 같은 byte, Append·Span 해제), TryWaitResult 직후의 해제는 성공한다.
TEST(CompressionCodec_AsyncJobsHoldLeaseUntilTerminal) {
    SystemAllocator allocator;
    XorCodec codec;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            Array<uint8> original(allocator);
            Fill(original, 300000, Pattern::Text, 4);
            Array<uint8> packed(allocator);
            Array<uint8> restored(allocator);
            Array<uint8> fixed(allocator);
            REQUIRE(fixed.TryResize(original.Size()));
            Array<uint8> rejectedOutput(allocator);
            platform::ManualResetEvent release;

            CompressionTask compress;
            CompressionTask decompress;
            CompressionTask decompressSpan;
            CompressionTask rejected;

            ScopedCodec registered("test.async", codec);
            const CompressionFormat format = registered.Format();
            const CompressionSettings settings = DefaultSettings(format);
            Array<uint8> expected(allocator);
            REQUIRE(TryCompressAppend(settings, View(original), expected));

            auto blockerBody = Body::TryCreate(allocator, [&release](WorkerTaskContext&) noexcept {
                (void)release.Wait(platform::InfiniteTimeout);
            });
            REQUIRE(blockerBody);
            auto blocker = scheduler.Submit(Move(blockerBody.Value()));
            REQUIRE(blocker);
            const auto dependency = Span<const WorkerTaskHandle>(&blocker.Value(), 1);

            REQUIRE(TrySubmitCompress(scheduler, compress, settings, View(original), packed,
                                      dependency));
            REQUIRE(TrySubmitDecompress(scheduler, decompress, format, View(expected),
                                        original.Size(), restored, dependency));
            REQUIRE(TrySubmitDecompress(scheduler, decompressSpan, format, View(expected),
                                        Span<uint8>(fixed.Data(), fixed.Size()), allocator,
                                        dependency));
            CHECK(TryUnregisterCompressionCodec(format).Error() == ErrorCode::Busy);
            CHECK(TrySubmitCompress(scheduler, rejected, settings, View(original), rejectedOutput)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(TrySubmitDecompress(scheduler, rejected, format, View(expected), original.Size(),
                                      rejectedOutput)
                      .Error() == ErrorCode::InvalidArgument);

            release.Signal();
            auto compressed = compress.TryWaitResult();
            auto decompressed = decompress.TryWaitResult();
            auto decompressedSpan = decompressSpan.TryWaitResult();
            CHECK(compressed && compressed.Value() == expected.Size());
            CHECK(Same(packed, View(expected)));
            CHECK(decompressed && Same(restored, View(original)));
            CHECK(decompressedSpan && decompressedSpan.Value() == original.Size() &&
                  Same(fixed, View(original)));
            CHECK(registered.Unregister());
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실행되지 않은 비동기 작업도 사용권을 놓는다: body 할당 실패(제출 실패), 실행 전 취소, 의존 작업
// 실패. 각 경우 TryWaitResult(또는 제출 실패) 직후 해제가 성공한다.
TEST(CompressionCodec_AsyncLeaseReleasedWithoutRunning) {
    SystemAllocator allocator;
    XorCodec codec;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 2, .maxInFlight = 16}));
        {
            const Span<const uint8> source = Text("never runs");

            // body 할당 실패: output 할당기(body 할당에 쓴다)의 첫 할당을 실패시킨다.
            {
                ScopedCodec registered("test.unrun", codec);
                NFailAllocator failing(allocator);
                failing.FailOnAttempt(1);
                Array<uint8> output(failing);
                CompressionTask task;
                CHECK(TrySubmitCompress(scheduler, task, DefaultSettings(registered.Format()),
                                        source, output)
                          .Error() == ErrorCode::OutOfMemory);
                CHECK(registered.Unregister());
            }

            // 실행 전 취소와 의존 작업 실패.
            static constexpr bool CancelFirst[] = {true, false};
            for (const bool cancel : CancelFirst) {
                ScopedCodec registered("test.unrun", codec);
                platform::ManualResetEvent release;
                Array<uint8> output(allocator);
                CompressionTask task;
                auto blockerBody = Body::TryCreate(
                    allocator, [&release, cancel](WorkerTaskContext& context) noexcept {
                        (void)release.Wait(platform::InfiniteTimeout);
                        if (!cancel) {
                            context.Fail(ErrorCode::InvalidArgument);
                        }
                    });
                REQUIRE(blockerBody);
                auto blocker = scheduler.Submit(Move(blockerBody.Value()));
                REQUIRE(blocker);
                REQUIRE(TrySubmitCompress(scheduler, task, DefaultSettings(registered.Format()),
                                          source, output,
                                          Span<const WorkerTaskHandle>(&blocker.Value(), 1)));
                CHECK(TryUnregisterCompressionCodec(registered.Format()).Error() ==
                      ErrorCode::Busy);
                if (cancel) {
                    CHECK(task.Handle().RequestCancel());
                }
                release.Signal();
                auto result = task.TryWaitResult();
                CHECK(result.Error() ==
                      (cancel ? ErrorCode::Cancelled : ErrorCode::DependencyFailed));
                CHECK(output.Size() == 0);
                CHECK(registered.Unregister());
            }
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 코덱 안에서 이 모듈을 다시 부른다: 내장 형식(Zstd) 왕복, 다른 사용자 등록 형식 압축, 자기 형식
// 해제 시도(Busy). 결과는 시험이 읽는다(단일 스레드).
class ReentrantCodec final : public CompressionCodec {
public:
    CompressionFormat self = CompressionFormat::Lz4;
    CompressionFormat other = CompressionFormat::Lz4;
    mutable bool zstdRoundTrip = false;
    mutable bool otherCompressed = false;
    mutable ErrorCode selfUnregister = ErrorCode::None;

    Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept override {
        return Result<usize>::Success(sourceBytes);
    }

    Result<usize> TryCompress(int32, Span<const uint8> source, Span<uint8> destination,
                              Allocator& scratch) const noexcept override {
        {
            Array<uint8> packed(scratch);
            Array<uint8> restored(scratch);
            zstdRoundTrip =
                TryCompressAppend(DefaultSettings(CompressionFormat::Zstd), source, packed) &&
                TryDecompressAppend(CompressionFormat::Zstd, View(packed), source.Size(),
                                    restored) &&
                Same(restored, source);
            Array<uint8> viaOther(scratch);
            otherCompressed = TryCompressAppend(DefaultSettings(other), source, viaOther) &&
                              viaOther.Size() == source.Size() + 1;
        }
        auto unregistered = TryUnregisterCompressionCodec(self);
        selfUnregister = unregistered ? ErrorCode::None : unregistered.Error();

        if (destination.Size() < source.Size()) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }
        for (usize index = 0; index < source.Size(); ++index) {
            destination[index] = source[index];
        }
        return Result<usize>::Success(source.Size());
    }

    Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination,
                                Allocator&) const noexcept override {
        if (destination.Size() < source.Size()) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }
        for (usize index = 0; index < source.Size(); ++index) {
            destination[index] = source[index];
        }
        return Result<usize>::Success(source.Size());
    }
};

} // namespace

// 재진입: 코덱 호출 중에는 표 잠금을 쥐지 않으므로 코덱이 이 모듈을 다시 불러도 된다. 자기 형식
// 해제는 Busy이며 그 순간부터 새 사용이 막힌다.
TEST(CompressionCodec_CodecsMayReenterTheModule) {
    SystemAllocator allocator;
    XorCodec xorCodec;
    ReentrantCodec reentrant;
    ScopedCodec other("test.reentrant-other", xorCodec);
    ScopedCodec self("test.reentrant", reentrant);
    reentrant.self = self.Format();
    reentrant.other = other.Format();

    Array<uint8> original(allocator);
    Fill(original, 5000, Pattern::Text, 8);
    Array<uint8> packed(allocator);
    auto written = TryCompressAppend(DefaultSettings(self.Format()), View(original), packed);
    CHECK(written && Same(packed, View(original)));
    CHECK(reentrant.zstdRoundTrip);
    CHECK(reentrant.otherCompressed);
    CHECK(reentrant.selfUnregister == ErrorCode::Busy);
    CHECK(TryGetCompressionFormatInfo(self.Format()).Error() == ErrorCode::InvalidArgument);
}

// 할당 실패: 문맥 상태·코덱 scratch·output 성장의 OOM은 OutOfMemory이고 길이가 복원되며 사용권이
// 새지 않는다(마지막 해제 성공).
TEST(CompressionCodec_OutOfMemoryReleasesLeases) {
    SystemAllocator system;
    XorCodec codec;
    ScopedCodec registered("test.oom", codec);
    const CompressionFormat format = registered.Format();
    const CompressionSettings settings = DefaultSettings(format);
    {
        const Span<const uint8> source = Text("allocation failure paths");
        Array<uint8> packed(system);
        REQUIRE(TryCompressAppend(settings, source, packed));

        bool succeeded = false;
        for (uint64 attempt = 1; attempt < 16 && !succeeded; ++attempt) {
            NFailAllocator failing(system);
            failing.FailOnAttempt(attempt);
            auto compressor = Compressor::TryCreate(settings, failing);
            auto decompressor = Decompressor::TryCreate(format, failing);
            Array<uint8> output(failing);
            static constexpr uint8 Base[] = {'b'};
            const bool based = static_cast<bool>(output.TryAppend(Span<const uint8>(Base, 1)));
            const usize beforeCompress = output.Size();
            auto compressed = TryCompressAppend(settings, source, output, failing);
            CHECK(compressed || output.Size() == beforeCompress);
            const usize beforeRestore = output.Size();
            auto restored = TryDecompressAppend(format, View(packed), 1024, output, failing);
            CHECK(restored || output.Size() == beforeRestore);
            const ErrorCode errors[] = {compressor ? ErrorCode::None : compressor.Error(),
                                        decompressor ? ErrorCode::None : decompressor.Error(),
                                        compressed ? ErrorCode::None : compressed.Error(),
                                        restored ? ErrorCode::None : restored.Error()};
            for (ErrorCode error : errors) {
                CHECK(error == ErrorCode::None || error == ErrorCode::OutOfMemory);
            }
            succeeded = based && compressor && decompressor && compressed && restored;
        }
        CHECK(succeeded);
    }
    CHECK(registered.Unregister());
    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// 동시성 시험의 worker 결과다. worker마다 자기 칸에만 쓴다.
struct FlipCounters {
    ErrorCode failure = ErrorCode::None;
    uint32 successes = 0;
    uint32 rejections = 0;
};

} // namespace

// 동시성: worker 4개가 일회성 왕복을 반복하는 동안 주 스레드가 다른 형식의 등록·해제를 되풀이한다.
// 코덱은 라운드마다 heap에 만들고 해제가 성공하면 파괴하므로, 사용권이 틀리면 ASan이 해제 뒤
// 사용을 잡는다. 해제 중인 형식의 사용은 InvalidArgument·NotFound로만 실패하고, 성공한 결과는 항상
// 왕복하며, 성공과 거부가 모두 일어나야 한다(실제로 겹쳤는지 확인).
TEST(CompressionCodec_ConcurrentUseAndRegistration) {
    SystemAllocator allocator;
    XorCodec stable;
    ScopedCodec stableRegistration("test.stable", stable);
    const CompressionFormat format = stableRegistration.Format();
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 64}));
        {
            constexpr uint32 Workers = 4;
            Array<uint8> original(allocator);
            Fill(original, 4096, Pattern::Text, 6);
            FlipCounters counters[Workers] = {};
            uint32 stop = 0;
            uint32 started = 0;
            uint32 flipSuccesses = 0;
            WorkerTaskHandle handles[Workers];
            for (uint32 worker = 0; worker < Workers; ++worker) {
                auto body = Body::TryCreate(allocator, [&original, &counters, &allocator, &stop,
                                                        &started, &flipSuccesses, worker,
                                                        format](WorkerTaskContext&) noexcept {
                    FlipCounters& mine = counters[worker];
                    platform::AtomicFetchAdd(&started, 1);
                    while (platform::AtomicLoad(&stop) == 0) {
                        Array<uint8> packed(allocator);
                        Array<uint8> restored(allocator);
                        if (!TryCompressAppend(DefaultSettings(format), View(original), packed) ||
                            !TryDecompressAppend(format, View(packed), original.Size(), restored) ||
                            !Same(restored, View(original))) {
                            mine.failure = ErrorCode::DependencyFailed;
                            return;
                        }

                        auto flipped = TryFindCompressionFormat(StringView("test.flipping"));
                        if (!flipped) {
                            ++mine.rejections;
                            continue;
                        }
                        auto settings = CompressionSettings::TryMake(flipped.Value(), 3);
                        Array<uint8> other(allocator);
                        auto written =
                            settings ? TryCompressAppend(settings.Value(), View(original), other)
                                     : Result<usize>::Failure(settings.Error());
                        Array<uint8> back(allocator);
                        auto read = written ? TryDecompressAppend(flipped.Value(), View(other),
                                                                  original.Size(), back)
                                            : Result<usize>::Failure(written.Error());
                        if (read) {
                            if (!Same(back, View(original))) {
                                mine.failure = ErrorCode::DependencyFailed;
                                return;
                            }
                            ++mine.successes;
                            platform::AtomicFetchAdd(&flipSuccesses, 1);
                        } else if (read.Error() == ErrorCode::InvalidArgument) {
                            ++mine.rejections;
                        } else {
                            mine.failure = read.Error();
                            return;
                        }
                    }
                });
                REQUIRE(body);
                auto submitted = scheduler.Submit(Move(body.Value()));
                REQUIRE(submitted);
                handles[worker] = Move(submitted.Value());
            }
            while (platform::AtomicLoad(&started) != Workers) {
                platform::YieldThread();
            }

            // 라운드마다 코덱을 heap에 만들고, 등록한 뒤 worker 하나가 성공할 때까지(또는 잠시)
            // 기다렸다가 해제한다. Busy면 다시 부르고, 성공하면 코덱을 파괴한다.
            uint32 rounds = 0;
            bool busyBounded = true;
            for (; rounds < 200 && busyBounded; ++rounds) {
                auto memory = allocator.Allocate(sizeof(XorCodec), alignof(XorCodec));
                REQUIRE(memory);
                XorCodec* flipping = ConstructAt<XorCodec>(memory.Value());
                auto registered = TryRegisterCompressionCodec(
                    {StringView("test.flipping"), 1, TestLevels}, *flipping);
                CHECK(registered);
                const uint32 before = platform::AtomicLoad(&flipSuccesses);
                for (uint32 spin = 0; spin < 2000 && platform::AtomicLoad(&flipSuccesses) == before;
                     ++spin) {
                    platform::YieldThread();
                }
                uint64 busy = 0;
                while (registered) {
                    auto removed = TryUnregisterCompressionCodec(registered.Value());
                    if (removed) {
                        break;
                    }
                    CHECK(removed.Error() == ErrorCode::Busy);
                    if (++busy > 10000000) {
                        busyBounded = false;
                        break;
                    }
                    platform::YieldThread();
                }
                if (busyBounded) {
                    DestroyAt(flipping);
                    CHECK(allocator.Deallocate(memory.Value()));
                }
            }
            CHECK(busyBounded);
            platform::AtomicStore(&stop, 1);

            for (WorkerTaskHandle& handle : handles) {
                CHECK(handle.Wait(platform::InfiniteTimeout).Value() ==
                      platform::WaitResult::Signaled);
            }
            uint32 successes = 0;
            uint32 rejections = 0;
            for (const FlipCounters& counter : counters) {
                CHECK(counter.failure == ErrorCode::None);
                successes += counter.successes;
                rejections += counter.rejections;
            }
            CHECK(rounds == 200);
            CHECK(successes > 0);
            CHECK(rejections > 0);
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(TryFindCompressionFormat(StringView("test.flipping")).Error() == ErrorCode::NotFound);
}

} // namespace rocks::test
