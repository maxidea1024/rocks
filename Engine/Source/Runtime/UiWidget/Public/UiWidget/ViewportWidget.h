#pragma once

#include <UiWidget/ContentWidget.h>
#include <UiWidget/ViewportWorld.h>

namespace rocks::ui {

// 외부에서 그린 장면 텍스처를 표시하고 자식 위젯 하나를 화면 위에 배치한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ViewportWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ViewportWidget(Allocator& allocator) noexcept;

    // 네이티브 뷰포트가 표시 중인 월드를 소유한다. 표시 전 스폰 또는 화면 재구성 사이에
    // 액터를 유지하려면 호출자가 AcquireViewportWorld에서 받은 수명 핸들을 보관한다.
    Result<SharedPtr<ViewportWorld>> AcquireViewportWorld(ObjectRegistry& objects) noexcept;
    Result<World*> GetViewportWorld(ObjectRegistry& objects) noexcept;
    Result<ObjectHandle> Spawn(ObjectRegistry& objects, TypeId actorType) noexcept;
    Result<void> SetSceneRenderer(ObjectRegistry& objects, ViewportWorld::Renderer renderer,
                                  bool depth = false) noexcept;
    Result<void> SetCamera(Vec3 location, Quat rotation, float fieldOfView) noexcept;
    Vec3 ViewLocation() const noexcept;
    Quat ViewRotation() const noexcept;
    float FieldOfView() const noexcept;
    Result<void> SetBackground(LinearColor color) noexcept;
    // 등록된 장면 텍스처는 호스트가 유지한다. 비우면 배경과 자식만 표시된다.
    void SetSceneTexture(SlotHandle texture) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "background") LinearColor background_ { 0, 0, 0, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "viewLocation") Vec3 viewLocation_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "viewRotation") Quat viewRotation_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "fieldOfView") float fieldOfView_ = 90;
    ROCKS_NATIVE() WeakPtr<ViewportWorld> world_;
    ROCKS_NATIVE() SlotHandle scene_ {};
};

} // namespace rocks::ui
