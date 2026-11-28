#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiCore/StyleRegistry.h>
#include <UiCore/StyleSet.h>
#include <UiCore/Styles/CoreIcons.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/CheckBox.h>

// 체크 상자 형식 시험이다(Docs/Design/UiToolkit 23.3절 — UE
// ESlateCheckBoxType·"ToggleButtonCheckbox"·
// "ToggleButtonCheckboxAlt"·"TransparentCheckBox"·"RadioButton"): 스타일 키와 값, 토글 형식의 바탕
// 이미지·여백·원하는 크기, 라디오의 해제 이미지·전경 tint, 투명 토글, 앱 스타일 "Checkbox" 받기.

namespace rocks::ui {

namespace {

// 받은 키를 적고 키마다 다른 자리를 돌려주는 resolver다(벡터 이미지를 그리게 한다).
class StateIconProbe final : public VectorImageResolver {
public:
    Result<ResolvedVectorImage> Resolve(VectorImageKey key, uint32, uint32) noexcept override {
        lastKey = key;
        ResolvedVectorImage image;
        image.resource = SlotHandle{static_cast<uint32>(key.hash >> 48) + 1, 1};
        return Result<ResolvedVectorImage>::Success(image);
    }

    uint64 Revision() const noexcept override {
        return 0;
    }

    VectorImageKey lastKey{};
};

LinearColor CheckTheme(ThemeSlot slot) noexcept {
    return DefaultThemeColors().Get(slot);
}

// 크기를 정한 내용(레이블 대신)이다.
SharedPtr<Frame> MakeContent(Allocator& allocator, Vec2 size) noexcept {
    auto frame = SharedPtr<Frame>::TryMake(allocator, allocator);
    if (frame) {
        frame.Value()->SetDesiredSize(size);
        return frame.Value();
    }

    return {};
}

constexpr Vec2 ContentSize{40, 14};

} // namespace

TEST(UiFramework_CheckBoxTypeStylesAreRegisteredWithUeValues) {
    SystemAllocator allocator;
    StyleSet styles(allocator);
    REQUIRE(AddCoreStyles(styles));
    const CheckBoxStyle* toggle = styles.Find<CheckBoxStyle>("ToggleButtonCheckbox");
    const CheckBoxStyle* alt = styles.Find<CheckBoxStyle>("ToggleButtonCheckboxAlt");
    const CheckBoxStyle* transparent = styles.Find<CheckBoxStyle>("TransparentCheckBox");
    const CheckBoxStyle* radio = styles.Find<CheckBoxStyle>("RadioButton");
    REQUIRE(toggle && alt && transparent && radio);
    CHECK(toggle->type == CheckBoxType::ToggleButton && toggle->padding == ToggleButtonMargins);
    CHECK(toggle->checkedImage == StyleBrush::RoundedBox(ThemeSlot::Primary, 4));
    CHECK(toggle->uncheckedImage.mode == BrushMode::None);
    CHECK(toggle->uncheckedHoveredImage == StyleBrush::RoundedBox(ThemeSlot::Dropdown, 4));
    CHECK(alt->checkedForeground == ColorRef::FromTheme(ThemeSlot::White));
    CHECK(transparent->type == CheckBoxType::ToggleButton &&
          transparent->checkedImage.mode == BrushMode::None);
    CHECK(radio->type == CheckBoxType::CheckBox && radio->background.mode == BrushMode::None);
    CHECK(radio->uncheckedImage ==
          StyleBrush::Svg(icons::RadioOff, icons::Icon16, ThemeSlot::Dropdown));
    CHECK(radio->checkedImage ==
          StyleBrush::Svg(icons::RadioOn, icons::Icon16, ThemeSlot::Primary));
    CHECK(radio->padding == RadioButtonPadding && radio->boxSize == RadioButtonBoxSize);
}

TEST(UiFramework_ToggleButtonCheckBoxPaintsStateImageAsBackground) {
    SystemAllocator allocator;
    CheckBox toggle(allocator);
    REQUIRE(toggle.SetCheckBoxStyle(DefaultToggleButtonCheckBoxStyle()));
    auto content = MakeContent(allocator, ContentSize);
    REQUIRE(content && toggle.TryAddChild(content));
    // 상자 칸이 없고 내용은 사방 여백 (16, 2) 안이다.
    REQUIRE(toggle.Prepass(1));
    CHECK(toggle.GetDesiredSize() == Vec2(ContentSize.x + 32, ContentSize.y + 4));
    REQUIRE(toggle.Arrange(Geometry::Root({100, 20}, {})));
    const auto paint = [&](PaintList& list) noexcept { return toggle.OnPaint(list, 0); };
    // 해제·보통: 바탕 없음.
    PaintList empty(allocator);
    REQUIRE(paint(empty));
    CHECK(empty.Boxes().IsEmpty());
    // 해제·올림: Dropdown 둥근 상자가 요소 전체다.
    toggle.OnMouseEnter(0);
    PaintList hovered(allocator);
    REQUIRE(paint(hovered));
    REQUIRE(hovered.Boxes().Size() == 1);
    CHECK(hovered.Boxes()[0].color == CheckTheme(ThemeSlot::Dropdown));
    CHECK(hovered.Boxes()[0].geometry.size == Vec2(100, 20));
    toggle.OnMouseLeave(0);
    // 체크: Primary, 전경은 ForegroundHover.
    REQUIRE(toggle.SetState(CheckState::Checked));
    PaintList checked(allocator);
    REQUIRE(paint(checked));
    REQUIRE(checked.Boxes().Size() == 1);
    CHECK(checked.Boxes()[0].color == CheckTheme(ThemeSlot::Primary));
    CHECK(toggle.GetForegroundColor() == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    // 투명 토글은 바탕을 그리지 않는다.
    REQUIRE(toggle.SetCheckBoxStyle(DefaultTransparentCheckBoxStyle()));
    PaintList transparent(allocator);
    REQUIRE(paint(transparent));
    CHECK(transparent.Boxes().IsEmpty());
}

TEST(UiFramework_RadioButtonDrawsUncheckedAndCheckedImages) {
    SystemAllocator allocator;
    StateIconProbe probe;
    CheckBox radio(allocator);
    REQUIRE(radio.SetCheckBoxStyle(DefaultRadioButtonStyle()));
    REQUIRE(radio.Arrange(Geometry::Root({80, 20}, {})));
    const auto paint = [&](PaintList& list) noexcept {
        list.SetVectorImageResolver(&probe);
        // 라디오 전경은 White다 — 그리기 문맥 전경도 White로 둔다(상속 tint 확인).
        return radio.OnPaint(list, 0);
    };
    // 해제도 이미지(RadioOff × Dropdown)를 그린다 — 바탕은 없다.
    PaintList unchecked(allocator);
    REQUIRE(paint(unchecked));
    REQUIRE(unchecked.Boxes().Size() == 1);
    CHECK(probe.lastKey == icons::RadioOff);
    CHECK(unchecked.Boxes()[0].geometry.size == RadioButtonBoxSize);
    // 체크: RadioOn.
    REQUIRE(radio.SetState(CheckState::Checked));
    PaintList checked(allocator);
    REQUIRE(paint(checked));
    REQUIRE(checked.Boxes().Size() == 1);
    CHECK(probe.lastKey == icons::RadioOn);
    // 내용은 상자(16) 오른쪽 여백 4 뒤다.
    CHECK(radio.GetContentPadding().x == RadioButtonBoxSize.x + RadioButtonPadding.x);
}

TEST(UiFramework_CheckBoxTakesTheAppStyleUnlessExplicit) {
    SystemAllocator allocator;
    auto registry = SharedPtr<StyleRegistry>::TryMake(allocator, allocator);
    auto core = SharedPtr<StyleSet>::TryMake(allocator, allocator);
    REQUIRE(registry && core && AddCoreStyles(*core.Value()));
    // 앱 스타일의 "Checkbox"를 라디오 값으로 바꿔 둔다.
    REQUIRE(core.Value()->Set<CheckBoxStyle>("Checkbox", DefaultRadioButtonStyle()));
    REQUIRE(registry.Value()->Register(CoreStyleName, core.Value()));
    auto box = SharedPtr<CheckBox>::TryMake(allocator, allocator);
    auto pinned = SharedPtr<CheckBox>::TryMake(allocator, allocator);
    REQUIRE(box && pinned);
    REQUIRE(pinned.Value()->SetCheckBoxStyle(DefaultToggleButtonCheckBoxStyle()));
    Application app(allocator);
    app.SetStyleRegistry(registry.Value());
    REQUIRE(app.SetAppStyle(CoreStyleName));
    app.SetRoot(box.Value());
    REQUIRE(app.PrepareLayout(Geometry::Root({100, 30}, {}), 1));
    CHECK(box.Value()->GetCheckBoxStyle() == DefaultRadioButtonStyle());
    app.SetRoot(pinned.Value());
    REQUIRE(app.PrepareLayout(Geometry::Root({100, 30}, {}), 1));
    CHECK(pinned.Value()->GetCheckBoxStyle() == DefaultToggleButtonCheckBoxStyle());
    app.Shutdown();
}

} // namespace rocks::ui
