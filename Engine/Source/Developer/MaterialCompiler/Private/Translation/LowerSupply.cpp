#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 공급 노드 lowering이다(Docs/Design/MaterialSystem 6.9절 MS2b4b). 렌더러 자원을 읽는 값은 pass가
// 공급하는 문맥 함수 값이고, 여기서는 UE 기본 번역기의 인자 기본값·검사·식을 맞춘다. 근거는 UE
// MaterialExpressions.cpp(ME)·HLSLMaterialTranslator.cpp(HT)다.
namespace rocks::material::detail {

namespace {

// 하늘 대기 광원 번호 상한이다(UE LightIndex ClampMax 1 — 대기 광원 둘).
constexpr int64 MaxAtmosphereLightIndex = 1;
// DynamicParameter 출력 번호 상한이다(UE ParameterIndex ClampMax 3).
constexpr int64 MaxDynamicParameterIndex = 3;
// DistanceFieldApproxAo 단계 수 범위다(UE clamp(NumSteps, 1, 4) — HT:12232).
constexpr int64 MinApproxAoSteps = 1;
constexpr int64 MaxApproxAoSteps = 4;
// 도 → 라디안의 반(UE 0.5 × π / 180 — ME:20587).
constexpr float HalfDegreesToRadians = 0.5f * 3.14159265358979323846f / 180.0f;

Result<void> EmitSupply(LoweringContext& context, StringView key,
                        Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

Result<IrValueId> UIntValue(LoweringContext& context, uint32 value) noexcept {
    return context.Builder().UIntConstant(Span<const uint32>(&value, 1));
}

// key 입력이 연결되면 그 값, 아니면 fallback 상수(성분 components개)다.
Result<IrValueId> InputOrConstant(LoweringContext& context, StringView key,
                                  Span<const float> fallback) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    return connected.Value() ? context.Input(key) : context.FloatConstant(fallback, StringView());
}

// 위치 입력이 연결되면 그 값, 아니면 원점 종류의 위치 문맥 값이다(UE GetWorldPositionOrDefault —
// HT:3316–3329).
Result<IrValueId> PositionOrDefault(LoweringContext& context, StringView key,
                                    bool& absolute) noexcept {
    int64 origin = 0;
    auto read = context.ReadInteger("worldPositionOriginType", 0, origin);
    if (!read || !read.Value()) {
        return read ? Result<IrValueId>::Success(NoIrValue)
                    : Result<IrValueId>::Failure(read.Error());
    }

    absolute = origin == static_cast<int64>(PositionOrigin::Absolute);
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (connected.Value()) {
        return context.Input(key);
    }

    return context.ContextValue(absolute ? StringView("WorldPosition")
                                         : StringView("TranslatedWorldPosition"));
}

// value를 Float3로 맞춘다(Double은 낮춘다 — UE float3 인자).
Result<IrValueId> AsFloat3(LoweringContext& context, IrValueId value, StringView pin) noexcept {
    auto narrowed = context.Convert(value, IrScalar::Float);
    if (!narrowed) {
        return narrowed;
    }

    return context.CoerceTo(narrowed.Value(), IrFloat(3), pin);
}

Result<void> LowerDecalDerivative(LoweringContext& context) noexcept {
    if (auto ddx = EmitSupply(context, "DDX", context.ContextValue("DecalDerivativeX")); !ddx) {
        return ddx;
    }

    return EmitSupply(context, "DDY", context.ContextValue("DecalDerivativeY"));
}

// UE DecalMipmapLevel(HT:8039–8061): TextureSize는 정확히 float2다.
Result<void> LowerDecalMipmapLevel(LoweringContext& context) noexcept {
    auto size = context.Input("TextureSize");
    if (!size || size.Value() == NoIrValue) {
        return size ? Result<void>::Success() : Result<void>::Failure(size.Error());
    }

    if (!(context.TypeOf(size.Value()) == IrFloat(2))) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "TextureSize",
                             "Unmatching conversion to float2");
    }

    const IrValueId arguments[] = {size.Value()};
    return EmitSupply(context, "Result", context.ContextValue("DecalMipmapLevel", arguments));
}

// UE DynamicParameter(HT:11533–11549).
Result<void> LowerDynamicParameter(LoweringContext& context) noexcept {
    constexpr StringView Keys[] = {"Param1", "Param2", "Param3", "Param4"};
    float color[MaxIrComponents] = {1.0f, 1.0f, 1.0f, 1.0f};
    int64 index = 0;
    auto colorRead = context.ReadColor("defaultValue", color);
    auto indexRead = context.ReadInteger("parameterIndex", 0, index);
    if (!colorRead || !indexRead) {
        return Result<void>::Failure(!colorRead ? colorRead.Error() : indexRead.Error());
    }

    if (!colorRead.Value() || !indexRead.Value()) {
        return Result<void>::Success();
    }

    const int64 clamped = index < 0                          ? 0
                          : index > MaxDynamicParameterIndex ? MaxDynamicParameterIndex
                                                             : index;
    auto fallback = context.FloatConstant(color, StringView());
    auto slot = UIntValue(context, static_cast<uint32>(clamped));
    if (!fallback || !slot) {
        return Result<void>::Failure(!fallback ? fallback.Error() : slot.Error());
    }

    const IrValueId arguments[] = {fallback.Value(), slot.Value()};
    auto value = context.ContextValue("DynamicParameter", arguments);
    if (!value || value.Value() == NoIrValue) {
        return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
    }

    uint8 component = 0;
    for (StringView key : Keys) {
        const uint8 pick[] = {component++};
        if (auto written = EmitSupply(context, key, context.Swizzle(value.Value(), pick));
            !written) {
            return written;
        }
    }

    if (auto rgb = EmitSupply(context, "RGB", context.Prefix(value.Value(), 3)); !rgb) {
        return rgb;
    }

    return context.Output("RGBA", value.Value());
}

// UE SphericalParticleOpacity(HT:6069–6086, scene depth 조건 HT:1796–1799).
Result<void> LowerSphericalParticleOpacity(LoweringContext& context) noexcept {
    const MaterialTraitsDesc& traits = context.Traits();
    const bool translucent =
        traits.blendMode != BlendMode::Opaque && traits.blendMode != BlendMode::Masked;
    if (traits.domain != MaterialDomain::PostProcess && !translucent) {
        return context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                             "Only transparent or postprocess materials can read from scene "
                             "depth.");
    }

    auto density = context.Input("Density");
    if (!density) {
        return Result<void>::Failure(density.Error());
    }

    const IrValueId arguments[] = {density.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SphericalParticleOpacity", arguments));
}

Result<void> LowerMainDirectionalLight(LoweringContext& context) noexcept {
    if (auto light = EmitSupply(context, "Illuminance",
                                context.ContextValue("MainDirectionalLightIlluminance"));
        !light) {
        return light;
    }

    return EmitSupply(context, "Direction", context.ContextValue("MainDirectionalLightDirection"));
}

// lightIndex를 0–1로 자른 UInt다(UE ClampMin 0·ClampMax 1).
Result<IrValueId> LightIndex(LoweringContext& context) noexcept {
    int64 index = 0;
    auto read = context.ReadInteger("lightIndex", 0, index);
    if (!read || !read.Value()) {
        return read ? Result<IrValueId>::Success(NoIrValue)
                    : Result<IrValueId>::Failure(read.Error());
    }

    const int64 clamped = index < 0                         ? 0
                          : index > MaxAtmosphereLightIndex ? MaxAtmosphereLightIndex
                                                            : index;
    return UIntValue(context, static_cast<uint32>(clamped));
}

Result<void> LowerSkyAtmosphereLightDirection(LoweringContext& context) noexcept {
    auto index = LightIndex(context);
    if (!index) {
        return Result<void>::Failure(index.Error());
    }

    const IrValueId arguments[] = {index.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereLightDirection", arguments));
}

Result<void> LowerSkyAtmosphereLightIlluminanceOnGround(LoweringContext& context) noexcept {
    auto index = LightIndex(context);
    if (!index) {
        return Result<void>::Failure(index.Error());
    }

    const IrValueId arguments[] = {index.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereLightIlluminanceOnGround", arguments));
}

Result<void> LowerSkyAtmosphereLightIlluminance(LoweringContext& context) noexcept {
    bool absolute = true;
    auto position = PositionOrDefault(context, "WorldPosition", absolute);
    auto index = LightIndex(context);
    if (!position || !index) {
        return Result<void>::Failure(!position ? position.Error() : index.Error());
    }

    const IrValueId arguments[] = {position.Value(), index.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereLightIlluminance", arguments));
}

// UE SkyAtmosphereLightDiskLuminance(ME:20587–20593, HT:12363): 연결되면 cos(0.5·π/180 × 입력),
// 없으면 −1.
Result<void> LowerSkyAtmosphereLightDiskLuminance(LoweringContext& context) noexcept {
    auto index = LightIndex(context);
    auto connected = context.IsConnected("DiskAngularDiameterOverride");
    if (!index || !connected) {
        return Result<void>::Failure(!index ? index.Error() : connected.Error());
    }

    Result<IrValueId> cosine = Result<IrValueId>::Success(NoIrValue);
    if (connected.Value()) {
        auto degrees = context.Input("DiskAngularDiameterOverride");
        auto scale = context.FloatScalar(HalfDegreesToRadians);
        auto radians = degrees && scale ? context.Binary(IrOp::Multiply, scale.Value(),
                                                         degrees.Value(), NumberRule::Arithmetic)
                                        : (!degrees ? Move(degrees) : Move(scale));
        cosine = radians ? context.Unary(IrOp::Cos, radians.Value(), NumberRule::FloatOnly)
                         : Move(radians);
    } else {
        cosine = context.FloatScalar(-1.0f);
    }

    if (!cosine) {
        return Result<void>::Failure(cosine.Error());
    }

    const IrValueId arguments[] = {index.Value(), cosine.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereLightDiskLuminance", arguments));
}

Result<void> LowerSkyAtmosphereAerialPerspective(LoweringContext& context) noexcept {
    bool absolute = true;
    auto position = PositionOrDefault(context, "WorldPosition", absolute);
    if (!position) {
        return Result<void>::Failure(position.Error());
    }

    const IrValueId arguments[] = {position.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereAerialPerspective", arguments));
}

Result<void> LowerSkyAtmosphereDistantLightScatteredLuminance(LoweringContext& context) noexcept {
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereDistantLightScatteredLuminance"));
}

// UE SkyAtmosphereViewLuminance(HT:12366–12380): WorldDirection이 없으면 (−1, −1, −1) ×
// CameraVector.
Result<void> LowerSkyAtmosphereViewLuminance(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("WorldDirection");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    Result<IrValueId> direction = Result<IrValueId>::Success(NoIrValue);
    if (connected.Value()) {
        direction = context.Input("WorldDirection");
    } else {
        const float minusOne[] = {-1.0f, -1.0f, -1.0f};
        auto flip = context.FloatConstant(minusOne, StringView());
        auto camera = context.ContextValue("CameraVector");
        direction = flip && camera ? context.Binary(IrOp::Multiply, flip.Value(), camera.Value(),
                                                    NumberRule::Arithmetic)
                                   : (!flip ? Move(flip) : Move(camera));
    }

    if (!direction) {
        return Result<void>::Failure(direction.Error());
    }

    const IrValueId arguments[] = {direction.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyAtmosphereViewLuminance", arguments));
}

// UE SkyLightEnvMapSample(ME:21911–21920): Direction 기본 (0, 0, 1), Roughness 기본 0.
Result<void> LowerSkyLightEnvironmentSample(LoweringContext& context) noexcept {
    const float up[] = {0.0f, 0.0f, 1.0f};
    const float smooth[] = {0.0f};
    auto direction = InputOrConstant(context, "Direction", up);
    auto roughness = InputOrConstant(context, "Roughness", smooth);
    if (!direction || !roughness) {
        return Result<void>::Failure(!direction ? direction.Error() : roughness.Error());
    }

    const IrValueId arguments[] = {direction.Value(), roughness.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("SkyLightEnvironmentSample", arguments));
}

// UE EyeAdaptationInverse(HT:14731–14764): LightValue × EyeAdaptationInverse(Alpha).
Result<void> LowerEyeAdaptationInverse(LoweringContext& context) noexcept {
    const float white[] = {1.0f, 1.0f, 1.0f};
    const float one[] = {1.0f};
    auto light = InputOrConstant(context, "LightValueInput", white);
    auto alpha = InputOrConstant(context, "AlphaInput", one);
    if (!light || !alpha) {
        return Result<void>::Failure(!light ? light.Error() : alpha.Error());
    }

    if (light.Value() == NoIrValue || alpha.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    const IrType lightType = context.TypeOf(light.Value());
    if (lightType.IsMatrix() ||
        (lightType.scalar != IrScalar::Float && lightType.scalar != IrScalar::Double)) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "LightValueInput",
                             "EyeAdaptationInverse expects a float numeric type for LightValue");
    }

    if (!(context.TypeOf(alpha.Value()) == IrFloat(1))) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "AlphaInput",
                             "EyeAdaptationInverse expects a float type for Alpha");
    }

    const IrValueId arguments[] = {alpha.Value()};
    auto inverse = context.ContextValue("EyeAdaptationInverse", arguments);
    if (!inverse) {
        return Result<void>::Failure(inverse.Error());
    }

    return EmitSupply(
        context, "EyeAdaptationInverse",
        context.Binary(IrOp::Multiply, light.Value(), inverse.Value(), NumberRule::Arithmetic));
}

// UE DepthOfFieldFunction(HT:11922–11932): Depth가 없으면 PixelDepth.
Result<void> LowerDepthOfFieldFunction(LoweringContext& context) noexcept {
    int64 function = 0;
    auto read = context.ReadInteger("functionValue", 0, function);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto connected = context.IsConnected("Depth");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    auto depth = connected.Value() ? context.Input("Depth") : context.ContextValue("PixelDepth");
    auto kind = UIntValue(context, static_cast<uint32>(function));
    if (!depth || !kind) {
        return Result<void>::Failure(!depth ? depth.Error() : kind.Error());
    }

    const IrValueId arguments[] = {depth.Value(), kind.Value()};
    return EmitSupply(context, "Result", context.ContextValue("DepthOfFieldFunction", arguments));
}

Result<void> LowerPostProcessVolumeFlagTest(LoweringContext& context) noexcept {
    auto bit = context.Input("BitIndex");
    if (!bit) {
        return Result<void>::Failure(bit.Error());
    }

    const IrValueId arguments[] = {bit.Value()};
    return EmitSupply(context, "Result",
                      context.ContextValue("PostProcessVolumeFlagTest", arguments));
}

// 위치 하나를 받는 distance field 값이다(UE HT:12169–12230).
Result<void> LowerDistanceFieldValue(LoweringContext& context, StringView name) noexcept {
    bool absolute = true;
    auto position = PositionOrDefault(context, "Position", absolute);
    auto narrowed = position && position.Value() != NoIrValue
                        ? AsFloat3(context, position.Value(), "Position")
                        : Move(position);
    if (!narrowed) {
        return Result<void>::Failure(narrowed.Error());
    }

    const IrValueId arguments[] = {narrowed.Value()};
    return EmitSupply(context, "Result", context.ContextValue(name, arguments));
}

Result<void> LowerDistanceToNearestSurface(LoweringContext& context) noexcept {
    return LowerDistanceFieldValue(context, "DistanceToNearestSurface");
}

Result<void> LowerDistanceFieldGradient(LoweringContext& context) noexcept {
    return LowerDistanceFieldValue(context, "DistanceFieldGradient");
}

// UE DistanceFieldApproxAO(HT:12232–12264).
Result<void> LowerDistanceFieldApproxAo(LoweringContext& context) noexcept {
    int64 steps = 1;
    float stepScale = 3.0f;
    auto stepsRead = context.ReadInteger("numSteps", 1, steps);
    auto scaleRead = context.ReadFloat("stepScaleDefault", 3.0f, stepScale);
    if (!stepsRead || !scaleRead) {
        return Result<void>::Failure(!stepsRead ? stepsRead.Error() : scaleRead.Error());
    }

    if (!stepsRead.Value() || !scaleRead.Value()) {
        return Result<void>::Success();
    }

    const int64 count = steps < MinApproxAoSteps   ? MinApproxAoSteps
                        : steps > MaxApproxAoSteps ? MaxApproxAoSteps
                                                   : steps;
    const float scale = stepScale < 1.0f ? 1.0f : stepScale;
    bool absolute = true;
    auto position = PositionOrDefault(context, "Position", absolute);
    auto normalConnected = context.IsConnected("Normal");
    if (!position || !normalConnected) {
        return Result<void>::Failure(!position ? position.Error() : normalConnected.Error());
    }

    auto normal =
        normalConnected.Value() ? context.Input("Normal") : context.ContextValue("VertexNormal");
    auto base = context.Input("BaseDistance");
    if (!normal || !base) {
        return Result<void>::Failure(!normal ? normal.Error() : base.Error());
    }

    if (position.Value() == NoIrValue || normal.Value() == NoIrValue || base.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    auto scaleValue = context.FloatScalar(scale);
    if (!scaleValue) {
        return Result<void>::Failure(scaleValue.Error());
    }

    IrValueId step = NoIrValue;
    IrValueId bias = NoIrValue;
    IrValueId maximum = NoIrValue;
    if (count == 1) {
        auto zero = context.FloatScalar(0.0f);
        if (!zero) {
            return Result<void>::Failure(zero.Error());
        }

        step = zero.Value();
        bias = base.Value();
        maximum = base.Value();
    } else {
        // step = (Radius − Base) / (S^(N−1) − 1), bias = Base − step, max = Radius.
        auto radius = context.Input("Radius");
        if (!radius || radius.Value() == NoIrValue) {
            return radius ? Result<void>::Success() : Result<void>::Failure(radius.Error());
        }

        float power = 1.0f;
        for (int64 index = 1; index < count; ++index) {
            power *= scale;
        }

        auto denominator = context.FloatScalar(power - 1.0f);
        auto span =
            context.Binary(IrOp::Subtract, radius.Value(), base.Value(), NumberRule::Arithmetic);
        auto divided = span && denominator
                           ? context.Binary(IrOp::Divide, span.Value(), denominator.Value(),
                                            NumberRule::Arithmetic)
                           : (!span ? Move(span) : Move(denominator));
        auto offset = divided ? context.Binary(IrOp::Subtract, base.Value(), divided.Value(),
                                               NumberRule::Arithmetic)
                              : Move(divided);
        if (!offset || !divided) {
            return Result<void>::Failure(!offset ? offset.Error() : divided.Error());
        }

        step = divided.Value();
        bias = offset.Value();
        maximum = radius.Value();
    }

    auto stepAndScale = context.Append(step, scaleValue.Value());
    auto withBias = stepAndScale ? context.Append(stepAndScale.Value(), bias) : Move(stepAndScale);
    auto packed = withBias ? context.Append(withBias.Value(), maximum) : Move(withBias);
    auto positionThree = AsFloat3(context, position.Value(), "Position");
    auto normalThree = AsFloat3(context, normal.Value(), "Normal");
    auto stepCount = UIntValue(context, static_cast<uint32>(count));
    if (!packed || !positionThree || !normalThree || !stepCount) {
        return Result<void>::Failure(!packed          ? packed.Error()
                                     : !positionThree ? positionThree.Error()
                                     : !normalThree   ? normalThree.Error()
                                                      : stepCount.Error());
    }

    const IrValueId arguments[] = {positionThree.Value(), normalThree.Value(), packed.Value(),
                                   stepCount.Value()};
    return EmitSupply(context, "Result", context.ContextValue("DistanceFieldApproxAo", arguments));
}

// UE HairAttributes(ME:21055–21114, HT:12080–12148).
Result<void> LowerHairAttributes(LoweringContext& context) noexcept {
    struct HairOutput {
        StringView key;
        StringView value;
        // 성분 하나를 고르면 그 번호, 아니면 NoComponent다.
        int8 component;
    };

    constexpr int8 NoComponent = -1;
    constexpr HairOutput Outputs[] = {
        {"U", "HairUv", 0},
        {"V", "HairUv", 1},
        {"Length", "HairDimensions", 0},
        {"Radius", "HairDimensions", 1},
        {"Seed", "HairSeed", NoComponent},
        {"Root UV", "HairRootUv", NoComponent},
        {"BaseColor", "HairBaseColor", NoComponent},
        {"Roughness", "HairRoughness", NoComponent},
        {"Depth", "HairDepth", NoComponent},
        {"Coverage", "HairCoverage", NoComponent},
        {"AuxilaryData", "HairAuxilaryData", NoComponent},
        {"AtlasUVs", "HairAtlasUvs", NoComponent},
        {"Group Index", "HairGroupIndex", NoComponent},
        {"AO", "HairAo", NoComponent},
        {"Clump ID", "HairClumpId", 0},
    };

    for (const HairOutput& output : Outputs) {
        auto value = context.ContextValue(output.value);
        if (value && output.component != NoComponent && value.Value() != NoIrValue) {
            const uint8 pick[] = {static_cast<uint8>(output.component)};
            value = context.Swizzle(value.Value(), pick);
        }

        if (auto written = EmitSupply(context, output.key, Move(value)); !written) {
            return written;
        }
    }

    bool tangentSpace = true;
    auto read = context.ReadBool("useTangentSpace", true, tangentSpace);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto flag = context.Builder().BoolConstant(Span<const bool>(&tangentSpace, 1));
    if (!flag) {
        return Result<void>::Failure(flag.Error());
    }

    const IrValueId arguments[] = {flag.Value()};
    return EmitSupply(context, "Tangent", context.ContextValue("HairTangent", arguments));
}

// UE RuntimeVirtualTextureCustomData(HT:8828–8837): RVT 출력 노드 입력 범위 밖은 상수 0이다(RVT
// 출력 노드 — MS2d — 전까지 늘 0).
Result<void> LowerRuntimeVirtualTextureCustomData(LoweringContext& context) noexcept {
    const float zero[] = {0.0f, 0.0f, 0.0f, 0.0f};
    return EmitSupply(context, "Result", context.FloatConstant(zero, StringView()));
}

Result<void> LowerMeshPaintTextureCoordinateIndex(LoweringContext& context) noexcept {
    return EmitSupply(context, "Result", context.ContextValue("MeshPaintTextureCoordinateIndex"));
}

// UE ProcessPhysicsField(HT:12267–12323): CameraRelative면 TranslatedWorld → World, Float3로 낮춘
// 위치와 fieldTarget.
Result<void> LowerPhysicsField(LoweringContext& context, StringView name) noexcept {
    int64 target = 0;
    auto read = context.ReadInteger("fieldTarget", 0, target);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    bool absolute = true;
    auto position = PositionOrDefault(context, "WorldPosition", absolute);
    if (!position || position.Value() == NoIrValue) {
        return position ? Result<void>::Success() : Result<void>::Failure(position.Error());
    }

    IrValueId world = position.Value();
    if (!absolute) {
        // TranslatedWorld − PreViewTranslation(UE WSSubtract — 자르지 않는 Double 뺄셈).
        auto preView = context.ContextValue("PreViewTranslation");
        auto translated = AsFloat3(context, position.Value(), "WorldPosition");
        auto wide =
            translated ? context.Convert(translated.Value(), IrScalar::Double) : Move(translated);
        if (!preView || !wide) {
            return Result<void>::Failure(!preView ? preView.Error() : wide.Error());
        }

        auto difference = context.Builder().Binary(IrOp::Subtract, wide.Value(), preView.Value());
        if (!difference) {
            return Result<void>::Failure(difference.Error());
        }

        world = difference.Value();
    }

    auto narrowed = AsFloat3(context, world, "WorldPosition");
    auto kind = UIntValue(context, static_cast<uint32>(target));
    if (!narrowed || !kind) {
        return Result<void>::Failure(!narrowed ? narrowed.Error() : kind.Error());
    }

    const IrValueId arguments[] = {narrowed.Value(), kind.Value()};
    return EmitSupply(context, "Result", context.ContextValue(name, arguments));
}

Result<void> LowerSamplePhysicsVectorField(LoweringContext& context) noexcept {
    return LowerPhysicsField(context, "SamplePhysicsVectorField");
}

Result<void> LowerSamplePhysicsScalarField(LoweringContext& context) noexcept {
    return LowerPhysicsField(context, "SamplePhysicsScalarField");
}

Result<void> LowerSamplePhysicsIntegerField(LoweringContext& context) noexcept {
    return LowerPhysicsField(context, "SamplePhysicsIntegerField");
}

Result<void> LowerEvaluatePhysicsVectorField(LoweringContext& context) noexcept {
    return LowerPhysicsField(context, "EvaluatePhysicsVectorField");
}

Result<void> LowerEvaluatePhysicsScalarField(LoweringContext& context) noexcept {
    return LowerPhysicsField(context, "EvaluatePhysicsScalarField");
}

Result<void> LowerEvaluatePhysicsIntegerField(LoweringContext& context) noexcept {
    return LowerPhysicsField(context, "EvaluatePhysicsIntegerField");
}

// UE SpeedTree(ME:20130–20140, HT:14699–14720). 정점 단계 전용 값이다.
Result<void> LowerSpeedTree(LoweringContext& context) noexcept {
    int64 geometry = 0;
    int64 wind = 0;
    int64 lod = 0;
    float threshold = 0.9f;
    bool accurate = false;
    const Result<bool> reads[] = {context.ReadInteger("geometryType", 0, geometry),
                                  context.ReadInteger("windType", 0, wind),
                                  context.ReadInteger("lodType", 0, lod),
                                  context.ReadFloat("billboardThreshold", 0.9f, threshold),
                                  context.ReadBool("accurateWindVelocities", false, accurate)};
    for (const Result<bool>& read : reads) {
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<void>::Success();
        }
    }

    // 연결 없는 입력은 속성 enum 값의 float 상수다.
    const float geometryValue[] = {static_cast<float>(geometry)};
    const float windValue[] = {static_cast<float>(wind)};
    const float lodValue[] = {static_cast<float>(lod)};
    auto geometryInput = InputOrConstant(context, "GeometryInput", geometryValue);
    auto windInput = InputOrConstant(context, "WindInput", windValue);
    auto lodInput = InputOrConstant(context, "LODInput", lodValue);
    auto bendConnected = context.IsConnected("ExtraBendWS");
    if (!geometryInput || !windInput || !lodInput || !bendConnected) {
        return Result<void>::Failure(!geometryInput ? geometryInput.Error()
                                     : !windInput   ? windInput.Error()
                                     : !lodInput    ? lodInput.Error()
                                                    : bendConnected.Error());
    }

    const float zero[] = {0.0f, 0.0f, 0.0f};
    auto bend = InputOrConstant(context, "ExtraBendWS", zero);
    if (!bend) {
        return Result<void>::Failure(bend.Error());
    }

    // (geometry, wind, lod)와 (threshold, 이전 프레임, extra bend 여부)로 묶는다(6.9절 MS2b4b).
    const IrValueId kinds[] = {geometryInput.Value(), windInput.Value(), lodInput.Value()};
    IrValueId packed = NoIrValue;
    for (IrValueId kind : kinds) {
        auto scalar = kind == NoIrValue ? Result<IrValueId>::Success(NoIrValue)
                                        : context.CoerceTo(kind, IrFloat(1), "GeometryInput");
        if (!scalar) {
            return Result<void>::Failure(scalar.Error());
        }

        if (packed == NoIrValue) {
            packed = scalar.Value();
            continue;
        }

        auto appended = context.Append(packed, scalar.Value());
        if (!appended) {
            return Result<void>::Failure(appended.Error());
        }

        packed = appended.Value();
    }

    const bool previous = context.Context() == EvaluationContext::VertexPrevious && accurate;
    const float flags[] = {threshold, previous ? 1.0f : 0.0f, bendConnected.Value() ? 1.0f : 0.0f};
    auto flagValue = context.FloatConstant(flags, StringView());
    auto bendThree = bend && bend.Value() != NoIrValue
                         ? AsFloat3(context, bend.Value(), "ExtraBendWS")
                         : Move(bend);
    if (!flagValue || !bendThree) {
        return Result<void>::Failure(!flagValue ? flagValue.Error() : bendThree.Error());
    }

    const IrValueId arguments[] = {packed, flagValue.Value(), bendThree.Value()};
    return EmitSupply(context, "Result", context.ContextValue("SpeedTreeVertexOffset", arguments));
}

constexpr LoweringEntry SupplyEntries[] = {
    {"Material.DecalDerivative", &LowerDecalDerivative},
    {"Material.DecalMipmapLevel", &LowerDecalMipmapLevel},
    {"Material.DynamicParameter", &LowerDynamicParameter},
    {"Material.SphericalParticleOpacity", &LowerSphericalParticleOpacity},
    {"Material.MainDirectionalLight", &LowerMainDirectionalLight},
    {"Material.SkyAtmosphereLightDirection", &LowerSkyAtmosphereLightDirection},
    {"Material.SkyAtmosphereLightIlluminance", &LowerSkyAtmosphereLightIlluminance},
    {"Material.SkyAtmosphereLightIlluminanceOnGround", &LowerSkyAtmosphereLightIlluminanceOnGround},
    {"Material.SkyAtmosphereLightDiskLuminance", &LowerSkyAtmosphereLightDiskLuminance},
    {"Material.SkyAtmosphereAerialPerspective", &LowerSkyAtmosphereAerialPerspective},
    {"Material.SkyAtmosphereDistantLightScatteredLuminance",
     &LowerSkyAtmosphereDistantLightScatteredLuminance},
    {"Material.SkyAtmosphereViewLuminance", &LowerSkyAtmosphereViewLuminance},
    {"Material.SkyLightEnvironmentSample", &LowerSkyLightEnvironmentSample},
    {"Material.EyeAdaptationInverse", &LowerEyeAdaptationInverse},
    {"Material.DepthOfFieldFunction", &LowerDepthOfFieldFunction},
    {"Material.PostProcessVolumeFlagTest", &LowerPostProcessVolumeFlagTest},
    {"Material.DistanceToNearestSurface", &LowerDistanceToNearestSurface},
    {"Material.DistanceFieldGradient", &LowerDistanceFieldGradient},
    {"Material.DistanceFieldApproxAo", &LowerDistanceFieldApproxAo},
    {"Material.HairAttributes", &LowerHairAttributes},
    {"Material.RuntimeVirtualTextureCustomData", &LowerRuntimeVirtualTextureCustomData},
    {"Material.MeshPaintTextureCoordinateIndex", &LowerMeshPaintTextureCoordinateIndex},
    {"Material.SamplePhysicsVectorField", &LowerSamplePhysicsVectorField},
    {"Material.SamplePhysicsScalarField", &LowerSamplePhysicsScalarField},
    {"Material.SamplePhysicsIntegerField", &LowerSamplePhysicsIntegerField},
    {"Material.EvaluatePhysicsVectorField", &LowerEvaluatePhysicsVectorField},
    {"Material.EvaluatePhysicsScalarField", &LowerEvaluatePhysicsScalarField},
    {"Material.EvaluatePhysicsIntegerField", &LowerEvaluatePhysicsIntegerField},
    {"Material.SpeedTree", &LowerSpeedTree},
};

} // namespace

Span<const LoweringEntry> SupplyLowerings() noexcept {
    return SupplyEntries;
}

} // namespace rocks::material::detail
