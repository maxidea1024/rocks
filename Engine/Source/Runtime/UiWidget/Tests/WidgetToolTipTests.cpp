#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Popups/ToolTip.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/TextLabelWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WidgetTextShaper.h>

namespace rocks::ui {

namespace {

// 글자마다 폭 8의 클러스터를 만드는 셰이퍼다(툴팁 글자 시험용).
class TipTextShaper final : public WidgetTextShaper {
public:
    Result<void> Shape(TextLabel& label, const Text& value, float size) noexcept override {
        TextPersistentView view;
        auto persisted = value.TryGetPersistentForm(view);
        if (!persisted) {
            return persisted;
        }

        constexpr usize MaxClusters = 32;
        text::TextCluster clusters[MaxClusters];
        const usize count = view.source.ByteCount();
        if (count > MaxClusters) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize i = 0; i < count; ++i) {
            clusters[i] = {.byteEnd = i + 1, .advance = 8};
        }

        return label.SetShapedText(view.source, {clusters, count}, {}, size, false);
    }
};

} // namespace

TEST(UiWidget_ToolTipsUseToolTipBoxes) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 77});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto root = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(root);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, root.Value().Handle());
    REQUIRE(tree);
    auto* widgets = tree.Value().Get().Value();
    auto shaper = SharedPtr<TipTextShaper>::TryMake(allocator);
    REQUIRE(shaper);
    widgets->SetTextShaper(shaper.Value());
    auto ownerHandle =
        widgets->ConstructWidget(objects, GeneratedType<TextLabelWidget>::Id(), "Owner");
    auto contentHandle =
        widgets->ConstructWidget(objects, GeneratedType<BorderWidget>::Id(), "Tip");
    REQUIRE(ownerHandle && contentHandle);
    auto owner = objects.ResolveForWrite<TextLabelWidget>(access, ownerHandle.Value().Handle());
    REQUIRE(owner);
    auto* label = owner.Value().Get().Value();
    auto text = Text::Invariant(allocator, "Saves");
    REQUIRE(text);
    auto element = label->TakeElement(objects);
    REQUIRE(element);
    // 글자 툴팁은 밝은 글자 툴팁 상자다.
    REQUIRE(label->SetToolTipText(objects, text.Value()));
    auto plain = element.Value()->MakeToolTip();
    REQUIRE(plain);
    CHECK(StaticElementCast<ToolTip>(*plain.Value()).GetStyle() == DefaultToolTipStyle());
    // 사용자 위젯 툴팁은 바탕·여백 없는 상자에 담는다(UE SToolTip TextMargin 0, BorderImage null).
    REQUIRE(label->SetToolTip(objects, contentHandle.Value().Handle()));
    auto first = element.Value()->MakeToolTip();
    REQUIRE(first);
    CHECK(StaticElementCast<ToolTip>(*first.Value()).GetStyle() == BareToolTipStyle());
    // 닫힌 지난 상자에 남은 내용은 떼어 새 상자에 다시 담는다.
    auto second = element.Value()->MakeToolTip();
    REQUIRE(second);
    CHECK(second.Value().Get() != first.Value().Get());
    CHECK(first.Value()->ChildCount() == 0 && second.Value()->ChildCount() == 1);
    // 지난 상자가 아직 어딘가에 붙어 있으면(열려 있으면) 내용을 빼앗지 않는다.
    auto host = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(host && host.Value()->TryAddChild(second.Value()));
    auto busy = element.Value()->MakeToolTip();
    CHECK(!busy && busy.Error() == ErrorCode::Busy);
}

} // namespace rocks::ui
