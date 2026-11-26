#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <RenderBase/ShaderParameters.h>
#include <RenderBase/SharedConstantSlotMap.h>
#include <RenderBase/SharedConstantSlotTable.h>

// 공유 상수 slot 값 묶음이다(Docs/Design/RenderBase 9.14절 3항 — 사용 예는 SharedConstantSlotTable.h).
namespace rocks::render {

// 기록 객체 하나의 공유 slot 값(slot → 상수 블록)이다(9.14절 3항). 블록은 Apply까지 살아 있어야 한다.
class ROCKS_RENDERBASE_API SharedConstantBindings {
public:
    explicit SharedConstantBindings(const SharedConstantSlotTable& table) noexcept
        : table_(&table) {
    }

    // slot에 블록을 둔다(null은 비운다). 실패: 범위 밖·이름 없음 NotFound, 블록의 표 identity가 slot
    // 구조체와 다름 InvalidArgument(앞 값은 그대로다).
    Result<void> Bind(uint32 slot, const ConstantBlock* block) noexcept;
    Result<void> Bind(StringView name, const ConstantBlock* block) noexcept;

    const ConstantBlock* Bound(uint32 slot) const noexcept;

    // 지도의 자리마다 ApplyConstantBlock으로 묶는다(지도 순서 = slot 순번 순서). 셰이더가 읽는 slot이
    // 묶이지 않았으면 InvalidArgument("shared constant slot … is not bound" — reason)이고 그 앞 자리는
    // 이미 묶였을 수 있다. ApplyConstantBlock의 실패는 그 오류다.
    Result<void> Apply(graphics::CommandContext& commands, const SharedConstantSlotMap& map,
                       const char** reason = nullptr) const noexcept;

private:
    const SharedConstantSlotTable* table_ = nullptr;
    const ConstantBlock* blocks_[MaxSharedConstantSlots] = {};
};

} // namespace rocks::render
