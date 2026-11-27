#include "LoweringContext.h"
#include "LoweringTable.h"

// 출력 범주의 입력 노드 lowering이다(Docs/Design/MaterialSystem 6.11절 MS2d3 "입력 노드"·"신경망").
// 볼륨·구름 입력은 문맥 값을 그대로 내고(UE ExternalCode 노드), NeuralNetworkOutput은 신경망 출력
// 문맥 값이다.
namespace rocks::material::detail {

namespace {

// UE ENeuralIndexType::NIT_TextureIndex다.
constexpr int64 OutputInputTextureIndex = 0;

// 문맥 값 하나를 출력 key로 낸다.
Result<void> OutputInputValue(LoweringContext& context, StringView key,
                              Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

// UE VolumetricAdvancedMaterialInput(MaterialExpressions.cpp:21481–21493): 0은 float3로, 1은
// 그대로다.
Result<void> LowerVolumetricAdvancedInput(LoweringContext& context) noexcept {
    auto density = context.ContextValue("VolumeSampleConservativeDensity");
    if (!density) {
        return Result<void>::Failure(density.Error());
    }

    if (auto written = OutputInputValue(context, "ConservativeDensity as Float3",
                                        context.Prefix(density.Value(), 3));
        !written) {
        return written;
    }

    return context.Output("ConservativeDensity as Float4", density.Value());
}

// UE CloudSampleAttribute(ExternalCode — GetCloudSample*).
Result<void> LowerCloudSampleAttribute(LoweringContext& context) noexcept {
    struct Output {
        StringView key;
        StringView value;
    };

    const Output outputs[] = {
        {"Altitude", "CloudSampleAltitude"},
        {"AltitudeInLayer", "CloudSampleAltitudeInLayer"},
        {"NormAltitudeInLayer", "CloudSampleNormAltitudeInLayer"},
        {"ShadowSampleDistance", "CloudSampleShadowSampleDistance"},
    };
    for (const Output& output : outputs) {
        if (auto written =
                OutputInputValue(context, output.key, context.ContextValue(output.value));
            !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// UE VolumetricCloudEmptySpaceSkippingInput(ExternalCode — GetCloudEmptySpaceSkippingSphere*).
Result<void> LowerVolumetricCloudEmptySpaceSkippingInput(LoweringContext& context) noexcept {
    if (auto written = OutputInputValue(
            context, "Sphere Center",
            context.ContextValue("CloudEmptySpaceSkippingSphereCenterWorldPosition"));
        !written) {
        return written;
    }

    return OutputInputValue(context, "Sphere Radius",
                            context.ContextValue("CloudEmptySpaceSkippingSphereRadius"));
}

// UE NeuralNetworkOutput(MaterialExpressions.cpp:22556–22566 → HT:12770–12806): TextureIndex는
// NeuralTextureOutput(uv — 없으면 ViewportUv), BufferIndex는 NeuralBufferOutput(index — 없으면
// (0, 0, ViewportUv))다. PostProcess 영역에서만 된다.
Result<void> LowerNeuralNetworkOutput(LoweringContext& context) noexcept {
    if (context.Traits().domain != MaterialDomain::PostProcess) {
        return context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                             "NNE Output Node are only available on post process material.");
    }

    int64 type = OutputInputTextureIndex;
    auto read = context.ReadInteger("neuralIndexType", type, type);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const bool texture = type == OutputInputTextureIndex;
    auto connected = context.IsConnected("Coordinates");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId argument = NoIrValue;
    if (connected.Value()) {
        auto input = context.Input("Coordinates");
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        auto coerced = context.CoerceToWidth(input.Value(), texture ? 2 : 4, "Coordinates");
        if (!coerced || coerced.Value() == NoIrValue) {
            return coerced ? Result<void>::Success() : Result<void>::Failure(coerced.Error());
        }

        argument = coerced.Value();
    } else {
        auto viewport = context.ContextValue("ViewportUv");
        if (!viewport) {
            return Result<void>::Failure(viewport.Error());
        }

        argument = viewport.Value();
        if (!texture) {
            const float zeros[] = {0.0f, 0.0f};
            auto prefix = context.FloatConstant(zeros, StringView());
            if (!prefix) {
                return Result<void>::Failure(prefix.Error());
            }

            auto index = context.Append(prefix.Value(), viewport.Value());
            if (!index) {
                return Result<void>::Failure(index.Error());
            }

            argument = index.Value();
        }
    }

    const IrValueId arguments[] = {argument};
    return OutputInputValue(context, "RGBA",
                            context.ContextValue(texture ? StringView("NeuralTextureOutput")
                                                         : StringView("NeuralBufferOutput"),
                                                 arguments));
}

constexpr LoweringEntry OutputInputEntries[] = {
    {"Material.VolumetricAdvancedInput", &LowerVolumetricAdvancedInput},
    {"Material.CloudSampleAttribute", &LowerCloudSampleAttribute},
    {"Material.VolumetricCloudEmptySpaceSkippingInput",
     &LowerVolumetricCloudEmptySpaceSkippingInput},
    {"Material.NeuralNetworkOutput", &LowerNeuralNetworkOutput},
};

} // namespace

Span<const LoweringEntry> OutputInputLowerings() noexcept {
    return OutputInputEntries;
}

} // namespace rocks::material::detail
