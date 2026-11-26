#pragma once

#include <Core/Containers/HashMap.h>
#include <UiFramework/Application/Platform.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

class WidgetTextShaper;

// 화면 한 개에 속한 위젯을 생성하고 루트와 안정 ID 색인을 관리한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WidgetTree final : public Object {
    ROCKS_GENERATED_BODY()

public:
    // 생성 결과의 임시 루트는 슬롯 또는 트리 루트에 연결할 때까지 유지한다.
    Result<RootRef<Widget>> ConstructWidget(ObjectRegistry& objects, TypeId type,
                                            StringView name) noexcept;
    Result<void> SetRoot(ObjectRegistry& objects, ObjectHandle widget) noexcept;
    // 이 트리에서 새로 만드는 텍스트 위젯이 사용할 서비스다. 게임이나 에디터 화면별로
    // 서로 다른 로캘과 글리프 아틀라스를 연결할 수 있다. 이미 구성한 레이블은 기존 서비스를 쓴다.
    void SetTextShaper(const SharedPtr<WidgetTextShaper>& shaper) noexcept;
    const SharedPtr<WidgetTextShaper>& TextShaper() const noexcept;
    // 네이티브 요소를 만들기 전에 창 호스트의 OS 서비스를 지정한다.
    void SetPlatform(Platform platform) noexcept;
    Platform GetPlatform() const noexcept;
    ObjectHandle RootWidget() const noexcept;
    // 슬롯 순서의 깊이 우선 순회다. false를 반환하면 즉시 중단한다.
    // 순회 중 구조를 바꾸려면 GetAllWidgets로 먼저 핸들의 스냅샷을 얻는다.
    // includeNested를 켜면 초기화된 자식 UserWidget의 내부 트리까지 방문한다.
    using Visitor = Delegate<Result<bool>(ObjectHandle)>;
    Result<void> ForEachWidget(ObjectRegistry& objects, const Visitor& visitor,
                               bool includeNested = false) const noexcept;
    Result<Array<ObjectHandle>> GetAllWidgets(ObjectRegistry& objects,
                                              bool includeNested = false) const noexcept;
    Result<ObjectHandle> FindWidget(ObjectRegistry& objects, StringView name) const noexcept;
    Result<ObjectHandle> FindWidget(ObjectRegistry& objects, const Element& element) const noexcept;
    Result<void> RemoveWidget(ObjectRegistry& objects, ObjectHandle widget) noexcept;
    Result<HashMap<Guid, ObjectHandle>> IndexWidgets(ObjectRegistry& objects) const noexcept;
    Result<ObjectHandle> FindWidget(ObjectRegistry& objects, Guid stableId) const noexcept;
    Result<void> AttachHost(ObjectRegistry& objects, const SharedPtr<Element>& host) noexcept;
    void DetachHost(ObjectRegistry& objects, Element& host) noexcept;

private:
    ROCKS_NATIVE() Platform platform_;
    ROCKS_NATIVE() WeakPtr<Element> host_;
    ROCKS_NATIVE() SharedPtr<WidgetTextShaper> textShaper_;
    ROCKS_PROPERTY(ReadOnly, Name = "rootWidget") ObjectRef<Widget> root_;
};

} // namespace rocks::ui
