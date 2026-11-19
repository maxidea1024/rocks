#include <Core/Memory/SystemAllocator.h>
#include <Serialization/TextWire.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;
namespace text_wire_tests_file {

// Text 값(WireType::Text = 18)의 세 형태 형식, 손상 입력 거부와 cursor 불변, writer의 칸 검사·실패
// 시 byte 불변·자기 View 원문 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.2·13절 — 리플렉션
// B9a3).
namespace {

void AppendU64(Array<uint8>& out, uint64 value) noexcept {
    for (int index = 0; index < 8; ++index) {
        static_cast<void>(out.TryPushBack(static_cast<uint8>(value >> (8 * index))));
    }
}

void AppendBytes(Array<uint8>& out, Span<const uint8> bytes) noexcept {
    static_cast<void>(out.TryAppend(bytes));
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.Size() == 0 || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

} // namespace

TEST(TextWire_FormsMatchGoldenAndRoundTrip) {
    SystemAllocator allocator;
    ArchiveWriter out(allocator);
    TextWireValue invariant;
    invariant.form = TextWireForm::Invariant;
    invariant.source = "ab";
    TextWireValue localized;
    localized.form = TextWireForm::Localized;
    localized.textId = Guid{0x0102030405060708ull, 0x1112131415161718ull};
    localized.sourceRevision = 3;
    localized.source = "c";
    REQUIRE(WriteTextWire(out, TextWireValue{}));
    REQUIRE(WriteTextWire(out, invariant));
    REQUIRE(WriteTextWire(out, localized));
    const uint8 golden[] = {
        0,                                                            // Empty
        1,    2,    0,    0,    0,    0,    0,    0,    0,  'a', 'b', // Invariant "ab"
        2,    8,    7,    6,    5,    4,    3,    2,    1,            // Localized textId high
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,               // textId low
        3,    0,    0,    0,    0,    0,    0,    0,                  // sourceRevision
        1,    0,    0,    0,    0,    0,    0,    0,    'c'};         // fallback "c"
    CHECK(SameBytes(out.View(), golden));

    // writer 출력이 아닌 golden을 읽는다.
    auto reader = ArchiveReader::Create(golden);
    REQUIRE(reader);
    auto empty = ReadTextWire(reader.Value());
    REQUIRE(empty);
    CHECK(empty.Value().form == TextWireForm::Empty && empty.Value().source.ByteCount() == 0 &&
          empty.Value().textId == Guid{} && empty.Value().sourceRevision == 0);
    auto first = ReadTextWire(reader.Value());
    REQUIRE(first);
    CHECK(first.Value().form == TextWireForm::Invariant &&
          first.Value().source == StringView("ab") && first.Value().textId == Guid{});
    auto second = ReadTextWire(reader.Value());
    REQUIRE(second);
    CHECK(second.Value().form == TextWireForm::Localized &&
          second.Value().textId == localized.textId && second.Value().sourceRevision == 3 &&
          second.Value().source == StringView("c"));
    CHECK(reader.Value().AtEnd());

    // 빈 Invariant 원문도 형태 1이다(비어 있음과 다른 값).
    ArchiveWriter blank(allocator);
    TextWireValue blankInvariant;
    blankInvariant.form = TextWireForm::Invariant;
    REQUIRE(WriteTextWire(blank, blankInvariant));
    const uint8 blankGolden[] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(SameBytes(blank.View(), blankGolden));
}

TEST(TextWire_MalformedFormsAreRejectedWithoutMovingCursor) {
    SystemAllocator allocator;
    // 형태 byte 3·255는 InvalidArgument다.
    const uint8 badForms[] = {3, 255};
    for (uint8 form : badForms) {
        const uint8 bytes[] = {form, 0, 0, 0, 0, 0, 0, 0, 0};
        auto reader = ArchiveReader::Create(bytes);
        REQUIRE(reader);
        CHECK(ReadTextWire(reader.Value()).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.Value().Position() == 0);
    }

    // 빈 입력, 잘린 Invariant 원문, 잘못된 UTF-8.
    auto none = ArchiveReader::Create(Span<const uint8>());
    REQUIRE(none);
    CHECK(ReadTextWire(none.Value()).Error() == ErrorCode::InvalidArgument);
    const uint8 truncated[] = {1, 3, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'};
    auto truncatedReader = ArchiveReader::Create(truncated);
    REQUIRE(truncatedReader);
    CHECK(ReadTextWire(truncatedReader.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(truncatedReader.Value().Position() == 0);
    const uint8 badUtf8[] = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0xc3, 0x28};
    auto utf8Reader = ArchiveReader::Create(badUtf8);
    REQUIRE(utf8Reader);
    CHECK(ReadTextWire(utf8Reader.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(utf8Reader.Value().Position() == 0);

    // Localized: textId 0, sourceRevision 0, 각 칸의 잘림은 InvalidArgument이고 cursor는 그대로다.
    Array<uint8> localized(allocator);
    REQUIRE(localized.TryPushBack(2));
    AppendU64(localized, 1);
    AppendU64(localized, 2);
    AppendU64(localized, 5);
    AppendU64(localized, 1);
    REQUIRE(localized.TryPushBack('z'));
    auto valid = ArchiveReader::Create(localized.View());
    REQUIRE(valid);
    CHECK(ReadTextWire(valid.Value()));
    for (usize length = 1; length < localized.Size(); ++length) {
        auto reader = ArchiveReader::Create(Span<const uint8>(localized.Data(), length));
        REQUIRE(reader);
        CHECK(ReadTextWire(reader.Value()).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.Value().Position() == 0);
    }

    Array<uint8> zeroId(allocator);
    AppendBytes(zeroId, localized.View());
    for (usize index = 1; index < 17; ++index) {
        zeroId[index] = 0;
    }

    auto zeroIdReader = ArchiveReader::Create(zeroId.View());
    REQUIRE(zeroIdReader);
    CHECK(ReadTextWire(zeroIdReader.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(zeroIdReader.Value().Position() == 0);
    Array<uint8> zeroRevision(allocator);
    AppendBytes(zeroRevision, localized.View());
    zeroRevision[17] = 0;
    auto zeroRevisionReader = ArchiveReader::Create(zeroRevision.View());
    REQUIRE(zeroRevisionReader);
    CHECK(ReadTextWire(zeroRevisionReader.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(zeroRevisionReader.Value().Position() == 0);

    // 원문 길이가 maxValueBytes를 넘으면 CapacityExceeded다.
    const uint8 longSource[] = {1, 4, 0, 0, 0, 0, 0, 0, 0, 'a', 'b', 'c', 'd'};
    ArchiveLimits small;
    small.maxValueBytes = 3;
    auto limited = ArchiveReader::Create(longSource, small);
    REQUIRE(limited);
    CHECK(ReadTextWire(limited.Value()).Error() == ErrorCode::CapacityExceeded);
    CHECK(limited.Value().Position() == 0);
}

TEST(TextWire_WriterRejectsInconsistentValuesWithoutPartialBytes) {
    SystemAllocator allocator;
    ArchiveWriter out(allocator);
    REQUIRE(out.WriteU32(0xfeedfaceu));
    TextWireValue emptyWithSource;
    emptyWithSource.source = "x";
    TextWireValue emptyWithId;
    emptyWithId.textId = Guid{1, 1};
    TextWireValue invariantWithRevision;
    invariantWithRevision.form = TextWireForm::Invariant;
    invariantWithRevision.sourceRevision = 1;
    TextWireValue localizedWithoutId;
    localizedWithoutId.form = TextWireForm::Localized;
    localizedWithoutId.sourceRevision = 1;
    TextWireValue localizedWithoutRevision;
    localizedWithoutRevision.form = TextWireForm::Localized;
    localizedWithoutRevision.textId = Guid{1, 1};
    TextWireValue unknownForm;
    unknownForm.form = static_cast<TextWireForm>(3);
    const TextWireValue* rejected[] = {&emptyWithSource,          &emptyWithId,
                                       &invariantWithRevision,    &localizedWithoutId,
                                       &localizedWithoutRevision, &unknownForm};
    for (const TextWireValue* value : rejected) {
        CHECK(WriteTextWire(out, *value).Error() == ErrorCode::InvalidArgument);
        CHECK(out.ByteCount() == 4);
    }

    // 잘못된 UTF-8 원문은 InvalidArgument이고 out은 호출 전 그대로다(형태 byte도 남지 않는다).
    const char malformed[] = {static_cast<char>(0xc3), static_cast<char>(0x28)};
    TextWireValue badSource;
    badSource.form = TextWireForm::Localized;
    badSource.textId = Guid{2, 3};
    badSource.sourceRevision = 4;
    badSource.source = StringView(malformed, 2);
    CHECK(WriteTextWire(out, badSource).Error() == ErrorCode::InvalidArgument);
    CHECK(out.ByteCount() == 4);

    // 크기 한도: 전체가 maxTotalBytes를 넘으면 CapacityExceeded이고 형태 byte도 남지 않는다.
    ArchiveLimits small;
    small.maxTotalBytes = 20;
    ArchiveWriter limited(allocator, small);
    TextWireValue longText;
    longText.form = TextWireForm::Invariant;
    longText.source = "0123456789abc";
    CHECK(WriteTextWire(limited, longText).Error() == ErrorCode::CapacityExceeded);
    CHECK(limited.ByteCount() == 0);

    // 할당 실패의 모든 지점에서 out은 호출 전 byte 그대로이고, 다시 쓰면 같은 byte가 나온다.
    SystemAllocator backing;
    TextWireValue localized;
    localized.form = TextWireForm::Localized;
    localized.textId = Guid{7, 8};
    localized.sourceRevision = 9;
    localized.source = "longer fallback source text";
    ArchiveWriter reference(backing);
    REQUIRE(WriteTextWire(reference, localized));
    for (uint64 attempt = 1; attempt < 16; ++attempt) {
        test::NFailAllocator failing(backing);
        ArchiveWriter writer(failing);
        failing.FailOnAttempt(attempt);
        const auto written = WriteTextWire(writer, localized);
        if (!written) {
            CHECK(written.Error() == ErrorCode::OutOfMemory);
            CHECK(writer.ByteCount() == 0);
            failing.DisableFailure();
            REQUIRE(WriteTextWire(writer, localized));
        }

        CHECK(SameBytes(writer.View(), reference.View()));
    }
}

TEST(TextWire_SourceMayAliasTheWriterBuffer) {
    // ArchiveWriter의 Write*처럼 원문이 out 자신의 View를 가리켜도 된다: 형태 byte·고정 칸·원문을
    // 한 번에 붙이므로 성장이 옛 버퍼를 해제한 뒤 원문을 읽지 않는다.
    SystemAllocator allocator;
    const char text[] = "0123456789abcdef";
    const TextWireForm forms[] = {TextWireForm::Invariant, TextWireForm::Localized};
    for (TextWireForm form : forms) {
        ArchiveWriter out(allocator);
        REQUIRE(out.Reserve(16));
        REQUIRE(out.WriteRaw(Span<const uint8>(reinterpret_cast<const uint8*>(text), 16)));
        REQUIRE(out.View().Data() != nullptr);
        TextWireValue value;
        value.form = form;
        value.source = StringView(reinterpret_cast<const char*>(out.View().Data()), 16);
        if (form == TextWireForm::Localized) {
            value.textId = Guid{3, 4};
            value.sourceRevision = 5;
        }

        REQUIRE(WriteTextWire(out, value)); // 용량 16이 꽉 차 있어 쓰기마다 성장한다
        auto reader = ArchiveReader::Create(out.View());
        REQUIRE(reader);
        REQUIRE(reader.Value().ReadRaw(16));
        auto read = ReadTextWire(reader.Value());
        REQUIRE(read);
        CHECK(read.Value().form == form);
        CHECK(read.Value().source == StringView(text, 16));
        CHECK(reader.Value().AtEnd());
    }

    // 자기 View의 부분 범위(가운데 4 byte)도 같다.
    ArchiveWriter partial(allocator);
    REQUIRE(partial.Reserve(8));
    REQUIRE(partial.WriteRaw(Span<const uint8>(reinterpret_cast<const uint8*>(text), 8)));
    TextWireValue middle;
    middle.form = TextWireForm::Invariant;
    middle.source = StringView(reinterpret_cast<const char*>(partial.View().Data()) + 2, 4);
    REQUIRE(WriteTextWire(partial, middle));
    auto reader = ArchiveReader::Create(
        Span<const uint8>(partial.View().Data() + 8, partial.ByteCount() - 8));
    REQUIRE(reader);
    auto read = ReadTextWire(reader.Value());
    CHECK(read && read.Value().source == StringView("2345"));
}
} // namespace text_wire_tests_file
