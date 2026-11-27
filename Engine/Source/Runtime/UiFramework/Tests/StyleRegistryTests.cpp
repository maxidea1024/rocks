#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/StyleRegistry.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiCore/Styles/GameWidgetStyles.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>

// 스타일 등록부·앱 스타일 시험이다(Docs/Design/UiToolkit 20.11절 — UE
// FSlateStyleRegistry·FAppStyle· ParentStyleName): 부모 사슬, 명시하지 않은 부품이 활성 집합의 값을
// 받음, 명시 스타일은 그대로, 값이 바뀌면 다시 받음, 없는 집합은 거부.

namespace rocks::ui {

namespace {

struct StyleBench {
    explicit StyleBench(Allocator& allocator) noexcept : app(allocator) {
    }

    SharedPtr<StyleRegistry> registry;
    SharedPtr<StyleSet> core;
    SharedPtr<StyleSet> editor;
    Application app;
};

bool Build(Allocator& allocator, StyleBench& bench) noexcept {
    auto registry = SharedPtr<StyleRegistry>::TryMake(allocator, allocator);
    auto core = SharedPtr<StyleSet>::TryMake(allocator, allocator);
    auto editor = SharedPtr<StyleSet>::TryMake(allocator, allocator);
    if (!registry || !core || !editor || !AddCoreStyles(*core.Value()) ||
        !editor.Value()->SetParentName(CoreStyleName) ||
        !registry.Value()->Register(CoreStyleName, core.Value()) ||
        !registry.Value()->Register(EditorStyleName, editor.Value())) {
        return false;
    }

    bench.registry = registry.Value();
    bench.core = core.Value();
    bench.editor = editor.Value();
    bench.app.SetStyleRegistry(bench.registry);
    return true;
}

} // namespace

TEST(UiFramework_StyleRegistryWalksParentSets) {
    SystemAllocator allocator;
    StyleBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    // EditorStyle에 없는 이름은 부모 CoreStyle에서 찾는다.
    CHECK(bench.registry->FindStyle<ButtonStyle>(*bench.editor, "PrimaryButton") != nullptr);
    CHECK(*bench.registry->FindStyle<ButtonStyle>(*bench.editor, "PrimaryButton") ==
          PrimaryButtonStyle());
    // 자기 값이 부모보다 먼저다.
    ButtonStyle custom = DefaultButtonStyle();
    custom.normalPadding = {7, 7, 7, 7};
    REQUIRE(bench.editor->Set<ButtonStyle>("PrimaryButton", custom));
    CHECK(bench.registry->FindStyle<ButtonStyle>(*bench.editor, "PrimaryButton")->normalPadding ==
          Vec4(7, 7, 7, 7));
    CHECK(!bench.registry->FindStyle<ButtonStyle>(*bench.editor, "Nothing"));
    // 없는 집합 이름은 거부하고 바꾸지 않는다.
    CHECK(bench.app.SetAppStyle("Missing").Error() == ErrorCode::NotFound);
}

TEST(UiFramework_ButtonTakesTheActiveAppStyleUnlessExplicit) {
    SystemAllocator allocator;
    StyleBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    ButtonStyle editorButton = DefaultButtonStyle();
    editorButton.normalPadding = {9, 3, 9, 3};
    REQUIRE(bench.editor->Set<ButtonStyle>("Button", editorButton));

    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto plain = SharedPtr<Button>::TryMake(allocator, allocator);
    auto fixed = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(root && plain && fixed);
    REQUIRE(plain.Value()->Construct({}));
    ButtonStyle explicitStyle = DefaultButtonStyle();
    explicitStyle.normalPadding = {1, 1, 1, 1};
    REQUIRE(fixed.Value()->Construct({.style = explicitStyle}));
    REQUIRE(root.Value()->TryAddChild(plain.Value()));
    REQUIRE(root.Value()->TryAddChild(fixed.Value()));
    bench.app.SetRoot(root.Value());
    const auto viewport = Geometry::Root({200, 100}, {});

    // CoreStyle이 활성이면 기본 버튼 값이다.
    REQUIRE(bench.app.SetAppStyle(CoreStyleName));
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle() == DefaultButtonStyle());
    // EditorStyle로 바꾸면 명시하지 않은 버튼만 바뀐다(측정 때 — 측정과 그리기가 같은 값).
    REQUIRE(bench.app.SetAppStyle(EditorStyleName));
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle().normalPadding == Vec4(9, 3, 9, 3));
    CHECK(fixed.Value()->GetStyle().normalPadding == Vec4(1, 1, 1, 1));
    // 집합의 값이 바뀌면 다음 배치에서 다시 받는다.
    editorButton.normalPadding = {4, 4, 4, 4};
    REQUIRE(bench.editor->Set<ButtonStyle>("Button", editorButton));
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle().normalPadding == Vec4(4, 4, 4, 4));
}

TEST(UiFramework_StandardStyleRegistryHoldsTheThreeUeSets) {
    SystemAllocator allocator;
    auto made = CreateStandardStyleRegistry(allocator);
    REQUIRE(made);
    const auto& registry = *made.Value();
    auto core = registry.FindSet(CoreStyleName);
    auto editor = registry.FindSet(EditorStyleName);
    auto game = registry.FindSet(GameWidgetStyleName);
    REQUIRE(core && editor && game);
    // 에디터 집합은 코어 값을 부모에서 받는다(UE 에디터 스타일이 다시 정하지 않음 — 20.11절).
    CHECK(*registry.FindStyle<ButtonStyle>(*editor, "Button") == DefaultButtonStyle());
    CHECK(*registry.FindStyle<ButtonStyle>(*editor, "PrimaryButton") == PrimaryButtonStyle());
    CHECK(*registry.FindStyle<Vec4>(*editor, "Menu.Label.ContentPadding") == Vec4(10, 2, 10, 2));
    // 게임 집합은 부모가 없다 — UMG에 없는 키는 찾지 못하고 부품 기본값이 쓰인다.
    CHECK(*registry.FindStyle<ButtonStyle>(*game, "Button") == GameButtonStyle());
    CHECK(!registry.FindStyle<ButtonStyle>(*game, "PrimaryButton"));

    // 앱 스타일이 게임 집합이면 명시하지 않은 버튼은 게임 값, 없는 키의 부품은 기본값이다.
    Application app(allocator);
    app.SetStyleRegistry(made.Value());
    REQUIRE(app.SetAppStyle(GameWidgetStyleName));
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto plain = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(root && plain);
    REQUIRE(plain.Value()->Construct({}));
    REQUIRE(root.Value()->TryAddChild(plain.Value()));
    app.SetRoot(root.Value());
    REQUIRE(app.PrepareLayout(Geometry::Root({200, 100}, {}), 1));
    CHECK(plain.Value()->GetStyle() == GameButtonStyle());
    // 에디터 집합으로 바꾸면 부모(코어)의 값이다.
    REQUIRE(app.SetAppStyle(EditorStyleName));
    REQUIRE(app.PrepareLayout(Geometry::Root({200, 100}, {}), 1));
    CHECK(plain.Value()->GetStyle() == DefaultButtonStyle());
}

TEST(UiFramework_StyleRevisionNeverRepeatsAndMissingKeysFallBack) {
    SystemAllocator allocator;
    StyleBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto plain = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(root && plain);
    REQUIRE(plain.Value()->Construct({}));
    REQUIRE(root.Value()->TryAddChild(plain.Value()));
    bench.app.SetRoot(root.Value());
    const auto viewport = Geometry::Root({200, 100}, {});
    REQUIRE(bench.app.SetAppStyle(EditorStyleName));

    // 값을 여러 번 바꾼 집합을 덜 바꾼 집합으로 같은 이름에 다시 등록해도 revision이 줄지 않고,
    // 버튼은 새 집합 값을 받는다(활성 집합을 이름으로 푼다).
    ButtonStyle custom = DefaultButtonStyle();
    for (int32 i = 0; i < 3; ++i) {
        custom.normalPadding = {static_cast<float>(i), 0, 0, 0};
        REQUIRE(bench.editor->Set<ButtonStyle>("Button", custom));
    }

    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle().normalPadding == Vec4(2, 0, 0, 0));
    auto replacement = SharedPtr<StyleSet>::TryMake(allocator, allocator);
    REQUIRE(replacement);
    REQUIRE(replacement.Value()->SetParentName(CoreStyleName));
    custom.normalPadding = {6, 6, 6, 6};
    REQUIRE(replacement.Value()->Set<ButtonStyle>("Button", custom));
    const uint64 before = bench.registry->Revision();
    REQUIRE(bench.registry->Register(EditorStyleName, replacement.Value()));
    CHECK(bench.registry->Revision() > before);
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle().normalPadding == Vec4(6, 6, 6, 6));

    // 루트를 바꾼 사이에 집합이 바뀌어도, 깨끗한 트리를 다시 붙이면 새 값을 받는다.
    auto other = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(other);
    bench.app.SetRoot(other.Value());
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    REQUIRE(bench.app.SetAppStyle(CoreStyleName));
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    bench.app.SetRoot(root.Value());
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle() == DefaultButtonStyle());

    // 활성 집합에 키가 없으면 앞서 받은 값이 아니라 내장 기본값이다.
    REQUIRE(bench.app.SetAppStyle(EditorStyleName));
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle().normalPadding == Vec4(6, 6, 6, 6));
    auto empty = SharedPtr<StyleSet>::TryMake(allocator, allocator);
    REQUIRE(empty);
    REQUIRE(bench.registry->Register(EditorStyleName, empty.Value()));
    REQUIRE(bench.app.PrepareLayout(viewport, 1));
    CHECK(plain.Value()->GetStyle() == DefaultButtonStyle());
}

} // namespace rocks::ui
