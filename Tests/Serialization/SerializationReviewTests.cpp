#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Serialization/Record.h>

using namespace rocks;

namespace {
// 공개 wire 명세로 고정한 1-field record다. writer 출력에서 기대값을 생성하지 않는다.
constexpr uint8 GoldenRecord[] = {
    'R', 'O', 'C', 'K', 'S', 'R', 'E', 'C', 1, 0, 0, 0, 1, 0, 0, 0,    0,    0,    0,
    0,   2,   0,   0,   0,   0,   0,   0,   0, 1, 0, 0, 0, 1, 0, 0,    0,    32,   0,
    0,   0,   0,   0,   0,   0,   3,   0,   0, 0, 0, 0, 0, 0, 4, 0,    0,    0,    0,
    0,   0,   0,   2,   0,   0,   0,   4,   0, 0, 0, 0, 0, 0, 0, 0x78, 0x56, 0x34, 0x12};
static_assert(sizeof(GoldenRecord) == 76);

bool BytesEqual(Span<const uint8> actual, Span<const uint8> expected) noexcept {
    if (actual.Size() != expected.Size()) {
        return false;
    }
    for (usize index = 0; index < actual.Size(); ++index) {
        if (actual[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

bool ExpectedMalformedError(ErrorCode error) noexcept {
    return error == ErrorCode::InvalidArgument || error == ErrorCode::Unsupported ||
           error == ErrorCode::TypeMismatch || error == ErrorCode::CapacityExceeded ||
           error == ErrorCode::AlreadyExists;
}
} // namespace

TEST(RecordEveryByteAndBitMutationPreservesPublishedViewBoundsAndBudget) {
    SystemAllocator allocator;
    uint32 accepted = 0;
    uint32 rejected = 0;
    for (usize offset = 0; offset < sizeof(GoldenRecord); ++offset) {
        for (uint32 bit = 0; bit < 8; ++bit) {
            {
                uint8 bytes[sizeof(GoldenRecord)];
                for (usize index = 0; index < sizeof(bytes); ++index) {
                    bytes[index] = GoldenRecord[index];
                }
                bytes[offset] ^= static_cast<uint8>(uint32{1} << bit);
                RecordLimits limits;
                limits.archive.maxTotalBytes = sizeof(bytes);
                limits.archive.maxValueBytes = sizeof(bytes);
                limits.archive.maxOperations = 32;
                limits.archive.maxWork = 512;
                limits.maxFields = 4;
                auto opened = RecordReader::Open(allocator, bytes, {1, 2}, 1, 1, limits);
                if (!opened) {
                    ++rejected;
                    CHECK(ExpectedMalformedError(opened.Error()));
                } else {
                    ++accepted;
                    auto& reader = opened.Value();
                    CHECK(reader.FieldCount() <= limits.maxFields);
                    CHECK(reader.Position() == 0);
                    for (usize index = 0; index < reader.FieldCount(); ++index) {
                        auto field = reader.Next();
                        REQUIRE(field);
                        CHECK(field.Value().id != Guid{});
                        CHECK(field.Value().wireType != 0);
                        const uptr base = reinterpret_cast<uptr>(bytes);
                        const uptr address = reinterpret_cast<uptr>(field.Value().payload.Data());
                        // 유효한 값/unknown tag 변이는 허용한다. 게시된 borrowed span은 원본을
                        // 벗어나면 안 된다.
                        CHECK(address >= base && address - base <= sizeof(bytes) &&
                              field.Value().payload.Size() <= sizeof(bytes) - (address - base));
                        CHECK(reader.Position() == index + 1);
                    }
                    const usize position = reader.Position();
                    auto end = reader.Next();
                    CHECK(!end && end.Error() == ErrorCode::NotFound);
                    CHECK(reader.Position() == position);
                    CHECK(reader.WorkUsed() <= limits.archive.maxWork);
                }
            }
            CHECK(allocator.Stats().liveAllocations == 0);
        }
    }
    CHECK(accepted != 0 && rejected != 0);
    CHECK(accepted + rejected == 608);
}

TEST(ArchiveFailuresAtNonzeroCursorPreservePreviousSuccessfulReads) {
    const uint8 truncated[] = {4, 3, 2, 1, 5, 0, 0, 0, 0, 0, 0, 0, 'a', 'b', 'c'};
    auto reader = ArchiveReader::Create(truncated);
    REQUIRE(reader);
    auto prefix = reader.Value().ReadU32();
    REQUIRE(prefix);
    CHECK(prefix.Value() == 0x01020304);
    CHECK(reader.Value().Position() == 4);
    auto invalidBool = reader.Value().ReadBool();
    CHECK(!invalidBool && invalidBool.Error() == ErrorCode::InvalidArgument);
    CHECK(reader.Value().Position() == 4);
    auto shortGuid = reader.Value().ReadGuid();
    CHECK(!shortGuid && shortGuid.Error() == ErrorCode::InvalidArgument);
    CHECK(reader.Value().Position() == 4);
    auto shortBytes = reader.Value().ReadBytes();
    CHECK(!shortBytes && shortBytes.Error() == ErrorCode::InvalidArgument);
    CHECK(reader.Value().Position() == 4);
    auto shortString = reader.Value().ReadStringView();
    CHECK(!shortString && shortString.Error() == ErrorCode::InvalidArgument);
    CHECK(reader.Value().Position() == 4);
    auto length = reader.Value().ReadU64();
    REQUIRE(length);
    CHECK(length.Value() == 5);
    auto payload = reader.Value().ReadRaw(3);
    REQUIRE(payload);
    CHECK(payload.Value().Data() == truncated + 12);
    CHECK(reader.Value().AtEnd());

    SystemAllocator allocator;
    {
        ArchiveWriter writer(allocator);
        REQUIRE(writer.WriteU32(9));
        REQUIRE(writer.WriteString("owned"));
        auto ownedReader = ArchiveReader::Create(writer.View());
        REQUIRE(ownedReader);
        REQUIRE(ownedReader.Value().ReadU32());
        test::NFailAllocator failure(allocator);
        failure.FailOnAttempt(1);
        auto failed = ownedReader.Value().ReadString(failure);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(ownedReader.Value().Position() == 4);
        failure.DisableFailure();
        auto retry = ownedReader.Value().ReadString(failure);
        REQUIRE(retry);
        CHECK(retry.Value().View() == StringView("owned"));
        CHECK(ownedReader.Value().AtEnd());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArchiveUtf8SelfStringGrowthAndFailedGrowthPreserveExactBytes) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failure(allocator);
        ArchiveWriter writer(failure);
        const uint8 korean[] = {0xed, 0x95, 0x9c, 0xea, 0xb8, 0x80};
        REQUIRE(writer.WriteRaw(korean));
        const uint8* original = writer.View().Data();
        StringView source(reinterpret_cast<const char*>(original), sizeof(korean));
        failure.FailOnAttempt(1);
        auto failed = writer.WriteString(source);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(writer.View().Data() == original);
        CHECK(BytesEqual(writer.View(), korean));
        failure.DisableFailure();
        REQUIRE(writer.WriteString(source));
        REQUIRE(writer.WriteString({reinterpret_cast<const char*>(writer.View().Data() + 3), 3}));
        const uint8 expected[] = {0xed, 0x95, 0x9c, 0xea, 0xb8, 0x80, 6,    0,    0,    0, 0,
                                  0,    0,    0,    0xed, 0x95, 0x9c, 0xea, 0xb8, 0x80, 3, 0,
                                  0,    0,    0,    0,    0,    0,    0xea, 0xb8, 0x80};
        CHECK(BytesEqual(writer.View(), expected));
        auto reader = ArchiveReader::Create(writer.View());
        REQUIRE(reader);
        REQUIRE(reader.Value().ReadRaw(6));
        auto whole = reader.Value().ReadStringView();
        auto part = reader.Value().ReadStringView();
        REQUIRE(whole && part);
        CHECK(whole.Value() == StringView("\xED\x95\x9C\xEA\xB8\x80"));
        CHECK(part.Value() == StringView("\xEA\xB8\x80"));
        CHECK(reader.Value().AtEnd());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
