#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 정점 → 픽셀 값 노드 lowering이다(Docs/Design/MaterialSystem 6.9절 MS2b4a). 근거는 UE
// MaterialExpressions.cpp:20377–20446(VertexInterpolator)·HLSLMaterialTranslator.cpp:9157–9237이다.
namespace rocks::material::detail {

namespace {

// UE VertexInterpolator: 입력을 정점 문맥에서 번역하고 픽셀로 보간한다.
Result<void> LowerVertexInterpolator(LoweringContext& context) noexcept {
    if (context.Context() != EvaluationContext::Pixel) {
        return context.Error(MaterialDiagnosticCode::StageViolation, "PS",
                             "Custom interpolator outputs only available in pixel shaders.");
    }

    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input",
                             "VertexInterpolator> Input missing");
    }

    auto vertex = context.InputInContext(EvaluationContext::Vertex, "Input");
    if (!vertex || vertex.Value() == NoIrValue) {
        return vertex ? Result<void>::Success() : Result<void>::Failure(vertex.Error());
    }

    const IrType type = context.TypeOf(vertex.Value());
    if (type.IsMatrix() || (type.scalar != IrScalar::Float && type.scalar != IrScalar::Double)) {
        // UE "Bad type %s for %s"(HLSLMaterialTranslator.cpp:14446–14448).
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                             "Bad type for VertexInterpolator: only float values interpolate");
    }

    // LWC는 낮춘다(UE ValidCast — MaterialExpressions.cpp:20415–20446).
    auto narrowed = context.Convert(vertex.Value(), IrScalar::Float);
    if (!narrowed) {
        return Result<void>::Failure(narrowed.Error());
    }

    auto interpolated = context.Builder().Interpolate(narrowed.Value());
    if (!interpolated) {
        return Result<void>::Failure(interpolated.Error());
    }

    return context.Output("PS", interpolated.Value());
}

constexpr LoweringEntry InterpolatorEntries[] = {
    // 입력은 정점 문맥에서 요청할 때 번역한다(픽셀 계획에 넣지 않는다).
    {"Material.VertexInterpolator", &LowerVertexInterpolator, true, {}},
};

} // namespace

Span<const LoweringEntry> InterpolatorLowerings() noexcept {
    return InterpolatorEntries;
}

} // namespace rocks::material::detail
