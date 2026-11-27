#include <Core/Memory/SystemAllocator.h>
#include <Object/NativeClass.h>
#include <Object/PropertyPath.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>

// UV7 Widget 층 수명·진단 시험이다(Docs/Design/UiToolkit 8장 UV7, 21.4·21.5절).
//   - 명시 해제: OnConstruct·OnDestruct가 짝, 두 번 해제해도 통지 한 번
//   - 미룬 해체: 트리를 버리면 소멸자 안에서는 0회, 다음 프레임에 정확히 1회, 그동안 GC되지 않음
//   - 앱에 붙지 않은 채 버린 Element: OnDestruct 없음(경고만)
//   - 결합 표 진단: 없는 필드 NotFound, 형식이 다른 필드 TypeMismatch, 같은 필드 둘 AlreadyExists,
//     빈 값 InvalidArgument(이벤트 결합 진단은 WidgetEventBindingTests)

namespace rocks::ui {

namespace {

// 수명 hook을 세는 시험 화면이다(리플렉션 헤더가 아니라 native 클래스로 등록한다).
class LifecycleProbe final : public UserWidget {
    ROCKS_NATIVE_TYPE(LifecycleProbe, rocks::ui::UserWidget, 0xe51779a7be0d4833ull,
                      0xb178c1ed3525c3fcull)

public:
    explicit LifecycleProbe(Allocator& allocator) noexcept : UserWidget(allocator) {
    }

    void OnConstruct() noexcept override {
        ++constructs;
    }

    void OnDestruct() noexcept override {
        ++destructs;
    }

    uint32 constructs = 0;
    uint32 destructs = 0;
};

struct LifecycleBench {
    explicit LifecycleBench(Allocator& allocator) noexcept
        : types(allocator), modules(types, allocator), objects(allocator, types, {0x554957, 96}) {
    }

    TypeRegistry types;
    ModuleRegistration modules;
    ObjectRegistry objects;
};

const LifecycleProbe* FindProbe(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto probe = objects.Resolve<LifecycleProbe>(objects.OwnerAccess(), handle);
    return probe ? probe.Value().Get().Value() : nullptr;
}

const Geometry LifecycleViewport = Geometry::Root({100, 60}, {});

} // namespace

TEST(UiWidget_ExplicitReleasePairsConstructAndDestructOnce) {
    SystemAllocator allocator;
    LifecycleBench bench(allocator);
    REQUIRE(RegisterWidgetTypes(bench.modules));
    auto token = RegisterNativeClass<LifecycleProbe>(bench.types);
    REQUIRE(token);
    const auto access = bench.objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto made = bench.objects.NewObject<LifecycleProbe>(access, ObjectTypeIdOf<LifecycleProbe>());
    REQUIRE(made);
    const ObjectHandle handle = made.Value().Handle();
    auto probe = bench.objects.ResolveForWrite<LifecycleProbe>(access, handle);
    REQUIRE(probe);
    auto& widget = *probe.Value().Get().Value();
    {
        auto live = widget.TakeElement(bench.objects);
        REQUIRE(live);
        CHECK(widget.constructs == 1 && widget.destructs == 0);
        widget.ReleaseElements();
        CHECK(widget.destructs == 1);
        // 두 번 해제해도 통지는 한 번이다.
        widget.ReleaseElements();
    }

    // Element를 마지막으로 놓아도(이미 해제됨) 더 부르지 않는다.
    CHECK(widget.constructs == 1 && widget.destructs == 1);
}

TEST(UiWidget_DroppedTreeDefersDestructToTheNextFrame) {
    SystemAllocator allocator;
    LifecycleBench bench(allocator);
    REQUIRE(RegisterWidgetTypes(bench.modules));
    auto token = RegisterNativeClass<LifecycleProbe>(bench.types);
    REQUIRE(token);
    const auto access = bench.objects.OwnerAccess();
    ObjectAccessScope scope(access);
    Application app(allocator);
    ObjectHandle handle{};
    {
        auto made =
            bench.objects.NewObject<LifecycleProbe>(access, ObjectTypeIdOf<LifecycleProbe>());
        REQUIRE(made);
        handle = made.Value().Handle();
        auto probe = bench.objects.ResolveForWrite<LifecycleProbe>(access, handle);
        REQUIRE(probe);
        auto live = probe.Value().Get().Value()->TakeElement(bench.objects);
        REQUIRE(live);
        app.SetRoot(live.Value());
        REQUIRE(app.PrepareLayout(LifecycleViewport, 1));
    }

    // 앱이 트리를 버린다 — 소멸자 안에서는 사용자 hook이 돌지 않는다.
    app.SetRoot({});
    REQUIRE(FindProbe(bench.objects, handle));
    CHECK(FindProbe(bench.objects, handle)->destructs == 0);
    CHECK(app.PendingTeardownCount() == 1);
    // 대기열이 붙잡으므로 GC가 수거하지 않는다.
    REQUIRE(bench.objects.Collect(access));
    REQUIRE(FindProbe(bench.objects, handle));
    app.BeginFrame();
    REQUIRE(FindProbe(bench.objects, handle));
    CHECK(FindProbe(bench.objects, handle)->destructs == 1);
    app.BeginFrame();
    CHECK(FindProbe(bench.objects, handle)->destructs == 1);
}

TEST(UiWidget_ElementDroppedBeforeAnyApplicationSkipsDestruct) {
    SystemAllocator allocator;
    LifecycleBench bench(allocator);
    REQUIRE(RegisterWidgetTypes(bench.modules));
    auto token = RegisterNativeClass<LifecycleProbe>(bench.types);
    REQUIRE(token);
    const auto access = bench.objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto made = bench.objects.NewObject<LifecycleProbe>(access, ObjectTypeIdOf<LifecycleProbe>());
    REQUIRE(made);
    auto probe = bench.objects.ResolveForWrite<LifecycleProbe>(access, made.Value().Handle());
    REQUIRE(probe);
    auto& widget = *probe.Value().Get().Value();
    {
        auto live = widget.TakeElement(bench.objects);
        REQUIRE(live);
        CHECK(widget.constructs == 1);
    }

    // 측정된 적이 없어 통지할 자리가 없다 — 소멸자에서 부르지 않는다(경고 로그).
    CHECK(widget.destructs == 0);
}

TEST(UiWidget_BindingTableRejectsMissingAndDuplicateFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 97});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto made = objects.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
    REQUIRE(made);
    auto asset = objects.ResolveForWrite<WidgetAsset>(access, made.Value().Handle());
    REQUIRE(asset);
    auto* blueprint = asset.Value().Get().Value();
    REQUIRE(blueprint->Initialize(objects));
    auto tree = objects.ResolveForWrite<WidgetTree>(access, blueprint->Tree());
    REQUIRE(tree);
    auto box = tree.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<CheckBoxWidget>::Id(), "toggle");
    REQUIRE(box);
    REQUIRE(tree.Value().Get().Value()->SetRoot(objects, box.Value().Handle()));
    auto widget = objects.Resolve<Widget>(access, box.Value().Handle());
    REQUIRE(widget);
    const Guid target = widget.Value().Get().Value()->StableId();
    REQUIRE(blueprint->Compile(objects, allocator));

    // 컨트롤러(UserWidget)에 없는 필드다.
    constexpr Guid MissingField{0x6a0e2c55d3b04d1full, 0x9c41e7a20b8f3d66ull};
    REQUIRE(blueprint->AddBinding({MissingField, target, false}));
    // 같은 필드를 두 번 결합하지 않는다(한 필드에 Widget 하나).
    CHECK(blueprint->AddBinding({MissingField, target, true}).Error() == ErrorCode::AlreadyExists);
    // 빈 필드·빈 Widget은 받지 않는다.
    CHECK(blueprint->AddBinding({Guid{}, target, false}).Error() == ErrorCode::InvalidArgument);
    CHECK(blueprint->AddBinding({MissingField, Guid{}, false}).Error() ==
          ErrorCode::InvalidArgument);
    auto missing = blueprint->Compile(objects, allocator);
    REQUIRE(!missing);
    CHECK(missing.Error() == ErrorCode::NotFound);
}

TEST(UiWidget_BindingTableRejectsNonReferenceAndRepeatedFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 98});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto made = objects.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
    auto controller = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
    REQUIRE(made && controller);
    auto asset = objects.ResolveForWrite<WidgetAsset>(access, made.Value().Handle());
    REQUIRE(asset);
    auto* blueprint = asset.Value().Get().Value();
    REQUIRE(blueprint->Initialize(objects));
    auto tree = objects.ResolveForWrite<WidgetTree>(access, blueprint->Tree());
    REQUIRE(tree);
    auto box = tree.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<CheckBoxWidget>::Id(), "toggle");
    REQUIRE(box);
    REQUIRE(tree.Value().Get().Value()->SetRoot(objects, box.Value().Handle()));
    auto widget = objects.Resolve<Widget>(access, box.Value().Handle());
    REQUIRE(widget);
    const Guid target = widget.Value().Get().Value()->StableId();
    // 컨트롤러의 bool 필드(객체 참조가 아님)에 Widget을 결합할 수 없다.
    PropertyPathBuffer focusable;
    REQUIRE(
        ParsePropertyPath(objects, access, controller.Value().Handle(), "focusable", focusable));
    REQUIRE(blueprint->AddBinding({focusable.steps[0].fieldId, target, false}));
    auto mistyped = blueprint->Compile(objects, allocator);
    REQUIRE(!mistyped);
    CHECK(mistyped.Error() == ErrorCode::TypeMismatch);
}

} // namespace rocks::ui
