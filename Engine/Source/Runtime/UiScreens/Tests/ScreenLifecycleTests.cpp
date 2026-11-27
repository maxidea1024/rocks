#include <Core/Memory/SystemAllocator.h>
#include <Object/NativeClass.h>
#include <TestSupport/Test.h>
#include <UiScreens/Screen.generated.h>
#include <UiScreens/ScreenContainer.h>

// 화면 컨테이너 소멸 시험이다(Docs/Design/UiToolkit 21.4절 — UV7): 컨테이너가 버려지면 소멸자
// 안에서는 활성 화면의 OnDeactivated·OnDestruct가 돌지 않고, 다음 프레임에 그 순서로(UE는 비활성화
// 뒤 NativeDestruct) 한 번씩 돈다.

namespace rocks::ui {

namespace {

class LifecycleScreen final : public Screen {
    ROCKS_NATIVE_TYPE(LifecycleScreen, rocks::ui::Screen, 0xa43b4ab01e5b4e27ull,
                      0xab8fd0e49b29dee9ull)

public:
    explicit LifecycleScreen(Allocator& allocator) noexcept : Screen(allocator) {
    }

    void OnActivated() noexcept override {
        ++activations;
    }

    void OnDeactivated() noexcept override {
        ++deactivations;
        deactivatedBeforeDestruct = destructs == 0;
    }

    void OnDestruct() noexcept override {
        ++destructs;
    }

    uint32 activations = 0;
    uint32 deactivations = 0;
    uint32 destructs = 0;
    bool deactivatedBeforeDestruct = false;
};

} // namespace

TEST(UiScreens_DroppedContainerDefersDeactivationAndDestruct) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    auto token = RegisterNativeClass<LifecycleScreen>(types);
    REQUIRE(token);
    ObjectRegistry objects(allocator, types, {0x534352, 3});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    Application app(allocator);
    auto made = objects.NewObject<LifecycleScreen>(access, ObjectTypeIdOf<LifecycleScreen>());
    REQUIRE(made);
    const ObjectHandle handle = made.Value().Handle();
    const auto find = [&]() noexcept -> const LifecycleScreen* {
        auto screen = objects.Resolve<LifecycleScreen>(access, handle);
        return screen ? screen.Value().Get().Value() : nullptr;
    };
    {
        auto stack = SharedPtr<ScreenStack>::TryMake(allocator, objects, app, 0u);
        REQUIRE(stack);
        app.SetRoot(stack.Value());
        REQUIRE(stack.Value()->AddScreen(handle));
        REQUIRE(app.PrepareLayout(Geometry::Root({100, 60}, {}), 1));
        REQUIRE(find());
        CHECK(find()->activations == 1);
        app.SetRoot({});
    }

    // 컨테이너가 부서졌다 — 소멸자 안에서는 사용자 hook이 0회다.
    REQUIRE(find());
    CHECK(find()->deactivations == 0 && find()->destructs == 0);
    CHECK(!find()->IsActive());
    CHECK(app.PendingTeardownCount() == 2);
    app.BeginFrame();
    REQUIRE(find());
    CHECK(find()->deactivations == 1 && find()->destructs == 1);
    CHECK(find()->deactivatedBeforeDestruct);
}

TEST(UiScreens_InactivePagesStillGetDeferredDestruct) {
    // 회귀(21.4절 리뷰): 비활성 페이지는 측정되지 않는다. 묶음을 붙을 때 기억하므로 컨테이너를
    // 버려도 다음 프레임에 OnDestruct를 받는다(OnConstruct와 짝).
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    auto token = RegisterNativeClass<LifecycleScreen>(types);
    REQUIRE(token);
    ObjectRegistry objects(allocator, types, {0x534352, 4});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    Application app(allocator);
    auto first = objects.NewObject<LifecycleScreen>(access, ObjectTypeIdOf<LifecycleScreen>());
    auto second = objects.NewObject<LifecycleScreen>(access, ObjectTypeIdOf<LifecycleScreen>());
    REQUIRE(first && second);
    const ObjectHandle under = first.Value().Handle();
    const ObjectHandle top = second.Value().Handle();
    const auto find = [&](ObjectHandle handle) noexcept -> const LifecycleScreen* {
        auto screen = objects.Resolve<LifecycleScreen>(access, handle);
        return screen ? screen.Value().Get().Value() : nullptr;
    };
    {
        auto stack = SharedPtr<ScreenStack>::TryMake(allocator, objects, app, 0u);
        REQUIRE(stack);
        app.SetRoot(stack.Value());
        // 같은 프레임에 둘을 넣는다 — 아래 화면은 한 번도 측정되지 않는다.
        REQUIRE(stack.Value()->AddScreen(under));
        REQUIRE(stack.Value()->AddScreen(top));
        REQUIRE(app.PrepareLayout(Geometry::Root({100, 60}, {}), 1));
        REQUIRE(find(under) && find(top));
        CHECK(find(under)->deactivations == 1 && find(top)->activations == 1);
        app.SetRoot({});
    }

    CHECK(find(under)->destructs == 0 && find(top)->destructs == 0);
    app.BeginFrame();
    CHECK(find(under)->destructs == 1);
    CHECK(find(top)->deactivations == 1 && find(top)->destructs == 1);
}

} // namespace rocks::ui
