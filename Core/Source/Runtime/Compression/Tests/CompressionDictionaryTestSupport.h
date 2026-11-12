#pragma once

#include "CompressionDictionaryTestData.h"
#include "CompressionTestSupport.h"

#include <Base/Endian.h>
#include <Compression/CompressionDictionary.h>
#include <Core/Move.h>
#include <TestSupport/Test.h>

// 압축 사전 시험 공용 도구: 결정적 레코드 생성기(시험 자료 probe와 같다), 시험 사전, 사전 생성
// 도우미.
namespace rocks::test::dictionary_test {

using namespace compression;
using compression_test::Dictionary;
using compression_test::FormattedDictionaryA;

inline constexpr uint32 DictionaryIdA = 0x2A2A2A2Au;

inline Span<const uint8> Slice(Span<const uint8> data, usize offset, usize size) {
    return Span<const uint8>(data.Data() + offset, size);
}

inline Span<const uint8> Text(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

template <usize Count>
inline Span<const uint8> Bytes(const uint8 (&values)[Count]) {
    return Span<const uint8>(values, Count);
}

// 시험 자료 생성기(probe)와 같은 결정적 레코드다. JSON 비슷한 짧은 줄을 bytes만큼 만든다.
inline void MakeRecords(Array<uint8>& out, usize bytes, uint32 seed) {
    static const char* const Words[16] = {
        "alpha", "bravo",  "charlie", "delta", "echo", "foxtrot",  "golf",  "hotel",
        "india", "juliet", "kilo",    "lima",  "mike", "november", "oscar", "papa"};
    REQUIRE(out.TryResize(0));
    REQUIRE(out.TryReserve(bytes));
    uint32 state = seed * 2654435761u + 1u;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    auto put = [&out, bytes](const char* text) {
        for (const char* cursor = text; *cursor != '\0' && out.Size() < bytes; ++cursor) {
            (void)out.TryPushBack(static_cast<uint8>(*cursor));
        }
    };
    auto number = [&out, bytes](uint32 value) {
        char digits[16];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0 && out.Size() < bytes) {
            (void)out.TryPushBack(static_cast<uint8>(digits[--count]));
        }
    };
    uint32 id = 0;
    while (out.Size() < bytes) {
        put("{\"id\":");
        number(id++);
        put(",\"name\":\"");
        put(Words[next() & 15]);
        put("-");
        put(Words[next() & 15]);
        put("\",\"kind\":\"");
        put(Words[next() & 15]);
        put("\",\"x\":");
        number(next() % 1000);
        put(",\"y\":");
        number(next() % 1000);
        put(",\"enabled\":");
        put((next() & 1) != 0 ? "true" : "false");
        put("}\n");
    }
}

inline Span<const uint8> DictionaryA() {
    return Span<const uint8>(FormattedDictionaryA, sizeof(FormattedDictionaryA));
}

// A의 사전 ID 필드(offset 4)만 바꾼 형식 사전이다. 내용·엔트로피 표는 같고 ID·Digest가 다르다.
inline void DictionaryWithId(Array<uint8>& out, uint32 id) {
    REQUIRE(out.TryResize(0));
    REQUIRE(out.TryAppend(DictionaryA()));
    StoreLittleEndian<uint32>(out.Data() + 4, id);
}

inline void DictionaryBytes(Array<uint8>& out, Dictionary which) {
    switch (which) {
    case Dictionary::FormattedA:
        REQUIRE(out.TryResize(0));
        REQUIRE(out.TryAppend(DictionaryA()));
        return;
    case Dictionary::Raw4K:
        MakeRecords(out, 4096, 3);
        return;
    case Dictionary::Raw300K:
        MakeRecords(out, usize{300} << 10, 4);
        return;
    }
}

// 실패하면 기록하고 빈 handle을 돌려준다(이후 사용은 StaleHandle로 드러난다).
inline CompressDictionary MakeCompress(Span<const uint8> bytes, int32 level, Allocator& allocator) {
    auto created =
        CompressDictionary::TryCreate(Settings(CompressionFormat::Zstd, level), bytes, allocator);
    CHECK(created);
    return created ? Move(created.Value()) : CompressDictionary();
}

inline DecompressDictionary MakeDecompress(Span<const uint8> bytes, Allocator& allocator) {
    auto created = DecompressDictionary::TryCreate(CompressionFormat::Zstd, bytes, allocator);
    CHECK(created);
    return created ? Move(created.Value()) : DecompressDictionary();
}

inline ErrorCode CompressError(Span<const uint8> bytes, Allocator& allocator,
                               CompressionFormat format = CompressionFormat::Zstd) {
    auto settings = CompressionSettings::TryDefault(format);
    if (!settings) {
        return settings.Error();
    }
    auto created = CompressDictionary::TryCreate(settings.Value(), bytes, allocator);
    return created ? ErrorCode::None : created.Error();
}

inline ErrorCode DecompressError(Span<const uint8> bytes, Allocator& allocator,
                                 CompressionFormat format = CompressionFormat::Zstd) {
    auto created = DecompressDictionary::TryCreate(format, bytes, allocator);
    return created ? ErrorCode::None : created.Error();
}

// 사전 문맥 하나로 압축한다.
inline void CompressWith(const CompressDictionary& dictionary, Span<const uint8> source,
                         Array<uint8>& packed, Allocator& allocator) {
    auto compressor = Compressor::TryCreate(dictionary, allocator);
    REQUIRE(compressor);
    REQUIRE(packed.TryResize(0));
    REQUIRE(compressor.Value().TryCompressAppend(source, packed));
}

} // namespace rocks::test::dictionary_test
