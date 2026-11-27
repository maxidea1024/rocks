#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Element.h>
#include <UiCore/TextInputPositions.h>

// 텍스트 입력 문맥의 UTF-16 위치 변환과 서비스 찾기 시험이다(Docs/Design/UiToolkit 19.2절).
using namespace rocks;
using namespace rocks::ui;

namespace text_input_positions_tests_file {

namespace {

// "a가🙂\n": byte 0 a, 1–3 가, 4–7 🙂(surrogate 쌍), 8 \n. UTF-16 위치 a 0, 가 1, 🙂 2–3, \n 4.
constexpr StringView Mixed = "a\xEA\xB0\x80\xF0\x9F\x99\x82\n";

class NullService final : public TextInputService {
public:
    void Activate(TextInputContext&) noexcept override {
    }

    void Deactivate(TextInputContext&) noexcept override {
    }

    void CancelComposition(TextInputContext&) noexcept override {
    }

    void NotifyLayoutChanged(TextInputContext&) noexcept override {
    }

    void Process(uint32) noexcept override {
    }
};

} // namespace

TEST(UiCore_TextInputPositionsConvertBetweenUtf8AndUtf16) {
    CHECK(text_input::Utf16Length(Mixed) == 5);
    CHECK(text_input::Utf16Length("") == 0);
    CHECK(text_input::ToUtf16Index(Mixed, 0) == 0);
    CHECK(text_input::ToUtf16Index(Mixed, 1) == 1);
    // code point 가운데 byte는 그 code point 앞이다.
    CHECK(text_input::ToUtf16Index(Mixed, 2) == 1);
    CHECK(text_input::ToUtf16Index(Mixed, 4) == 2);
    CHECK(text_input::ToUtf16Index(Mixed, 8) == 4);
    CHECK(text_input::ToUtf16Index(Mixed, 9) == 5);
    CHECK(text_input::ToUtf16Index(Mixed, 99) == 5);
    CHECK(text_input::ToByteOffset(Mixed, 0) == 0);
    CHECK(text_input::ToByteOffset(Mixed, 1) == 1);
    CHECK(text_input::ToByteOffset(Mixed, 2) == 4);
    // surrogate 쌍 가운데는 그 code point 앞이다.
    CHECK(text_input::ToByteOffset(Mixed, 3) == 4);
    CHECK(text_input::ToByteOffset(Mixed, 4) == 8);
    CHECK(text_input::ToByteOffset(Mixed, 5) == 9);
    CHECK(text_input::ToByteOffset(Mixed, 99) == 9);
    // 올바르지 않은 UTF-8은 0이다.
    CHECK(text_input::Utf16Length("\xFF") == 0);
}

TEST(UiCore_TextInputPositionsRoundTripUtf16) {
    SystemAllocator allocator;
    Array<char16_t> units(allocator);
    REQUIRE(text_input::AppendUtf16(Mixed, units));
    REQUIRE(units.Size() == 5);
    CHECK(units[0] == u'a');
    CHECK(units[1] == 0xAC00);
    CHECK(units[2] == 0xD83D);
    CHECK(units[3] == 0xDE42);
    CHECK(units[4] == u'\n');

    StringBuilder<> utf8(allocator);
    REQUIRE(text_input::AppendUtf8(units.View(), utf8));
    CHECK(utf8.View() == Mixed);

    // 짝 없는 surrogate(앞·뒤)는 U+FFFD다. 글자를 버리지 않는다.
    const char16_t lone[] = {0xD83D, u'x', 0xDE42};
    StringBuilder<> replaced(allocator);
    REQUIRE(text_input::AppendUtf8(Span<const char16_t>(lone, 3), replaced));
    CHECK(replaced.View() == "\xEF\xBF\xBDx\xEF\xBF\xBD");
    CHECK(!text_input::AppendUtf16("\xFF", units));
    CHECK(units.Size() == 5);
}

TEST(UiCore_TextInputServiceIsFoundFromTheNearestAncestor) {
    SystemAllocator allocator;
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto middle = SharedPtr<Element>::TryMake(allocator, allocator);
    auto leaf = SharedPtr<Element>::TryMake(allocator, allocator);
    auto outer = SharedPtr<NullService>::TryMake(allocator);
    auto inner = SharedPtr<NullService>::TryMake(allocator);
    REQUIRE(root && middle && leaf && outer && inner);
    REQUIRE(root.Value()->TryAddChild(middle.Value()));
    REQUIRE(middle.Value()->TryAddChild(leaf.Value()));
    CHECK(!leaf.Value()->FindTextInputService());

    auto outerHost = SharedPtr<UiHostServices>::TryMake(allocator, allocator);
    auto innerHost = SharedPtr<UiHostServices>::TryMake(allocator, allocator);
    REQUIRE(outerHost && innerHost);
    outerHost.Value()->SetTextInputService(outer.Value());
    innerHost.Value()->SetTextInputService(inner.Value());
    outerHost.Value()->SetFrameTime({2.5, 0.25});
    REQUIRE(root.Value()->SetUiHostServices(outerHost.Value()));
    CHECK(leaf.Value()->FindTextInputService().Get() == outer.Value().Get());
    CHECK(leaf.Value()->FindFrameTime().current == 2.5);
    REQUIRE(middle.Value()->SetUiHostServices(innerHost.Value()));
    CHECK(leaf.Value()->FindTextInputService().Get() == inner.Value().Get());
    CHECK(root.Value()->FindTextInputService().Get() == outer.Value().Get());
    CHECK(leaf.Value()->FindFrameTime().current == 0);

    // 빈 포인터는 뗀다.
    REQUIRE(middle.Value()->SetUiHostServices({}));
    CHECK(leaf.Value()->FindTextInputService().Get() == outer.Value().Get());
    CHECK(middle.Value()->RemoveChild(*leaf.Value()));
    CHECK(!leaf.Value()->FindTextInputService());
    CHECK(!leaf.Value()->FindUiHostServices());
}

} // namespace text_input_positions_tests_file
