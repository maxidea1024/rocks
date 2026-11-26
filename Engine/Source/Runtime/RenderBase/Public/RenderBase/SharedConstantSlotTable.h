#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/Atomics.h>
#include <RenderBase/ShaderParameters.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 공유 상수 slot 표 기반이다(Docs/Design/RenderBase 9.5·9.14절 RR13, UE 정적 uniform buffer slot
// 대응).
//
// slot 하나는 (이름, 생성 파라미터 구조체 표, 수명)이다. 셰이더 쪽은 바인딩 layout의 상수 버퍼
// identity(SX8 — ShaderSystem 16.2절 SSD54)로 slot을 찾는다: 파이프라인마다 SharedConstantSlotMap을
// 만들고, 기록 객체가 가진 SharedConstantBindings(slot → 상수 블록)를 그 지도대로 묶는다. 렌더러
// slot 다섯(View·Scene·ForwardLight·Shadow·Fog)의 선언과 view마다 다시 묶기는 렌더러 RS0이다(9.14절
// 4항).
//
// 사용 예:
//   // 시작(로더 스레드, EngineShaderHost::Freeze 전):
//   auto view = host.SharedConstantSlots().Register(
//       {"View", &shader::ShaderParameterLayoutOf<ViewParameters>(),
//       ConstantBlockLifetime::PerFrame});
//   // 파이프라인마다(Freeze 뒤 어느 스레드에서나):
//   const shader::ShaderBindingLayout* stages[] = {vertexLayout, pixelLayout};
//   auto map = SharedConstantSlotMap::Build(host.SharedConstantSlots(), stages);
//   // 기록마다:
//   SharedConstantBindings bindings(host.SharedConstantSlots());
//   bindings.Bind("View", viewBlock);
//   bindings.Apply(commands, map.Value());
namespace rocks::render {

// 표 하나의 최대 slot 수다(렌더러 다섯 + 여유 — 셰이더 하나의 상수 버퍼 예산 10 안, 9.5절).
inline constexpr usize MaxSharedConstantSlots = 8;
// slot 이름의 최대 byte 수(NUL 제외)다.
inline constexpr usize MaxSharedConstantSlotNameBytes = 64;
// 순번이 없다는 표지다.
inline constexpr uint32 NoSharedConstantSlot = 0xFFFF'FFFFu;

// slot 하나의 선언이다. layout은 정적 수명이어야 한다(생성 표 ShaderParameterLayoutOf<T>()).
struct SharedConstantSlotDesc {
    StringView name;
    const shader::ShaderParameterLayout* layout = nullptr;
    ConstantBlockLifetime lifetime = ConstantBlockLifetime::PerFrame;
};

// 공유 상수 slot 표다. 등록은 Freeze 전 한 스레드(로더 스레드)에서 하고, Freeze 뒤 조회는 어느
// 스레드에서나 된다(Freeze가 release로 닫고 조회가 acquire로 본다).
class ROCKS_RENDERBASE_API SharedConstantSlotTable {
public:
    SharedConstantSlotTable() noexcept = default;
    SharedConstantSlotTable(const SharedConstantSlotTable&) = delete;
    SharedConstantSlotTable& operator=(const SharedConstantSlotTable&) = delete;

    // slot을 더하고 순번(0부터 등록 순서)을 돌려준다. 실패: Freeze 뒤 ScopeClosed, 가득 참
    // CapacityExceeded, 이름·identity 중복 AlreadyExists, 그 밖 InvalidArgument(이름이 식별자가
    // 아니거나 64 byte 초과, 표 없음, 표가 루트 상수, 표에 identity 없음(생성 표가 아님), 표 검사
    // 위반, 모르는 수명). reason에 영어 이유를 둔다.
    Result<uint32> Register(const SharedConstantSlotDesc& desc,
                            const char** reason = nullptr) noexcept;

    // 등록을 닫는다. 두 번째 호출은 아무 일도 하지 않는다.
    void Freeze() noexcept;

    bool IsFrozen() const noexcept {
        return frozen_.Load<platform::MemoryOrder::Acquire>();
    }

    uint32 Count() const noexcept {
        return count_;
    }

    // 순번의 선언이다. 범위 밖이면 null이다.
    const SharedConstantSlotDesc* At(uint32 slot) const noexcept;

    // 파라미터 구조체 identity·이름의 순번이다. 없으면 NotFound다.
    Result<uint32> FindByStruct(uint64 structId) const noexcept;
    Result<uint32> FindByName(StringView name) const noexcept;

private:
    SharedConstantSlotDesc slots_[MaxSharedConstantSlots] = {};
    uint32 count_ = 0;
    platform::Atomic<bool> frozen_{false};
};

} // namespace rocks::render
