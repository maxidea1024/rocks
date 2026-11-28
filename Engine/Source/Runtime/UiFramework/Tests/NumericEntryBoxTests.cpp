#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/CheckBox.h>
#include <UiFramework/Controls/EditableText.h>
#include <UiFramework/Controls/EditableTextBox.h>
#include <UiFramework/Controls/NumericEntryBox.h>
#include <UiFramework/Popups/ToolTip.h>
#include <UiFramework/Text/TextLabel.h>
#include <UiFramework/Text/TextShaper.h>

// 숫자 입력 칸 시험이다(Docs/Design/UiToolkit 24.6절 — UE SNumericEntryBox): 미정 표시와 스핀·상자
// 바꿈, 상자 경로 확정·미정 확정 받기, 레이블 자리(바깥·안쪽), 색 막대, 토글 비활성, 툴팁 글자.

namespace rocks::ui {

namespace {

// 코드 포인트마다 8 px 폭 클러스터 하나다.
class EntryTestShaper final : public TextShaper {
    // 마지막 글자(last)보다 먼저 만든다.
    Allocator& allocator_;

public:
    explicit EntryTestShaper(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    Result<void> Shape(TextLabel& label, const Text& text, float size) noexcept override {
        TextPersistentView view;
        auto got = text.TryGetPersistentForm(view);
        if (!got) {
            return got;
        }

        Array<text::TextCluster> clusters(allocator_);
        auto points = view.source.TryCodePoints();
        if (!points) {
            return Result<void>::Failure(points.Error());
        }

        usize index = 0;
        for (char32_t point : points.Value()) {
            (void)point;
            auto offset = view.source.TryByteOffsetOfCodePoint(++index);
            if (!offset) {
                return Result<void>::Failure(offset.Error());
            }

            auto added = clusters.TryPushBack({.byteEnd = offset.Value(), .advance = 8.0f});
            if (!added) {
                return added;
            }
        }

        last.Clear();
        (void)last.TryAssign(view.source);
        return label.SetShapedText(view.source, clusters.View(), {}, size, false);
    }

    String last{allocator_};
};

constexpr float EntryFontSize = 12.0f;
constexpr Vec2 EntryView{160, 24};

struct EntryBench {
    explicit EntryBench(Allocator& allocator) noexcept : app(allocator) {
    }

    Result<void> Build(Allocator& allocator, bool allowSpin) noexcept {
        auto made = SharedPtr<EntryTestShaper>::TryMake(allocator, allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        shaper = made.Value();
        auto box = SharedPtr<NumericEntryBox>::TryMake(allocator, allocator, shaper, Platform{});
        if (!box) {
            return Result<void>::Failure(box.Error());
        }

        entry = box.Value();
        auto ready = entry->Initialize(EntryFontSize, allowSpin);
        if (!ready) {
            return ready;
        }

        entry->SetOnValueCommitted(Delegate<void(double, SpinCommitReason)>::CreateLambda(
            [this](double value, SpinCommitReason reason) noexcept {
                ++commits;
                lastCommitted = value;
                lastReason = reason;
            }));
        app.SetRoot(entry);
        return Layout();
    }

    Result<void> Layout() noexcept {
        return app.PrepareLayout(Geometry::Root(EntryView, {}), 1);
    }

    StringView BoxText() const noexcept {
        return entry->TextBox()->Editor().GetText();
    }

    // 상자 편집기에 초점을 주고 글자를 친 뒤 Enter다.
    void TypeAndEnter(StringView text) noexcept {
        (void)app.SetFocus(0, entry->TextBox()->Editor(), FocusCause::Pointer);
        for (usize i = 0; i < text.ByteCount(); ++i) {
            (void)app.RouteKey(0, input::InputEvent::Text(static_cast<uint8>(text.Data()[i])));
        }

        (void)app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Enter));
    }

    Application app;
    SharedPtr<EntryTestShaper> shaper;
    SharedPtr<NumericEntryBox> entry;
    int commits = 0;
    double lastCommitted = 0;
    SpinCommitReason lastReason = SpinCommitReason::Count;
};

} // namespace

TEST(UiFramework_NumericEntryBoxShowsUndeterminedValuesInTheTextBox) {
    SystemAllocator allocator;
    EntryBench bench(allocator);
    REQUIRE(bench.Build(allocator, true));
    // 미정: 스핀은 접히고 상자가 "---"를 보인다(UE).
    REQUIRE(bench.entry->Spin() && bench.entry->TextBox());
    CHECK(!bench.entry->Value());
    CHECK(bench.entry->Spin()->GetVisibility() == Visibility::Collapsed);
    CHECK(bench.entry->TextBox()->GetVisibility() == Visibility::Visible);
    CHECK(bench.BoxText() == NumericUndeterminedText);
    // 값이 있으면 스핀이 보인다. 스핀 칸의 슬라이더 범위 기본은 0–100이다(UE).
    REQUIRE(bench.entry->SetValue(25.0));
    CHECK(bench.entry->Spin()->GetVisibility() == Visibility::Visible);
    CHECK(bench.entry->TextBox()->GetVisibility() == Visibility::Collapsed);
    CHECK(bench.entry->Spin()->Value() == 25);
    REQUIRE(bench.entry->Spin()->FillFraction());
    CHECK(bench.entry->Spin()->FillFraction().Value() == 0.25f);
    // 다시 미정.
    REQUIRE(bench.entry->SetValue({}));
    CHECK(bench.entry->Spin()->GetVisibility() == Visibility::Collapsed);
    REQUIRE(bench.entry->SetUndeterminedText("Multiple Values"));
    CHECK(bench.BoxText() == "Multiple Values");
    bench.app.Shutdown();
}

TEST(UiFramework_NumericEntryBoxCommitsTypedTextAndUndeterminedEntries) {
    SystemAllocator allocator;
    EntryBench bench(allocator);
    REQUIRE(bench.Build(allocator, false));
    REQUIRE(!bench.entry->Spin());
    REQUIRE(bench.entry->SetValueRange(0.0, 10.0));
    REQUIRE(bench.entry->SetValue(2.0));
    CHECK(bench.BoxText() == "2.0");
    // 상자 경로: 수식을 읽고 값 범위로 자른다.
    bench.TypeAndEnter("3*5");
    CHECK(bench.entry->Value() && bench.entry->Value().Value() == 10);
    CHECK(bench.commits == 1 && bench.lastReason == SpinCommitReason::Enter);
    CHECK(bench.BoxText() == "10.0");
    // 미정 값에 친 글자는 받는 쪽이 있으면 그대로 간다.
    REQUIRE(bench.app.SetFocus(0, SharedPtr<Element>{}));
    REQUIRE(bench.entry->SetValue({}));
    String received(allocator);
    bench.entry->SetOnUndeterminedCommitted(
        Delegate<void(StringView, SpinCommitReason)>::CreateLambda(
            [&](StringView text, SpinCommitReason) noexcept { (void)received.TryAssign(text); }));
    bench.TypeAndEnter("7");
    CHECK(received.View() == "7" && !bench.entry->Value());
    // 받는 쪽이 없으면 읽어 값으로 확정한다(스핀 칸이면 스핀으로 돌아간다).
    bench.entry->SetOnUndeterminedCommitted({});
    REQUIRE(bench.app.SetFocus(0, SharedPtr<Element>{}));
    bench.TypeAndEnter("4");
    CHECK(bench.entry->Value() && bench.entry->Value().Value() == 4);
    bench.app.Shutdown();
}

TEST(UiFramework_NumericEntryBoxPlacesLabelsAndTogglesTheField) {
    SystemAllocator allocator;
    EntryBench bench(allocator);
    REQUIRE(bench.Build(allocator, true));
    REQUIRE(bench.entry->SetValue(1.0));
    // 바깥 글자 레이블은 칸 왼쪽 별도 칸이다(여백 3).
    auto label = NumericEntryBox::MakeLabel(allocator, *bench.shaper, "X", EntryFontSize,
                                            ColorRef::FromTheme(ThemeSlot::White),
                                            ColorRef::FromTheme(ThemeSlot::Primary));
    REQUIRE(label && bench.entry->SetLabel(label.Value(), NumericLabelLocation::Outside));
    REQUIRE(bench.Layout());
    const float labelWidth = label.Value()->GetDesiredSize().x;
    CHECK(label.Value()->GetGeometry().render.Apply({}).x == NumericLabelPadding.x);
    CHECK(bench.entry->Spin()->GetGeometry().render.Apply({}).x ==
          labelWidth + NumericLabelPadding.x + NumericLabelPadding.z);
    CHECK(label.Value()->GetVisibility() == Visibility::HitTestInvisible);
    // 안쪽 색 막대는 칸 위에 겹친다(폭 2, 칸 높이).
    auto bar = NumericEntryBox::MakeNarrowColorLabel(allocator, {1, 0, 0, 1});
    REQUIRE(bar && bench.entry->SetLabel(bar.Value(), NumericLabelLocation::Inside));
    REQUIRE(bench.Layout());
    CHECK(bench.entry->Spin()->GetGeometry().render.Apply({}).x == 0);
    CHECK(bar.Value()->GetGeometry().size == Vec2(2, EntryView.y));
    CHECK(bar.Value()->GetGeometry().render.Apply({}).x == NumericLabelPadding.x);
    // 토글을 끄면 칸이 비활성이다.
    bool toggled = true;
    bench.entry->SetOnToggleChanged(
        Delegate<void(bool)>::CreateLambda([&](bool on) noexcept { toggled = on; }));
    REQUIRE(bench.entry->SetToggle(true, true));
    REQUIRE(bench.entry->Toggle());
    CHECK(bench.entry->Spin()->IsEnabled() && bench.entry->IsToggleChecked());
    // 사용자가 누르면 끈다(SetState는 프로그램 변경이라 알리지 않는다).
    REQUIRE(bench.Layout());
    const Geometry& toggle = bench.entry->Toggle()->GetGeometry();
    const Vec2 center = toggle.render.Apply(toggle.size * 0.5f);
    REQUIRE(bench.app.RoutePointer({.type = PointerEventType::Down, .position = center}));
    REQUIRE(bench.app.RoutePointer({.type = PointerEventType::Up, .position = center}));
    CHECK(!toggled && !bench.entry->Spin()->IsEnabled() && !bench.entry->TextBox()->IsEnabled());
    REQUIRE(bench.entry->SetToggle(false, true));
    CHECK(!bench.entry->Toggle() && bench.entry->Spin()->IsEnabled());
    bench.app.Shutdown();
}

TEST(UiFramework_NumericEntryBoxToolTipShowsPrefixValueAndRange) {
    SystemAllocator allocator;
    EntryBench bench(allocator);
    REQUIRE(bench.Build(allocator, true));
    REQUIRE(bench.entry->SetValueRange(0.0, 10.0));
    REQUIRE(bench.entry->SetValue(2.5));
    REQUIRE(bench.entry->SetToolTipPrefix("X: "));
    REQUIRE(bench.entry->HasToolTip());
    auto tip = bench.entry->MakeToolTip();
    REQUIRE(tip);
    CHECK(bench.shaper->last.View() == "X: 2.5 (0.0 \xE2\x80\x93 10.0)");
    // 거의 정수면 정확한 값을 지수 표기로 보인다(UE NearZeroWarning).
    REQUIRE(bench.entry->SetValueRange({}, {}));
    REQUIRE(bench.entry->SetValue(1e-12));
    tip = bench.entry->MakeToolTip();
    REQUIRE(tip);
    CHECK(bench.shaper->last.View().StartsWith("X: Exact value: 1"));
    bench.app.Shutdown();
}

} // namespace rocks::ui
