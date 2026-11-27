#pragma once

#include <Core/Containers/Array.h>
#include <Core/Delegate.h>
#include <Core/Math/Rect.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Text/String.h>
#include <UiCore/FrameTime.h>
#include <UiCore/StyleRegistry.h>
#include <UiCore/TextInputService.h>

namespace rocks::ui {

// 창 호스트가 UI에 주는 서비스 묶음이다(Docs/Design/UiToolkit 20.2절). Application마다 하나이고
// Application이 루트(툴팁 임시 호스트 포함)에 단다. Element는 Element::FindUiHostServices로
// 루트까지 올라가 찾는다. 트리에 붙기 전에는 못 찾는다(시각 0, 서비스 없음).
//
// - 팝업 허용 영역(9.1절 UW7d): 창 좌표 지점이 놓인 모니터의 작업 영역을 창 좌표로 준다.
// - 텍스트 입력 서비스(19.2절): 편집 요소가 초점 때 Activate한다.
// - 프레임 시각(20.3절): Application::BeginFrame이 정한다. 그리기 밖(속성 갱신·배치·입력)에서도
//   같은 값을 읽는다.
//
// 스레드: 게임 lane에서만 쓴다.
class ROCKS_UICORE_API UiHostServices {
public:
    explicit UiHostServices(Allocator& allocator) noexcept;

    using PopupWorkArea = Delegate<bool(Vec2 window, Box2<float>& area)>;

    void SetPopupWorkArea(PopupWorkArea&& provider) noexcept;
    // 공급자가 없거나 모르면 false다.
    bool QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept;

    void SetTextInputService(SharedPtr<TextInputService> service) noexcept;
    const SharedPtr<TextInputService>& TextInput() const noexcept;

    void SetFrameTime(const FrameTime& time) noexcept;
    const FrameTime& Time() const noexcept;

    // 스타일 등록부와 활성 앱 스타일 집합 이름이다(20.11절 — UE FAppStyle). 집합은 찾을 때마다
    // 이름으로 푼다(같은 이름으로 다시 등록한 집합을 본다). 바꾸면 StyleRevision이 오른다. 이름을
    // 담지 못하면(할당 실패) OutOfMemory이고 바꾸지 않는다.
    Result<void> SetStyles(SharedPtr<StyleRegistry> registry, StringView appStyle) noexcept;
    // 활성 집합에서 부모로 올라가며 찾는다. 없으면 nullptr이다(부품은 내장 기본값을 쓴다).
    template <typename T>
    const T* FindStyle(StringView name) const noexcept {
        const StyleSet* set = registry_ ? registry_->FindSetRaw(appStyle_.View()) : nullptr;
        return set ? registry_->FindStyle<T>(*set, name) : nullptr;
    }

    // 미룬 해체 통지다(Docs/Design/UiToolkit 21.4절). Element·Widget 소멸자는 사용자
    // 코드(OnDestruct· OnDeactivated 등)를 부르지 않고 여기에 넣는다. Application이 프레임 시작과
    // Shutdown에서 돌린다. 항목은 대상 Object를 RootRef로 붙잡아 통지 전에 GC되지 않게 한다. 할당
    // 실패면 OutOfMemory이고 항목은 버려진다(호출자가 경고를 남긴다).
    Result<void> DeferTeardown(Delegate<void()>&& teardown) noexcept;
    // 대기열을 넣은 순서대로 돌린다. 도는 중 더해진 항목도 같은 호출에서 돈다(상한
    // MaxTeardownRounds 바퀴 — 넘는 항목은 다음 호출로 남는다). 다시 들어오면 0이다. 돈 항목 수다.
    usize RunDeferredTeardown() noexcept;
    usize PendingTeardownCount() const noexcept;
    // 돌리지 않고 버린다(호스트 소멸 — 소멸자에서 사용자 코드를 부르지 않는다). 버린 수다.
    usize DiscardDeferredTeardown() noexcept;

    // 한 번의 RunDeferredTeardown이 도는 바퀴(통지 안에서 다시 넣은 통지) 상한이다.
    static constexpr usize MaxTeardownRounds = 64;

    // 프레임 시작에 할 일이다(Docs/Design/UiToolkit 22.6절). 트리 순회(붙는 통지·측정·배치) 안에서
    // 다른 가지를 바꾸면 순회가 깨지는 일(예: 상자가 트리에 붙을 때 여는 오류 팝업 — 루트 Overlay
    // 자식 목록을 바꾼다)을 순회 밖으로 미룬다. Application이 프레임 시작(해체 대기열 다음)에 넣은
    // 순서대로 돌린다. 도는 중 넣은 항목은 다음 프레임에 돈다. Shutdown·소멸에서는 돌리지 않고
    // 버린다(닫는 앱에서 팝업을 열지 않는다). 항목은 대상을 약하게 잡는다(그 사이 버려진 대상이면
    // 아무 일도 하지 않게). 실패: 빈 delegate InvalidArgument, 할당 실패 OutOfMemory.
    //
    // 사용 예(붙는 통지에서):
    //   (void)host->DeferFrameAction(Delegate<void()>::CreateLambda(
    //       [weak = WeakPtr<Element>(self)]() noexcept { if (auto e = weak.Lock()) { ... } }));
    Result<void> DeferFrameAction(Delegate<void()>&& action) noexcept;
    // 대기열을 지금 돌린다. 다시 들어오면 0이다. 돈 항목 수다.
    usize RunFrameActions() noexcept;
    usize PendingFrameActionCount() const noexcept;
    // 돌리지 않고 버린다. 버린 수다.
    usize DiscardFrameActions() noexcept;

    // 이 묶음을 가진 상위 층 객체다(Docs/Design/UiToolkit 22.7a절 — UiFramework Application이
    // 자기를 달고 Application::Find가 찾는다). UiCore는 형식을 모르므로 표지 kind(형식마다 하나인
    // 주소)가 같을 때만 돌려준다. 소유자가 사라지기 전에 SetOwner(nullptr, nullptr)로 뗀다.
    void SetOwner(const void* kind, void* owner) noexcept;
    void* FindOwner(const void* kind) const noexcept;

    // 스타일이 바뀐 횟수다(등록부·집합 값·활성 집합). 0이 아니고 늘어나기만 한다(처음 측정에서 한
    // 번 적용하게, 등록부를 바꿔도 줄지 않게).
    uint64 StyleRevision() const noexcept;

private:
    PopupWorkArea popupWorkArea_;
    SharedPtr<TextInputService> textInput_;
    FrameTime time_{};
    SharedPtr<StyleRegistry> registry_;
    String appStyle_;
    Array<Delegate<void()>> teardown_;
    Array<Delegate<void()>> frameActions_;
    const void* ownerKind_ = nullptr;
    void* owner_ = nullptr;
    bool runningTeardown_ = false;
    bool runningFrameActions_ = false;
    uint64 styleChanges_ = 1;
};

} // namespace rocks::ui
