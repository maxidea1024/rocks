#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

struct PaintCacheStats {
    usize paintCalls = 0;
    usize overlayCalls = 0;
    bool rebuilt = false;
};

struct PaintRange {
    usize first;
    usize count;
};

// 한 트리의 배경·전경 기록 순서와 유효 clip을 보관한다. 트리 교체 시 Reset한다.
class ROCKS_UICORE_API CacheRoot {
public:
    explicit CacheRoot(Allocator& allocator) noexcept;
    CacheRoot(const CacheRoot&) = delete;
    CacheRoot& operator=(const CacheRoot&) = delete;
    void Reset() noexcept;

    // 기록에 쓸 테마를 바꾸고 기록을 모두 버린다(다음 Update가 전체를 다시 그린다). nullptr이면
    // DefaultThemeColors()다. 테마는 이 캐시보다 오래 살아야 한다.
    void SetTheme(const ThemeColors* theme, uint64 revision = 0) noexcept;
    // inheritedForeground는 root가 부모에게서 물려받은 전경색이다(창 루트는 흰색). 기록 때와 다르면
    // 전체를 다시 만든다.
    // inheritedDisabled는 root의 조상이 비활성인지다(비활성 효과 — PaintList::IsDisabledEffect).
    Result<void> Update(const Element& root, int32 layer,
                        LinearColor inheritedForeground = {1, 1, 1, 1},
                        bool inheritedDisabled = false) noexcept;
    const PaintList& List() const noexcept;
    int32 HighestLayer() const noexcept;
    uint64 Revision() const noexcept;
    const PaintCacheStats& LastUpdate() const noexcept;

    // 직전 판에서 정점만 바뀐 경우의 명령 범위다. 순서·자원·clip이 바뀌면 전체 배치를 만든다.
    bool CanUpdateVertices(uint64 previousRevision) const noexcept;
    Span<const PaintRange> ChangedRanges() const noexcept;

private:
    friend class Element;
    struct Entry {
        const Element* element;
        uint64 revision;
        usize first;
        usize count;
        int32 layer;
        int32 highest;
        PaintList::ClipState clip;
        float opacity;
        float nextOpacity;
        // 기록 때 이 요소가 자식에게 물려준 전경색과, 이번 판에 다시 계산한 값이다.
        LinearColor foreground;
        LinearColor nextForeground;
        // 기록 때 비활성 효과 안이었는지다. 활성 상태가 바뀌면 그리기 계획이 바뀌어 전체를 다시
        // 만든다.
        bool disabled;
        // 본문과 전경은 같은 부모 본문을 참조한다. 캐시 루트는 부모를 갖지 않는다.
        usize parentBody = MaxSize;
        bool clipped;
        bool overlay;
        bool everyFrame;
        // 본문 항목에서만 사용한다. 변화 없는 가지는 전경 다음 항목까지 건너뛴다.
        usize subtreeEnd;
        uint64 subtreeRevision;
        bool subtreeVolatile;
    };

    Result<void> Record(const Element& element, int32 layer, bool overlay, bool everyFrame,
                        int32& highest) noexcept;
    Result<void> Rebuild(const Element& root, int32 layer) noexcept;
    Result<void> Refresh(const Element& root, int32 layer) noexcept;
    void Publish() noexcept;

    PaintList list_, scratch_;
    Array<Entry> entries_, next_;
    Array<usize> volatileEntries_, changed_;
    Array<usize> visited_;
    Array<PaintRange> ranges_;
    PaintList assembled_;
    const Element* root_ = nullptr;
    uint64 plan_ = 0, paint_ = 0, revision_ = 0;
    int32 layer_ = 0, highest_ = 0;
    LinearColor rootForeground_{1, 1, 1, 1};
    bool rootDisabled_ = false;
    PaintCacheStats stats_{};
    bool verticesOnly_ = false;
};

} // namespace rocks::ui
