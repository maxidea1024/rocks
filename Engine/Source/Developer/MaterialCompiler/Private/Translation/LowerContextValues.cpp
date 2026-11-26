#include "LoweringContext.h"
#include "LoweringTable.h"

// 문맥 값 노드 lowering이다(Docs/Design/MaterialSystem 6.9절 MS2b1 lowering 표). 값 하나를 읽는
// 노드는 문맥 값 표(19.3.2절)의 값을 그대로 내고, 출력 마스크(RGB·R·G·B·A 등)는 그 값의 성분이다.
// 근거는 UE MaterialExpressions.cpp의 Compile과 HLSLMaterialTranslator.cpp의 번역 함수,
// BaseMaterialExpressions.ini 외부 코드 선언이다.
namespace rocks::material::detail {

namespace {

// 성분 출력 키다(R·G·B·A).
constexpr StringView ContextComponentKeys[MaxIrComponents] = {"R", "G", "B", "A"};

// value의 성분 하나다.
Result<IrValueId> Component(LoweringContext& context, IrValueId value, uint8 index) noexcept {
    const uint8 component[] = {index};
    return context.Swizzle(value, Span<const uint8>(component, 1));
}

// 출력 key에 문맥 값 name을 낸다.
Result<void> OutputContextValue(LoweringContext& context, StringView key,
                                StringView name) noexcept {
    auto value = context.ContextValue(name);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

// 색 출력(rgbKey = 앞 셋, R·G·B·A, rgbaKey가 비어 있지 않으면 넷 전부)이다.
Result<void> OutputColor(LoweringContext& context, StringView name, StringView rgbKey,
                         StringView rgbaKey) noexcept {
    auto value = context.ContextValue(name);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    auto rgb = context.Prefix(value.Value(), 3);
    if (!rgb) {
        return Result<void>::Failure(rgb.Error());
    }

    if (auto written = context.Output(rgbKey, rgb.Value()); !written) {
        return written;
    }

    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        auto picked = Component(context, value.Value(), index);
        if (!picked) {
            return Result<void>::Failure(picked.Error());
        }

        if (auto written = context.Output(ContextComponentKeys[index], picked.Value()); !written) {
            return written;
        }
    }

    return rgbaKey.IsEmpty() ? Result<void>::Success() : context.Output(rgbaKey, value.Value());
}

// 값 하나를 Result 핀으로 내는 노드다(노드 종류 → 표 이름).
struct SingleValueNode {
    StringView type;
    StringView key;
    StringView value;
};

constexpr SingleValueNode SingleValueNodes[] = {
    {"Material.DeltaTime", "Result", "DeltaTime"},
    {"Material.IsOrthographic", "Result", "IsOrthographic"},
    {"Material.TwoSidedSign", "Result", "TwoSidedSign"},
    {"Material.PerInstanceRandom", "Result", "PerInstanceRandom"},
    {"Material.PerInstanceFadeAmount", "Result", "PerInstanceFadeAmount"},
    {"Material.DistanceCullFade", "Result", "DistanceCullFade"},
    {"Material.PrecomputedAoMask", "Result", "PrecomputedAoMask"},
    {"Material.IsFirstPerson", "Result", "IsFirstPerson"},
    {"Material.ObjectRadius", "Result", "ObjectRadius"},
    {"Material.ObjectOrientation", "Result", "ObjectOrientation"},
    {"Material.ObjectBounds", "Result", "ObjectBounds"},
    {"Material.PreSkinnedPosition", "Result", "PreSkinnedPosition"},
    {"Material.PreSkinnedNormal", "Result", "PreSkinnedNormal"},
    {"Material.VertexNormal", "Result", "VertexNormal"},
    {"Material.VertexTangent", "Result", "VertexTangent"},
    {"Material.PixelNormal", "Result", "PixelNormal"},
    {"Material.CameraVector", "Result", "CameraVector"},
    {"Material.LightVector", "Result", "LightVector"},
    {"Material.PixelDepth", "Result", "PixelDepth"},
    {"Material.ViewSize", "Result", "ViewSize"},
    {"Material.SceneTexelSize", "Result", "SceneTexelSize"},
    {"Material.LightmapUvs", "Result", "LightmapUvs"},
    {"Material.DecalLifetimeOpacity", "Opacity", "DecalLifetimeOpacity"},
    {"Material.ParticleDirection", "Result", "ParticleDirection"},
    {"Material.ParticleMacroUv", "Result", "ParticleMacroUv"},
    {"Material.ParticleMotionBlurFade", "Result", "ParticleMotionBlurFade"},
    {"Material.ParticleRadius", "Result", "ParticleRadius"},
    {"Material.ParticleRandom", "Result", "ParticleRandom"},
    {"Material.ParticleRelativeTime", "Result", "ParticleRelativeTime"},
    {"Material.ParticleSize", "Result", "ParticleSize"},
    {"Material.ParticleSpeed", "Result", "ParticleSpeed"},
    {"Material.AtmosphericLightVector", "Result", "AtmosphericLightVector"},
    {"Material.AtmosphericLightColor", "Result", "AtmosphericLightColor"},
    {"Material.EyeAdaptation", "EyeAdaptation", "EyeAdaptation"},
    {"Material.ScalarBlueNoise", "Result", "ScalarBlueNoise"},
};

Result<void> LowerSingleValue(LoweringContext& context) noexcept {
    for (const SingleValueNode& node : SingleValueNodes) {
        if (context.Definition().type == node.type) {
            return OutputContextValue(context, node.key, node.value);
        }
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// UE StaticBool: 속성 값의 bool 상수(MCT_StaticBool — HLSLMaterialTranslator.cpp:8914).
Result<void> LowerStaticBool(LoweringContext& context) noexcept {
    bool value = false;
    auto read = context.ReadBool("value", false, value);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto constant = context.Builder().BoolConstant(Span<const bool>(&value, 1));
    if (!constant) {
        return Result<void>::Failure(constant.Error());
    }

    return context.Output("Result", constant.Value());
}

// UE DDX·DDY(HLSLMaterialTranslator.cpp:11805–11892): float 값의 화면 미분, Double은 낮춘다. 픽셀
// 단계 전용(V4 — IR 연산 Ddx·Ddy).
Result<void> LowerDerivative(LoweringContext& context, IrOp op) noexcept {
    auto value = context.Input("Value");
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    if (value.Value() == NoIrValue) {
        auto connected = context.IsConnected("Value");
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        // UE 기본 번역기는 연결이 없으면 진단 없이 값 없음(INDEX_NONE)이고 IR 경로는 필수
        // 입력이다. 값 없는 결과가 조용히 번지지 않게 IR 경로처럼 진단한다(6.9절 편차).
        return connected.Value() ? Result<void>::Success()
                                 : context.Error(MaterialDiagnosticCode::MissingInput, "Value",
                                                 "Missing input Value");
    }

    auto derivative = context.Unary(op, value.Value(), NumberRule::FloatOnly);
    if (!derivative) {
        return Result<void>::Failure(derivative.Error());
    }

    return context.Output("Result", derivative.Value());
}

Result<void> LowerDdx(LoweringContext& context) noexcept {
    return LowerDerivative(context, IrOp::Ddx);
}

Result<void> LowerDdy(LoweringContext& context) noexcept {
    return LowerDerivative(context, IrOp::Ddy);
}

// UE Time(HLSLMaterialTranslator.cpp:5276–5323): ignorePause면 RealTime, 아니면 GameTime(이전 정점
// 문맥은 이전 프레임 시간). override_Period면 period 0은 상수 0, 아니면 fmod(시간, period).
Result<void> LowerTime(LoweringContext& context) noexcept {
    bool ignorePause = false;
    bool overridePeriod = false;
    float period = 0.0f;
    auto ignore = context.ReadBool("ignorePause", false, ignorePause);
    auto overrideRead = context.ReadBool("override_Period", false, overridePeriod);
    auto periodRead = context.ReadFloat("period", 0.0f, period);
    if (!ignore || !overrideRead || !periodRead) {
        return Result<void>::Failure(!ignore         ? ignore.Error()
                                     : !overrideRead ? overrideRead.Error()
                                                     : periodRead.Error());
    }

    if (!ignore.Value() || !overrideRead.Value() || !periodRead.Value()) {
        return Result<void>::Success();
    }

    if (overridePeriod && period == 0.0f) {
        auto zero = context.FloatScalar(0.0f);
        if (!zero) {
            return Result<void>::Failure(zero.Error());
        }

        return context.Output("Result", zero.Value());
    }

    auto time = context.ContextValue(ignorePause ? StringView("RealTime") : StringView("Time"));
    if (!time) {
        return Result<void>::Failure(time.Error());
    }

    if (!overridePeriod) {
        return context.Output("Result", time.Value());
    }

    auto divisor = context.FloatConstant(Span<const float>(&period, 1), StringView());
    if (!divisor) {
        return Result<void>::Failure(divisor.Error());
    }

    auto wrapped = context.Binary(IrOp::Fmod, time.Value(), divisor.Value(), NumberRule::FloatOnly);
    if (!wrapped) {
        return Result<void>::Failure(wrapped.Error());
    }

    return context.Output("Result", wrapped.Value());
}

// UE VertexColor(HLSLMaterialTranslator.cpp:9116): 정점 색, Result는 RGB(마스크 — UE 출력 0).
Result<void> LowerVertexColor(LoweringContext& context) noexcept {
    return OutputColor(context, "VertexColor", "Result", StringView());
}

// UE DecalColor(외부 코드, BaseMaterialExpressions.ini:1041).
Result<void> LowerDecalColor(LoweringContext& context) noexcept {
    return OutputColor(context, "DecalColor", "RGB", "RGBA");
}

// UE ParticleColor(HLSLMaterialTranslator.cpp:6029, ini:1056).
Result<void> LowerParticleColor(LoweringContext& context) noexcept {
    return OutputColor(context, "ParticleColor", "RGB", "RGBA");
}

// UE FontSignedDistance(ini:1046): 네 출력은 FontSignedDistanceData의 x·y·z·w다.
Result<void> LowerFontSignedDistance(LoweringContext& context) noexcept {
    constexpr StringView Keys[MaxIrComponents] = {"Signed Distance", "Smooth Signed Distance",
                                                  "Pixel Distance Factor", "Implicit Opacity"};
    auto value = context.ContextValue("FontSignedDistanceData");
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        auto picked = Component(context, value.Value(), index);
        if (!picked) {
            return Result<void>::Failure(picked.Error());
        }

        if (auto written = context.Output(Keys[index], picked.Value()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// UE ScreenPosition(HLSLMaterialTranslator.cpp:5924–5960): ViewportUV·PixelPosition.
Result<void> LowerScreenPosition(LoweringContext& context) noexcept {
    if (auto viewport = OutputContextValue(context, "ViewportUV", "ViewportUv"); !viewport) {
        return viewport;
    }

    return OutputContextValue(context, "PixelPosition", "PixelPosition");
}

// UE ParticleSpriteRotation(ini:1067): float2(라디안, 도)의 x·y.
Result<void> LowerParticleSpriteRotation(LoweringContext& context) noexcept {
    constexpr StringView Keys[] = {"Rad", "Deg"};
    auto value = context.ContextValue("ParticleSpriteRotation");
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    uint8 index = 0;
    for (StringView key : Keys) {
        auto picked = Component(context, value.Value(), index++);
        if (!picked) {
            return Result<void>::Failure(picked.Error());
        }

        if (auto written = context.Output(key, picked.Value()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// UE ParticleSubUVProperties(HLSLMaterialTranslator.cpp:6006–6027, ini:1068–1070).
Result<void> LowerParticleSubUvProperties(LoweringContext& context) noexcept {
    if (auto first = OutputContextValue(context, "TextureCoordinate0", "ParticleSubUvCoords0");
        !first) {
        return first;
    }

    if (auto second = OutputContextValue(context, "TextureCoordinate1", "ParticleSubUvCoords1");
        !second) {
        return second;
    }

    return OutputContextValue(context, "Blend", "ParticleSubUvLerp");
}

// UE PerInstanceCustomData(3Vector)(HLSLMaterialTranslator.cpp:14643–14672): (dataIndex, 기본값)
// 인자의 함수형 값. 기본값은 결과 폭의 Float로 맞춘다(UE는 그대로 넘겨 HLSL 암묵 변환에 맡긴다 —
// 6.9절 편차).
Result<void> LowerPerInstanceCustomDataOf(LoweringContext& context, StringView name,
                                          IrType type) noexcept {
    int64 index = 0;
    auto read = context.ReadInteger("dataIndex", 0, index);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto fallback = context.Input("DefaultValue");
    if (!fallback) {
        return Result<void>::Failure(fallback.Error());
    }

    auto coerced = context.CoerceTo(fallback.Value(), type, "DefaultValue");
    if (!coerced) {
        return Result<void>::Failure(coerced.Error());
    }

    const uint32 slot = static_cast<uint32>(index);
    auto indexValue = context.Builder().UIntConstant(Span<const uint32>(&slot, 1));
    if (!indexValue) {
        return Result<void>::Failure(indexValue.Error());
    }

    const IrValueId arguments[] = {indexValue.Value(), coerced.Value()};
    auto value = context.ContextValue(name, arguments);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

Result<void> LowerPerInstanceCustomData(LoweringContext& context) noexcept {
    return LowerPerInstanceCustomDataOf(context, "PerInstanceCustomData", IrFloat(1));
}

Result<void> LowerPerInstanceCustomData3(LoweringContext& context) noexcept {
    return LowerPerInstanceCustomDataOf(context, "PerInstanceCustomData3Vector", IrFloat(3));
}

constexpr LoweringEntry ContextValueEntries[] = {
    {"Material.DeltaTime", &LowerSingleValue},
    {"Material.IsOrthographic", &LowerSingleValue},
    {"Material.TwoSidedSign", &LowerSingleValue},
    {"Material.PerInstanceRandom", &LowerSingleValue},
    {"Material.PerInstanceFadeAmount", &LowerSingleValue},
    {"Material.DistanceCullFade", &LowerSingleValue},
    {"Material.PrecomputedAoMask", &LowerSingleValue},
    {"Material.IsFirstPerson", &LowerSingleValue},
    {"Material.ObjectRadius", &LowerSingleValue},
    {"Material.ObjectOrientation", &LowerSingleValue},
    {"Material.ObjectBounds", &LowerSingleValue},
    {"Material.PreSkinnedPosition", &LowerSingleValue},
    {"Material.PreSkinnedNormal", &LowerSingleValue},
    {"Material.VertexNormal", &LowerSingleValue},
    {"Material.VertexTangent", &LowerSingleValue},
    {"Material.PixelNormal", &LowerSingleValue},
    {"Material.CameraVector", &LowerSingleValue},
    {"Material.LightVector", &LowerSingleValue},
    {"Material.PixelDepth", &LowerSingleValue},
    {"Material.ViewSize", &LowerSingleValue},
    {"Material.SceneTexelSize", &LowerSingleValue},
    {"Material.LightmapUvs", &LowerSingleValue},
    {"Material.DecalLifetimeOpacity", &LowerSingleValue},
    {"Material.ParticleDirection", &LowerSingleValue},
    {"Material.ParticleMacroUv", &LowerSingleValue},
    {"Material.ParticleMotionBlurFade", &LowerSingleValue},
    {"Material.ParticleRadius", &LowerSingleValue},
    {"Material.ParticleRandom", &LowerSingleValue},
    {"Material.ParticleRelativeTime", &LowerSingleValue},
    {"Material.ParticleSize", &LowerSingleValue},
    {"Material.ParticleSpeed", &LowerSingleValue},
    {"Material.AtmosphericLightVector", &LowerSingleValue},
    {"Material.AtmosphericLightColor", &LowerSingleValue},
    {"Material.EyeAdaptation", &LowerSingleValue},
    {"Material.ScalarBlueNoise", &LowerSingleValue},
    {"Material.StaticBool", &LowerStaticBool},
    {"Material.Ddx", &LowerDdx},
    {"Material.Ddy", &LowerDdy},
    {"Material.Time", &LowerTime},
    {"Material.VertexColor", &LowerVertexColor},
    {"Material.DecalColor", &LowerDecalColor},
    {"Material.ParticleColor", &LowerParticleColor},
    {"Material.FontSignedDistance", &LowerFontSignedDistance},
    {"Material.ScreenPosition", &LowerScreenPosition},
    {"Material.ParticleSpriteRotation", &LowerParticleSpriteRotation},
    {"Material.ParticleSubUvProperties", &LowerParticleSubUvProperties},
    {"Material.PerInstanceCustomData", &LowerPerInstanceCustomData},
    {"Material.PerInstanceCustomData3", &LowerPerInstanceCustomData3},
};

} // namespace

Span<const LoweringEntry> ContextValueLowerings() noexcept {
    return ContextValueEntries;
}

} // namespace rocks::material::detail
