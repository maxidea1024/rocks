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

} // namespace rocks::ui
