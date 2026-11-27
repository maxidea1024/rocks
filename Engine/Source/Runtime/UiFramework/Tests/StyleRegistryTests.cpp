#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/StyleRegistry.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>

// 스타일 등록부·앱 스타일 시험이다(Docs/Design/UiToolkit 20.11절 — UE
// FSlateStyleRegistry·FAppStyle· ParentStyleName): 부모 사슬, 명시하지 않은 부품이 활성 집합의 값을
// 받음, 명시 스타일은 그대로, 값이 바뀌면 다시 받음, 없는 집합은 거부.

namespace rocks::ui {

namespace {

constexpr StringView EditorStyleName = "EditorStyle";

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

} // namespace rocks::ui
