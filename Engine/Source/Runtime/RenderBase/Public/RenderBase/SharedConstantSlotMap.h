#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <RenderBase/ShaderParameters.h>
#include <RenderBase/SharedConstantSlotTable.h>
#include <ShaderCore/ShaderBindingLayout.h>

// 공유 상수 slot의 파이프라인 지도다(Docs/Design/RenderBase 9.14절 2항 — 사용 예는 SharedConstantSlotTable.h).
namespace rocks::render {

// 파이프라인 하나의 공유 slot 자리다(slot 순번과 그 구조체의 단계별 자리).
struct SharedConstantSlotUse {
    uint32 slot = NoSharedConstantSlot;
    ShaderBindingMap map;
};

// 파이프라인 하나의 공유 slot 지도다(9.14절 2항).
class ROCKS_RENDERBASE_API SharedConstantSlotMap {
public:
    // 단계 layout들(null은 건너뛴다)의 상수 버퍼 가운데 identity가 표에 등록된 것마다 자리를 만든다.
    // 이름이 아니라 구조체 identity로 찾는다. 실패(InvalidArgument, reason 영어): 표가 Freeze되지
    // 않음, 같은 slot 버퍼의 크기·멤버가 slot 표와 다름(ShaderBindingMap::Build 규칙).
    static Result<SharedConstantSlotMap> Build(const SharedConstantSlotTable& table,
                                               Span<const shader::ShaderBindingLayout* const> stages,
                                               const char** reason = nullptr) noexcept;

    Span<const SharedConstantSlotUse> Uses() const noexcept {
        return Span<const SharedConstantSlotUse>(uses_, count_);
    }

    // slot을 읽는 단계가 있는가.
    bool Reads(uint32 slot) const noexcept;

private:
    SharedConstantSlotUse uses_[MaxSharedConstantSlots] = {};
    usize count_ = 0;
};

} // namespace rocks::render
