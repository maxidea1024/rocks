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

// 큰 좌표 uv의 주소 모드 값이다(UE LWCADDRESSMODE_* — 성분마다 2 bit).
constexpr uint32 AddressModeClamp = 0;
constexpr uint32 AddressModeWrap = 1;
constexpr uint32 AddressModeMirror = 2;
constexpr uint32 AddressModeBits = 2;

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
    const bool virtualTexturing =
        context.Target(MaterialTargetField::VirtualTexturing).virtualTexturing;
    const SamplerType expected = ExpectedSamplerType(facts, false);
    uint8 used = samplerType;
    if (IsVirtualSamplerType(used) && !virtualTexturing) {
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
                                  "Texture parameter is used with another texture type");
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
// addressModes로 낮춘다(UE WSApplyAddressMode).
Result<IrValueId> FitFloat(LoweringContext& context, IrValueId value, uint8 width,
                           uint32 addressModes, StringView pin, StringView invalid) noexcept {
    const IrType type = context.TypeOf(value);
    if (type.IsMatrix() || type.IsTexture() || type.scalar == IrScalar::Bool) {
        if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, pin, invalid);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    IrValueId current = value;
    if (type.scalar == IrScalar::Double) {
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

uint8 UvWidth(IrTextureKind kind) noexcept {
    switch (kind) {
    case IrTextureKind::TextureCubeArray:
        return 4;
    case IrTextureKind::TextureCube:
    case IrTextureKind::Texture2DArray:
    case IrTextureKind::Texture3D:
        return 3;
    default:
        return 2;
    }
}

uint32 AddressModeOf(TextureAddress address) noexcept {
    switch (address) {
    case TextureAddress::Wrap:
        return AddressModeWrap;
    case TextureAddress::Mirror:
        return AddressModeMirror;
    default:
        // Border(Rocks 확장)는 Clamp처럼 낮춘다(범위 밖은 장치가 경계색으로 바꾼다).
        return AddressModeClamp;
    }
}

// 샘플러 출처의 주소 모드다(UE GetTextureAddressForSamplerSource).
uint32 AddressModes(const MaterialTextureFacts* facts, SamplerSourceMode source) noexcept {
    uint32 modes = 0;
    for (usize axis = 0; axis < MaterialTextureAddressAxes; ++axis) {
        uint32 mode = AddressModeWrap;
        if (source == SamplerSourceMode::ClampWorldGroupSettings) {
            mode = AddressModeClamp;
        } else if (source == SamplerSourceMode::FromTextureAsset && facts != nullptr) {
            mode = AddressModeOf(facts->address[axis]);
        }

        modes |= mode << (axis * AddressModeBits);
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

// 샘플 하나다(UE TextureSample Compile 5–7단계와 HT TextureSample). texture는 텍스처 값이고
// samplerType은 실효 샘플 종류다. 결과 Float4 또는 NoIrValue(진단함).
Result<IrValueId> Sample(LoweringContext& context, IrValueId texture,
                         const SampleSettings& settings, uint8 samplerType,
                         bool useViewMipBias) noexcept {
    IrTexture slot;
    const MaterialTextureFacts* facts = FactsOf(context, texture, slot);
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

    // 좌표: 연결 또는 TexCoord(constCoordinate).
    IrValueId uv = NoIrValue;
    if (connected.Value()) {
        auto input = context.Input("Coordinates");
        if (!input || input.Value() == NoIrValue) {
            return input;
        }

        uv = input.Value();
    } else {
        const uint32 index = settings.constCoordinate;
        auto slotValue = context.Builder().UIntConstant(Span<const uint32>(&index, 1));
        if (!slotValue) {
            return slotValue;
        }

        const IrValueId arguments[] = {slotValue.Value()};
        auto texCoord = context.ContextValue("TexCoord", arguments);
        if (!texCoord) {
            return texCoord;
        }

        uv = texCoord.Value();
    }

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

    const uint8 width = UvWidth(kind);
    const uint32 modes = AddressModes(facts, settings.samplerSource);
    if (mode == IrTextureMipMode::Gradient) {
        const uint8 gradientWidth = kind == IrTextureKind::TextureCubeArray ? 3
                                    : kind == IrTextureKind::Texture2DArray ? 2
                                                                            : width;
        static constexpr StringView Invalid[] = {"Invalid DDX(UVs) parameter",
                                                 "Invalid DDY(UVs) parameter"};
        IrValueId* targets[] = {&first, &second};
        static constexpr StringView Keys[] = {"CoordinatesDX", "CoordinatesDY"};
        for (usize index = 0; index < 2; ++index) {
            auto fitted = FitFloat(context, *targets[index], gradientWidth, modes, Keys[index],
                                   Invalid[index]);
            if (!fitted || fitted.Value() == NoIrValue) {
                return fitted;
            }

            *targets[index] = fitted.Value();
        }
    } else {
        auto fitted = FitFloat(context, first, 1, modes, "MipValue", "Invalid mip map parameter");
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
    if (!pixel) {
        if (mode == IrTextureMipMode::Implicit) {
            mode = IrTextureMipMode::Level;
        }

        viewMipBias = false;
    }

    const MaterialTraitsDesc& traits = context.Traits();
    const bool surfaceOrDecal =
        traits.domain == MaterialDomain::Surface || traits.domain == MaterialDomain::DeferredDecal;
    if (!surfaceOrDecal || context.Target(MaterialTargetField::IsMobile).isMobile ||
        kind != IrTextureKind::Texture2D) {
        viewMipBias = false;
    }

    // 좌표를 종류의 폭으로 맞춘다.
    auto fittedUv =
        FitFloat(context, uv, width, modes, "Coordinates", "Texture coordinates must be numeric");
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
        samplerType = static_cast<uint8>(value.immediate[2] >> 8);
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

    // 파라미터 샘플은 UE처럼 gather·AutomaticViewMipBias 입력을 쓰지 않는다(속성만).
    settings.gather = TextureGatherMode::None;
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

    auto texture = name.IsEmpty() ? AssetTexture(context, settings.texture, settings.samplerType)
                                  : ParameterTexture(context, parameter, settings.texture,
                                                     settings.samplerType);
    if (!texture || texture.Value() == NoIrValue) {
        return texture ? Result<void>::Success() : Result<void>::Failure(texture.Error());
    }

    auto sample =
        Sample(context, texture.Value(), settings, settings.samplerType, settings.viewMipBias);
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

constexpr LoweringEntry TextureEntries[] = {
    {"Material.TextureObject", &LowerTextureObject},
    {"Material.TextureObjectParameter", &LowerTextureObjectParameter},
    {"Material.TextureSample", &LowerTextureSample},
    {"Material.TextureSampleParameter2D", &LowerTextureSampleParameter2D},
    {"Material.TextureSampleParameterCube", &LowerTextureSampleParameterCube},
    {"Material.TextureSampleParameter2DArray", &LowerTextureSampleParameter2DArray},
    {"Material.TextureSampleParameterCubeArray", &LowerTextureSampleParameterCubeArray},
    {"Material.TextureSampleParameterVolume", &LowerTextureSampleParameterVolume},
};

} // namespace

Span<const LoweringEntry> TextureLowerings() noexcept {
    return TextureEntries;
}

} // namespace rocks::material::detail
