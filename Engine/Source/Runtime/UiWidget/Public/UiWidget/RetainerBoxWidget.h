#pragma once

#include <UiWidget/ContentWidget.h>
#include <UiWidget/RetainerEffectMaterial.h>

namespace rocks::ui {

// 일정한 프레임 간격으로 자식의 텍스처를 갱신하는 컨테이너다. 캡처와 텍스처 게시를 호스트와
// 연동한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API RetainerBoxWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit RetainerBoxWidget(Allocator& allocator) noexcept;

    Result<void> SetPhase(uint32 phase, uint32 count) noexcept;
    // 에셋을 강한 GC 참조로 보관하고 화면에 전달할 셰이더의 불변 스냅숏을 만든다.
    Result<void> SetEffectMaterial(ObjectRegistry& objects, ObjectHandle material,
                                   StringView textureParameter = "Texture") noexcept;
    ObjectHandle GetEffectMaterial() const noexcept;
    Result<void> SetEffectTint(LinearColor tint) noexcept;
    void RequestRender() noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "effectMaterial")
    ObjectRef<RetainerEffectMaterial> effectMaterial_;
    ROCKS_PROPERTY(Name = "textureParameter") String textureParameter_;
    ROCKS_PROPERTY(Name = "effectTint") LinearColor effectTint_ { 1, 1, 1, 1 };
    ROCKS_NATIVE() SharedPtr<const RetainerEffect> effect_;
    ROCKS_PROPERTY(Name = "phase") uint32 phase_ = 0;
    ROCKS_PROPERTY(Name = "phaseCount") uint32 phaseCount_ = 1;
};

} // namespace rocks::ui
