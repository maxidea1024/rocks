#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <MaterialSchema/Nodes/Constants/ConstantsNodeEnums.h>
#include <MaterialSchema/Nodes/Coordinates/CoordinatesNodeEnums.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 좌표·위치·view 값·경계 노드 lowering이다(Docs/Design/MaterialSystem 6.9절 MS2b2a lowering 표).
// 근거는 UE MaterialExpressions.cpp의 Compile과 HLSLMaterialTranslator.cpp의 번역 함수다.
namespace rocks::material::detail {

namespace {

// UE가 지원하는 UV 채널 수다(HLSLMaterialTranslator.cpp:6688 — 기능 등급 상한 8).
constexpr int64 MaxTextureCoordinates = 8;
// TextureCoordinate 타일링 비교 오차다(UE UE_SMALL_NUMBER).
constexpr float TilingTolerance = 1.e-8f;
// UnMirror 식의 0.5다(UE MaterialTemplate.ush UnMirror — x × UnMirrored × 0.5 + 0.5).
constexpr float UnMirrorHalf = 0.5f;

// 위치 출력(XYZ·XY·Z)이다.
Result<void> OutputPosition(LoweringContext& context, IrValueId value) noexcept {
    if (auto written = context.Output("XYZ", value); !written) {
        return written;
    }

    const uint8 xy[] = {0, 1};
    auto planar = context.Swizzle(value, xy);
    if (!planar) {
        return Result<void>::Failure(planar.Error());
    }

    if (auto written = context.Output("XY", planar.Value()); !written) {
        return written;
    }

    const uint8 z[] = {2};
    auto height = context.Swizzle(value, z);
    if (!height) {
        return Result<void>::Failure(height.Error());
    }

    return context.Output("Z", height.Value());
}

// value의 성분 index다. 스칼라면 그 값이다(UE ComponentMask의 스칼라 복제 — 6.8절).
Result<IrValueId> ComponentOrScalar(LoweringContext& context, IrValueId value,
                                    uint8 index) noexcept {
    if (value == NoIrValue || context.TypeOf(value).components == 1) {
        return Result<IrValueId>::Success(value);
    }

    const uint8 component[] = {index};
    return context.Swizzle(value, component);
}

// TexCoord(index) 문맥 값이다(UInt 상수 인자).
Result<IrValueId> TexCoord(LoweringContext& context, uint32 index) noexcept {
    auto slot = context.Builder().UIntConstant(Span<const uint32>(&index, 1));
    if (!slot) {
        return slot;
    }

    const IrValueId arguments[] = {slot.Value()};
    return context.ContextValue("TexCoord", arguments);
}

// key 입력이 연결되면 그 값, 아니면 fallback 문맥 값이다.
Result<IrValueId> InputOrContext(LoweringContext& context, StringView key,
                                 StringView fallback) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    return connected.Value() ? context.Input(key) : context.ContextValue(fallback);
}

// Coordinate 입력이 연결되면 그 값, 아니면 TexCoord(constCoordinate)다(UE Panner·Rotator).
Result<IrValueId> CoordinateOrTexCoord(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("Coordinate");
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (connected.Value()) {
        return context.Input("Coordinate");
    }

    int64 index = 0;
    auto read = context.ReadInteger("constCoordinate", 0, index);
    if (!read || !read.Value()) {
        return read ? Result<IrValueId>::Success(NoIrValue)
                    : Result<IrValueId>::Failure(read.Error());
    }

    return TexCoord(context, static_cast<uint32>(index));
}

// UE TextureCoordinate(MaterialExpressions.cpp:5324–5346, HLSLMaterialTranslator.cpp:6686–6727).
Result<void> LowerTextureCoordinate(LoweringContext& context) noexcept {
    int64 index = 0;
    float uTiling = 1.0f;
    float vTiling = 1.0f;
    bool unMirrorU = false;
    bool unMirrorV = false;
    const Result<bool> reads[] = {context.ReadInteger("coordinateIndex", 0, index),
                                  context.ReadFloat("uTiling", 1.0f, uTiling),
                                  context.ReadFloat("vTiling", 1.0f, vTiling),
                                  context.ReadBool("unMirrorU", false, unMirrorU),
                                  context.ReadBool("unMirrorV", false, unMirrorV)};
    for (const Result<bool>& read : reads) {
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<void>::Success();
        }
    }

    if (index < 0 || index >= MaxTextureCoordinates) {
        SystemAllocator scratch;
        String message(scratch);
        StringBuilder<> number(scratch);
        number << (index + 1);
        if (auto status = number.Status(); !status) {
            return status;
        }

        const StringView parts[] = {
            "Only 8 texture coordinate sets can be used by this feature level, currently using ",
            number.View()};
        for (StringView part : parts) {
            if (auto appended = message.TryAppend(part); !appended) {
                return appended;
            }
        }

        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), message.View());
    }

    auto coordinate = TexCoord(context, static_cast<uint32>(index));
    if (!coordinate) {
        return Result<void>::Failure(coordinate.Error());
    }

    IrValueId value = coordinate.Value();
    if (value == NoIrValue) {
        return Result<void>::Success();
    }

    if (unMirrorU || unMirrorV) {
        // UnMirror(x) = x × UnMirrored × 0.5 + 0.5(픽셀 전용 값 — UE 픽셀 파라미터 오버로드뿐).
        auto mirrored = context.ContextValue("UnMirrored");
        auto half = context.FloatScalar(UnMirrorHalf);
        if (!mirrored || !half) {
            return Result<void>::Failure(!mirrored ? mirrored.Error() : half.Error());
        }

        IrValueId parts[2] = {NoIrValue, NoIrValue};
        const bool flags[] = {unMirrorU, unMirrorV};
        for (uint8 component = 0; component < 2; ++component) {
            const uint8 pick[] = {component};
            auto part = context.Swizzle(value, pick);
            if (!part) {
                return Result<void>::Failure(part.Error());
            }

            parts[component] = part.Value();
            if (!flags[component]) {
                continue;
            }

            auto scaled = context.Binary(IrOp::Multiply, parts[component], mirrored.Value(),
                                         NumberRule::Arithmetic);
            auto halved = scaled ? context.Binary(IrOp::Multiply, scaled.Value(), half.Value(),
                                                  NumberRule::Arithmetic)
                                 : Move(scaled);
            auto shifted = halved ? context.Binary(IrOp::Add, halved.Value(), half.Value(),
                                                   NumberRule::Arithmetic)
                                  : Move(halved);
            if (!shifted) {
                return Result<void>::Failure(shifted.Error());
            }

            parts[component] = shifted.Value();
        }

        auto joined = context.Append(parts[0], parts[1]);
        if (!joined) {
            return Result<void>::Failure(joined.Error());
        }

        value = joined.Value();
    }

    const float difference = uTiling - vTiling;
    const float unit = 1.0f - uTiling;
    if ((difference < 0.0f ? -difference : difference) > TilingTolerance) {
        const float tiling[] = {uTiling, vTiling};
        auto factor = context.FloatConstant(tiling, StringView());
        auto scaled =
            factor ? context.Binary(IrOp::Multiply, value, factor.Value(), NumberRule::Arithmetic)
                   : Move(factor);
        if (!scaled) {
            return Result<void>::Failure(scaled.Error());
        }

        value = scaled.Value();
    } else if ((unit < 0.0f ? -unit : unit) > TilingTolerance) {
        auto factor = context.FloatConstant(Span<const float>(&uTiling, 1), StringView());
        auto scaled =
            factor ? context.Binary(IrOp::Multiply, value, factor.Value(), NumberRule::Arithmetic)
                   : Move(factor);
        if (!scaled) {
            return Result<void>::Failure(scaled.Error());
        }

        value = scaled.Value();
    }

    return context.Output("Result", value);
}

// UE WorldPosition(HLSLMaterialTranslator.cpp:6220–6302): _NoMaterialOffsets는 픽셀 문맥에서만.
Result<void> LowerWorldPosition(LoweringContext& context) noexcept {
    int64 mode = 0;
    auto read = context.ReadInteger("worldPositionShaderOffset", 0, mode);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const bool pixel = context.Context() == EvaluationContext::Pixel;
    StringView name;
    switch (static_cast<WorldPositionIncludedOffsets>(mode)) {
    case WorldPositionIncludedOffsets::Default:
        name = "WorldPosition";
        break;
    case WorldPositionIncludedOffsets::ExcludeAllShaderOffsets:
        name = pixel ? StringView("WorldPositionNoShaderOffsets") : StringView("WorldPosition");
        break;
    case WorldPositionIncludedOffsets::CameraRelative:
        name = "TranslatedWorldPosition";
        break;
    case WorldPositionIncludedOffsets::CameraRelativeNoOffsets:
        name = pixel ? StringView("TranslatedWorldPositionNoShaderOffsets")
                     : StringView("TranslatedWorldPosition");
        break;
    default:
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Encountered unknown world position type");
    }

    auto value = context.ContextValue(name);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return value.Value() == NoIrValue ? Result<void>::Success()
                                      : OutputPosition(context, value.Value());
}

// UE LocalPosition(HLSLMaterialTranslator.cpp:6131–6218).
Result<void> LowerLocalPosition(LoweringContext& context) noexcept {
    int64 offsets = 0;
    int64 origin = 0;
    auto offsetsRead = context.ReadInteger("includedOffsets", 0, offsets);
    auto originRead = context.ReadInteger("localOrigin", 0, origin);
    if (!offsetsRead || !originRead) {
        return Result<void>::Failure(!offsetsRead ? offsetsRead.Error() : originRead.Error());
    }

    if (!offsetsRead.Value() || !originRead.Value()) {
        return Result<void>::Success();
    }

    const bool exclude =
        static_cast<PositionIncludedOffsets>(offsets) == PositionIncludedOffsets::ExcludeOffsets &&
        context.Context() == EvaluationContext::Pixel;
    StringView name;
    switch (static_cast<LocalPositionOrigin>(origin)) {
    case LocalPositionOrigin::Instance:
        name = exclude ? StringView("LocalPositionNoShaderOffsets") : StringView("LocalPosition");
        break;
    case LocalPositionOrigin::InstancePreSkinning:
        name = "PreSkinnedPosition";
        break;
    case LocalPositionOrigin::Primitive:
        name = exclude ? StringView("PrimitivePositionNoShaderOffsets")
                       : StringView("PrimitivePosition");
        break;
    default:
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Encountered unknown local position origin");
    }

    auto value = context.ContextValue(name);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return value.Value() == NoIrValue ? Result<void>::Success()
                                      : OutputPosition(context, value.Value());
}

// originType(Absolute·CameraRelative)으로 두 값 가운데 하나를 Result로 낸다.
Result<void> LowerOriginValue(LoweringContext& context, StringView absolute,
                              StringView relative) noexcept {
    int64 origin = 0;
    auto read = context.ReadInteger("originType", 0, origin);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    if (origin != static_cast<int64>(PositionOrigin::Absolute) &&
        origin != static_cast<int64>(PositionOrigin::CameraRelative)) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Encountered unknown position origin");
    }

    auto value = context.ContextValue(
        origin == static_cast<int64>(PositionOrigin::Absolute) ? absolute : relative);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

Result<void> LowerObjectPosition(LoweringContext& context) noexcept {
    return LowerOriginValue(context, "ObjectPosition", "ObjectTranslatedPosition");
}

Result<void> LowerActorPosition(LoweringContext& context) noexcept {
    return LowerOriginValue(context, "ActorPosition", "ActorTranslatedPosition");
}

Result<void> LowerParticlePosition(LoweringContext& context) noexcept {
    return LowerOriginValue(context, "ParticlePositionAbsolute", "ParticlePositionTranslated");
}

Result<void> LowerCameraPosition(LoweringContext& context) noexcept {
    auto value = context.ContextValue("CameraPosition");
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

// ViewProperty 속성의 값·역수 값이다(역수가 비면 1 / 값 — UE HLSLMaterialTranslator.cpp:5259).
struct ViewPropertyValue {
    StringView value;
    StringView inverse;
};

// ExposedViewProperty 순서다(UE MaterialExternalCodeRegistry.cpp:143–177). BufferSize는 ViewSize로
// 바꾼다(MaterialExpressions.cpp:10412). ViewportOffset은 따로 다룬다.
constexpr ViewPropertyValue ViewPropertyValues[] = {
    {"ViewSize", "SceneTexelSize"},
    {"ViewFieldOfView", {}},
    {"ViewTanHalfFieldOfView", "ViewCotanHalfFieldOfView"},
    {"ViewSize", "SceneTexelSize"},
    {"WorldViewOrigin", {}},
    {"CameraPosition", {}},
    {{}, {}},
    {"TemporalSampleCount", {}},
    {"TemporalSampleIndex", {}},
    {"TemporalSampleOffset", {}},
    {"RuntimeVirtualTextureOutputLevel", {}},
    {"RuntimeVirtualTextureOutputDerivative", {}},
    {"PreExposure", "OneOverPreExposure"},
    {"RuntimeVirtualTextureMaxLevel", {}},
    {"ResolutionFraction", "ResolutionFractionInverse"},
    {"PostVolumeUserFlags", {}},
    {"FirstPersonFieldOfView", {}},
    {"FirstPersonTanHalfFieldOfView", "FirstPersonCotanHalfFieldOfView"},
    {"FirstPersonScale", {}},
    {"NearPlane", {}},
};

// UE ViewProperty(MaterialExpressions.cpp:10394–10428).
Result<void> LowerViewProperty(LoweringContext& context) noexcept {
    int64 property = 0;
    auto read = context.ReadInteger("property", 1, property);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    if (property < 0 || property >= static_cast<int64>(sizeof(ViewPropertyValues) /
                                                       sizeof(ViewPropertyValues[0]))) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Encountered unknown view property");
    }

    if (static_cast<ExposedViewProperty>(property) == ExposedViewProperty::ViewportOffset) {
        const float zero[] = {0.0f, 0.0f};
        auto offset = context.FloatConstant(zero, StringView());
        if (!offset) {
            return Result<void>::Failure(offset.Error());
        }

        if (auto written = context.Output("Property", offset.Value()); !written) {
            return written;
        }

        return context.Output("InvProperty", offset.Value());
    }

    const ViewPropertyValue& entry = ViewPropertyValues[property];
    auto value = context.ContextValue(entry.value);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    if (auto written = context.Output("Property", value.Value()); !written) {
        return written;
    }

    Result<IrValueId> inverse = Result<IrValueId>::Success(NoIrValue);
    if (!entry.inverse.IsEmpty()) {
        inverse = context.ContextValue(entry.inverse);
    } else {
        auto one = context.FloatScalar(1.0f);
        inverse =
            one ? context.Binary(IrOp::Divide, one.Value(), value.Value(), NumberRule::Arithmetic)
                : Move(one);
    }

    if (!inverse) {
        return Result<void>::Failure(inverse.Error());
    }

    return context.Output("InvProperty", inverse.Value());
}

// UE Panner(MaterialExpressions.cpp:5626–5656).
Result<void> LowerPanner(LoweringContext& context) noexcept {
    bool fractional = false;
    auto read = context.ReadBool("fractionalPart", false, fractional);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto time = InputOrContext(context, "Time", "Time");
    auto speed = context.Input("Speed");
    if (!time || !speed) {
        return Result<void>::Failure(!time ? time.Error() : speed.Error());
    }

    IrValueId arguments[2] = {NoIrValue, NoIrValue};
    for (uint8 axis = 0; axis < 2; ++axis) {
        auto component = ComponentOrScalar(context, speed.Value(), axis);
        auto scaled = component ? context.Binary(IrOp::Multiply, time.Value(), component.Value(),
                                                 NumberRule::Arithmetic)
                                : Move(component);
        if (scaled && fractional) {
            scaled = context.Unary(IrOp::Frac, scaled.Value(), NumberRule::FloatOnly);
        }

        if (!scaled) {
            return Result<void>::Failure(scaled.Error());
        }

        arguments[axis] = scaled.Value();
    }

    auto offset = context.Append(arguments[0], arguments[1]);
    auto coordinate = CoordinateOrTexCoord(context);
    if (!offset || !coordinate) {
        return Result<void>::Failure(!offset ? offset.Error() : coordinate.Error());
    }

    auto sum =
        context.Binary(IrOp::Add, offset.Value(), coordinate.Value(), NumberRule::Arithmetic);
    if (!sum) {
        return Result<void>::Failure(sum.Error());
    }

    return context.Output("Result", sum.Value());
}

// Dot(row, value − origin)이다.
Result<IrValueId> RotatedComponent(LoweringContext& context, IrValueId row, IrValueId value,
                                   IrValueId origin) noexcept {
    auto centered = context.Binary(IrOp::Subtract, value, origin, NumberRule::Arithmetic);
    if (!centered) {
        return centered;
    }

    return context.Binary(IrOp::Dot, row, centered.Value(), NumberRule::FloatOnly);
}

// UE Rotator(MaterialExpressions.cpp:5676–5713).
Result<void> LowerRotator(LoweringContext& context) noexcept {
    float centerX = 0.5f;
    float centerY = 0.5f;
    float speed = 0.25f;
    const Result<bool> reads[] = {context.ReadFloat("centerX", 0.5f, centerX),
                                  context.ReadFloat("centerY", 0.5f, centerY),
                                  context.ReadFloat("speed", 0.25f, speed)};
    for (const Result<bool>& read : reads) {
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<void>::Success();
        }
    }

    auto time = InputOrContext(context, "Time", "Time");
    auto rate = context.FloatConstant(Span<const float>(&speed, 1), StringView());
    if (!time || !rate) {
        return Result<void>::Failure(!time ? time.Error() : rate.Error());
    }

    auto angle = context.Binary(IrOp::Multiply, time.Value(), rate.Value(), NumberRule::Arithmetic);
    if (!angle) {
        return Result<void>::Failure(angle.Error());
    }

    auto cosine = context.Unary(IrOp::Cos, angle.Value(), NumberRule::FloatOnly);
    auto sine = context.Unary(IrOp::Sin, angle.Value(), NumberRule::FloatOnly);
    auto minusOne = context.FloatScalar(-1.0f);
    if (!cosine || !sine || !minusOne) {
        return Result<void>::Failure(!cosine ? cosine.Error()
                                     : !sine ? sine.Error()
                                             : minusOne.Error());
    }

    auto negativeSine =
        context.Binary(IrOp::Multiply, minusOne.Value(), sine.Value(), NumberRule::Arithmetic);
    if (!negativeSine) {
        return Result<void>::Failure(negativeSine.Error());
    }

    auto rowX = context.Append(cosine.Value(), negativeSine.Value());
    auto rowY = context.Append(sine.Value(), cosine.Value());
    const float center[] = {centerX, centerY};
    auto origin = context.FloatConstant(center, StringView());
    auto base = CoordinateOrTexCoord(context);
    if (!rowX || !rowY || !origin || !base) {
        return Result<void>::Failure(!rowX     ? rowX.Error()
                                     : !rowY   ? rowY.Error()
                                     : !origin ? origin.Error()
                                               : base.Error());
    }

    if (base.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    const bool volumetric = context.TypeOf(base.Value()) == IrFloat(3);
    IrValueId planar = base.Value();
    if (volumetric) {
        const uint8 xy[] = {0, 1};
        auto picked = context.Swizzle(base.Value(), xy);
        if (!picked) {
            return Result<void>::Failure(picked.Error());
        }

        planar = picked.Value();
    }

    auto first = RotatedComponent(context, rowX.Value(), planar, origin.Value());
    auto second = RotatedComponent(context, rowY.Value(), planar, origin.Value());
    auto joined = first && second
                      ? context.Append(first.Value(), second.Value())
                      : Result<IrValueId>::Failure(!first ? first.Error() : second.Error());
    auto rotated =
        joined ? context.Binary(IrOp::Add, joined.Value(), origin.Value(), NumberRule::Arithmetic)
               : Move(joined);
    if (!rotated) {
        return Result<void>::Failure(rotated.Error());
    }

    if (!volumetric) {
        return context.Output("Result", rotated.Value());
    }

    const uint8 z[] = {2};
    auto depth = context.Swizzle(base.Value(), z);
    auto whole = depth ? context.Append(rotated.Value(), depth.Value()) : Move(depth);
    if (!whole) {
        return Result<void>::Failure(whole.Error());
    }

    return context.Output("Result", whole.Value());
}

// UE TruncateLWC(HLSLMaterialTranslator.cpp:4857–4881): Double은 Float로, 그 밖은 그대로.
Result<void> LowerTruncateWorldPosition(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    if (input.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    auto truncated = context.Convert(input.Value(), IrScalar::Float);
    if (!truncated) {
        return Result<void>::Failure(truncated.Error());
    }

    return context.Output("Result", context.TypeOf(input.Value()).scalar == IrScalar::Double
                                        ? truncated.Value()
                                        : input.Value());
}

// 경계 출력(Half Extents·Extents·Min·Max)이다(값 이름 접두 + Half·Full·Min·Max).
Result<void> OutputBounds(LoweringContext& context, StringView prefix) noexcept {
    struct Pin {
        StringView key;
        StringView suffix;
    };

    constexpr Pin Pins[] = {
        {"Half Extents", "Half"}, {"Extents", "Full"}, {"Min", "Min"}, {"Max", "Max"}};
    for (const Pin& pin : Pins) {
        SystemAllocator scratch;
        String name(scratch);
        if (auto built = name.TryAssign(prefix); !built) {
            return built;
        }

        if (auto built = name.TryAppend(pin.suffix); !built) {
            return built;
        }

        auto value = context.ContextValue(name.View());
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (auto written = context.Output(pin.key, value.Value()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// UE ObjectLocalBounds·PreSkinnedLocalBounds·Bounds(HLSLMaterialTranslator.cpp:6327–6397).
Result<void> LowerObjectLocalBounds(LoweringContext& context) noexcept {
    return OutputBounds(context, "ObjectLocalBounds");
}

Result<void> LowerPreSkinnedLocalBounds(LoweringContext& context) noexcept {
    return OutputBounds(context, "PreSkinnedLocalBounds");
}

Result<void> LowerBounds(LoweringContext& context) noexcept {
    int64 type = 0;
    auto read = context.ReadInteger("type", 0, type);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    switch (static_cast<BoundsType>(type)) {
    case BoundsType::InstanceLocal:
        return OutputBounds(context, "InstanceLocalBounds");
    case BoundsType::ObjectLocal:
        return OutputBounds(context, "ObjectLocalBounds");
    case BoundsType::PreSkinnedLocal:
        return OutputBounds(context, "PreSkinnedLocalBounds");
    }

    return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                         "Encountered unknown bounds type");
}

// UE ReflectionVectorWS(HLSLMaterialTranslator.cpp:5879–5908, MaterialTemplate.ush:2674–2682).
Result<void> LowerReflectionVector(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("CustomWorldNormal");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        auto value = context.ContextValue("ReflectionVector");
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        return context.Output("Result", value.Value());
    }

    bool normalize = false;
    auto read = context.ReadBool("normalizeCustomWorldNormal", false, normalize);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto input = context.Input("CustomWorldNormal");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    auto normal = context.CoerceTo(input.Value(), IrFloat(3), "CustomWorldNormal");
    if (normal && normalize) {
        normal = context.Unary(IrOp::Normalize, normal.Value(), NumberRule::FloatOnly);
    }

    auto camera = context.ContextValue("CameraVector");
    if (!normal || !camera) {
        return Result<void>::Failure(!normal ? normal.Error() : camera.Error());
    }

    // −CameraVector + N × dot(N, CameraVector) × 2.0
    auto facing = context.Binary(IrOp::Dot, normal.Value(), camera.Value(), NumberRule::FloatOnly);
    auto projected = facing ? context.Binary(IrOp::Multiply, normal.Value(), facing.Value(),
                                             NumberRule::Arithmetic)
                            : Move(facing);
    auto two = context.FloatScalar(2.0f);
    auto doubled = projected && two ? context.Binary(IrOp::Multiply, projected.Value(), two.Value(),
                                                     NumberRule::Arithmetic)
                                    : (!projected ? Move(projected) : Move(two));
    auto negated = context.Unary(IrOp::Negate, camera.Value(), NumberRule::Arithmetic);
    auto reflected = doubled && negated ? context.Binary(IrOp::Add, negated.Value(),
                                                         doubled.Value(), NumberRule::Arithmetic)
                                        : (!doubled ? Move(doubled) : Move(negated));
    if (!reflected) {
        return Result<void>::Failure(reflected.Error());
    }

    return context.Output("Result", reflected.Value());
}

// UE MapARPassthroughCameraUV(HLSLMaterialTranslator.cpp:14061–14072).
Result<void> LowerXrPassthroughCameraUv(LoweringContext& context) noexcept {
    auto input = context.Input("Coordinates");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    auto uv = context.CoerceTo(input.Value(), IrFloat(2), "Coordinates");
    auto first = context.ContextValue("XrPassthroughCameraUv0");
    auto second = context.ContextValue("XrPassthroughCameraUv1");
    if (!uv || !first || !second) {
        return Result<void>::Failure(!uv ? uv.Error() : !first ? first.Error() : second.Error());
    }

    if (uv.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    const uint8 x[] = {0};
    const uint8 y[] = {1};
    const uint8 low[] = {0, 1};
    const uint8 high[] = {2, 3};
    auto u = context.Swizzle(uv.Value(), x);
    auto v = context.Swizzle(uv.Value(), y);
    auto row = u ? context.Ternary(IrOp::Lerp, first.Value(), second.Value(), u.Value(),
                                   NumberRule::FloatOnly)
                 : Move(u);
    auto left = row ? context.Swizzle(row.Value(), low) : Move(row);
    auto right = row ? context.Swizzle(row.Value(), high) : Move(row);
    if (!v || !left || !right) {
        return Result<void>::Failure(!v ? v.Error() : !left ? left.Error() : right.Error());
    }

    auto result =
        context.Ternary(IrOp::Lerp, left.Value(), right.Value(), v.Value(), NumberRule::FloatOnly);
    if (!result) {
        return Result<void>::Failure(result.Error());
    }

    return context.Output("Result", result.Value());
}

constexpr LoweringEntry CoordinateEntries[] = {
    {"Material.TextureCoordinate", &LowerTextureCoordinate},
    {"Material.WorldPosition", &LowerWorldPosition},
    {"Material.LocalPosition", &LowerLocalPosition},
    {"Material.CameraPosition", &LowerCameraPosition},
    {"Material.ObjectPosition", &LowerObjectPosition},
    {"Material.ActorPosition", &LowerActorPosition},
    {"Material.ParticlePosition", &LowerParticlePosition},
    {"Material.ViewProperty", &LowerViewProperty},
    {"Material.Panner", &LowerPanner},
    {"Material.Rotator", &LowerRotator},
    {"Material.TruncateWorldPosition", &LowerTruncateWorldPosition},
    {"Material.ObjectLocalBounds", &LowerObjectLocalBounds},
    {"Material.PreSkinnedLocalBounds", &LowerPreSkinnedLocalBounds},
    {"Material.Bounds", &LowerBounds},
    {"Material.ReflectionVector", &LowerReflectionVector},
    {"Material.XrPassthroughCameraUv", &LowerXrPassthroughCameraUv},
};

} // namespace

Span<const LoweringEntry> CoordinateLowerings() noexcept {
    return CoordinateEntries;
}

} // namespace rocks::material::detail
