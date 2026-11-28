#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/StyleSet.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/EditableText.h>
#include <UiFramework/Controls/SpinBox.h>
#include <UiFramework/Text/TextLabel.h>
#include <UiFramework/Text/TextShaper.h>

// 숫자 입력(스핀 상자) 시험이다(Docs/Design/UiToolkit 24.5절 — UE SSpinBox): 스타일 키·값, 표시
// 글자(자릿수·단위), 채움 비율(지수·중립), 끌기(문턱·범위 있음·없음·Delta 맞춤·확정 사건·커서
// 잠금·Esc 되돌림), 키, 글자 모드(진입·수식 확정·잘못된 글자·초점 잃음), 휠(글자 모드 초점일 때만),
// 터치 끌기.

namespace rocks::ui {

namespace {

// 코드 포인트마다 8 px 폭 클러스터 하나다.
class SpinTestShaper final : public TextShaper {
public:
    explicit SpinTestShaper(Allocator& allocator) noexcept : allocator_(allocator) {
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

        return label.SetShapedText(view.source, clusters.View(), {}, size, false);
    }

private:
    Allocator& allocator_;
};

// 잠금·풀기 자리를 적는 커서 서비스다(23.5절).
class SpinCursorProbe final : public CursorService {
public:
    Result<void> LockPointer(Vec2 at) noexcept override {
        ++locks;
        lockedAt = at;
        return Result<void>::Success();
    }

    void UnlockPointer(Vec2 releaseAt) noexcept override {
        ++unlocks;
        releasedAt = releaseAt;
    }

    int locks = 0;
    int unlocks = 0;
    Vec2 lockedAt{};
    Vec2 releasedAt{};
};

constexpr float SpinFontSize = 12.0f;
constexpr Vec2 SpinView{120, 24};

// 앱 루트 하나인 스핀 상자와 사건 기록이다.
struct SpinBench {
    explicit SpinBench(Allocator& allocator) noexcept : app(allocator) {
    }

    Result<void> Build(Allocator& allocator) noexcept {
        auto shaper = SharedPtr<SpinTestShaper>::TryMake(allocator, allocator);
        if (!shaper) {
            return Result<void>::Failure(shaper.Error());
        }

        auto made = SharedPtr<SpinBox>::TryMake(allocator, allocator, shaper.Value(), Platform{});
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        spin = made.Value();
        auto ready = spin->Initialize(SpinFontSize);
        if (!ready) {
            return ready;
        }

        spin->SetOnChanged(Delegate<void(double)>::CreateLambda([this](double value) noexcept {
            ++changes;
            lastChanged = value;
        }));
        spin->SetOnCommitted(Delegate<void(double, SpinCommitReason)>::CreateLambda(
            [this](double value, SpinCommitReason reason) noexcept {
                ++commits;
                lastCommitted = value;
                lastReason = reason;
            }));
        spin->SetOnDragBegin(Delegate<void()>::CreateLambda([this]() noexcept { ++begins; }));
        spin->SetOnDragEnd(Delegate<void(double)>::CreateLambda([this](double value) noexcept {
            ++ends;
            lastEnded = value;
        }));
        app.SetRoot(spin);
        return Layout();
    }

    Result<void> Layout() noexcept {
        return app.PrepareLayout(Geometry::Root(SpinView, {}), 1);
    }

    Result<bool> Pointer(PointerEventType type, Vec2 at,
                         PointerSource source = PointerSource::Mouse) noexcept {
        return app.RoutePointer({.type = type, .position = at, .source = source});
    }

    Result<bool> Key(input::Key key) noexcept {
        return app.RouteKey(0, input::InputEvent::KeyDown(key));
    }

    // 글자를 한 자씩 친다(초점 편집기로 간다).
    void Type(StringView text) noexcept {
        for (usize i = 0; i < text.ByteCount(); ++i) {
            (void)app.RouteKey(0, input::InputEvent::Text(static_cast<uint8>(text.Data()[i])));
        }
    }

    StringView Shown() const noexcept {
        return spin->Editor() ? spin->Editor()->GetText() : StringView{};
    }

    Application app;
    SharedPtr<SpinBox> spin;
    int changes = 0;
    int commits = 0;
    int begins = 0;
    int ends = 0;
    double lastChanged = 0;
    double lastCommitted = 0;
    double lastEnded = 0;
    SpinCommitReason lastReason = SpinCommitReason::Count;
};

bool SpinNear(double a, double b) noexcept {
    return math::Abs(a - b) < 1e-4;
}

} // namespace

TEST(UiFramework_SpinBoxStylesAreRegisteredWithUeValues) {
    SystemAllocator allocator;
    StyleSet styles(allocator);
    REQUIRE(AddCoreStyles(styles));
    const SpinBoxStyle* spin = styles.Find<SpinBoxStyle>("SpinBox");
    const SpinBoxStyle* entry = styles.Find<SpinBoxStyle>("NumericEntrySpinBox");
    const SpinBoxStyle* dark = styles.Find<SpinBoxStyle>("NumericEntrySpinBox_Dark");
    REQUIRE(spin && entry && dark);
    CHECK(*spin == DefaultSpinBoxStyle() && *entry == DefaultSpinBoxStyle());
    CHECK(spin->background ==
          StyleBrush::RoundedBox(ThemeSlot::Input, 4, ThemeSlot::InputOutline, 1));
    CHECK(spin->activeBackground ==
          StyleBrush::RoundedBox(ThemeSlot::Input, 4, ThemeSlot::Primary, 1));
    CHECK(spin->activeFill == StyleBrush::RoundedBox(ThemeSlot::Primary, 4));
    CHECK(spin->inactiveFill == StyleBrush::RoundedBox(ThemeSlot::Dropdown, 4));
    CHECK(spin->textPadding == SpinBoxTextPadding && spin->insetPadding == SpinBoxInsetPadding);
    CHECK(spin->foreground == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    CHECK(dark->activeBackground.mode == BrushMode::None);
    CHECK(dark->inactiveFill == StyleBrush::RoundedBox(ThemeSlot::Secondary, 4));
    CHECK(dark->textPadding == Vec4{});
}

TEST(UiFramework_SpinBoxShowsFormattedValuesAndFillFractions) {
    SystemAllocator allocator;
    SpinBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    // 기본 자릿수 1–6(UE).
    REQUIRE(bench.spin->SetValue(2.5));
    CHECK(bench.Shown() == "2.5");
    REQUIRE(bench.spin->SetValue(3));
    CHECK(bench.Shown() == "3.0");
    // 단위 칸.
    bench.spin->SetUnits({.underlying = NumericUnit::Centimeters});
    CHECK(bench.Shown() == "3.0 cm");
    bench.spin->SetUnits({});
    // 정수 형식.
    bench.spin->SetNumericKind(NumericKind::Int32);
    REQUIRE(bench.spin->SetValue(7.6));
    CHECK(bench.spin->Value() == 8 && bench.Shown() == "8");
    bench.spin->SetNumericKind(NumericKind::Double);
    // 범위가 없으면 채움 막대도 없다. 값 범위가 둘 다 있으면 그 비율이다.
    CHECK(!bench.spin->FillFraction());
    REQUIRE(bench.spin->SetValueRange(0.0, 100.0));
    REQUIRE(bench.spin->SetValue(50));
    REQUIRE(bench.spin->FillFraction());
    CHECK(bench.spin->FillFraction().Value() == 0.5f);
    // 지수 2: 1 - (1 - 0.5)^2.
    REQUIRE(bench.spin->SetSliderExponent(2));
    CHECK(SpinNear(bench.spin->FillFraction().Value(), 0.75));
    // 슬라이더 범위가 값 범위보다 앞선다.
    REQUIRE(bench.spin->SetSliderExponent(1));
    REQUIRE(bench.spin->SetSliderRange(0.0, 200.0));
    CHECK(bench.spin->FillFraction().Value() == 0.25f);
    // 값 범위로 자른다.
    REQUIRE(bench.spin->SetValue(500));
    CHECK(bench.spin->Value() == 100);
    CHECK(!bench.spin->SetValueRange(5.0, 1.0));
    bench.app.Shutdown();
}

TEST(UiFramework_SpinBoxDragsWithinTheSliderRangeAndLocksTheCursor) {
    SystemAllocator allocator;
    SpinBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    auto cursor = SharedPtr<SpinCursorProbe>::TryMake(allocator);
    REQUIRE(cursor && bench.app.SetCursorService(cursor.Value()));
    REQUIRE(bench.spin->SetValueRange(0.0, 100.0));
    REQUIRE(bench.spin->SetDelta(1));
    REQUIRE(bench.spin->SetValue(42));
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    // 문턱(5 px) 안은 끌기가 아니다.
    REQUIRE(bench.Pointer(PointerEventType::Move, {14, 12}));
    CHECK(!bench.spin->IsDragging() && bench.begins == 0);
    // 문턱을 넘으면 끌기 시작 — 그 자리에서 커서를 잠근다(값은 아직 그대로).
    REQUIRE(bench.Pointer(PointerEventType::Move, {16, 12}));
    CHECK(bench.spin->IsDragging() && bench.begins == 1 && bench.spin->Value() == 42);
    CHECK(cursor.Value()->locks == 1 && cursor.Value()->lockedAt == Vec2(16, 12));
    // 잠근 뒤 이동량 12 px: 채움 0.42 × 120 + 12 = 62.4 → 52%(Delta 1에 맞춤).
    PointerEvent moved{.type = PointerEventType::Move, .position = {16, 12}};
    moved.hasDelta = true;
    moved.delta = {12, 0};
    REQUIRE(bench.app.RoutePointer(moved));
    CHECK(bench.spin->Value() == 52 && bench.lastChanged == 52);
    // 놓으면 확정(Spin)·끌기 끝이고, 커서는 잠금 자리로 돌아간다.
    REQUIRE(bench.Pointer(PointerEventType::Up, {16, 12}));
    CHECK(!bench.spin->IsDragging() && bench.ends == 1 && bench.lastEnded == 52);
    CHECK(bench.lastReason == SpinCommitReason::Spin && bench.lastCommitted == 52);
    CHECK(cursor.Value()->unlocks == 1 && cursor.Value()->releasedAt == Vec2(16, 12));
    CHECK(!bench.spin->IsEditing());
    // 끄는 중 Esc는 끌기 전 값으로 되돌린다.
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Move, {20, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Move, {40, 12}));
    CHECK(bench.spin->Value() != 52);
    REQUIRE(bench.Key(input::Key::Escape));
    CHECK(bench.spin->Value() == 52 && bench.lastCommitted == 52 && !bench.spin->IsDragging());
    (void)bench.Pointer(PointerEventType::Up, {40, 12});
    bench.app.Shutdown();
}

TEST(UiFramework_SpinBoxUnboundedDragScalesWithTheValueAndHonorsMultipliers) {
    SystemAllocator allocator;
    SpinBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    REQUIRE(bench.spin->SetValue(10));
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Move, {16, 12}));
    // 범위 없음: 12 / 120 × max(|10|, 1) × 단계 1 = 1(서비스가 없으면 이동량은 위치 차이).
    REQUIRE(bench.Pointer(PointerEventType::Move, {28, 12}));
    CHECK(SpinNear(bench.spin->Value(), 11));
    // Shift는 ×10이다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftShift)));
    REQUIRE(bench.Pointer(PointerEventType::Move, {40, 12}));
    CHECK(SpinNear(bench.spin->Value(), 11 + 12.0 / 120.0 * 11 * 10));
    (void)bench.app.RouteKey(0, input::InputEvent::KeyUp(input::Key::LeftShift));
    REQUIRE(bench.Pointer(PointerEventType::Up, {40, 12}));
    // 선형 감도와 Delta: 12 / 6 × Delta 0.5^1 × 1 = 1.
    REQUIRE(bench.spin->SetValue(0));
    bench.spin->SetLinearDeltaSensitivity(6);
    REQUIRE(bench.spin->SetDelta(0.5));
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Move, {16, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Move, {28, 12}));
    CHECK(SpinNear(bench.spin->Value(), 1));
    REQUIRE(bench.Pointer(PointerEventType::Up, {28, 12}));
    // 끌기를 끄면 값이 바뀌지 않는다.
    bench.spin->SetSliderEnabled(false);
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Move, {60, 12}));
    CHECK(SpinNear(bench.spin->Value(), 1) && !bench.spin->IsDragging());
    (void)bench.Pointer(PointerEventType::Up, {60, 12});
    bench.app.Shutdown();
}

TEST(UiFramework_SpinBoxArrowKeysStepAndClampToTheSliderRange) {
    SystemAllocator allocator;
    SpinBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    REQUIRE(bench.spin->SetValueRange(0.0, 100.0));
    REQUIRE(bench.spin->SetValue(5));
    REQUIRE(bench.app.SetFocus(0, bench.spin, FocusCause::Pointer));
    // Delta가 없으면 기본 단계 1(범위 > 10)이다.
    REQUIRE(bench.Key(input::Key::Up));
    CHECK(bench.spin->Value() == 6 && bench.lastReason == SpinCommitReason::ArrowKey);
    REQUIRE(bench.Key(input::Key::Left));
    CHECK(bench.spin->Value() == 5);
    // Delta가 있으면 Delta다. 슬라이더 범위로 자른 뒤 Delta에 맞춘다(UE — 5 + 4 → 6 → 8, 슬라이더
    // 범위는 느슨하다).
    REQUIRE(bench.spin->SetDelta(4));
    REQUIRE(bench.spin->SetSliderRange(0.0, 6.0));
    REQUIRE(bench.Key(input::Key::Right));
    CHECK(bench.spin->Value() == 8);
    // 값 범위는 넘지 않는다(UE는 맞춘 값이 최대를 넘는다 — 24.5절 보강).
    REQUIRE(bench.spin->SetValueRange(0.0, 6.0));
    REQUIRE(bench.spin->SetValue(5));
    REQUIRE(bench.Key(input::Key::Right));
    CHECK(bench.spin->Value() == 6);
    bench.app.Shutdown();
}

TEST(UiFramework_SpinBoxTypesExpressionsInTextMode) {
    SystemAllocator allocator;
    SpinBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    REQUIRE(bench.spin->SetValueRange(0.0, 100.0));
    REQUIRE(bench.spin->SetValue(3));
    // 끌지 않고 놓으면 글자 모드 — 편집기가 초점을 받는다.
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Up, {10, 12}));
    REQUIRE(bench.spin->IsEditing());
    CHECK(bench.app.GetFocus(0).Get() == bench.spin->Editor());
    // 전체 선택이라 친 글자가 바꾼다. 잘못된 글자('a')는 앞까지만 남는다.
    bench.Type("10+5*2");
    CHECK(bench.Shown() == "10+5*2");
    bench.Type("a");
    CHECK(bench.Shown() == "10+5*2");
    // Enter: 확정하고 글자 모드에 남는다.
    REQUIRE(bench.Key(input::Key::Enter));
    CHECK(bench.spin->Value() == 20 && bench.lastReason == SpinCommitReason::Enter);
    CHECK(bench.spin->IsEditing() && bench.Shown() == "20.0");
    // 값 범위로 자른다.
    bench.Type("*=10");
    REQUIRE(bench.Key(input::Key::Enter));
    CHECK(bench.spin->Value() == 100);
    // 글자 모드 초점에서는 휠이 바꾼다(기본 단계 1).
    PointerEvent wheel{.type = PointerEventType::Wheel, .position = {10, 12}};
    wheel.wheel = -1;
    REQUIRE(bench.app.RoutePointer(wheel));
    CHECK(bench.spin->Value() == 99 && bench.Shown() == "99.0");
    // 초점을 잃으면 확정하고 표시 모드로 돌아간다.
    bench.Type("7");
    REQUIRE(bench.app.SetFocus(0, SharedPtr<Element>{}));
    CHECK(bench.spin->Value() == 7 && bench.lastReason == SpinCommitReason::FocusLost);
    CHECK(!bench.spin->IsEditing());
    // 표시 모드에서는 휠이 바꾸지 않는다.
    REQUIRE(bench.app.RoutePointer(wheel));
    CHECK(bench.spin->Value() == 7);
    // 읽지 못한 글자는 값을 바꾸지 않는다.
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}));
    REQUIRE(bench.Pointer(PointerEventType::Up, {10, 12}));
    bench.Type("1/0");
    REQUIRE(bench.Key(input::Key::Enter));
    CHECK(bench.spin->Value() == 7 && bench.Shown() == "7.0");
    bench.app.Shutdown();
}

TEST(UiFramework_SpinBoxTouchDragChangesTheValueWithoutCursorLock) {
    SystemAllocator allocator;
    SpinBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    auto cursor = SharedPtr<SpinCursorProbe>::TryMake(allocator);
    REQUIRE(cursor && bench.app.SetCursorService(cursor.Value()));
    REQUIRE(bench.spin->SetValueRange(0.0, 100.0));
    REQUIRE(bench.spin->SetDelta(1));
    REQUIRE(bench.spin->SetValue(42));
    REQUIRE(bench.Pointer(PointerEventType::Down, {10, 12}, PointerSource::Touch));
    REQUIRE(bench.Pointer(PointerEventType::Move, {16, 12}, PointerSource::Touch));
    REQUIRE(bench.Pointer(PointerEventType::Move, {28, 12}, PointerSource::Touch));
    CHECK(bench.spin->Value() == 52 && cursor.Value()->locks == 0);
    REQUIRE(bench.Pointer(PointerEventType::Up, {28, 12}, PointerSource::Touch));
    CHECK(bench.ends == 1 && bench.lastCommitted == 52);
    bench.app.Shutdown();
}

} // namespace rocks::ui
