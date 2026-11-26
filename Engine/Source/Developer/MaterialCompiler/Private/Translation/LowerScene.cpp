#include <MaterialSchema/Nodes/Decals/DecalsNodeEnums.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// scene·깊이·버퍼 노드 lowering이다(Docs/Design/MaterialSystem 6.10절 MS2c4). 장면 텍스처는 pass가
// 공급하는 문맥 값으로 읽는다. UE가 머티리얼 단위로 한 번 내는 검사(HLSLMaterialTranslator.cpp:
// 1796–1823, MaterialShared.cpp FinalCompileValidation)는 같은 조건·문구로 그 값을 읽는 노드
// 위치에서 낸다.
namespace rocks::material::detail {

namespace {

// DepthFade의 FadeDistance 하한이다(UE UE_DELTA).
constexpr float DepthFadeMinimumDistance = 0.00001f;

// 모바일 scene depth의 상한이다(UE DepthFade의 ES3_1 clamp 65500).
constexpr float MobileSceneDepthLimit = 65500.0f;

bool IsSceneTranslucent(BlendMode mode) noexcept {
    return mode != BlendMode::Opaque && mode != BlendMode::Masked;
}

Result<void> EmitScene(LoweringContext& context, StringView key, Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

Result<IrValueId> SceneUInt(LoweringContext& context, uint32 value) noexcept {
    return context.Builder().UIntConstant(Span<const uint32>(&value, 1));
}

// value를 Float2로 맞춘다(UE CoerceParameter(…, MCT_Float2)).
Result<IrValueId> Float2Of(LoweringContext& context, IrValueId value, StringView pin) noexcept {
    return context.CoerceTo(value, IrFloat(2), pin);
}

// 오류를 내고 계속한다(UE Errorf는 번역을 멈추지 않는다). 시스템 실패만 돌려준다.
Result<void> Complain(LoweringContext& context, StringView message) noexcept {
    return context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(), message);
}

// scene depth를 읽는 노드의 머티리얼 조건이다(UE HT:1796–1799·SceneDepth). 문제가 없으면 true다.
Result<bool> CheckSceneDepth(LoweringContext& context, bool velocity) noexcept {
    const MaterialTraitsDesc& traits = context.Traits();
    bool ok = true;
    if (velocity && IsSceneTranslucent(traits.blendMode) && traits.outputTranslucentVelocity) {
        ok = false;
        if (auto reported =
                Complain(context, "Translucenct material with 'Output Velocity' enabled "
                                  "will write to depth buffer, therefore cannot read "
                                  "from depth buffer at the same time.");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    if (traits.domain != MaterialDomain::PostProcess && !IsSceneTranslucent(traits.blendMode)) {
        ok = false;
        if (auto reported = Complain(
                context, "Only transparent or postprocess materials can read from scene depth.");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    return Result<bool>::Success(ok);
}

// 화면 uv다(UE GetScreenAlignedUV): OffsetFraction이면 Input(연결이 없으면 핀 값 = ConstInput)의
// 오프셋, Coordinates면 연결된 Input의 buffer uv, 연결이 없으면 화면 정렬 uv다.
Result<IrValueId> ScreenUv(LoweringContext& context) noexcept {
    int64 mode = 0;
    auto read = context.ReadInteger("inputMode", 0, mode);
    if (!read || !read.Value()) {
        return read ? Result<IrValueId>::Success(NoIrValue)
                    : Result<IrValueId>::Failure(read.Error());
    }

    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (static_cast<SceneAttributeInputMode>(mode) == SceneAttributeInputMode::OffsetFraction) {
        auto offset = context.Input("Input");
        if (!offset || offset.Value() == NoIrValue) {
            return offset;
        }

        auto fitted = Float2Of(context, offset.Value(), "Input");
        if (!fitted || fitted.Value() == NoIrValue) {
            return fitted;
        }

        const IrValueId arguments[] = {fitted.Value()};
        return context.ContextValue("ScreenUvFromOffsetFraction", arguments);
    }

    if (!connected.Value()) {
        return context.ContextValue("ScreenAlignedUv");
    }

    auto uv = context.Input("Input");
    if (!uv || uv.Value() == NoIrValue) {
        return uv;
    }

    auto fitted = Float2Of(context, uv.Value(), "Input");
    if (!fitted || fitted.Value() == NoIrValue) {
        return fitted;
    }

    const IrValueId arguments[] = {fitted.Value()};
    return context.ContextValue("BufferUvFromViewportUv", arguments);
}

// UE SceneDepth(ME:10580–10607, HT:8127–8158).
Result<void> LowerSceneDepth(LoweringContext& context) noexcept {
    auto checked = CheckSceneDepth(context, true);
    if (!checked || !checked.Value()) {
        return checked ? Result<void>::Success() : Result<void>::Failure(checked.Error());
    }

    auto uv = ScreenUv(context);
    if (!uv || uv.Value() == NoIrValue) {
        return uv ? Result<void>::Success() : Result<void>::Failure(uv.Error());
    }

    const IrValueId arguments[] = {uv.Value()};
    return EmitScene(context, "Result", context.ContextValue("SceneDepthAt", arguments));
}

// UE DepthFade(ME:19764–19786).
Result<void> LowerDepthFade(LoweringContext& context) noexcept {
    auto opacity = context.Input("InOpacity");
    auto distance = context.Input("FadeDistance");
    if (!opacity || !distance) {
        return Result<void>::Failure(!opacity ? opacity.Error() : distance.Error());
    }

    if (opacity.Value() == NoIrValue || distance.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    auto checked = CheckSceneDepth(context, true);
    if (!checked || !checked.Value()) {
        return checked ? Result<void>::Success() : Result<void>::Failure(checked.Error());
    }

    auto minimum = context.FloatScalar(DepthFadeMinimumDistance);
    if (!minimum) {
        return Result<void>::Failure(minimum.Error());
    }

    auto fade = context.Binary(IrOp::Max, distance.Value(), minimum.Value(), NumberRule::FloatOnly);
    auto pixel = context.ContextValue("PixelDepth");
    if (!fade || !pixel) {
        return Result<void>::Failure(!fade ? fade.Error() : pixel.Error());
    }

    IrValueId pixelDepth = pixel.Value();
    if (context.Target(MaterialTargetField::IsMobile).isMobile) {
        // 모바일 scene depth는 65500까지다(가깝거나 넘는 물체가 잘못 흐려지지 않게 자른다).
        auto limit = context.FloatScalar(MobileSceneDepthLimit);
        if (!limit) {
            return Result<void>::Failure(limit.Error());
        }

        auto room =
            context.Binary(IrOp::Subtract, limit.Value(), fade.Value(), NumberRule::FloatOnly);
        if (!room) {
            return Result<void>::Failure(room.Error());
        }

        auto clamped = context.Binary(IrOp::Min, pixelDepth, room.Value(), NumberRule::FloatOnly);
        if (!clamped) {
            return Result<void>::Failure(clamped.Error());
        }

        pixelDepth = clamped.Value();
    }

    auto uv = context.ContextValue("ScreenAlignedUv");
    if (!uv) {
        return Result<void>::Failure(uv.Error());
    }

    const IrValueId arguments[] = {uv.Value()};
    auto scene = context.ContextValue("SceneDepthAt", arguments);
    if (!scene) {
        return Result<void>::Failure(scene.Error());
    }

    auto gap = context.Binary(IrOp::Subtract, scene.Value(), pixelDepth, NumberRule::FloatOnly);
    if (!gap) {
        return Result<void>::Failure(gap.Error());
    }

    auto ratio = context.Binary(IrOp::Divide, gap.Value(), fade.Value(), NumberRule::FloatOnly);
    if (!ratio) {
        return Result<void>::Failure(ratio.Error());
    }

    auto saturated = context.Unary(IrOp::Saturate, ratio.Value(), NumberRule::FloatOnly);
    if (!saturated) {
        return Result<void>::Failure(saturated.Error());
    }

    return EmitScene(
        context, "Result",
        context.Binary(IrOp::Multiply, opacity.Value(), saturated.Value(), NumberRule::Arithmetic));
}

// UE SceneDepthWithoutWater(ME:21805–21831, HT:12424–12466). 셰이딩 모델 집합은 MS2d BSDF 트리가
// 정하므로 그 전에는 Single Layer Water가 없는 것으로 본다(6.10절 MS2c4).
Result<void> LowerSceneDepthWithoutWater(LoweringContext& context) noexcept {
    if (context.Context() != EvaluationContext::Pixel) {
        return Complain(context, "Cannot read scene depth without water from the vertex shader.");
    }

    const MaterialTraitsDesc& traits = context.Traits();
    if (traits.domain != MaterialDomain::PostProcess) {
        constexpr bool UsesSingleLayerWater = false;
        if (!UsesSingleLayerWater) {
            return Complain(context, "Can only read scene depth below water when material Shading "
                                     "Model is Single Layer Water or when material Domain is "
                                     "PostProcess.");
        }

        if (traits.domain != MaterialDomain::Surface) {
            return Complain(context, "Can only read scene depth below water when material Domain "
                                     "is set to Surface or PostProcess.");
        }

        if (IsSceneTranslucent(traits.blendMode)) {
            return Complain(context, "Can only read scene depth below water when material Blend "
                                     "Mode isn't translucent.");
        }
    }

    float fallback = 0.0f;
    auto read = context.ReadFloat("fallbackDepth", 1000000.0f, fallback);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto uv = ScreenUv(context);
    if (!uv || uv.Value() == NoIrValue) {
        return uv ? Result<void>::Success() : Result<void>::Failure(uv.Error());
    }

    auto depth = context.FloatScalar(fallback);
    if (!depth) {
        return Result<void>::Failure(depth.Error());
    }

    const IrValueId arguments[] = {uv.Value(), depth.Value()};
    return EmitScene(context, "Result",
                     context.ContextValue("SceneDepthWithoutWaterAt", arguments));
}

// scene color를 읽는 노드의 FinalCompileValidation 조건이다(MaterialShared.cpp
// RequiresSceneColorCopy).
Result<void> CheckSceneColorCopy(LoweringContext& context) noexcept {
    const MaterialTraitsDesc& traits = context.Traits();
    if (traits.domain != MaterialDomain::Surface) {
        return Complain(context, "Only 'surface' material domain can use the scene color node.");
    }

    if (!IsSceneTranslucent(traits.blendMode)) {
        return Complain(context, "Only translucent materials can use the scene color node.");
    }

    return Result<void>::Success();
}

// UE SceneColor(ME:10775–10805, HT:8355–8386).
Result<void> LowerSceneColor(LoweringContext& context) noexcept {
    if (context.Context() != EvaluationContext::Pixel) {
        return context.Error(MaterialDiagnosticCode::StageViolation, StringView(),
                             "Invalid node used in vertex/hull/domain shader input!");
    }

    if (context.Traits().domain != MaterialDomain::Surface) {
        if (auto reported = Complain(
                context, "SceneColor lookups are only available when MaterialDomain = Surface.");
            !reported) {
            return reported;
        }
    }

    if (auto checked = CheckSceneColorCopy(context); !checked) {
        return checked;
    }

    auto uv = ScreenUv(context);
    if (!uv || uv.Value() == NoIrValue) {
        return uv ? Result<void>::Success() : Result<void>::Failure(uv.Error());
    }

    const IrValueId arguments[] = {uv.Value()};
    auto color = context.ContextValue("SceneColorAt", arguments);
    if (!color || color.Value() == NoIrValue) {
        return color ? Result<void>::Success() : Result<void>::Failure(color.Error());
    }

    if (auto rgb = EmitScene(context, "RGB", context.Prefix(color.Value(), 3)); !rgb) {
        return rgb;
    }

    const uint8 alpha[] = {3};
    return EmitScene(context, "A", context.Swizzle(color.Value(), alpha));
}

bool IsPostProcessInput(uint32 id) noexcept {
    return (id >= static_cast<uint32>(SceneTextureId::PostProcessInput0) &&
            id <= static_cast<uint32>(SceneTextureId::PostProcessInput6)) ||
           (id >= MaterialUserSceneTextureFirstId &&
            id < MaterialUserSceneTextureFirstId + MaxUserSceneTextures);
}

// UE UseSceneTextureId와 머티리얼 단위 scene texture 검사(HT:8262–8352, 1807–1823)다. lookup을 막는
// 오류가 있으면 false다(그 밖은 진단만 하고 계속 — UE Errorf).
Result<bool> CheckSceneTexture(LoweringContext& context, uint32 id) noexcept {
    const MaterialTraitsDesc& traits = context.Traits();
    if (id == static_cast<uint32>(SceneTextureId::DecalMask)) {
        if (auto reported =
                Complain(context, "Decal Mask bit was moved from GBuffer to the Stencil "
                                  "Buffer for performance optimisation so therefore no "
                                  "longer available.");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    bool ok = true;
    const auto complain = [&](StringView message) noexcept {
        ok = false;
        return Complain(context, message);
    };
    if (traits.domain == MaterialDomain::DeferredDecal &&
        id != static_cast<uint32>(SceneTextureId::SceneDepth) &&
        id != static_cast<uint32>(SceneTextureId::WorldNormal) &&
        id != static_cast<uint32>(SceneTextureId::CustomDepth) &&
        id != static_cast<uint32>(SceneTextureId::CustomStencil)) {
        if (auto reported = complain(
                "Decals can only access SceneDepth, CustomDepth, CustomStencil, and WorldNormal.");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    const bool sceneColor = id == static_cast<uint32>(SceneTextureId::SceneColor);
    if (sceneColor && traits.domain != MaterialDomain::Surface) {
        const StringView message =
            traits.domain == MaterialDomain::PostProcess
                ? StringView("SceneColor lookups are only available when MaterialDomain = Surface. "
                             "PostProcessMaterials should use the SceneTexture PostProcessInput0.")
                : StringView(
                      "SceneColor lookups are only available when MaterialDomain = Surface.");
        if (auto reported = complain(message); !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    const bool velocity = id == static_cast<uint32>(SceneTextureId::Velocity);
    if (velocity && traits.domain != MaterialDomain::PostProcess) {
        if (auto reported =
                complain("Velocity scene textures are only available in post process materials.");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    if (traits.domain != MaterialDomain::DeferredDecal &&
        traits.domain != MaterialDomain::PostProcess) {
        if (!IsSceneTranslucent(traits.blendMode)) {
            if (auto reported = complain("SceneTexture expressions cannot be used in opaque "
                                         "materials except if used with the Single Layer Water "
                                         "shading model.");
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }
        } else if (IsPostProcessInput(id) || velocity || sceneColor) {
            if (auto reported = complain("SceneTexture expressions cannot use post process inputs "
                                         "or scene color in non post process domain materials");
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }
        }
    }

    // FinalCompileValidation(RequiresSceneColorCopy)의 문구도 UE처럼 낸다.
    if (sceneColor) {
        if (traits.domain != MaterialDomain::Surface || !IsSceneTranslucent(traits.blendMode)) {
            ok = false;
            if (auto checked = CheckSceneColorCopy(context); !checked) {
                return Result<bool>::Failure(checked.Error());
            }
        }
    }

    if (id == static_cast<uint32>(SceneTextureId::SceneDepth)) {
        auto depth = CheckSceneDepth(context, false);
        if (!depth) {
            return depth;
        }

        ok = ok && depth.Value();
    }

    return Result<bool>::Success(ok);
}

// SceneTexture·UserSceneTexture의 출력이다(UE SceneTextureLookup·GetSceneTextureViewSize).
Result<void> EmitSceneTexture(LoweringContext& context, uint32 id, bool filtered,
                              bool clamped) noexcept {
    auto sceneId = SceneUInt(context, id);
    if (!sceneId) {
        return Result<void>::Failure(sceneId.Error());
    }

    // Size·InvSize는 검사 없이 view 크기다.
    const IrValueId sizeArguments[] = {sceneId.Value()};
    auto size = context.ContextValue("SceneTextureViewSize", sizeArguments);
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    static constexpr uint8 Xy[] = {0, 1};
    static constexpr uint8 Zw[] = {2, 3};
    if (auto written = EmitScene(context, "Size", context.Swizzle(size.Value(), Xy)); !written) {
        return written;
    }

    if (auto written = EmitScene(context, "InvSize", context.Swizzle(size.Value(), Zw)); !written) {
        return written;
    }

    // Color는 쓰일 때만 lookup한다(UE는 Color 출력을 컴파일할 때만 lookup·검사한다).
    auto used = context.IsOutputUsed("Color");
    if (!used || !used.Value()) {
        return used ? Result<void>::Success() : Result<void>::Failure(used.Error());
    }

    auto checked = CheckSceneTexture(context, id);
    if (!checked || !checked.Value()) {
        return checked ? Result<void>::Success() : Result<void>::Failure(checked.Error());
    }

    auto connected = context.IsConnected("Coordinates");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId uv = NoIrValue;
    if (connected.Value()) {
        auto input = context.Input("Coordinates");
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        auto fitted = Float2Of(context, input.Value(), "Coordinates");
        if (!fitted || fitted.Value() == NoIrValue) {
            return fitted ? Result<void>::Success() : Result<void>::Failure(fitted.Error());
        }

        const IrValueId arguments[] = {fitted.Value(), sceneId.Value()};
        auto converted = context.ContextValue("SceneTextureUvFromViewportUv", arguments);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        uv = converted.Value();
    } else {
        const IrValueId arguments[] = {sceneId.Value()};
        auto standard = context.ContextValue("SceneTextureDefaultUv", arguments);
        if (!standard) {
            return Result<void>::Failure(standard.Error());
        }

        uv = standard.Value();
        if (clamped) {
            const IrValueId clampArguments[] = {uv, sceneId.Value()};
            auto bounded = context.ContextValue("SceneTextureClampUv", clampArguments);
            if (!bounded) {
                return Result<void>::Failure(bounded.Error());
            }

            uv = bounded.Value();
        }
    }

    const bool filter = filtered;
    auto filterValue = context.Builder().BoolConstant(Span<const bool>(&filter, 1));
    if (!filterValue) {
        return Result<void>::Failure(filterValue.Error());
    }

    const IrValueId lookupArguments[] = {uv, sceneId.Value(), filterValue.Value()};
    auto lookup = context.ContextValue("SceneTextureLookup", lookupArguments);
    if (!lookup || lookup.Value() == NoIrValue) {
        return lookup ? Result<void>::Success() : Result<void>::Failure(lookup.Error());
    }

    // post process 입력의 pre-exposure 되돌림(UE HT:8236–8241).
    const MaterialTraitsDesc& traits = context.Traits();
    IrValueId color = lookup.Value();
    if (IsPostProcessInput(id) && traits.domain == MaterialDomain::PostProcess &&
        traits.postProcess.location != BlendableLocation::SceneColorAfterTonemapping &&
        !traits.postProcess.disablePreExposureScale) {
        auto inverse = context.ContextValue("OneOverPreExposure");
        auto one = context.FloatScalar(1.0f);
        if (!inverse || !one) {
            return Result<void>::Failure(!inverse ? inverse.Error() : one.Error());
        }

        auto spread = context.Splat(inverse.Value(), 3);
        if (!spread) {
            return Result<void>::Failure(spread.Error());
        }

        auto scale = context.Append(spread.Value(), one.Value());
        if (!scale) {
            return Result<void>::Failure(scale.Error());
        }

        auto scaled = context.Binary(IrOp::Multiply, scale.Value(), color, NumberRule::FloatOnly);
        if (!scaled) {
            return Result<void>::Failure(scaled.Error());
        }

        color = scaled.Value();
    }

    return context.Output("Color", color);
}

// UE SceneTexture(ME:10649–10673).
Result<void> LowerSceneTexture(LoweringContext& context) noexcept {
    int64 id = 0;
    bool filtered = false;
    auto idRead = context.ReadInteger("sceneTextureId", 0, id);
    auto filterRead = context.ReadBool("filtered", false, filtered);
    if (!idRead || !filterRead) {
        return Result<void>::Failure(!idRead ? idRead.Error() : filterRead.Error());
    }

    if (!idRead.Value() || !filterRead.Value()) {
        return Result<void>::Success();
    }

    return EmitSceneTexture(context, static_cast<uint32>(id), filtered, false);
}

// UE UserSceneTexture(ME:10707–10738).
Result<void> LowerUserSceneTexture(LoweringContext& context) noexcept {
    auto properties = context.Properties();
    if (!properties) {
        return Result<void>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<void>::Success();
    }

    auto name = properties.Value()->ReadText("userSceneTexture");
    if (!name) {
        if (name.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(name.Error());
        }

        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Node property cannot be read");
    }

    if (name.Value().IsEmpty()) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "UserSceneTexture missing name -- value must be set to something "
                             "other than None");
    }

    uint32 index = 0;
    auto admitted = context.UseUserSceneTexture(name.Value(), index);
    if (!admitted || !admitted.Value()) {
        return admitted ? Result<void>::Success() : Result<void>::Failure(admitted.Error());
    }

    bool filtered = false;
    bool clamped = false;
    auto filterRead = context.ReadBool("filtered", false, filtered);
    auto clampRead = context.ReadBool("clamped", false, clamped);
    if (!filterRead || !clampRead) {
        return Result<void>::Failure(!filterRead ? filterRead.Error() : clampRead.Error());
    }

    if (!filterRead.Value() || !clampRead.Value()) {
        return Result<void>::Success();
    }

    return EmitSceneTexture(context, MaterialUserSceneTextureFirstId + index, filtered, clamped);
}

// RGBA·RGB·A 출력이다.
Result<void> EmitRgba(LoweringContext& context, IrValueId value) noexcept {
    if (value == NoIrValue) {
        return Result<void>::Success();
    }

    if (auto rgba = context.Output("RGBA", value); !rgba) {
        return rgba;
    }

    if (auto rgb = EmitScene(context, "RGB", context.Prefix(value, 3)); !rgb) {
        return rgb;
    }

    const uint8 alpha[] = {3};
    return EmitScene(context, "A", context.Swizzle(value, alpha));
}

// UE DBufferTexture(ME:10832–10842, HT:8388–8412).
Result<void> LowerDecalBufferTexture(LoweringContext& context) noexcept {
    const MaterialTraitsDesc& traits = context.Traits();
    if (traits.domain != MaterialDomain::Surface || IsSceneTranslucent(traits.blendMode)) {
        if (auto reported = Complain(
                context, "DBuffer scene textures are only available on opaque or masked surfaces.");
            !reported) {
            return reported;
        }
    }

    int64 index = 0;
    auto read = context.ReadInteger("dBufferTextureId", 0, index);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto connected = context.IsConnected("Coordinates");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId uv = NoIrValue;
    if (connected.Value()) {
        auto input = context.Input("Coordinates");
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        auto fitted = Float2Of(context, input.Value(), "Coordinates");
        if (!fitted || fitted.Value() == NoIrValue) {
            return fitted ? Result<void>::Success() : Result<void>::Failure(fitted.Error());
        }

        const IrValueId arguments[] = {fitted.Value()};
        auto converted = context.ContextValue("DBufferUvFromViewportUv", arguments);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        uv = converted.Value();
    } else {
        auto zero = SceneUInt(context, 0);
        if (!zero) {
            return Result<void>::Failure(zero.Error());
        }

        const IrValueId arguments[] = {zero.Value()};
        auto standard = context.ContextValue("SceneTextureDefaultUv", arguments);
        if (!standard) {
            return Result<void>::Failure(standard.Error());
        }

        uv = standard.Value();
    }

    auto slot = SceneUInt(context, static_cast<uint32>(index));
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    const IrValueId arguments[] = {uv, slot.Value()};
    auto lookup = context.ContextValue("DBufferTextureLookup", arguments);
    if (!lookup) {
        return Result<void>::Failure(lookup.Error());
    }

    return EmitRgba(context, lookup.Value());
}

// UE PathTracingBufferTexture(ME:17933–17943, HT:8414–8434).
Result<void> LowerPathTracingBufferTexture(LoweringContext& context) noexcept {
    if (context.Traits().domain != MaterialDomain::PostProcess) {
        if (auto reported = Complain(
                context,
                "Path tracing buffer textures are only available on post process material.");
            !reported) {
            return reported;
        }
    }

    int64 index = 0;
    auto read = context.ReadInteger("pathTracingBufferTextureId", 0, index);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto connected = context.IsConnected("Coordinates");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId uv = NoIrValue;
    if (connected.Value()) {
        auto input = context.Input("Coordinates");
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        auto fitted = Float2Of(context, input.Value(), "Coordinates");
        if (!fitted || fitted.Value() == NoIrValue) {
            return fitted ? Result<void>::Success() : Result<void>::Failure(fitted.Error());
        }

        uv = fitted.Value();
    } else {
        auto standard = context.ContextValue("PathTracingBufferDefaultUv");
        if (!standard) {
            return Result<void>::Failure(standard.Error());
        }

        uv = standard.Value();
    }

    auto slot = SceneUInt(context, static_cast<uint32>(index));
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    const IrValueId arguments[] = {uv, slot.Value()};
    auto lookup = context.ContextValue("PathTracingBufferTextureLookup", arguments);
    if (!lookup) {
        return Result<void>::Failure(lookup.Error());
    }

    return EmitRgba(context, lookup.Value());
}

constexpr LoweringEntry SceneEntries[] = {
    {"Material.SceneTexture", &LowerSceneTexture},
    {"Material.UserSceneTexture", &LowerUserSceneTexture},
    {"Material.SceneColor", &LowerSceneColor},
    {"Material.SceneDepth", &LowerSceneDepth},
    {"Material.DepthFade", &LowerDepthFade},
    {"Material.SceneDepthWithoutWater", &LowerSceneDepthWithoutWater},
    {"Material.DecalBufferTexture", &LowerDecalBufferTexture},
    {"Material.PathTracingBufferTexture", &LowerPathTracingBufferTexture},
};

} // namespace

Span<const LoweringEntry> SceneLowerings() noexcept {
    return SceneEntries;
}

} // namespace rocks::material::detail
