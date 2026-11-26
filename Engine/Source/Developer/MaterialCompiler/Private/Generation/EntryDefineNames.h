#pragma once

#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrModule.h>

// 진입점 사용 정의 이름의 진입점 부분이다(Docs/Design/MaterialSystem 6.9절
// MATERIAL_<E>_USES_<F>). IrEntry 순서다. 생성 원문(Material.rsh)과 문맥
// 원문(MaterialContext.rsh)이 함께 쓴다.
namespace rocks::material::detail {

inline constexpr StringView EntryDefineNames[IrEntryCount] = {
    "VERTEX",       "VERTEX_PREVIOUS",    "SURFACE",       "OPACITY",
    "OPACITY_MASK", "PIXEL_DEPTH_OFFSET", "CUSTOM_OUTPUT", "PREVIEW",
};

} // namespace rocks::material::detail
