#pragma once

#include <Core/Memory/SharedPtr.h>
#include <UiCore/StyleSet.h>

namespace rocks::ui {

// 이름 있는 스타일 집합 등록부다(UE FSlateStyleRegistry — Docs/Design/UiToolkit 20.11절). 전역이
// 아니다 — 호스트가 하나 만들어 모든 Application(창·월드 위젯)에
// 준다(Application::SetStyleRegistry). 찾을 때는 집합에서 시작해 부모 이름으로 올라간다(UE
// FSlateStyleSet::ParentStyleName).
//
// 사용 예:
//   auto core = SharedPtr<StyleSet>::TryMake(allocator, allocator);
//   (void)AddCoreStyles(*core.Value());
//   (void)registry.Register(CoreStyleName, core.Value());
//   app.SetStyleRegistry(registryPtr);
//   (void)app.SetAppStyle(CoreStyleName);
class ROCKS_UICORE_API StyleRegistry {
public:
    // 부모 사슬의 깊이 상한이다(순환한 부모 이름을 막는다).
    static constexpr usize MaxParentDepth = 8;

    explicit StyleRegistry(Allocator& allocator) noexcept;

    // 같은 이름이면 바꾼다. 빈 이름·빈 집합은 InvalidArgument다.
    Result<void> Register(StringView name, const SharedPtr<StyleSet>& set) noexcept;
    void Unregister(StringView name) noexcept;
    SharedPtr<StyleSet> FindSet(StringView name) const noexcept;

    // set에서 시작해 부모로 올라가며 name을 찾는다. 없으면 nullptr이다.
    template <typename T>
    const T* FindStyle(const StyleSet& set, StringView name) const noexcept {
        const StyleSet* current = &set;
        for (usize depth = 0; current && depth <= MaxParentDepth; ++depth) {
            if (const T* found = current->Find<T>(name)) {
                return found;
            }

            const StringView parent = current->ParentName();
            current = parent.IsEmpty() ? nullptr : FindSetRaw(parent);
        }

        return nullptr;
    }

    // 등록·해제와 등록된 집합의 값 변경을 모두 센다.
    uint64 Revision() const noexcept;

private:
    struct Entry {
        String name;
        SharedPtr<StyleSet> set;
    };

    const StyleSet* FindSetRaw(StringView name) const noexcept;

    Allocator& allocator_;
    Array<Entry> sets_;
    uint64 revision_ = 0;
};

// 핵심 스타일 집합 이름이다(UE FStarshipCoreStyle "CoreStyle").
inline constexpr StringView CoreStyleName = "CoreStyle";

} // namespace rocks::ui
