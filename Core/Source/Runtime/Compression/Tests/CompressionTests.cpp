#include "CompressionTestSupport.h"

#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <stdio.h>

namespace rocks::test {
namespace {

using namespace compression;

template <usize Count>
Span<const uint8> Bytes(const uint8 (&values)[Count]) {
    return Span<const uint8>(values, Count);
}

Span<const uint8> Text(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

// 해제 결과가 기대값과 같은지 Span·Append 두 경로로 확인한다.
bool DecodesTo(CompressionFormat format, Span<const uint8> encoded, Span<const uint8> expected) {
    SystemAllocator allocator;
    bool ok = true;
    {
        Array<uint8> output(allocator);
        auto appended = TryDecompressAppend(format, encoded, expected.Size(), output);
        ok = ok && appended && appended.Value() == expected.Size() && Same(output, expected);
        Array<uint8> exact(allocator);
        ok = ok && exact.TryResize(expected.Size() + 7);
        auto written =
            TryDecompress(format, encoded, Span<uint8>(exact.Data(), exact.Size()), allocator);
        ok = ok && written && written.Value() == expected.Size() &&
             Same(Span<const uint8>(exact.Data(), written.Value()), expected);
    }
    return ok && allocator.Stats().liveBytes == 0;
}

ErrorCode DecodeError(CompressionFormat format, Span<const uint8> encoded,
                      usize maxOutputBytes = 1 << 20) {
    SystemAllocator allocator;
    Array<uint8> output(allocator);
    auto appended = TryDecompressAppend(format, encoded, maxOutputBytes, output);
    return appended ? ErrorCode::None : appended.Error();
}

} // namespace

TEST(Compression_LevelRangesAndSettings) {
    CHECK(TryGetCompressionLevelRange(CompressionFormat::Lz4).Value().maximum == 12);
    CHECK(TryGetCompressionLevelRange(CompressionFormat::Zstd).Value().defaultLevel == 3);
    CHECK(TryGetCompressionLevelRange(CompressionFormat::Gzip).Value().minimum == 0);
    CHECK(!TryGetCompressionLevelRange(static_cast<CompressionFormat>(99)));
    for (CompressionFormat format : AllFormats) {
        const auto range = TryGetCompressionLevelRange(format).Value();
        CHECK(CompressionSettings::TryMake(format, range.minimum));
        CHECK(CompressionSettings::TryMake(format, range.maximum));
        CHECK(CompressionSettings::TryMake(format, range.minimum - 1).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(CompressionSettings::TryMake(format, range.maximum + 1).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(CompressionSettings::TryDefault(format).Value().Level() == range.defaultLevel);
    }
    CHECK(!CompressionSettings::TryDefault(static_cast<CompressionFormat>(99)));
    CHECK(!TryGetCompressBound(static_cast<CompressionFormat>(99), 1));
}

// 다른 구현이나 형식 명세로 직접 만든 입력이다(이 모듈의 압축기가 만든 것이 아니다).
TEST(Compression_IndependentGoldenVectors) {
    // RFC 1950: Python zlib.compress(b"abc"). Adler-32 0x024D0127.
    static constexpr uint8 ZlibAbc[] = {0x78, 0x9c, 0x4b, 0x4c, 0x4a, 0x06,
                                        0x00, 0x02, 0x4d, 0x01, 0x27};
    CHECK(DecodesTo(CompressionFormat::Zlib, Bytes(ZlibAbc), Text("abc")));
    // RFC 1951 고정 Huffman block(위 zlib 스트림의 본문)과 stored block(BFINAL=1, BTYPE=00, LEN=5,
    // NLEN).
    static constexpr uint8 DeflateAbc[] = {0x4b, 0x4c, 0x4a, 0x06, 0x00};
    CHECK(DecodesTo(CompressionFormat::Deflate, Bytes(DeflateAbc), Text("abc")));
    static constexpr uint8 DeflateStored[] = {0x01, 0x05, 0x00, 0xfa, 0xff,
                                              'h',  'e',  'l',  'l',  'o'};
    CHECK(DecodesTo(CompressionFormat::Deflate, Bytes(DeflateStored), Text("hello")));

    // GNU gzip 1.14: `gzip -9 -n`(이름 없음), `gzip -9`(FNAME "hello.txt"), 두 member를 이어 붙인
    // 파일.
    static constexpr uint8 GzipPlain[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x02, 0x03, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57,
                                          0x28, 0xca, 0x4f, 0xce, 0x2e, 0xe6, 0x02, 0x00,
                                          0x77, 0x08, 0xac, 0x17, 0x0c, 0x00, 0x00, 0x00};
    static constexpr uint8 GzipNamed[] = {
        0x1f, 0x8b, 0x08, 0x08, 0x83, 0x6b, 0xf5, 0x6a, 0x02, 0x03, 0x68, 0x65, 0x6c, 0x6c,
        0x6f, 0x2e, 0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0xca,
        0x4f, 0xce, 0x2e, 0xe6, 0x02, 0x00, 0x77, 0x08, 0xac, 0x17, 0x0c, 0x00, 0x00, 0x00};
    static constexpr uint8 GzipTwoMembers[] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xcb, 0x48, 0xcd, 0xc9,
        0xc9, 0x57, 0x28, 0xca, 0x4f, 0xce, 0x2e, 0xe6, 0x02, 0x00, 0x77, 0x08, 0xac, 0x17,
        0x0c, 0x00, 0x00, 0x00, 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x03,
        0x2b, 0x4e, 0x4d, 0xce, 0xcf, 0x4b, 0x51, 0xc8, 0x4d, 0xcd, 0x4d, 0x4a, 0x2d, 0xe2,
        0x02, 0x00, 0x36, 0x18, 0x4b, 0x0e, 0x0e, 0x00, 0x00, 0x00};
    CHECK(DecodesTo(CompressionFormat::Gzip, Bytes(GzipPlain), Text("hello rocks\n")));
    CHECK(DecodesTo(CompressionFormat::Gzip, Bytes(GzipNamed), Text("hello rocks\n")));
    CHECK(DecodesTo(CompressionFormat::Gzip, Bytes(GzipTwoMembers),
                    Text("hello rocks\nsecond member\n")));

    // LZ4 block 명세: literal만 있는 sequence, 그리고 offset 3·길이 12 match 뒤 마지막 literal 5개.
    static constexpr uint8 Lz4Literals[] = {0x50, 'h', 'e', 'l', 'l', 'o'};
    CHECK(DecodesTo(CompressionFormat::Lz4, Bytes(Lz4Literals), Text("hello")));
    static constexpr uint8 Lz4Match[] = {0x38, 'a', 'b', 'c', 0x03, 0x00,
                                         0x50, 'h', 'e', 'l', 'l',  'o'};
    CHECK(DecodesTo(CompressionFormat::Lz4, Bytes(Lz4Match), Text("abcabcabcabcabchello")));

    // RFC 8878: single-segment frame(FCS 1byte)의 raw block, RLE block, 앞에 붙은 skippable frame.
    static constexpr uint8 ZstdRaw[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x05, 0x29,
                                        0x00, 0x00, 'h',  'e',  'l',  'l',  'o'};
    CHECK(DecodesTo(CompressionFormat::Zstd, Bytes(ZstdRaw), Text("hello")));
    static constexpr uint8 ZstdRle[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x0a, 0x53, 0x00, 0x00, 'z'};
    CHECK(DecodesTo(CompressionFormat::Zstd, Bytes(ZstdRle), Text("zzzzzzzzzz")));
    static constexpr uint8 ZstdSkippableThenRaw[] = {
        0x50, 0x2a, 0x4d, 0x18, 0x03, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0x28, 0xb5,
        0x2f, 0xfd, 0x20, 0x05, 0x29, 0x00, 0x00, 'h',  'e',  'l',  'l',  'o'};
    CHECK(DecodesTo(CompressionFormat::Zstd, Bytes(ZstdSkippableThenRaw), Text("hello")));
    static constexpr uint8 ZstdSkippableOnly[] = {0x50, 0x2a, 0x4d, 0x18, 0x01,
                                                  0x00, 0x00, 0x00, 0x7f};
    CHECK(DecodesTo(CompressionFormat::Zstd, Bytes(ZstdSkippableOnly), Span<const uint8>()));
}

TEST(Compression_RoundTripAllFormatsLevelsAndSizes) {
    SystemAllocator allocator;
    {
        struct Case {
            usize size;
            Pattern pattern;
        };
        // 1MiB(분할 단위)를 넘는 입력으로 zlib·zstd의 분할 루프를 실행한다.
        static constexpr Case Cases[] = {{0, Pattern::Zeros},
                                         {1, Pattern::Random},
                                         {65535, Pattern::Text},
                                         {65536, Pattern::Random},
                                         {65537, Pattern::Text},
                                         {131073, Pattern::Text},
                                         {262144, Pattern::Random},
                                         {(1u << 20) + 17, Pattern::Zeros},
                                         {(1u << 20) * 2 + 3, Pattern::Text}};
        for (CompressionFormat format : AllFormats) {
            const auto range = TryGetCompressionLevelRange(format).Value();
            const int32 levels[] = {range.minimum, range.defaultLevel, range.maximum};
            for (const Case& item : Cases) {
                for (int32 level : levels) {
                    // 최대 수준은 느리므로 작은 입력에만 쓴다.
                    if (level == range.maximum && level != range.defaultLevel &&
                        item.size > 262144) {
                        continue;
                    }
                    Array<uint8> original(allocator);
                    Fill(original, item.size, item.pattern);
                    const auto settings = Settings(format, level);
                    const usize bound = TryGetCompressBound(format, item.size).Value();

                    Array<uint8> packed(allocator);
                    REQUIRE(packed.TryResize(bound));
                    auto packedBytes = TryCompress(settings, View(original),
                                                   Span<uint8>(packed.Data(), bound), allocator);
                    REQUIRE(packedBytes);
                    CHECK(packedBytes.Value() <= bound);
                    REQUIRE(packed.TryResize(packedBytes.Value()));

                    Array<uint8> appended(allocator);
                    REQUIRE(appended.TryAppend(Text("prefix")));
                    auto appendedBytes = TryCompressAppend(settings, View(original), appended);
                    REQUIRE(appendedBytes);
                    CHECK(appendedBytes.Value() == packedBytes.Value());
                    CHECK(Same(Span<const uint8>(appended.Data() + 6, appended.Size() - 6),
                               View(packed)));

                    // 정확히 원본 크기의 출력·상한은 성공한다.
                    Array<uint8> restored(allocator);
                    REQUIRE(restored.TryResize(item.size));
                    auto written =
                        TryDecompress(format, View(packed),
                                      Span<uint8>(restored.Data(), restored.Size()), allocator);
                    CHECK(written && written.Value() == item.size &&
                          Same(restored, View(original)));
                    Array<uint8> grown(allocator);
                    REQUIRE(grown.TryAppend(Text("xy")));
                    auto grownBytes = TryDecompressAppend(format, View(packed), item.size, grown);
                    CHECK(grownBytes && grownBytes.Value() == item.size);
                    CHECK(Same(Span<const uint8>(grown.Data() + 2, grown.Size() - 2),
                               View(original)));

                    // 1byte 부족하면 용량 부족이며 Append는 길이를 되돌린다.
                    if (item.size != 0) {
                        CHECK(TryDecompress(format, View(packed),
                                            Span<uint8>(restored.Data(), item.size - 1), allocator)
                                  .Error() == ErrorCode::CapacityExceeded);
                        Array<uint8> limited(allocator);
                        REQUIRE(limited.TryAppend(Text("keep")));
                        CHECK(TryDecompressAppend(format, View(packed), item.size - 1, limited)
                                  .Error() == ErrorCode::CapacityExceeded);
                        CHECK(limited.Size() == 4);
                    }
                    // 압축 출력도 1byte 부족하면 용량 부족이다.
                    if (packedBytes.Value() != 0) {
                        Array<uint8> small(allocator);
                        REQUIRE(small.TryResize(packedBytes.Value() - 1));
                        CHECK(TryCompress(settings, View(original),
                                          Span<uint8>(small.Data(), small.Size()), allocator)
                                  .Error() == ErrorCode::CapacityExceeded);
                    }
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 재사용 문맥은 입력 크기·이전 실패와 무관하게 새 문맥(자유 함수)과 같은 byte를 낸다. 크기를 번갈아
// 써서 LZ4 table 재사용(4KiB 미만 입력), zstd 세션 재설정, gzip 헤더 재적용, zlib 분할 루프를 함께
// 거친다.
TEST(Compression_ReusedContextsMatchFreeFunctionsAfterFailures) {
    SystemAllocator allocator;
    {
        static constexpr usize Sizes[] = {100, 3000, 300000, (usize{2} << 20) + 5, 3000, 100};
        struct LevelSet {
            CompressionFormat format;
            int32 levels[3];
        };
        static constexpr LevelSet Levels[] = {{CompressionFormat::Lz4, {1, 2, 12}},
                                              {CompressionFormat::Zstd, {1, 3, 19}},
                                              {CompressionFormat::Deflate, {0, 6, 9}},
                                              {CompressionFormat::Zlib, {0, 6, 9}},
                                              {CompressionFormat::Gzip, {0, 6, 9}}};
        Array<uint8> inputs[6] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                  Array<uint8>(allocator), Array<uint8>(allocator),
                                  Array<uint8>(allocator), Array<uint8>(allocator)};
        for (usize index = 0; index < 6; ++index) {
            Fill(inputs[index], Sizes[index], index % 2 == 0 ? Pattern::Text : Pattern::Random,
                 index + 3);
        }

        for (const LevelSet& set : Levels) {
            auto decompressor = Decompressor::TryCreate(set.format, allocator);
            REQUIRE(decompressor);
            for (int32 level : set.levels) {
                const auto settings = Settings(set.format, level);
                auto compressor = Compressor::TryCreate(settings, allocator);
                REQUIRE(compressor);
                for (usize index = 0; index < 6; ++index) {
                    // zstd 최고 수준은 느리므로 큰 입력에는 쓰지 않는다.
                    if (level == 19 && Sizes[index] > 300000) {
                        continue;
                    }
                    const Span<const uint8> input = View(inputs[index]);
                    Array<uint8> expected(allocator);
                    REQUIRE(TryCompressAppend(settings, input, expected));

                    // 실패(출력 부족) 직후에도 다음 결과가 같아야 한다.
                    uint8 tiny[4];
                    CHECK(compressor.Value().TryCompress(input, Span<uint8>(tiny, 4)).Error() ==
                          ErrorCode::CapacityExceeded);
                    Array<uint8> packed(allocator);
                    REQUIRE(compressor.Value().TryCompressAppend(input, packed));
                    CHECK(Same(packed, View(expected)));

                    if (input.Size() > 4) {
                        CHECK(decompressor.Value()
                                  .TryDecompress(View(packed), Span<uint8>(tiny, 4))
                                  .Error() == ErrorCode::CapacityExceeded);
                    }
                    Array<uint8> restored(allocator);
                    auto restoredBytes = decompressor.Value().TryDecompressAppend(
                        View(packed), input.Size(), restored);
                    CHECK(restoredBytes && Same(restored, input));
                }
            }
        }

        // 이동된 뒤의 문맥은 StaleHandle이고 이동받은 문맥은 그대로 동작한다.
        const auto settings = DefaultSettings(CompressionFormat::Zstd);
        auto compressor = Compressor::TryCreate(settings, allocator);
        REQUIRE(compressor);
        Array<uint8> expected(allocator);
        REQUIRE(TryCompressAppend(settings, View(inputs[2]), expected));
        Compressor moved = Move(compressor.Value());
        Array<uint8> after(allocator);
        CHECK(compressor.Value().TryCompressAppend(View(inputs[2]), after).Error() ==
              ErrorCode::StaleHandle);
        CHECK(moved.TryCompressAppend(View(inputs[2]), after) && Same(after, View(expected)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 입력·설정의 압축 결과는 플랫폼과 무관하게 같아야 한다(쿠킹 재현성). 라이브러리 버전을 올리면
// 결과가 바뀔 수 있으며 그때 이 표를 의도적으로 갱신한다(Docs/Design/Compression.ko.md 4.1).
TEST(Compression_DeterministicOutputGoldens) {
    struct Golden {
        CompressionFormat format;
        usize size;
        uint32 crc;
    };
    static constexpr Golden Goldens[] = {
        // 기준: Windows x64 clang 21.1.6, LZ4 1.11.1·zstd 1.5.7·zlib 1.4, 각 형식 기본 수준.
        {CompressionFormat::Lz4, 1561682, 0x68143ef1u},
        {CompressionFormat::Zstd, 847092, 0xa5bcc580u},
        {CompressionFormat::Deflate, 884860, 0x5479ce6eu},
        {CompressionFormat::Zlib, 884866, 0x7fffd6cdu},
        {CompressionFormat::Gzip, 884878, 0x16eb60f5u},
    };
    SystemAllocator allocator;
    Array<uint8> original(allocator);
    // 분할 단위(1MiB)를 넘겨 스트림 경계까지 고정한다.
    Fill(original, (usize{3} << 19) + 11, Pattern::Text, 7);
    for (const Golden& golden : Goldens) {
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(golden.format), View(original), packed));
        const uint32 crc = Crc32(View(packed));
        if (packed.Size() != golden.size || crc != golden.crc) {
            // 의도한 갱신(라이브러리 버전 변경)일 때 새 값을 표에 옮긴다.
            printf("compression golden mismatch: format %u size %llu crc 0x%08x\n",
                   static_cast<unsigned>(golden.format),
                   static_cast<unsigned long long>(packed.Size()), crc);
        }
        CHECK(packed.Size() == golden.size);
        CHECK(crc == golden.crc);
    }
}

TEST(Compression_InputContractsAndStrictness) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 5000, Pattern::Text);

        for (CompressionFormat format : AllFormats) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));

            // 빈 해제 입력, 겹치는 출력.
            CHECK(DecodeError(format, Span<const uint8>()) == ErrorCode::InvalidArgument);
            CHECK(TryDecompress(format, View(packed), Span<uint8>(packed.Data(), packed.Size()),
                                allocator)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(TryDecompressAppend(format, Span<const uint8>(packed.Data(), 4), 100, packed)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(TryCompressAppend(DefaultSettings(format), Span<const uint8>(packed.Data(), 4),
                                    packed)
                      .Error() == ErrorCode::InvalidArgument);

            // 모든 길이로 자른 입력은 실패한다(Lz4는 끝 표지가 없어 일부 접두사가 유효할 수
            // 있으므로 제외).
            if (format != CompressionFormat::Lz4) {
                for (usize length = 1; length < packed.Size(); ++length) {
                    CHECK(DecodeError(format, Span<const uint8>(packed.Data(), length)) !=
                          ErrorCode::None);
                }
            }

            // 스트림 뒤의 남는 byte는 손상이다(Lz4 raw block은 끝 표지가 없어 검사하지 않는다).
            if (format != CompressionFormat::Lz4) {
                Array<uint8> trailing(allocator);
                REQUIRE(trailing.TryAppend(View(packed)));
                REQUIRE(trailing.TryAppend(Text("!")));
                CHECK(DecodeError(format, View(trailing)) == ErrorCode::InvalidArgument);
            }
        }

        // gzip member 뒤의 0 padding도 거부한다.
        static constexpr uint8 GzipPlainThenZero[] = {
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xcb,
            0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0xca, 0x4f, 0xce, 0x2e, 0xe6,
            0x02, 0x00, 0x77, 0x08, 0xac, 0x17, 0x0c, 0x00, 0x00, 0x00, 0x00};
        CHECK(DecodeError(CompressionFormat::Gzip, Bytes(GzipPlainThenZero)) ==
              ErrorCode::InvalidArgument);

        // zlib 프리셋 사전(FDICT)은 지원하지 않는다: CMF 0x78, FLG 0x20(FDICT, FCHECK로 31의 배수),
        // DICTID.
        static constexpr uint8 ZlibPresetDictionary[] = {0x78, 0x20, 0x01, 0x02, 0x03,
                                                         0x04, 0x4b, 0x04, 0x00};
        CHECK(DecodeError(CompressionFormat::Zlib, Bytes(ZlibPresetDictionary)) ==
              ErrorCode::Unsupported);

        // 체크섬 불일치: zlib Adler-32 마지막 byte, gzip CRC 첫 byte.
        static constexpr uint8 ZlibBadAdler[] = {0x78, 0x9c, 0x4b, 0x4c, 0x4a, 0x06,
                                                 0x00, 0x02, 0x4d, 0x01, 0x28};
        CHECK(DecodeError(CompressionFormat::Zlib, Bytes(ZlibBadAdler)) ==
              ErrorCode::InvalidArgument);

        // zstd frame이 원본 크기를 거짓으로 기록하면(4라고 했지만 raw block은 5byte) 손상이다.
        static constexpr uint8 ZstdLyingSize[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x04, 0x29,
                                                  0x00, 0x00, 'h',  'e',  'l',  'l',  'o'};
        CHECK(DecodeError(CompressionFormat::Zstd, Bytes(ZstdLyingSize)) ==
              ErrorCode::InvalidArgument);

        // LZ4 블록 입력 한계는 읽기 전에 거부한다(실제 메모리 없이 크기만 넘긴다).
        uint8 dummy = 0;
        CHECK(TryGetCompressBound(CompressionFormat::Lz4, Lz4MaxInputBytes + 1).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TryCompress(DefaultSettings(CompressionFormat::Lz4),
                          Span<const uint8>(&dummy, Lz4MaxInputBytes + 1), Span<uint8>(&dummy, 1),
                          allocator)
                  .Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 손상 입력은 실패하거나, 성공하면(의미 없는 bit) 원본과 같아야 한다. 무결성 검사가 없는 형식은
// 범위 밖 쓰기가 없음만 확인한다(ASan 구성).
TEST(Compression_CorruptedInputNeverEscapes) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 4096, Pattern::Text, 11);
        for (CompressionFormat format : AllFormats) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));
            const bool checked = format == CompressionFormat::Zstd ||
                                 format == CompressionFormat::Zlib ||
                                 format == CompressionFormat::Gzip;
            uint64 state = 0x2545f4914f6cdd1dull + static_cast<uint64>(format);
            for (uint32 trial = 0; trial < 600; ++trial) {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                Array<uint8> damaged(allocator);
                REQUIRE(damaged.TryAppend(View(packed)));
                const usize position =
                    trial < 256 ? trial % packed.Size() : static_cast<usize>(state % packed.Size());
                damaged[position] =
                    static_cast<uint8>(damaged[position] ^ (1u << ((state >> 40) % 8)));

                Array<uint8> output(allocator);
                auto decoded =
                    TryDecompressAppend(format, View(damaged), original.Size() * 2, output);
                if (decoded && checked) {
                    CHECK(Same(output, View(original)));
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Compression_OutOfMemoryAtEveryAllocationRollsBack) {
    SystemAllocator system;
    Array<uint8> original(system);
    Fill(original, 150000, Pattern::Text, 3);
    for (CompressionFormat format : AllFormats) {
        Array<uint8> packed(system);
        REQUIRE(TryCompressAppend(DefaultSettings(format), View(original), packed));

        // 압축: 작업 메모리와 output 성장 모두 실패 주입 대상이다.
        NFailAllocator failing(system);
        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            {
                Array<uint8> output(failing);
                failing.DisableFailure();
                REQUIRE(output.TryAppend(Text("base")));
                failing.FailOnAttempt(attempt);
                auto result = TryCompressAppend(DefaultSettings(format), View(original), output);
                succeeded = result.IsOk();
                // 성공은 실패 주입 지점이 실제 할당 수를 넘었을 때만이다(주입이 실제로 일어났음을
                // 보장).
                CHECK(!succeeded || failing.Attempts() < attempt);
                CHECK(succeeded || result.Error() == ErrorCode::OutOfMemory);
                CHECK(succeeded || output.Size() == 4);
            }
            CHECK(system.Stats().liveAllocations == 2); // original·packed만 남는다.
        }
        CHECK(succeeded);

        succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            {
                Array<uint8> output(failing);
                failing.DisableFailure();
                REQUIRE(output.TryAppend(Text("base")));
                failing.FailOnAttempt(attempt);
                auto result = TryDecompressAppend(format, View(packed), original.Size(), output);
                succeeded = result.IsOk();
                // 성공은 실패 주입 지점이 실제 할당 수를 넘었을 때만이다(주입이 실제로 일어났음을
                // 보장).
                CHECK(!succeeded || failing.Attempts() < attempt);
                CHECK(succeeded || result.Error() == ErrorCode::OutOfMemory);
                CHECK(succeeded ? Same(Span<const uint8>(output.Data() + 4, output.Size() - 4),
                                       View(original))
                                : output.Size() == 4);
            }
            CHECK(system.Stats().liveAllocations == 2);
        }
        CHECK(succeeded);
    }
}

TEST(Compression_AllocationsGoThroughInjectedAllocator) {
    SystemAllocator system;
    CountingAllocator counting(system);
    Array<uint8> original(system);
    Fill(original, 100000, Pattern::Text, 5);
    for (CompressionFormat format : AllFormats) {
        const auto settings = DefaultSettings(format);
        Array<uint8> packed(system);
        REQUIRE(packed.TryResize(TryGetCompressBound(format, original.Size()).Value()));
        const uint64 before = counting.Allocations();
        auto written = TryCompress(settings, View(original),
                                   Span<uint8>(packed.Data(), packed.Size()), counting);
        REQUIRE(written);
        CHECK(counting.Allocations() > before);
        CHECK(counting.Current() == 0);
    }

    // LZ4 HC 최고 수준도 문맥을 만든 뒤에는 압축 중 할당하지 않는다(확장 상태).
    auto compressor = Compressor::TryCreate(Settings(CompressionFormat::Lz4, 12), counting);
    REQUIRE(compressor);
    Array<uint8> packed(system);
    REQUIRE(packed.TryResize(TryGetCompressBound(CompressionFormat::Lz4, original.Size()).Value()));
    const uint64 created = counting.Allocations();
    for (uint32 round = 0; round < 3; ++round) {
        CHECK(compressor.Value().TryCompress(View(original),
                                             Span<uint8>(packed.Data(), packed.Size())));
    }
    CHECK(counting.Allocations() == created);
}

// 상한은 해제 결과와 작업 메모리 모두를 묶는다(압축 폭탄·거짓 헤더).
TEST(Compression_DecompressionBombsStayBounded) {
    SystemAllocator system;
    CountingAllocator counting(system);
    constexpr usize Limit = usize{1} << 20;

    // zstd frame이 2^40byte를 선언하면 할당 전에 거부한다(FHD 0xE0: single segment, FCS 8byte).
    static constexpr uint8 ZstdHuge[] = {0x28, 0xb5, 0x2f, 0xfd, 0xe0, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x00, 'A'};
    {
        Array<uint8> output(counting);
        CHECK(
            TryDecompressAppend(CompressionFormat::Zstd, Bytes(ZstdHuge), Limit, output).Error() ==
            ErrorCode::CapacityExceeded);
        CHECK(counting.Peak() < Limit);
    }

    // window 2^31을 선언한 크기 미기록 frame도 출력 버퍼만 쓴다(window 메모리 없음).
    static constexpr uint8 ZstdWideWindow[] = {0x28, 0xb5, 0x2f, 0xfd, 0x00,
                                               0xa8, 0x09, 0x00, 0x00, 'A'};
    {
        counting.ResetPeak();
        Array<uint8> output(counting);
        auto decoded =
            TryDecompressAppend(CompressionFormat::Zstd, Bytes(ZstdWideWindow), Limit, output);
        CHECK(decoded && decoded.Value() == 1 && output[0] == 'A');
        CHECK(counting.Peak() < Limit * 2);
    }

    // 64MiB의 0을 압축한 zlib·gzip·zstd 스트림을 1MiB 상한으로 해제한다.
    Array<uint8> zeros(system);
    Fill(zeros, usize{64} << 20, Pattern::Zeros);
    static constexpr CompressionFormat Streams[] = {
        CompressionFormat::Zlib, CompressionFormat::Gzip, CompressionFormat::Zstd};
    for (CompressionFormat format : Streams) {
        Array<uint8> packed(system);
        REQUIRE(TryCompressAppend(Settings(format, format == CompressionFormat::Zstd ? 19 : 9),
                                  View(zeros), packed));
        counting.ResetPeak();
        Array<uint8> output(counting);
        CHECK(TryDecompressAppend(format, View(packed), Limit, output).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(output.Size() == 0);
        CHECK(counting.Peak() < Limit * 2);
    }
    CHECK(counting.Current() == 0);
}

} // namespace rocks::test
