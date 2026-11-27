#pragma once

#include <Base/Types.h>

// 번역 대상(MaterialTranslationTarget)의 항목 하나다(Docs/Design/MaterialSystem 6.9절 MS2b3).
// 번역은 실제로 읽은 항목을 비트로 기록하고, 쿠킹은 그 항목만 번역 키에 넣는다(19.3.4절). 비트
// 번호는 번역 키에 들어가므로 끝에만 더한다.
namespace rocks::material {

enum class MaterialTargetField : uint8 {
    Quality = 0,
    IsMobile = 1,
    IsPc = 2,
    IsConsole = 3,
    MaxSamplers = 4,
    Bindless = 5,
    DistanceFields = 6,
    VirtualTexturing = 7,
    RayTracing = 8,
    VirtualizedGeometry = 9,
    ProxyBake = 10,
    LightBake = 11,
    SimulateMobileForLowQuality = 12
};

// 항목 비트 집합이다(비트 i = 항목 i).
using MaterialTargetFieldMask = uint32;

inline constexpr MaterialTargetFieldMask NoMaterialTargetFields = 0;

constexpr MaterialTargetFieldMask ToMask(MaterialTargetField field) noexcept {
    return MaterialTargetFieldMask{1} << static_cast<uint32>(field);
}

} // namespace rocks::material
