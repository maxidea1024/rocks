#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Utf16.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// UTF-16 → UTF-8 변환 시험이다(Core/Text/Utf16.h): BMP 문자, surrogate 짝(LE·BE — 4 byte UTF-8),
// 끝의 high surrogate, 홀로 선 low surrogate, 홀수 byte, 빈 입력, 실패·할당 실패 때 out 그대로.
using namespace rocks;

namespace utf16_tests_file {

// "A€😀"(U+0041, U+20AC, U+1F600 = D83D DE00)의 LE·BE byte와 UTF-8.
TEST(Utf16_ConvertsBmpAndSurrogatePairs) {
    SystemAllocator allocator;
    const uint8 little[] = {0x41, 0x00, 0xAC, 0x20, 0x3D, 0xD8, 0x00, 0xDE};
    const uint8 big[] = {0x00, 0x41, 0x20, 0xAC, 0xD8, 0x3D, 0xDE, 0x00};
    const StringView expected = "A\xE2\x82\xAC\xF0\x9F\x98\x80";
    String out(allocator);
    REQUIRE(out.TryAssign("x"));
    REQUIRE(TryAppendUtf16AsUtf8(Span<const uint8>(little), Utf16ByteOrder::LittleEndian, out));
    CHECK(out.View().SliceBytes(1, out.View().ByteCount() - 1) == expected);
    String bigOut(allocator);
    REQUIRE(TryAppendUtf16AsUtf8(Span<const uint8>(big), Utf16ByteOrder::BigEndian, bigOut));
    CHECK(bigOut.View() == expected);
    String empty(allocator);
    REQUIRE(TryAppendUtf16AsUtf8(Span<const uint8>(), Utf16ByteOrder::LittleEndian, empty));
    CHECK(empty.View().IsEmpty());
}

// 잘못된 입력은 InvalidArgument이고 out은 그대로다.
TEST(Utf16_RejectsMalformedInput) {
    SystemAllocator allocator;
    const uint8 odd[] = {0x41, 0x00, 0x42};
    const uint8 trailingHigh[] = {0x41, 0x00, 0x3D, 0xD8};
    const uint8 loneLow[] = {0x00, 0xDE, 0x41, 0x00};
    const uint8 highThenText[] = {0x3D, 0xD8, 0x41, 0x00};
    for (const Span<const uint8> bytes :
         {Span<const uint8>(odd), Span<const uint8>(trailingHigh), Span<const uint8>(loneLow),
          Span<const uint8>(highThenText)}) {
        String out(allocator);
        REQUIRE(out.TryAssign("keep"));
        CHECK(TryAppendUtf16AsUtf8(bytes, Utf16ByteOrder::LittleEndian, out).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(out.View() == StringView("keep"));
    }

    test::NFailAllocator failing(allocator);
    String out(failing);
    REQUIRE(out.TryAssign("keep"));
    const uint8 text[] = {0x41, 0x00, 0x42, 0x00};
    usize sawFailure = 0;
    for (uint64 attempt = 1; attempt <= 4; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto converted =
            TryAppendUtf16AsUtf8(Span<const uint8>(text), Utf16ByteOrder::LittleEndian, out);
        failing.DisableFailure();
        if (!converted) {
            ++sawFailure;
            CHECK(converted.Error() == ErrorCode::OutOfMemory && out.View() == StringView("keep"));
        } else {
            CHECK(out.View() == StringView("keepAB"));
            break;
        }
    }

    CHECK(sawFailure > 0);
}

} // namespace utf16_tests_file
