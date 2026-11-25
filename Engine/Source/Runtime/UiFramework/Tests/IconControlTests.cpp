#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Styles/CoreIcons.h>
#include <UiFramework/CheckBox.h>
#include <UiFramework/ComboBox.h>

// 아이콘을 쓰는 컨트롤 시험이다(Docs/Design/UiToolkit 17.4절 UW4s5). 체크 표시·콤보 화살표가 SVG
// 아이콘 brush로 기록되는지(요청한 키·크기·tint)를 단언한다.

namespace rocks::ui {

namespace {

// 받은 키와 화소 크기를 적고, 키마다 다른 자리를 돌려주는 resolver다.
class IconProbe final : public VectorImageResolver {
public:
    Result<ResolvedVectorImage> Resolve(VectorImageKey key, uint32 width,
                                        uint32 height) noexcept override {
        lastKey = key;
        lastWidth = width;
        lastHeight = height;
        ResolvedVectorImage image;
        image.resource = SlotHandle{static_cast<uint32>(key.hash >> 48) + 1, 1};
        return Result<ResolvedVectorImage>::Success(image);
    }

    uint64 Revision() const noexcept override {
        return 0;
    }

    VectorImageKey lastKey{};
    uint32 lastWidth = 0;
    uint32 lastHeight = 0;
};

LinearColor IconTheme(ThemeSlot slot) noexcept {
    return DefaultThemeColors().Get(slot);
}

} // namespace

TEST(UiFramework_CheckBoxDrawsMarkIcons) {
    SystemAllocator allocator;
    IconProbe probe;
    CheckBox box(allocator);
    REQUIRE(box.Arrange(Geometry::Root({18, 18}, {})));
    PaintList list(allocator);
    list.SetVectorImageResolver(&probe);
    const auto paint = [&]() noexcept {
        list.Clear();
        return box.OnPaint(list, 0);
    };
    // 해제: 상자만 그린다.
    REQUIRE(paint());
    CHECK(list.Boxes().Size() == 1);
    // 체크: 상자 전체(18x18)에 Check 아이콘을 Primary로 그린다(UE "Checkbox" 1667-1691행).
    REQUIRE(box.SetState(CheckState::Checked));
    REQUIRE(paint());
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(probe.lastKey == icons::Check && probe.lastWidth == 18 && probe.lastHeight == 18);
    CHECK(list.Boxes()[1].color == IconTheme(ThemeSlot::Primary));
    // 올림은 PrimaryHover다.
    box.OnMouseEnter(0);
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color == IconTheme(ThemeSlot::PrimaryHover));
    box.OnMouseLeave(0);
    // 미정: Indeterminate 아이콘이다.
    REQUIRE(box.SetState(CheckState::Indeterminate));
    REQUIRE(paint());
    CHECK(probe.lastKey == icons::Indeterminate);
    // resolver가 없으면 표시를 그리지 않는다(창·GPU 없는 시험).
    list.SetVectorImageResolver(nullptr);
    REQUIRE(paint());
    CHECK(list.Boxes().Size() == 1);
}

TEST(UiFramework_ComboBoxDrawsDownArrow) {
    SystemAllocator allocator;
    IconProbe probe;
    ComboBox combo(allocator);
    // 화살표 칸(2 + 16 + 2)을 내용 오른쪽 여백으로 비운다.
    CHECK(combo.GetContentPadding().z == ComboArrowPadding * 2 + ComboArrowSize);
    REQUIRE(combo.Arrange(Geometry::Root({120, 24}, {})));
    PaintList list(allocator);
    list.SetVectorImageResolver(&probe);
    REQUIRE(combo.OnPaint(list, 0));
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(probe.lastKey == icons::WideChevronDown);
    CHECK(probe.lastWidth == 16 && probe.lastHeight == 16);
    // 오른쪽 끝에서 버튼 여백과 화살표 여백만큼 들어가고 세로 가운데다.
    const auto bounds = list.Boxes()[1].geometry.WindowBounds().Value();
    const float right = combo.GetStyle().normalPadding.z;
    CHECK(bounds.max.x == 120 - right - ComboArrowPadding);
    CHECK(bounds.min.y == 4 && bounds.max.y == 20);
    // 색은 버튼 전경색(UE UseForeground)이다.
    CHECK(list.Boxes()[1].color == IconTheme(ThemeSlot::ForegroundHover));
}

} // namespace rocks::ui
