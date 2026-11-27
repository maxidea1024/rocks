#include <Core/Text/StringBuilder.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 텍스처 노드 lowering이다(Docs/Design/MaterialSystem 6.10절 MS2c3a). 텍스처는 IR Texture 값(slot —
// 생성 원문 MaterialTexture<k>)이고 샘플은 TextureSample 값이다. 검사 순서·문구는 UE 기본 번역기와
// 같다(UMaterialExpressionTextureSample::Compile — MaterialExpressions.cpp:2770–2950,
// FHLSLMaterialTranslator::TextureSample — HLSLMaterialTranslator.cpp:6996–7520,
// MaterialExpressionUtils.cpp VerifySamplerType).
namespace rocks::material::detail {

namespace {

// UE EMaterialSamplerType 표시 이름이다(EngineTypes.h:946–970 — V10 문구).
constexpr StringView SamplerTypeNames[] = {"Color",
                                           "Grayscale",
                                           "Alpha",
                                           "Normal",
                                           "Masks",
                                           "Distance Field Font",
                                           "Linear Color",
                                           "Linear Grayscale",
                                           "Data",
                                           "External",
                                           "Virtual Color",
                                           "Virtual Grayscale",
                                           "Virtual Alpha",
                                           "Virtual Normal",
                                           "Virtual Mask",
                                           "Virtual Linear Color",
                                           "Virtual Linear Grayscale"};

constexpr usize SamplerTypeCount = sizeof(SamplerTypeNames) / sizeof(SamplerTypeNames[0]);

// 텍스처 샘플 노드의 속성이다(UE UMaterialExpressionTextureSample 멤버).
struct SampleSettings {
    Guid texture;
    uint8 samplerType = 0;
    TextureMipValueMode mipMode = TextureMipValueMode::None;
    SamplerSourceMode samplerSource = SamplerSourceMode::FromTextureAsset;
    TextureGatherMode gather = TextureGatherMode::None;
    bool viewMipBias = true;
    uint8 constCoordinate = 0;
    int32 constMipValue = -1;
};

bool IsVirtualSamplerType(uint8 type) noexcept {
    return type >= static_cast<uint8>(SamplerType::VirtualColor) &&
           type <= static_cast<uint8>(SamplerType::VirtualLinearGrayscale);
}

// UE GetSamplerTypeForTexture(MaterialExpressionUtils.cpp)다.
SamplerType ExpectedSamplerType(const MaterialTextureFacts& facts, bool forceNoVirtual) noexcept {
    if (facts.kind == IrTextureKind::External) {
        return SamplerType::External;
    }

    if (facts.lodGroup == TextureGroup::EightBitData ||
        facts.lodGroup == TextureGroup::SixteenBitData) {
        return SamplerType::Data;
    }

    const bool virtualTexture = !forceNoVirtual && facts.virtualTexture;
    switch (facts.compression) {
    case TextureCompressionSettings::Normalmap:
        return virtualTexture ? SamplerType::VirtualNormal : SamplerType::Normal;
    case TextureCompressionSettings::Grayscale:
        return facts.srgb
                   ? (virtualTexture ? SamplerType::VirtualGrayscale : SamplerType::Grayscale)
                   : (virtualTexture ? SamplerType::VirtualLinearGrayscale
                                     : SamplerType::LinearGrayscale);
    case TextureCompressionSettings::Alpha:
        return virtualTexture ? SamplerType::VirtualAlpha : SamplerType::Alpha;
    case TextureCompressionSettings::Masks:
        return virtualTexture ? SamplerType::VirtualMasks : SamplerType::Masks;
    case TextureCompressionSettings::DistanceFieldFont:
        return SamplerType::DistanceFieldFont;
    default:
        return facts.srgb
                   ? (virtualTexture ? SamplerType::VirtualColor : SamplerType::Color)
                   : (virtualTexture ? SamplerType::VirtualLinearColor : SamplerType::LinearColor);
    }
}

// asset id의 진단 표기다(UE는 텍스처 경로 이름 — Rocks는 AssetId 원문).
Result<void> AppendAsset(StringBuilder<>& out, Guid asset) noexcept {
    out << AssetId{asset};
    return out.Status();
}

// V10(UE VerifySamplerType)이다. 맞으면 true다.
Result<bool> VerifySamplerType(LoweringContext& context, const MaterialTextureFacts& facts,
                               uint8 samplerType) noexcept {
    const SamplerType expected = ExpectedSamplerType(facts, false);
    uint8 used = samplerType;
    // 가상 종류일 때만 번역 대상 항목을 읽는다(쿠킹 번역 키가 필요 이상으로 갈리지 않게).
    if (IsVirtualSamplerType(used) &&
        !context.Target(MaterialTargetField::VirtualTexturing).virtualTexturing) {
        // UE: 가상 텍스처를 끈 형식은 비가상 기대 종류와 비교한다.
        used = static_cast<uint8>(ExpectedSamplerType(facts, true));
    }

    if (used >= SamplerTypeCount) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    SystemAllocator scratch;
    StringBuilder<> message(scratch);
    if (used != static_cast<uint8>(expected)) {
        message << "Sampler type is " << SamplerTypeNames[used] << ", should be "
                << SamplerTypeNames[static_cast<uint8>(expected)] << " for ";
    } else if ((used == static_cast<uint8>(SamplerType::Normal) ||
                used == static_cast<uint8>(SamplerType::Masks)) &&
               facts.srgb) {
        message << "To use '" << SamplerTypeNames[used]
                << "' as sampler type, SRGB must be disabled for ";
    } else {
        return Result<bool>::Success(true);
    }

    if (auto appended = AppendAsset(message, facts.asset); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    if (auto reported =
            context.Error(MaterialDiagnosticCode::SamplerMismatch, StringView(), message.View());
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// enum 칸이 정의 범위 밖이다(InvalidProperty).
Result<void> OutOfRange(LoweringContext& context) noexcept {
    return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                         "Node property value is out of range");
}

Result<bool> ReadSampleSettings(LoweringContext& context, SampleSettings& out) noexcept {
    int64 samplerType = 0;
    int64 mipMode = 0;
    int64 samplerSource = 0;
    int64 gather = 0;
    int64 coordinate = 0;
    int64 mipValue = -1;
    const Result<bool> reads[] = {context.ReadAssetReference("texture", out.texture),
                                  context.ReadInteger("samplerType", 0, samplerType),
                                  context.ReadInteger("mipValueMode", 0, mipMode),
                                  context.ReadInteger("samplerSource", 0, samplerSource),
                                  context.ReadInteger("gatherMode", 0, gather),
                                  context.ReadBool("automaticViewMipBias", true, out.viewMipBias),
                                  context.ReadInteger("constCoordinate", 0, coordinate),
                                  context.ReadInteger("constMipValue", -1, mipValue)};
    for (const Result<bool>& read : reads) {
        if (!read) {
            return Result<bool>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<bool>::Success(false);
        }
    }

    // 저장 값이 enum 범위 밖이면 손상이다(진단 — 6.10절 MS2c3).
    if (samplerType < 0 || samplerType >= static_cast<int64>(SamplerTypeCount) || mipMode < 0 ||
        mipMode > static_cast<int64>(TextureMipValueMode::Derivative) || samplerSource < 0 ||
        samplerSource > static_cast<int64>(SamplerSourceMode::ClampWorldGroupSettings) ||
        gather < 0 || gather > static_cast<int64>(TextureGatherMode::Alpha)) {
        if (auto reported = OutOfRange(context); !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    out.samplerType = static_cast<uint8>(samplerType);
    out.mipMode = static_cast<TextureMipValueMode>(mipMode);
    out.samplerSource = static_cast<SamplerSourceMode>(samplerSource);
    out.gather = static_cast<TextureGatherMode>(gather);
    out.constCoordinate = static_cast<uint8>(coordinate);
    out.constMipValue = static_cast<int32>(mipValue);
    return Result<bool>::Success(true);
}

// 참조 텍스처 값이다. 사실이 없으면 UE 문구로 진단하고 NoIrValue다.
Result<IrValueId> AssetTexture(LoweringContext& context, Guid asset, uint8 samplerType) noexcept {
    const MaterialTextureFacts* facts = context.FindTexture(asset);
    if (facts == nullptr) {
        SystemAllocator scratch;
        StringBuilder<> message(scratch);
        message << "Could not resolve referenced texture '";
        if (auto appended = AppendAsset(message, asset); !appended) {
            return Result<IrValueId>::Failure(appended.Error());
        }

        message << "'.";
        if (auto status = message.Status(); !status) {
            return Result<IrValueId>::Failure(status.Error());
        }

        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          message.View());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    IrTexture texture;
    texture.source = IrTextureSource::Asset;
    texture.id = asset;
    texture.kind = facts->kind;
    texture.defaultAsset = asset;
    return context.Builder().Texture(texture, samplerType);
}

// 파라미터 텍스처 값이다(기본 asset의 사실이 있어야 한다). 같은 ParameterId가 다른 종류로 이미
// 쓰였으면 V8이다.
Result<IrValueId> ParameterTexture(LoweringContext& context, Guid parameter, Guid asset,
                                   uint8 samplerType) noexcept {
    const MaterialTextureFacts* facts = context.FindTexture(asset);
    if (facts == nullptr) {
        return AssetTexture(context, asset, samplerType);
    }

    for (const IrTexture& existing : context.Builder().Textures()) {
        if (existing.source == IrTextureSource::Parameter && existing.id == parameter &&
            existing.kind != facts->kind) {
            if (auto reported =
                    context.Error(MaterialDiagnosticCode::ParameterConflict, StringView(),
                                  "Parameter is used with another parameter type");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }
    }

    IrTexture texture;
    texture.source = IrTextureSource::Parameter;
    texture.id = parameter;
    texture.kind = facts->kind;
    texture.defaultAsset = asset;
    return context.Builder().Texture(texture, samplerType);
}

// 텍스처 값 slot의 (출처, 사실)이다.
const MaterialTextureFacts* FactsOf(LoweringContext& context, IrValueId texture,
                                    IrTexture& slot) noexcept {
    const IrValue& value = context.Builder().Value(texture);
    const Span<const IrTexture> table = context.Builder().Textures();
    if (value.op != IrOp::Texture || value.immediate[3] >= table.Size()) {
        return nullptr;
    }

    slot = table[static_cast<usize>(value.immediate[3])];
    return context.FindTexture(slot.defaultAsset);
}

// value를 Float width 폭으로 맞춘다(UE ForceCast — 스칼라 복제, 짧으면 0, 길면 앞). Double은
// addressModes로 낮춘다(UE WSApplyAddressMode — 좌표만, 그 밖 Double은 그냥 낮춘다).
Result<IrValueId> FitFloat(LoweringContext& context, IrValueId value, uint8 width,
                           uint32 addressModes, StringView pin, StringView invalid,
                           bool coordinate) noexcept {
    const IrType type = context.TypeOf(value);
    // 좌표 밖(mip 값·미분)은 UE IsFloatNumericType처럼 Float·Double만 받는다.
    const bool integer = type.scalar == IrScalar::Int || type.scalar == IrScalar::UInt;
    if (type.IsMatrix() || type.IsTexture() || type.scalar == IrScalar::Bool ||
        (!coordinate && integer)) {
        if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, pin, invalid);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    IrValueId current = value;
    if (type.scalar == IrScalar::Double && coordinate) {
        auto demoted = context.Builder().AddressDemote(current, addressModes);
        if (!demoted) {
            return demoted;
        }

        current = demoted.Value();
    } else {
        auto converted = context.Convert(current, IrScalar::Float);
        if (!converted) {
            return converted;
        }

        current = converted.Value();
    }

    const uint8 components = context.TypeOf(current).components;
    if (components == width) {
        return Result<IrValueId>::Success(current);
    }

    if (components == 1) {
        return context.Splat(current, width);
    }

    if (components > width) {
        return context.Prefix(current, width);
    }

    auto padding = context.Zero(IrFloat(static_cast<uint8>(width - components)));
    if (!padding) {
        return padding;
    }

    return context.Append(current, padding.Value());
}

uint32 AddressModeOf(TextureAddress address) noexcept {
    switch (address) {
    case TextureAddress::Wrap:
        return IrAddressModeWrap;
    case TextureAddress::Mirror:
        return IrAddressModeMirror;
    default:
        // Border(Rocks 확장)는 Clamp처럼 낮춘다(범위 밖은 장치가 경계색으로 바꾼다).
        return IrAddressModeClamp;
    }
}

// 샘플러 출처의 주소 모드다(UE GetTextureAddressForSamplerSource).
uint32 AddressModes(const MaterialTextureFacts* facts, SamplerSourceMode source) noexcept {
    uint32 modes = 0;
    for (usize axis = 0; axis < MaterialTextureAddressAxes; ++axis) {
        uint32 mode = IrAddressModeWrap;
        if (source == SamplerSourceMode::ClampWorldGroupSettings) {
            mode = IrAddressModeClamp;
        } else if (source == SamplerSourceMode::FromTextureAsset && facts != nullptr) {
            mode = AddressModeOf(facts->address[axis]);
        }

        modes |= mode << (axis * IrAddressModeBits);
    }

    return modes;
}

Result<void> EmitSampleOutputs(LoweringContext& context, IrValueId sample) noexcept {
    if (sample == NoIrValue) {
        return Result<void>::Success();
    }

    static constexpr uint8 Rgb[] = {0, 1, 2};
    auto rgb = context.Swizzle(sample, Rgb);
    if (!rgb) {
        return Result<void>::Failure(rgb.Error());
    }

    if (auto written = context.Output("RGB", rgb.Value()); !written) {
        return written;
    }

    static constexpr StringView Channels[MaxIrComponents] = {"R", "G", "B", "A"};
    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        auto channel = context.Swizzle(sample, Span<const uint8>(&index, 1));
        if (!channel) {
            return Result<void>::Failure(channel.Error());
        }

        if (auto written = context.Output(Channels[index], channel.Value()); !written) {
            return written;
        }
    }

    return context.Output("RGBA", sample);
}

// 늦은 진단 자리 값을 샘플 결과(Float4)로 옮긴다(TODO 텍스처 노드에서 온 텍스처 — 6.10절 MS2c1).
Result<IrValueId> PendingSample(LoweringContext& context, const IrValue& pending) noexcept {
    return context.Builder().Pending(
        IrFloat(MaxIrComponents), static_cast<IrPendingKind>(pending.immediate[0]),
        static_cast<uint8>(pending.immediate[1]), Guid{pending.immediate[2], pending.immediate[3]});
}

// 이 노드 자리의 늦은 진단 샘플이다(선행 prerequisite가 없는 샘플 종류).
Result<IrValueId> PrerequisiteSample(LoweringContext& context,
                                     MaterialPrerequisite prerequisite) noexcept {
    return context.Builder().Pending(IrFloat(MaxIrComponents), IrPendingKind::PrerequisiteMissing,
                                     static_cast<uint8>(prerequisite), context.Node().id);
}

// 정적 Bool 입력(AutomaticViewMipBiasValue)이 연결되면 그 값을 쓴다.
Result<bool> StaticBoolOverride(LoweringContext& context, StringView key, bool& value) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected || !connected.Value()) {
        return connected ? Result<bool>::Success(true) : Result<bool>::Failure(connected.Error());
    }

    auto input = context.Input(key);
    if (!input) {
        return Result<bool>::Failure(input.Error());
    }

    bool constant = false;
    if (input.Value() != NoIrValue && context.ConstantBool(input.Value(), constant)) {
        value = constant;
    }

    return Result<bool>::Success(true);
}

// Coordinates 입력이 연결되면 그 값, 아니면 TexCoord(constCoordinate)다.
Result<IrValueId> CoordinatesOrTexCoord(LoweringContext& context,
                                        const SampleSettings& settings) noexcept {
    auto connected = context.IsConnected("Coordinates");
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (connected.Value()) {
        return context.Input("Coordinates");
    }

    const uint32 index = settings.constCoordinate;
    auto slotValue = context.Builder().UIntConstant(Span<const uint32>(&index, 1));
    if (!slotValue) {
        return slotValue;
    }

    const IrValueId arguments[] = {slotValue.Value()};
    return context.ContextValue("TexCoord", arguments);
}

// texture를 uv에서 샘플한다(HT TextureSample — mip 값·방식 결정, gather·정점 문맥·view mip bias,
// uv 폭 맞춤, 샘플러 예산). samplerType은 실효 샘플 종류다. 결과 Float4 또는 NoIrValue(진단함).
Result<IrValueId> SampleAt(LoweringContext& context, IrValueId texture, IrValueId uv,
                           const SampleSettings& settings, uint8 samplerType,
                           bool useViewMipBias) noexcept {
    IrTexture slot;
    const MaterialTextureFacts* facts = FactsOf(context, texture, slot);
    const IrTextureKind kind = context.TypeOf(texture).texture;
    const bool pixel = context.Context() == EvaluationContext::Pixel;
    IrTextureMipMode mode =
        settings.mipMode == TextureMipValueMode::MipLevel     ? IrTextureMipMode::Level
        : settings.mipMode == TextureMipValueMode::MipBias    ? IrTextureMipMode::Bias
        : settings.mipMode == TextureMipValueMode::Derivative ? IrTextureMipMode::Gradient
                                                              : IrTextureMipMode::Implicit;

    // mip 값(UE CompileMipValue0·1 — Derivative 밖은 MipValue 입력 또는 constMipValue 상수).
    IrValueId first = NoIrValue;
    IrValueId second = NoIrValue;
    if (mode == IrTextureMipMode::Gradient) {
        static constexpr StringView Keys[] = {"CoordinatesDX", "CoordinatesDY"};
        static constexpr StringView Missing[] = {"Missing DDX(UVs) parameter",
                                                 "Missing DDY(UVs) parameter"};
        IrValueId gradients[2] = {NoIrValue, NoIrValue};
        for (usize index = 0; index < 2; ++index) {
            auto gradient = context.NamedDynamicInput(Keys[index], Optional<float>());
            if (!gradient) {
                return gradient;
            }

            if (gradient.Value() == NoIrValue) {
                if (context.Poisoned()) {
                    return Result<IrValueId>::Success(NoIrValue);
                }

                if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, Keys[index],
                                                  Missing[index]);
                    !reported) {
                    return Result<IrValueId>::Failure(reported.Error());
                }

                return Result<IrValueId>::Success(NoIrValue);
            }

            gradients[index] = gradient.Value();
        }

        first = gradients[0];
        second = gradients[1];
    } else {
        auto mip = context.NamedDynamicInput("MipValue", Optional<float>());
        if (!mip) {
            return mip;
        }

        if (mip.Value() == NoIrValue && context.Poisoned()) {
            return Result<IrValueId>::Success(NoIrValue);
        }

        if (mip.Value() == NoIrValue) {
            auto constant = context.FloatScalar(static_cast<float>(settings.constMipValue));
            if (!constant) {
                return constant;
            }

            first = constant.Value();
        } else {
            first = mip.Value();
        }
    }

    if (!pixel && mode == IrTextureMipMode::Bias) {
        if (auto reported = context.Error(MaterialDiagnosticCode::StageViolation, StringView(),
                                          "MipBias is only supported in the pixel shader");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    const uint8 width = IrTextureUvComponents(kind);
    const uint32 modes = AddressModes(facts, settings.samplerSource);
    if (mode == IrTextureMipMode::Gradient) {
        const uint8 gradientWidth = IrTextureGradientComponents(kind);
        static constexpr StringView Invalid[] = {"Invalid DDX(UVs) parameter",
                                                 "Invalid DDY(UVs) parameter"};
        IrValueId* targets[] = {&first, &second};
        static constexpr StringView Keys[] = {"CoordinatesDX", "CoordinatesDY"};
        for (usize index = 0; index < 2; ++index) {
            auto fitted = FitFloat(context, *targets[index], gradientWidth, modes, Keys[index],
                                   Invalid[index], false);
            if (!fitted || fitted.Value() == NoIrValue) {
                return fitted;
            }

            *targets[index] = fitted.Value();
        }
    } else {
        auto fitted =
            FitFloat(context, first, 1, modes, "MipValue", "Invalid mip map parameter", false);
        if (!fitted || fitted.Value() == NoIrValue) {
            return fitted;
        }

        first = fitted.Value();
    }

    // gather(HT:7068–7088).
    const IrTextureGather gather = static_cast<IrTextureGather>(settings.gather);
    bool viewMipBias = useViewMipBias;
    if (gather != IrTextureGather::None) {
        if (mode != IrTextureMipMode::Implicit) {
            if (auto reported =
                    context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                                  "Gather does not support mip map overrides (implicitly accesses "
                                  "a specific mip).");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }

        if (kind == IrTextureKind::Texture3D || kind == IrTextureKind::External) {
            if (auto reported =
                    context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                                  "Gather only supports 2D and Cube texture formats.");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }

        viewMipBias = false;
    }

    // 정점 문맥은 하드웨어 미분이 없으므로 Implicit을 Level(mip 값 = constMipValue 상수)로 바꾼다
    // (HT:7090–7099). 편차: UE는 Derivative도 Level로 바꾸고 ddx 값을 mip으로 쓰지만, 명시 미분
    // 샘플은 모든 단계에서 되므로 Rocks는 Gradient를 그대로 둔다(6.10절).
    // gather는 mip 없이 첫 mip을 읽으므로 그대로 둔다(UE HT:7708–7711).
    if (!pixel) {
        if (mode == IrTextureMipMode::Implicit && gather == IrTextureGather::None) {
            mode = IrTextureMipMode::Level;
        }

        viewMipBias = false;
    }

    const MaterialTraitsDesc& traits = context.Traits();
    const bool surfaceOrDecal =
        traits.domain == MaterialDomain::Surface || traits.domain == MaterialDomain::DeferredDecal;
    // 모바일 항목은 마지막에 읽는다(이미 꺼졌으면 번역 키에 기록하지 않는다).
    if (viewMipBias && (!surfaceOrDecal || kind != IrTextureKind::Texture2D ||
                        context.Target(MaterialTargetField::IsMobile).isMobile)) {
        viewMipBias = false;
    }

    // 좌표를 종류의 폭으로 맞춘다.
    auto fittedUv = FitFloat(context, uv, width, modes, "Coordinates",
                             "Texture coordinates must be numeric", true);
    if (!fittedUv || fittedUv.Value() == NoIrValue) {
        return fittedUv;
    }

    uv = fittedUv.Value();

    // 수동 view mip bias(HT:7400–7420 — 6.10절 편차: 공유 출처도 같다).
    if (viewMipBias) {
        if (mode == IrTextureMipMode::Gradient) {
            auto multiply = context.ContextValue("MaterialTextureDerivativeMultiply");
            if (!multiply) {
                return multiply;
            }

            IrValueId* targets[] = {&first, &second};
            for (IrValueId* target : targets) {
                auto scaled = context.Binary(IrOp::Multiply, *target, multiply.Value(),
                                             NumberRule::FloatOnly);
                if (!scaled) {
                    return scaled;
                }

                *target = scaled.Value();
            }
        } else {
            auto bias = context.ContextValue("MaterialTextureMipBias");
            if (!bias) {
                return bias;
            }

            if (mode == IrTextureMipMode::Implicit) {
                first = bias.Value();
                mode = IrTextureMipMode::Bias;
            } else {
                auto added = context.Binary(IrOp::Add, first, bias.Value(), NumberRule::FloatOnly);
                if (!added) {
                    return added;
                }

                first = added.Value();
            }
        }
    }

    if (mode == IrTextureMipMode::Implicit) {
        first = NoIrValue;
        second = NoIrValue;
    } else if (mode != IrTextureMipMode::Gradient) {
        second = NoIrValue;
    }

    IrTextureSampleDesc desc;
    desc.samplerType = samplerType;
    desc.mipMode = mode;
    desc.samplerSource = settings.samplerSource == SamplerSourceMode::WrapWorldGroupSettings
                             ? IrSamplerSource::SharedWrap
                         : settings.samplerSource == SamplerSourceMode::ClampWorldGroupSettings
                             ? IrSamplerSource::SharedClamp
                             : IrSamplerSource::Own;
    desc.gather = gather;
    desc.decal = pixel && traits.domain == MaterialDomain::DeferredDecal &&
                 mode == IrTextureMipMode::Implicit;
    if (desc.samplerSource == IrSamplerSource::Own) {
        auto admitted = context.UseSampler(slot.source, slot.id);
        if (!admitted || !admitted.Value()) {
            return admitted ? Result<IrValueId>::Success(NoIrValue)
                            : Result<IrValueId>::Failure(admitted.Error());
        }
    }

    return context.Builder().TextureSample(desc, texture, uv, first, second);
}

// 텍스처 샘플 노드의 샘플 하나다(UE TextureSample Compile 5–7단계 — Cube·배열·Volume은 Coordinates
// 필수, 그 뒤 좌표와 SampleAt).
Result<IrValueId> Sample(LoweringContext& context, IrValueId texture,
                         const SampleSettings& settings, uint8 samplerType,
                         bool useViewMipBias) noexcept {
    const IrTextureKind kind = context.TypeOf(texture).texture;
    static constexpr StringView UvwMessages[] = {
        "UVW input required for cubemap sample", "UVW input required for texturearray sample",
        "UVWX input required for texturecubearray sample", "UVW input required for volume sample"};
    auto connected = context.IsConnected("Coordinates");
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (!connected.Value() && kind != IrTextureKind::Texture2D && kind != IrTextureKind::External) {
        const usize message =
            static_cast<usize>(kind) - static_cast<usize>(IrTextureKind::TextureCube);
        if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, "Coordinates",
                                          UvwMessages[message]);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    auto uv = CoordinatesOrTexCoord(context, settings);
    if (!uv || uv.Value() == NoIrValue) {
        return uv;
    }

    return SampleAt(context, texture, uv.Value(), settings, samplerType, useViewMipBias);
}

// 샘플 종류가 선행 없는 종류(External·가상)면 그 자리 값이다. 아니면 NoIrValue다.
Result<IrValueId> UnsupportedSamplerType(LoweringContext& context, uint8 samplerType) noexcept {
    if (samplerType == static_cast<uint8>(SamplerType::External)) {
        // TODO(MPQ-ExternalTexture): 외부 텍스처 자원이 생기면 ExternalTexture 샘플을 낸다.
        return PrerequisiteSample(context, MaterialPrerequisite::ExternalTexture);
    }

    if (IsVirtualSamplerType(samplerType) &&
        context.Target(MaterialTargetField::VirtualTexturing).virtualTexturing) {
        // TODO(MPQ-VirtualTexture): 가상 텍스처 샘플(페이지 표 조회)이 생기면 낸다.
        return PrerequisiteSample(context, MaterialPrerequisite::VirtualTexture);
    }

    return Result<IrValueId>::Success(NoIrValue);
}

// UE TextureSample(MaterialExpressions.cpp:2770–2950).
Result<void> LowerTextureSample(LoweringContext& context) noexcept {
    SampleSettings settings;
    auto read = ReadSampleSettings(context, settings);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    bool viewMipBias = settings.viewMipBias;
    if (auto overridden = StaticBoolOverride(context, "AutomaticViewMipBiasValue", viewMipBias);
        !overridden) {
        return Result<void>::Failure(overridden.Error());
    }

    auto connected = context.IsConnected("TextureObject");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId texture = NoIrValue;
    uint8 samplerType = settings.samplerType;
    if (connected.Value()) {
        auto input = context.Input("TextureObject");
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        const IrValue& value = context.Builder().Value(input.Value());
        if (value.op == IrOp::Pending) {
            auto pending = PendingSample(context, value);
            if (!pending) {
                return Result<void>::Failure(pending.Error());
            }

            return EmitSampleOutputs(context, pending.Value());
        }

        if (value.op != IrOp::Texture) {
            return context.Error(MaterialDiagnosticCode::TypeMismatch, "TextureObject",
                                 "Tex input requires a texture value");
        }

        texture = input.Value();
        samplerType = static_cast<uint8>((value.immediate[2] >> IrTextureSamplerTypeShift) &
                                         IrTextureSamplerTypeMask);
    } else {
        if (settings.texture == Guid{}) {
            return context.Error(MaterialDiagnosticCode::MissingInput, StringView(),
                                 "Missing input texture");
        }

        auto made = AssetTexture(context, settings.texture, settings.samplerType);
        if (!made || made.Value() == NoIrValue) {
            return made ? Result<void>::Success() : Result<void>::Failure(made.Error());
        }

        texture = made.Value();
    }

    auto unsupported = UnsupportedSamplerType(context, samplerType);
    if (!unsupported || unsupported.Value() != NoIrValue) {
        return unsupported ? EmitSampleOutputs(context, unsupported.Value())
                           : Result<void>::Failure(unsupported.Error());
    }

    IrTexture slot;
    if (const MaterialTextureFacts* facts = FactsOf(context, texture, slot)) {
        auto verified = VerifySamplerType(context, *facts, samplerType);
        if (!verified || !verified.Value()) {
            return verified ? Result<void>::Success() : Result<void>::Failure(verified.Error());
        }
    }

    auto sample = Sample(context, texture, settings, samplerType, viewMipBias);
    if (!sample) {
        return Result<void>::Failure(sample.Error());
    }

    return EmitSampleOutputs(context, sample.Value());
}

// 파라미터 샘플 노드 종류다(UE TextureIsValid의 요구 종류).
struct ParameterSampleKind {
    IrTextureKind kind;
    StringView required;
    // Coordinates가 없을 때의 UE 문구다(빈 문자열이면 TexCoord를 쓴다).
    StringView needsCoordinates;
};

StringView ClassName(IrTextureKind kind) noexcept {
    switch (kind) {
    case IrTextureKind::TextureCube:
        return "TextureCube";
    case IrTextureKind::Texture2DArray:
        return "Texture2DArray";
    case IrTextureKind::TextureCubeArray:
        return "TextureCubeArray";
    case IrTextureKind::Texture3D:
        return "VolumeTexture";
    case IrTextureKind::External:
        return "MediaTexture";
    default:
        return "Texture2D";
    }
}

// UE TextureSampleParameter::Compile(MaterialExpressions.cpp:4008–4040)과 종류별 TextureIsValid·
// Coordinates 검사다.
Result<void> LowerParameterSample(LoweringContext& context,
                                  const ParameterSampleKind& want) noexcept {
    if (!want.needsCoordinates.IsEmpty()) {
        auto connected = context.IsConnected("Coordinates");
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        if (!connected.Value()) {
            return context.Error(MaterialDiagnosticCode::MissingInput, "Coordinates",
                                 want.needsCoordinates);
        }
    }

    SampleSettings settings;
    auto read = ReadSampleSettings(context, settings);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SystemAllocator scratch;
    StringBuilder<> message(scratch);
    const MaterialTextureFacts* facts =
        settings.texture == Guid{} ? nullptr : context.FindTexture(settings.texture);
    if (settings.texture == Guid{}) {
        message << "Found NULL, requires " << want.required;
    } else if (facts == nullptr) {
        auto made = AssetTexture(context, settings.texture, settings.samplerType);
        return made ? Result<void>::Success() : Result<void>::Failure(made.Error());
    } else if (!(facts->kind == want.kind || (want.kind == IrTextureKind::Texture2D &&
                                              facts->kind == IrTextureKind::External))) {
        message << "Found " << ClassName(facts->kind) << ", requires " << want.required;
    } else if (want.kind == IrTextureKind::Texture2D &&
               IsVirtualSamplerType(settings.samplerType) && !facts->virtualTexture) {
        message << "Sampler requires VirtualTexture";
    } else if (want.kind == IrTextureKind::Texture2D &&
               !IsVirtualSamplerType(settings.samplerType) && facts->virtualTexture) {
        message << "Sampler requires non-VirtualTexture";
    }

    if (auto status = message.Status(); !status) {
        return status;
    }

    if (!message.View().IsEmpty()) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), message.View());
    }

    auto unsupported = UnsupportedSamplerType(context, settings.samplerType);
    if (!unsupported || unsupported.Value() != NoIrValue) {
        return unsupported ? EmitSampleOutputs(context, unsupported.Value())
                           : Result<void>::Failure(unsupported.Error());
    }

    auto verified = VerifySamplerType(context, *facts, settings.samplerType);
    if (!verified || !verified.Value()) {
        return verified ? Result<void>::Success() : Result<void>::Failure(verified.Error());
    }

    // parameterName이 없으면 UE처럼 보통 텍스처 샘플이다.
    auto properties = context.Properties();
    if (!properties) {
        return Result<void>::Failure(properties.Error());
    }

    StringView name;
    if (properties.Value() != nullptr) {
        auto text = properties.Value()->ReadText("parameterName");
        if (!text && text.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(text.Error());
        }

        name = text ? text.Value() : StringView();
    }

    Guid parameter;
    auto idRead = context.ReadGuid("parameterId", parameter);
    if (!idRead || !idRead.Value()) {
        return idRead ? Result<void>::Success() : Result<void>::Failure(idRead.Error());
    }

    // 이름이 있는 파라미터 샘플은 UE처럼 gather·AutomaticViewMipBiasValue 입력을 쓰지
    // 않는다(속성만). 이름이 없으면 UE처럼 TextureSample 경로라 둘 다 쓴다(ME:4022–4025).
    bool viewMipBias = settings.viewMipBias;
    if (name.IsEmpty()) {
        if (auto overridden = StaticBoolOverride(context, "AutomaticViewMipBiasValue", viewMipBias);
            !overridden) {
            return Result<void>::Failure(overridden.Error());
        }
    } else {
        settings.gather = TextureGatherMode::None;
    }

    auto texture = name.IsEmpty() ? AssetTexture(context, settings.texture, settings.samplerType)
                                  : ParameterTexture(context, parameter, settings.texture,
                                                     settings.samplerType);
    if (!texture || texture.Value() == NoIrValue) {
        return texture ? Result<void>::Success() : Result<void>::Failure(texture.Error());
    }

    auto sample = Sample(context, texture.Value(), settings, settings.samplerType, viewMipBias);
    if (!sample) {
        return Result<void>::Failure(sample.Error());
    }

    return EmitSampleOutputs(context, sample.Value());
}

Result<void> LowerTextureSampleParameter2D(LoweringContext& context) noexcept {
    return LowerParameterSample(context, {IrTextureKind::Texture2D, "Texture2D", StringView()});
}

Result<void> LowerTextureSampleParameterCube(LoweringContext& context) noexcept {
    return LowerParameterSample(
        context, {IrTextureKind::TextureCube, "TextureCube", "Cube sample needs UV input"});
}

Result<void> LowerTextureSampleParameter2DArray(LoweringContext& context) noexcept {
    return LowerParameterSample(context, {IrTextureKind::Texture2DArray, "Texture2DArray",
                                          "2D array sample needs UVW input"});
}

Result<void> LowerTextureSampleParameterCubeArray(LoweringContext& context) noexcept {
    return LowerParameterSample(context, {IrTextureKind::TextureCubeArray, "TextureCubeArray",
                                          "Cube Array sample needs UV input"});
}

Result<void> LowerTextureSampleParameterVolume(LoweringContext& context) noexcept {
    return LowerParameterSample(
        context, {IrTextureKind::Texture3D, "VolumeTexture", "Volume sample needs UVW input"});
}

// UE TextureObject(MaterialExpressions.cpp:4211–4225)·TextureObjectParameter(4134–4149).
Result<void> LowerTextureObjectNode(LoweringContext& context, bool parameter) noexcept {
    Guid asset;
    int64 samplerType = 0;
    auto assetRead = context.ReadAssetReference("texture", asset);
    auto typeRead = context.ReadInteger("samplerType", 0, samplerType);
    if (!assetRead || !typeRead) {
        return Result<void>::Failure(!assetRead ? assetRead.Error() : typeRead.Error());
    }

    if (!assetRead.Value() || !typeRead.Value()) {
        return Result<void>::Success();
    }

    if (samplerType < 0 || samplerType >= static_cast<int64>(SamplerTypeCount)) {
        return OutOfRange(context);
    }

    if (asset == Guid{}) {
        return context.Error(MaterialDiagnosticCode::MissingInput, StringView(),
                             "Requires valid texture");
    }

    Result<IrValueId> texture = Result<IrValueId>::Success(NoIrValue);
    if (parameter) {
        Guid id;
        auto idRead = context.ReadGuid("parameterId", id);
        if (!idRead || !idRead.Value()) {
            return idRead ? Result<void>::Success() : Result<void>::Failure(idRead.Error());
        }

        texture = ParameterTexture(context, id, asset, static_cast<uint8>(samplerType));
    } else {
        texture = AssetTexture(context, asset, static_cast<uint8>(samplerType));
    }

    if (!texture) {
        return Result<void>::Failure(texture.Error());
    }

    return context.Output("Result", texture.Value());
}

Result<void> LowerTextureObject(LoweringContext& context) noexcept {
    return LowerTextureObjectNode(context, false);
}

Result<void> LowerTextureObjectParameter(LoweringContext& context) noexcept {
    return LowerTextureObjectNode(context, true);
}

// UE TextureProperty(MaterialExpressions.cpp:4277–4290, HLSLMaterialTranslator.cpp:7770–7787).
Result<void> LowerTextureProperty(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("TextureObject");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "TextureObject",
                             "TextureSample> Missing input texture");
    }

    auto input = context.Input("TextureObject");
    if (!input || input.Value() == NoIrValue) {
        return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
    }

    const IrValue& value = context.Builder().Value(input.Value());
    if (value.op == IrOp::Pending) {
        auto pending = context.Builder().Pending(
            IrFloat(2), static_cast<IrPendingKind>(value.immediate[0]),
            static_cast<uint8>(value.immediate[1]), Guid{value.immediate[2], value.immediate[3]});
        return pending ? context.Output("Result", pending.Value())
                       : Result<void>::Failure(pending.Error());
    }

    const IrTextureKind kind = context.TypeOf(input.Value()).texture;
    if (value.op != IrOp::Texture ||
        (kind != IrTextureKind::Texture2D && kind != IrTextureKind::Texture2DArray &&
         kind != IrTextureKind::Texture3D)) {
        SystemAllocator scratch;
        StringBuilder<> message(scratch);
        message << "Texture size only available for Texture2D, TextureVirtual, Texture2DArray, "
                   "VolumeTexture and SparseVolumeTexture, not "
                << LoweringTypeName(context.TypeOf(input.Value()));
        if (auto status = message.Status(); !status) {
            return status;
        }

        return context.Error(MaterialDiagnosticCode::TypeMismatch, "TextureObject", message.View());
    }

    int64 property = 0;
    auto read = context.ReadInteger("property", 0, property);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto size = context.Builder().TextureSize(input.Value(),
                                              static_cast<ExposedTextureProperty>(property) ==
                                                  ExposedTextureProperty::TexelSize);
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    return context.Output("Result", size.Value());
}

// 텍스처 입력이 연결되면 그 값을 검사해 돌려준다(RecordTextureStreamingInfo·ParticleSubUv).
// 텍스처가 아니면 message로 진단하고 NoIrValue다. pending은 늦은 진단 자리 값이면 참이다.
Result<IrValueId> TextureInput(LoweringContext& context, StringView message,
                               bool& pending) noexcept {
    pending = false;
    auto input = context.Input("TextureObject");
    if (!input || input.Value() == NoIrValue) {
        return input;
    }

    const IrValue& value = context.Builder().Value(input.Value());
    if (value.op == IrOp::Pending) {
        pending = true;
        return input;
    }

    if (value.op != IrOp::Texture) {
        if (auto reported =
                context.Error(MaterialDiagnosticCode::TypeMismatch, "TextureObject", message);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return input;
}

// UE RecordTextureStreamingInfo(MaterialExpressions.cpp:23095–23128). 스트리밍 기록은 UE도 보통
// no-op이라 IR을 내지 않는다(6.10절 MS2c3b).
Result<void> LowerRecordTextureStreamingInfo(LoweringContext& context) noexcept {
    auto hasTexture = context.IsConnected("TextureObject");
    if (!hasTexture) {
        return Result<void>::Failure(hasTexture.Error());
    }

    if (!hasTexture.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "TextureObject",
                             "Missing input TextureObject");
    }

    // UE 순서: 텍스처 번역 → Coordinates 없음 → 텍스처 값 검사(ME:23097–23125).
    auto raw = context.Input("TextureObject");
    if (!raw || raw.Value() == NoIrValue) {
        return raw ? Result<void>::Success() : Result<void>::Failure(raw.Error());
    }

    auto hasCoordinates = context.IsConnected("Coordinates");
    if (!hasCoordinates) {
        return Result<void>::Failure(hasCoordinates.Error());
    }

    if (!hasCoordinates.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Coordinates",
                             "Missing input UV Coordinates");
    }

    auto coordinates = context.Input("Coordinates");
    if (!coordinates || coordinates.Value() == NoIrValue) {
        return coordinates ? Result<void>::Success() : Result<void>::Failure(coordinates.Error());
    }

    bool pending = false;
    auto texture = TextureInput(context, "TextureObject input requires a texture value", pending);
    if (!texture || texture.Value() == NoIrValue) {
        return texture ? Result<void>::Success() : Result<void>::Failure(texture.Error());
    }

    return context.Output("Tex", texture.Value());
}

// UE ParticleSubUV(HLSLMaterialTranslator.cpp ParticleSubUV): SubUV 좌표 둘로 샘플하고 blend면
// SubUVLerp로 섞는다. 샘플은 mip 방식·값만 노드 것이다(UE TextureSample 기본 인자 — 자기 샘플러,
// gather 없음, view mip bias 없음).
Result<void> EmitParticleSubUv(LoweringContext& context, IrValueId texture,
                               const SampleSettings& settings, uint8 samplerType) noexcept {
    if (context.Context() != EvaluationContext::Pixel) {
        return context.Error(MaterialDiagnosticCode::StageViolation, StringView(),
                             "Invalid node used in vertex/hull/domain shader input!");
    }

    bool blend = true;
    auto read = context.ReadBool("blend", true, blend);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SampleSettings plain = settings;
    plain.samplerSource = SamplerSourceMode::FromTextureAsset;
    plain.gather = TextureGatherMode::None;
    const StringView coordinates[] = {"ParticleSubUvCoords0", "ParticleSubUvCoords1"};
    IrValueId samples[2] = {NoIrValue, NoIrValue};
    for (usize index = 0; index < (blend ? 2u : 1u); ++index) {
        auto uv = context.ContextValue(coordinates[index]);
        if (!uv) {
            return Result<void>::Failure(uv.Error());
        }

        auto sample = SampleAt(context, texture, uv.Value(), plain, samplerType, false);
        if (!sample || sample.Value() == NoIrValue) {
            return sample ? Result<void>::Success() : Result<void>::Failure(sample.Error());
        }

        samples[index] = sample.Value();
    }

    if (!blend) {
        return EmitSampleOutputs(context, samples[0]);
    }

    auto alpha = context.ContextValue("ParticleSubUvLerp");
    if (!alpha) {
        return Result<void>::Failure(alpha.Error());
    }

    auto mixed =
        context.Ternary(IrOp::Lerp, samples[0], samples[1], alpha.Value(), NumberRule::FloatOnly);
    if (!mixed) {
        return Result<void>::Failure(mixed.Error());
    }

    return EmitSampleOutputs(context, mixed.Value());
}

// UE ParticleSubUV(MaterialExpressions.cpp:10261–10299).
Result<void> LowerParticleSubUv(LoweringContext& context) noexcept {
    SampleSettings settings;
    auto read = ReadSampleSettings(context, settings);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto connected = context.IsConnected("TextureObject");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId texture = NoIrValue;
    uint8 samplerType = settings.samplerType;
    if (connected.Value()) {
        bool pending = false;
        auto input = TextureInput(context, "Tex input requires a texture value", pending);
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        if (pending) {
            auto placeholder = PendingSample(context, context.Builder().Value(input.Value()));
            if (!placeholder) {
                return Result<void>::Failure(placeholder.Error());
            }

            return EmitSampleOutputs(context, placeholder.Value());
        }

        texture = input.Value();
        samplerType = static_cast<uint8>(
            (context.Builder().Value(texture).immediate[2] >> IrTextureSamplerTypeShift) &
            IrTextureSamplerTypeMask);
    } else {
        if (settings.texture == Guid{}) {
            return context.Error(MaterialDiagnosticCode::MissingInput, StringView(),
                                 "Missing ParticleSubUV input texture");
        }

        auto made = AssetTexture(context, settings.texture, settings.samplerType);
        if (!made || made.Value() == NoIrValue) {
            return made ? Result<void>::Success() : Result<void>::Failure(made.Error());
        }

        texture = made.Value();
    }

    auto unsupported = UnsupportedSamplerType(context, samplerType);
    if (!unsupported || unsupported.Value() != NoIrValue) {
        return unsupported ? EmitSampleOutputs(context, unsupported.Value())
                           : Result<void>::Failure(unsupported.Error());
    }

    IrTexture slot;
    if (const MaterialTextureFacts* facts = FactsOf(context, texture, slot)) {
        auto verified = VerifySamplerType(context, *facts, samplerType);
        if (!verified || !verified.Value()) {
            return verified ? Result<void>::Success() : Result<void>::Failure(verified.Error());
        }
    }

    return EmitParticleSubUv(context, texture, settings, samplerType);
}

// Param2D와 같은 텍스처 검사(UE TextureSampleParameter2D::TextureIsValid)다. 맞으면 facts를 채우고
// true다. 아니면 진단했다.
Result<bool> CheckParameter2DTexture(LoweringContext& context, const SampleSettings& settings,
                                     const MaterialTextureFacts*& facts) noexcept {
    facts = settings.texture == Guid{} ? nullptr : context.FindTexture(settings.texture);
    SystemAllocator scratch;
    StringBuilder<> message(scratch);
    if (settings.texture == Guid{}) {
        message << "Found NULL, requires Texture2D";
    } else if (facts == nullptr) {
        auto made = AssetTexture(context, settings.texture, settings.samplerType);
        return made ? Result<bool>::Success(false) : Result<bool>::Failure(made.Error());
    } else if (facts->kind != IrTextureKind::Texture2D && facts->kind != IrTextureKind::External) {
        message << "Found " << ClassName(facts->kind) << ", requires Texture2D";
    } else if (IsVirtualSamplerType(settings.samplerType) && !facts->virtualTexture) {
        message << "Sampler requires VirtualTexture";
    } else if (!IsVirtualSamplerType(settings.samplerType) && facts->virtualTexture) {
        message << "Sampler requires non-VirtualTexture";
    }

    if (auto status = message.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    if (message.View().IsEmpty()) {
        return Result<bool>::Success(true);
    }

    if (auto reported =
            context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), message.View());
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// parameterName·parameterId로 파라미터 Texture(이름이 없으면 참조 Texture)를 만든다.
Result<IrValueId> NamedTexture(LoweringContext& context, const SampleSettings& settings) noexcept {
    auto properties = context.Properties();
    if (!properties) {
        return Result<IrValueId>::Failure(properties.Error());
    }

    StringView name;
    if (properties.Value() != nullptr) {
        auto text = properties.Value()->ReadText("parameterName");
        if (!text && text.Error() == ErrorCode::OutOfMemory) {
            return Result<IrValueId>::Failure(text.Error());
        }

        name = text ? text.Value() : StringView();
    }

    Guid parameter;
    auto idRead = context.ReadGuid("parameterId", parameter);
    if (!idRead || !idRead.Value()) {
        return idRead ? Result<IrValueId>::Success(NoIrValue)
                      : Result<IrValueId>::Failure(idRead.Error());
    }

    return name.IsEmpty()
               ? AssetTexture(context, settings.texture, settings.samplerType)
               : ParameterTexture(context, parameter, settings.texture, settings.samplerType);
}

// UE TextureSampleParameterSubUV(MaterialExpressions.cpp:4674–4695).
Result<void> LowerTextureSampleParameterSubUv(LoweringContext& context) noexcept {
    SampleSettings settings;
    auto read = ReadSampleSettings(context, settings);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const MaterialTextureFacts* facts = nullptr;
    auto valid = CheckParameter2DTexture(context, settings, facts);
    if (!valid || !valid.Value()) {
        return valid ? Result<void>::Success() : Result<void>::Failure(valid.Error());
    }

    auto unsupported = UnsupportedSamplerType(context, settings.samplerType);
    if (!unsupported || unsupported.Value() != NoIrValue) {
        return unsupported ? EmitSampleOutputs(context, unsupported.Value())
                           : Result<void>::Failure(unsupported.Error());
    }

    auto verified = VerifySamplerType(context, *facts, settings.samplerType);
    if (!verified || !verified.Value()) {
        return verified ? Result<void>::Success() : Result<void>::Failure(verified.Error());
    }

    // UE: Coordinates 입력은 무시하고 UV0 사용을 기록한다(정점 UV 채널 수).
    const uint32 first = 0;
    auto channel = context.Builder().UIntConstant(Span<const uint32>(&first, 1));
    if (!channel) {
        return Result<void>::Failure(channel.Error());
    }

    const IrValueId arguments[] = {channel.Value()};
    if (auto noted = context.ContextValue("TexCoord", arguments); !noted) {
        return Result<void>::Failure(noted.Error());
    }

    auto texture = NamedTexture(context, settings);
    if (!texture || texture.Value() == NoIrValue) {
        return texture ? Result<void>::Success() : Result<void>::Failure(texture.Error());
    }

    return EmitParticleSubUv(context, texture.Value(), settings, settings.samplerType);
}

// UE AntialiasedTextureMask(MaterialExpressions.cpp:19568–19606, Common.ush:1724).
Result<void> LowerAntialiasedTextureMask(LoweringContext& context) noexcept {
    SampleSettings settings;
    auto read = ReadSampleSettings(context, settings);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    if (settings.texture == Guid{}) {
        return context.Error(MaterialDiagnosticCode::MissingInput, StringView(),
                             "UMaterialExpressionAntialiasedTextureMask> Missing input texture");
    }

    const MaterialTextureFacts* known = context.FindTexture(settings.texture);
    if (known != nullptr && known->virtualTexture) {
        return context.Error(
            MaterialDiagnosticCode::UnsupportedCapability, StringView(),
            "UMaterialExpressionAntialiasedTextureMask> Virtual textures are not supported");
    }

    // AA 마스크 전용 TextureIsValid(UE ME:19614–19628 — 가상·외부 금지, Texture2D만).
    if (known == nullptr) {
        auto made = AssetTexture(context, settings.texture, settings.samplerType);
        return made ? Result<void>::Success() : Result<void>::Failure(made.Error());
    }

    if (known->kind != IrTextureKind::Texture2D) {
        SystemAllocator scratch;
        StringBuilder<> message(scratch);
        message << "Found " << ClassName(known->kind) << ", requires Texture2D";
        if (auto status = message.Status(); !status) {
            return status;
        }

        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), message.View());
    }

    const MaterialTextureFacts* facts = known;
    auto texture = NamedTexture(context, settings);
    if (!texture || texture.Value() == NoIrValue) {
        return texture ? Result<void>::Success() : Result<void>::Failure(texture.Error());
    }

    auto verified = VerifySamplerType(context, *facts, settings.samplerType);
    if (!verified || !verified.Value()) {
        return verified ? Result<void>::Success() : Result<void>::Failure(verified.Error());
    }

    if (context.Target(MaterialTargetField::IsMobile).isMobile) {
        return context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(),
                             "Node not supported in feature level ES3_1. SM5 required.");
    }

    float threshold = 0.5f;
    int64 channel = static_cast<int64>(TextureColorChannel::Alpha);
    auto thresholdRead = context.ReadFloat("threshold", 0.5f, threshold);
    auto channelRead =
        context.ReadInteger("channel", static_cast<int64>(TextureColorChannel::Alpha), channel);
    if (!thresholdRead || !channelRead) {
        return Result<void>::Failure(!thresholdRead ? thresholdRead.Error() : channelRead.Error());
    }

    if (!thresholdRead.Value() || !channelRead.Value()) {
        return Result<void>::Success();
    }

    // 원시 샘플(샘플 종류 처리 없음 — UE Texture2DSample)이다.
    SampleSettings plain = settings;
    plain.mipMode = TextureMipValueMode::None;
    plain.samplerSource = SamplerSourceMode::FromTextureAsset;
    plain.gather = TextureGatherMode::None;
    auto uv = CoordinatesOrTexCoord(context, settings);
    if (!uv || uv.Value() == NoIrValue) {
        return uv ? Result<void>::Success() : Result<void>::Failure(uv.Error());
    }

    auto sample = SampleAt(context, texture.Value(), uv.Value(), plain,
                           static_cast<uint8>(SamplerType::Masks), false);
    if (!sample || sample.Value() == NoIrValue) {
        return sample ? Result<void>::Success() : Result<void>::Failure(sample.Error());
    }

    float mask[MaxIrComponents] = {0.0f, 0.0f, 0.0f, 0.0f};
    mask[static_cast<usize>(channel) < MaxIrComponents ? static_cast<usize>(channel) : 3] = 1.0f;
    auto maskValue = context.FloatConstant(mask, StringView());
    auto thresholdValue = context.FloatScalar(threshold);
    if (!maskValue || !thresholdValue) {
        return Result<void>::Failure(!maskValue ? maskValue.Error() : thresholdValue.Error());
    }

    // s = dot(mask, sample), d = max(|ddx s|, |ddy s|), saturate((s − T) / d + T).
    auto value =
        context.Binary(IrOp::Dot, maskValue.Value(), sample.Value(), NumberRule::FloatOnly);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    auto dx = context.Unary(IrOp::Ddx, value.Value(), NumberRule::FloatOnly);
    auto dy = context.Unary(IrOp::Ddy, value.Value(), NumberRule::FloatOnly);
    if (!dx || !dy) {
        return Result<void>::Failure(!dx ? dx.Error() : dy.Error());
    }

    auto ax = context.Unary(IrOp::Abs, dx.Value(), NumberRule::FloatOnly);
    auto ay = context.Unary(IrOp::Abs, dy.Value(), NumberRule::FloatOnly);
    if (!ax || !ay) {
        return Result<void>::Failure(!ax ? ax.Error() : ay.Error());
    }

    auto slope = context.Binary(IrOp::Max, ax.Value(), ay.Value(), NumberRule::FloatOnly);
    auto top = context.Binary(IrOp::Subtract, value.Value(), thresholdValue.Value(),
                              NumberRule::FloatOnly);
    if (!slope || !top) {
        return Result<void>::Failure(!slope ? slope.Error() : top.Error());
    }

    auto ratio = context.Binary(IrOp::Divide, top.Value(), slope.Value(), NumberRule::FloatOnly);
    if (!ratio) {
        return Result<void>::Failure(ratio.Error());
    }

    auto biased =
        context.Binary(IrOp::Add, ratio.Value(), thresholdValue.Value(), NumberRule::FloatOnly);
    if (!biased) {
        return Result<void>::Failure(biased.Error());
    }

    auto result = context.Unary(IrOp::Saturate, biased.Value(), NumberRule::FloatOnly);
    if (!result) {
        return Result<void>::Failure(result.Error());
    }

    return context.Output("Result", result.Value());
}

constexpr LoweringEntry TextureEntries[] = {
    {"Material.TextureObject", &LowerTextureObject},
    {"Material.TextureObjectParameter", &LowerTextureObjectParameter},
    {"Material.TextureSample", &LowerTextureSample},
    {"Material.TextureSampleParameter2D", &LowerTextureSampleParameter2D},
    {"Material.TextureSampleParameterCube", &LowerTextureSampleParameterCube},
    {"Material.TextureSampleParameter2DArray", &LowerTextureSampleParameter2DArray},
    {"Material.TextureSampleParameterCubeArray", &LowerTextureSampleParameterCubeArray},
    {"Material.TextureSampleParameterVolume", &LowerTextureSampleParameterVolume},
    {"Material.TextureSampleParameterSubUv", &LowerTextureSampleParameterSubUv},
    {"Material.TextureProperty", &LowerTextureProperty},
    {"Material.AntialiasedTextureMask", &LowerAntialiasedTextureMask},
    {"Material.RecordTextureStreamingInfo", &LowerRecordTextureStreamingInfo},
    {"Material.ParticleSubUv", &LowerParticleSubUv},
};

} // namespace

Span<const LoweringEntry> TextureLowerings() noexcept {
    return TextureEntries;
}

} // namespace rocks::material::detail
