#include <MaterialCompiler/MaterialParameterDefaults.h>

// 파라미터 기본값 해석이다(Docs/Design/MaterialSystem 12.8절 MS3d1).
namespace rocks::material {

static_assert(MaterialParameterComponents == MaxIrComponents,
              "Parameter metadata defaults must match the IR component count");

bool ResolveParameterDefaults(const MaterialParameterMetadata& metadata,
                              const MaterialCurveAtlasSnapshot* atlas,
                              double (&out)[MaxIrComponents]) noexcept {
    for (usize index = 0; index < MaxIrComponents; ++index) {
        out[index] = metadata.defaults[index];
    }

    if (!metadata.usedAsAtlasPosition) {
        return true;
    }

    if (atlas == nullptr) {
        return false;
    }

    // 첫 행이다(UE GetCurveIndex — TArray::Find, ScalarParameter(Name, (float)CurveIndex)).
    for (usize at = 0; at < atlas->curves.Size(); ++at) {
        if (atlas->curves[at] == metadata.atlasCurve) {
            out[0] = static_cast<double>(static_cast<float>(at));
            for (usize index = 1; index < MaxIrComponents; ++index) {
                out[index] = 0.0;
            }

            return true;
        }
    }

    return false;
}

} // namespace rocks::material
