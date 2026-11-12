#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>

// 외부 형식용 ByteReader·ByteWriter의 byte 순서 golden 값, 이어지는 실패, 경계, 할당 실패 시험이다.
namespace rocks::test {
namespace {

constexpr uint8 Golden[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                            0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.Size() == 0 || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

} // namespace

TEST(ByteReader_ReadsEachWidthInBothByteOrders) {
    ByteReader little{Span<const uint8>(Golden)};
    CHECK(little.ReadU8() == 0x01);
    CHECK(little.ReadU16LE() == 0x0302);
    CHECK(little.ReadU32LE() == 0x07060504u);
    CHECK(little.ReadU8() == 0x08);
    CHECK(little.ReadU64LE() == 0x8796A5B4C3D2E1F0ull);
    CHECK(little.AtEnd() && !little.Failed());

    ByteReader big{Span<const uint8>(Golden)};
    CHECK(big.ReadU16BE() == 0x0102);
    CHECK(big.ReadU32BE() == 0x03040506u);
    CHECK(big.ReadU16BE() == 0x0708);
    CHECK(big.ReadU64BE() == 0xF0E1D2C3B4A59687ull);
    CHECK(big.AtEnd());

    // 부호 있는 정수는 two's complement 그대로다.
    ByteReader signedReader{Span<const uint8>(Golden + 8, 8)};
    CHECK(signedReader.ReadI8() == static_cast<int8>(-16));
    CHECK(signedReader.ReadBigEndian<int16>() == static_cast<int16>(0xE1D2));
    CHECK(signedReader.ReadLittleEndian<int32>() == static_cast<int32>(0x96A5B4C3u));
    CHECK(signedReader.Remaining() == 1);

    // 부동소수는 bit 표현을 옮긴다(부호 있는 0·NaN payload 보존).
    constexpr uint8 floats[] = {0x00, 0x00, 0x80, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x7F, 0xC0,
                                0x00, 0x01, 0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2D, 0x18};
    ByteReader floatReader{Span<const uint8>(floats)};
    CHECK(floatReader.ReadF32LE() == 1.0f);
    CHECK(platform::BitCast<uint32>(floatReader.ReadF32BE()) == 0x80000000u);
    CHECK(platform::BitCast<uint32>(floatReader.ReadF32BE()) == 0x7FC00001u);
    CHECK(platform::BitCast<uint64>(floatReader.ReadF64BE()) == 0x400921FB54442D18ull);
    CHECK(floatReader.AtEnd());
}

TEST(ByteReader_FailureIsStickyAndKeepsPosition) {
    ByteReader reader{Span<const uint8>(Golden, 6)};
    CHECK(reader.ReadU32BE() == 0x01020304u);
    // 남은 2 byte로는 u32를 읽을 수 없다: 0을 돌려주고 위치는 그대로다.
    CHECK(reader.ReadU32LE() == 0);
    CHECK(reader.Failed() && reader.Position() == 4 && reader.Remaining() == 0 && !reader.AtEnd());
    // 이후에는 범위 안 읽기도 실패한다.
    CHECK(reader.ReadU8() == 0 && reader.Position() == 4);
    CHECK(!reader.Skip(0) && reader.Position() == 4);
    CHECK(!reader.Seek(0) && reader.Position() == 4);
    CHECK(reader.Take(1).Data() == nullptr && reader.Position() == 4);
    CHECK(!reader.CanRead(0) && reader.Peek(1).Size() == 0);
    // 반복 조건 Remaining()은 실패하면 0이라 루프가 끝난다(AtEnd()는 계속 거짓이다).
    CHECK(reader.Remaining() == 0 && !reader.AtEnd());

    // 형식 의미 오류도 같은 상태로 합친다.
    ByteReader marked{Span<const uint8>(Golden)};
    CHECK(marked.ReadU8() == 0x01);
    marked.MarkFailed();
    CHECK(marked.Failed() && marked.ReadU8() == 0 && marked.Position() == 1);

    // 빈 reader는 끝이며, 읽으면 실패한다.
    ByteReader empty;
    CHECK(empty.AtEnd() && empty.Size() == 0);
    CHECK(empty.ReadU8() == 0 && empty.Failed());
}

TEST(ByteReader_CursorBoundsAndViews) {
    ByteReader reader{Span<const uint8>(Golden)};
    // Seek: 끝 위치(Size)는 허용, 그 너머는 실패.
    CHECK(reader.Seek(16) && reader.AtEnd());
    CHECK(reader.Seek(3) && reader.ReadU8() == 0x04);
    ByteReader beyond{Span<const uint8>(Golden)};
    CHECK(!beyond.Seek(17) && beyond.Failed() && beyond.Position() == 0);

    // Skip은 크기 계산이 넘치지 않는다.
    ByteReader huge{Span<const uint8>(Golden)};
    CHECK(huge.Skip(1) && !huge.Skip(~usize(0)) && huge.Failed() && huge.Position() == 1);

    // Take는 원본을 빌리고, Peek은 나아가지도 실패시키지도 않는다.
    ByteReader views{Span<const uint8>(Golden)};
    const Span<const uint8> peeked = views.Peek(4);
    CHECK(peeked.Data() == Golden && peeked.Size() == 4 && views.Position() == 0);
    CHECK(views.Peek(17).Size() == 0 && !views.Failed());
    const Span<const uint8> taken = views.Take(4);
    CHECK(taken.Data() == Golden && taken.Size() == 4 && views.Position() == 4);
    CHECK(views.Take(0).Size() == 0 && !views.Failed() && views.Position() == 4);
    CHECK(views.Source().Data() == Golden && views.Source().Size() == 16);

    // ReadInto: 성공하면 복사, 실패하면 출력을 0으로 채운다.
    uint8 copy[4] = {9, 9, 9, 9};
    CHECK(views.ReadInto(Span<uint8>(copy)) && copy[0] == 0x05 && copy[3] == 0x08);
    uint8 tooMany[16];
    platform::MemorySet(tooMany, 0xAB, sizeof(tooMany));
    CHECK(!views.ReadInto(Span<uint8>(tooMany)) && views.Failed());
    bool zeroed = true;
    for (uint8 value : tooMany) {
        zeroed = zeroed && value == 0;
    }

    CHECK(zeroed);
}

TEST(ByteReader_SubReaderIsBoundedAndIndependent) {
    // chunk: [u32 BE 길이 4][본문 4][trailer 1]
    constexpr uint8 chunk[] = {0, 0, 0, 4, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    ByteReader reader{Span<const uint8>(chunk)};
    ByteReader body = reader.Sub(reader.ReadU32BE());
    CHECK(!reader.Failed() && reader.Position() == 8);
    CHECK(body.Size() == 4 && body.Position() == 0 && body.ReadU16BE() == 0xAABB);
    // 본문 reader는 본문 밖을 읽지 못하며, 그 실패가 부모로 번지지 않는다.
    CHECK(body.ReadU32BE() == 0 && body.Failed());
    CHECK(!reader.Failed() && reader.ReadU8() == 0xEE && reader.AtEnd());

    // 모자란 Sub는 부모와 결과가 모두 실패한다.
    ByteReader shortReader{Span<const uint8>(chunk)};
    ByteReader missing = shortReader.Sub(10);
    CHECK(shortReader.Failed() && missing.Failed() && missing.Size() == 0);
    CHECK(missing.ReadU8() == 0);

    // 실패한 부모에서 뗀 Sub(0)도 실패 상태다.
    ByteReader failed{Span<const uint8>(chunk)};
    failed.MarkFailed();
    CHECK(failed.Sub(0).Failed());
}

TEST(ByteWriter_WritesGoldenBytesAndRoundTrips) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    ByteWriter writer(bytes);
    writer.WriteU8(0x01);
    writer.WriteU16LE(0x0302);
    writer.WriteU32LE(0x07060504u);
    writer.WriteI8(0x08);
    writer.WriteU64BE(0xF0E1D2C3B4A59687ull);
    CHECK(writer.Status() && writer.Error() == ErrorCode::None);
    CHECK(SameBytes(bytes.View(), Span<const uint8>(Golden)));

    // 모든 폭·순서·부동소수가 reader로 그대로 돌아온다.
    Array<uint8> round(allocator);
    ByteWriter roundWriter(round);
    roundWriter.WriteU16BE(0xBEEF);
    roundWriter.WriteU32BE(0xDEADBEEFu);
    roundWriter.WriteU64LE(0x0123456789ABCDEFull);
    roundWriter.WriteBigEndian<int32>(-2);
    roundWriter.WriteLittleEndian<int64>(-3);
    roundWriter.WriteF32LE(-0.0f);
    roundWriter.WriteF32BE(platform::BitCast<float>(0x7FC00001u));
    roundWriter.WriteF64LE(3.5);
    roundWriter.WriteF64BE(-1.25);
    roundWriter.WriteZeros(3);
    REQUIRE(roundWriter.Status());
    ByteReader reader(round.View());
    CHECK(reader.ReadU16BE() == 0xBEEF);
    CHECK(reader.ReadU32BE() == 0xDEADBEEFu);
    CHECK(reader.ReadU64LE() == 0x0123456789ABCDEFull);
    CHECK(reader.ReadBigEndian<int32>() == -2);
    CHECK(reader.ReadLittleEndian<int64>() == -3);
    CHECK(platform::BitCast<uint32>(reader.ReadF32LE()) == 0x80000000u);
    CHECK(platform::BitCast<uint32>(reader.ReadF32BE()) == 0x7FC00001u);
    CHECK(reader.ReadF64LE() == 3.5);
    CHECK(reader.ReadF64BE() == -1.25);
    CHECK(reader.ReadU16BE() == 0 && reader.ReadU8() == 0 && reader.AtEnd());

    // 부동소수 쓰기도 고정 byte로 확인한다(왕복만으로는 양쪽이 같이 틀린 순서를 못 잡는다).
    Array<uint8> floats(allocator);
    ByteWriter floatWriter(floats);
    floatWriter.WriteF64LE(3.5); // 0x400C000000000000
    floatWriter.WriteF64BE(3.5);
    floatWriter.WriteF32LE(1.0f); // 0x3F800000
    floatWriter.WriteF32BE(1.0f);
    REQUIRE(floatWriter.Status());
    constexpr uint8 floatGolden[] = {0, 0, 0, 0, 0, 0, 0x0C, 0x40, 0x40, 0x0C, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0x80, 0x3F, 0x3F, 0x80, 0, 0};
    CHECK(SameBytes(floats.View(), Span<const uint8>(floatGolden)));
    ByteReader floatReader{Span<const uint8>(floatGolden)};
    CHECK(floatReader.ReadF64LE() == 3.5 && floatReader.ReadF64BE() == 3.5);
    CHECK(floatReader.ReadF32LE() == 1.0f && floatReader.ReadF32BE() == 1.0f);
}

TEST(ByteReader_IsUsableInConstantExpressions) {
    static_assert([] {
        ByteReader reader{Span<const uint8>(Golden)};
        return reader.ReadU16BE();
    }() == 0x0102);
    static_assert([] {
        ByteReader reader{Span<const uint8>(Golden)};
        ByteReader body = reader.Sub(4);
        return body.ReadU32LE() == 0x04030201u && reader.Position() == 4 && body.AtEnd();
    }());
    static_assert([] {
        constexpr uint8 one[] = {0x3F, 0x80, 0, 0};
        ByteReader reader{Span<const uint8>(one)};
        return reader.ReadF32BE() == 1.0f;
    }());
    static_assert([] {
        ByteReader reader{Span<const uint8>(Golden, 2)};
        return reader.ReadU32BE() == 0 && reader.Failed() && reader.Position() == 0;
    }());
    CHECK(true);
}

TEST(ByteWriter_AppendsPatchesAndAliases) {
    SystemAllocator allocator;
    // 기존 내용 뒤에 덧붙인다.
    Array<uint8> bytes(allocator);
    REQUIRE(bytes.TryPushBack(0x77));
    ByteWriter writer(bytes);
    CHECK(writer.Size() == 1);
    const usize lengthAt = writer.Size();
    writer.WriteU32BE(0);
    writer.WriteBytes(Span<const uint8>(Golden, 5));
    writer.PatchBigEndian<uint32>(lengthAt, 5);
    writer.PatchLittleEndian<uint16>(writer.Size() - 2, 0xAABB);
    REQUIRE(writer.Status());
    constexpr uint8 expected[] = {0x77, 0, 0, 0, 5, 0x01, 0x02, 0x03, 0xBB, 0xAA};
    CHECK(SameBytes(bytes.View(), Span<const uint8>(expected)));

    // 끝에 딱 맞는 Patch는 되고, 한 byte라도 밖이면 InvalidArgument로 실패하며 내용은 그대로다.
    writer.PatchBigEndian<uint16>(bytes.Size() - 2, 0x0102);
    REQUIRE(writer.Status());
    writer.PatchBigEndian<uint32>(bytes.Size() - 3, 0);
    CHECK(writer.Failed() && writer.Error() == ErrorCode::InvalidArgument);
    CHECK(bytes.Size() == sizeof(expected) && bytes[8] == 0x01 && bytes[9] == 0x02);
    // 실패 뒤의 쓰기는 아무것도 하지 않는다.
    writer.WriteU8(0xFF);
    writer.WriteZeros(4);
    CHECK(bytes.Size() == sizeof(expected) &&
          writer.Status().Error() == ErrorCode::InvalidArgument);
    // offset + 폭 계산이 넘치지 않는다: 합이 1로 감기는 offset이라 `offset + 8 > Size`로 판정하면
    // 범위 밖에 쓴다.
    Array<uint8> small(allocator);
    REQUIRE(small.TryPushBack(1));
    ByteWriter overflow(small);
    overflow.PatchLittleEndian<uint64>(~usize(0) - 6, 0);
    CHECK(overflow.Error() == ErrorCode::InvalidArgument && small.Size() == 1 && small[0] == 1);

    // 크기가 표현 범위를 넘는 WriteZeros는 Overflow이며 target은 그대로다.
    Array<uint8> zeros(allocator);
    REQUIRE(zeros.TryPushBack(7));
    ByteWriter zeroWriter(zeros);
    zeroWriter.WriteZeros(~usize(0));
    CHECK(zeroWriter.Error() == ErrorCode::Overflow && zeros.Size() == 1 && zeros[0] == 7);

    // target 자신의 범위를 써도 된다: 재할당하며 전체를 붙이는 경우와 용량 안에서 부분을 붙이는
    // 경우.
    Array<uint8> self(allocator);
    ByteWriter selfWriter(self);
    selfWriter.WriteBytes(Span<const uint8>(Golden));
    for (int round = 0; round < 6; ++round) {
        selfWriter.WriteBytes(self.View());
    }

    REQUIRE(selfWriter.Status() && self.Size() == 16u * 64u);
    bool repeated = true;
    for (usize index = 0; index < self.Size(); ++index) {
        repeated = repeated && self[index] == Golden[index % 16];
    }

    CHECK(repeated);
    Array<uint8> inPlace(allocator);
    REQUIRE(inPlace.TryReserve(64));
    ByteWriter inPlaceWriter(inPlace);
    inPlaceWriter.WriteBytes(Span<const uint8>(Golden));
    const uint8* before = inPlace.Data();
    inPlaceWriter.WriteBytes(Span<const uint8>(inPlace.Data() + 3, 5));
    REQUIRE(inPlaceWriter.Status());
    CHECK(inPlace.Data() == before && inPlace.Size() == 21); // 재할당 없이 붙였다
    CHECK(SameBytes(Span<const uint8>(inPlace.Data() + 16, 5), Span<const uint8>(Golden + 3, 5)));
}

TEST(ByteWriter_EveryAllocationFailureKeepsSuccessfulPrefix) {
    SystemAllocator backing;
    auto writeAll = [](ByteWriter& writer) noexcept {
        for (uint32 index = 0; index < 64; ++index) {
            writer.WriteU32BE(index);
            writer.WriteZeros(index % 5);
            writer.WriteBytes(Span<const uint8>(Golden, index % 16));
        }
    };

    Array<uint8> clean(backing);
    ByteWriter cleanWriter(clean);
    writeAll(cleanWriter);
    REQUIRE(cleanWriter.Status());

    // 어느 할당이 실패해도 target은 실패 전까지의 결과(완전한 출력의 앞부분)이고 오류는
    // OutOfMemory다.
    NFailAllocator failing(backing);
    uint32 failures = 0;
    for (uint64 attempt = 1; attempt < 64; ++attempt) {
        failing.FailOnAttempt(attempt);
        Array<uint8> bytes(failing);
        ByteWriter writer(bytes);
        writeAll(writer);
        if (writer.Failed()) {
            ++failures;
            CHECK(writer.Error() == ErrorCode::OutOfMemory);
            CHECK(bytes.Size() < clean.Size());
            CHECK(SameBytes(bytes.View(), Span<const uint8>(clean.Data(), bytes.Size())));
        } else {
            CHECK(SameBytes(bytes.View(), clean.View()));
        }
    }

    failing.DisableFailure();
    CHECK(failures > 0);
}

} // namespace rocks::test
