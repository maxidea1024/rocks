#include <Core/Guid.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;

namespace {

bool FormatsAs(const Guid& value, const char* expected) noexcept {
    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    return text.TryAppendFormat("{}", value) && text.ByteCount() == strlen(expected) &&
           memcmp(text.CString(), expected, text.ByteCount()) == 0;
}

} // namespace

TEST(Guid_FormatAndParse) {
    const Guid value{0x3f2a9c1e7b4d4e8full, 0x9a012c3d4e5f6a7bull};
    CHECK(FormatsAs(value, "3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b"));
    CHECK(FormatsAs(Guid{}, "00000000-0000-0000-0000-000000000000"));
    CHECK(FormatsAs(Guid{~0ull, ~0ull}, "ffffffff-ffff-ffff-ffff-ffffffffffff"));
    CHECK(TryParseGuid("3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b").Value() == value);
    CHECK(TryParseGuid("3F2A9C1E-7B4D-4E8F-9A01-2C3D4E5F6A7B").Value() == value);
    CHECK(TryParseGuid("3f2a9c1e7b4d4e8f9a012c3d4e5f6a7b").Value() == value);

    const char* invalid[] = {"",
                             "3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7",
                             "{3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b}",
                             "3f2a9c1e_7b4d-4e8f-9a01-2c3d4e5f6a7b",
                             "3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7g",
                             "3f2a9c1e7b4d-4e8f-9a01-2c3d4e5f6a7b0",
                             " 3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7"};
    for (const char* text : invalid) {
        CHECK(TryParseGuid(text).Error() == ErrorCode::InvalidArgument);
    }

    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    CHECK(text.TryAppendFormat("{:x}", value).Error() == ErrorCode::InvalidArgument);
}

TEST(Guid_NewGuidIsRandomVersion4) {
    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    Guid previous{};
    for (int iteration = 0; iteration < 256; ++iteration) {
        auto created = TryNewGuid();
        REQUIRE(created);
        const Guid value = created.Value();
        CHECK(!(value == previous));
        previous = value;
        // version 4(14번째 문자), variant 10xx(19번째 문자 8·9·a·b).
        CHECK(((value.high >> 12) & 0xf) == 4);
        CHECK(((value.low >> 62) & 0x3) == 2);
        text.Clear();
        REQUIRE(text.TryAppendFormat("{}", value));
        CHECK(text.CString()[14] == '4');
        const char variant = text.CString()[19];
        CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
        CHECK(TryParseGuid(text.View()).Value() == value);
    }
}
