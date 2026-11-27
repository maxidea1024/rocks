#include <MaterialCompiler/MaterialIrLibrary.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/Nodes/Noise/NoiseNodeEnums.h>
#include <Platform/Memory.h>

#include "LoweringContext.h"
#include "LoweringTable.h"

// Noise·VectorNoise lowering이다(Docs/Design/MaterialSystem 6.13절 MS2f3). 결과는 IR Library 호출
// 하나다 (생성 원문 MaterialExpressionNoise·MaterialExpressionVectorNoise — MaterialNoise.rsh).
namespace rocks::material::detail {

namespace {

// levels 범위다(UE FMath::Clamp(Levels, 1, 10) — HLSLMaterialTranslator.cpp:11980).
constexpr int64 NoiseMinimumLevels = 1;
constexpr int64 NoiseMaximumLevels = 10;
// UE PositionOrigin CameraRelative 값이다.
constexpr int64 NoiseCameraRelativeOrigin = 1;
// VectorNoise 결과 폭과 RGB 폭이다.
constexpr uint8 VectorNoiseResultComponents = 4;
constexpr uint8 VectorNoiseColorComponents = 3;

uint64 NoiseFloatBits(float value) noexcept {
    uint32 bits = 0;
    platform::MemoryCopy(&bits, &value, sizeof(bits));
    return bits;
}

// Position 입력 또는 원점 종류의 월드 위치다. CameraRelative 위치는 World로 바꾼다(UE
// TransformPosition(TranslatedWorld → World) — Double(A) − PreViewTranslation). wide면 Double을
// 그대로 두고(Noise LWC 판), 아니면 float3로 낮춘다(VectorNoise — UE CoerceParameter Float3).
Result<IrValueId> NoisePosition(LoweringContext& context, int64 origin, bool wide) noexcept {
    auto connected = context.IsConnected("Position");
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    Result<IrValueId> position = Result<IrValueId>::Success(NoIrValue);
    if (!connected.Value()) {
        // CameraRelative는 TranslatedWorld를 World로 되돌리므로 곧 절대 월드 위치다.
        position = context.ContextValue("WorldPosition");
    } else {
        position = context.Input("Position");
        if (position && position.Value() != NoIrValue && origin == NoiseCameraRelativeOrigin) {
            auto preView = context.ContextValue("PreViewTranslation");
            auto widened = context.Convert(position.Value(), IrScalar::Double);
            if (!preView || !widened) {
                return !preView ? Move(preView) : Move(widened);
            }

            if (preView.Value() == NoIrValue || widened.Value() == NoIrValue) {
                return Result<IrValueId>::Success(NoIrValue);
            }

            position = context.Builder().Binary(IrOp::Subtract, widened.Value(), preView.Value());
        }
    }

    if (!position || position.Value() == NoIrValue) {
        return position;
    }

    const IrType type = context.TypeOf(position.Value());
    if (wide && type.scalar == IrScalar::Double) {
        return context.Coerce(position.Value(), MaterialPinType::Double3, "Position");
    }

    return context.Coerce(position.Value(), MaterialPinType::Float3, "Position");
}

// Noise다(UE MaterialExpressionNoise.cpp:109–134, HLSLMaterialTranslator.cpp:11968–12016).
Result<void> LowerNoise(LoweringContext& context) noexcept {
    int64 origin = 0;
    float scale = 1.0f;
    int64 quality = 1;
    int64 function = static_cast<int64>(NoiseFunction::SimplexTex);
    bool turbulence = true;
    int64 levels = 6;
    float outputMin = -1.0f;
    float outputMax = 1.0f;
    float levelScale = 2.0f;
    bool tiling = false;
    int64 repeatSize = 512;
    // 읽기를 모두 시도하고 첫 실패·읽지 못함을 남긴다(진단은 같은 문구 한 번).
    Result<void> failure = Result<void>::Success();
    bool readable = true;
    const auto take = [&](Result<bool> read) noexcept {
        if (!read) {
            failure = failure ? Result<void>::Failure(read.Error()) : Move(failure);
        } else {
            readable = readable && read.Value();
        }
    };

    take(context.ReadInteger("worldPositionOriginType", 0, origin));
    take(context.ReadFloat("scale", 1.0f, scale));
    take(context.ReadInteger("quality", 1, quality));
    take(context.ReadInteger("noiseFunction", function, function));
    take(context.ReadBool("turbulence", true, turbulence));
    take(context.ReadInteger("levels", 6, levels));
    take(context.ReadFloat("outputMin", -1.0f, outputMin));
    take(context.ReadFloat("outputMax", 1.0f, outputMax));
    take(context.ReadFloat("levelScale", 2.0f, levelScale));
    take(context.ReadBool("tiling", false, tiling));
    take(context.ReadInteger("repeatSize", 512, repeatSize));
    if (!failure || !readable) {
        return failure;
    }

    if (function == static_cast<int64>(NoiseFunction::SimplexTex) ||
        function == static_cast<int64>(NoiseFunction::GradientTex) ||
        function == static_cast<int64>(NoiseFunction::GradientTex3D)) {
        // TODO(MPQ-NoiseTexture): 렌더러 noise 시스템 텍스처가 생기면 LUT 함수를 번역한다.
        auto pending = context.Builder().Pending(
            IrFloat(1), IrPendingKind::PrerequisiteMissing,
            static_cast<uint8>(MaterialPrerequisite::NoiseTexture), context.Node().id);
        if (!pending) {
            return Result<void>::Failure(pending.Error());
        }

        return context.Output("Result", pending.Value());
    }

    auto position = NoisePosition(context, origin, true);
    if (!position || position.Value() == NoIrValue) {
        return position ? Result<void>::Success() : Result<void>::Failure(position.Error());
    }

    auto filterConnected = context.IsConnected("FilterWidth");
    if (!filterConnected) {
        return Result<void>::Failure(filterConnected.Error());
    }

    Result<IrValueId> filter =
        filterConnected.Value() ? context.Input("FilterWidth") : context.FloatScalar(0.0f);
    if (filter && filter.Value() != NoIrValue) {
        filter = context.CoerceTo(filter.Value(), IrFloat(1), "FilterWidth");
    }

    if (!filter || filter.Value() == NoIrValue) {
        return filter ? Result<void>::Success() : Result<void>::Failure(filter.Error());
    }

    const int64 clamped = levels < NoiseMinimumLevels   ? NoiseMinimumLevels
                          : levels > NoiseMaximumLevels ? NoiseMaximumLevels
                                                        : levels;
    const uint64 packed =
        (static_cast<uint64>(function) & IrLibraryByteMask) |
        ((static_cast<uint64>(quality) & IrLibraryByteMask) << IrLibraryQualityShift) |
        (static_cast<uint64>(turbulence ? 1 : 0) << IrLibraryNoiseTurbulenceShift) |
        (static_cast<uint64>(tiling ? 1 : 0) << IrLibraryNoiseTilingShift) |
        (static_cast<uint64>(clamped) << IrLibraryNoiseLevelsShift) |
        ((static_cast<uint64>(repeatSize) & IrLibraryWordMask) << IrLibraryHighWordShift);
    const uint64 parameters[] = {
        packed, NoiseFloatBits(scale) | (NoiseFloatBits(levelScale) << IrLibraryHighWordShift),
        NoiseFloatBits(outputMin) | (NoiseFloatBits(outputMax) << IrLibraryHighWordShift)};
    const IrValueId arguments[] = {position.Value(), filter.Value()};
    auto noise = context.Builder().Library(static_cast<uint32>(IrLibraryFunction::Noise),
                                           parameters, IrFloat(1), arguments);
    if (!noise) {
        return Result<void>::Failure(noise.Error());
    }

    return context.Output("Result", noise.Value());
}

// VectorNoise다(UE MaterialExpressionNoise.cpp:222–236, HLSLMaterialTranslator.cpp:12018–12056 —
// GradientALU·Voronoi는 float4, 나머지는 xyz).
Result<void> LowerVectorNoise(LoweringContext& context) noexcept {
    int64 origin = 0;
    int64 function = static_cast<int64>(VectorNoiseFunction::CellnoiseALU);
    int64 quality = 1;
    bool tiling = false;
    int64 tileSize = 300;
    // 읽기를 모두 시도하고 첫 실패·읽지 못함을 남긴다(진단은 같은 문구 한 번).
    Result<void> failure = Result<void>::Success();
    bool readable = true;
    const auto take = [&](Result<bool> read) noexcept {
        if (!read) {
            failure = failure ? Result<void>::Failure(read.Error()) : Move(failure);
        } else {
            readable = readable && read.Value();
        }
    };

    take(context.ReadInteger("worldPositionOriginType", 0, origin));
    take(context.ReadInteger("noiseFunction", function, function));
    take(context.ReadInteger("quality", 1, quality));
    take(context.ReadBool("tiling", false, tiling));
    take(context.ReadInteger("tileSize", 300, tileSize));
    if (!failure || !readable) {
        return failure;
    }

    auto position = NoisePosition(context, origin, false);
    if (!position || position.Value() == NoIrValue) {
        return position ? Result<void>::Success() : Result<void>::Failure(position.Error());
    }

    const uint64 packed =
        (static_cast<uint64>(function) & IrLibraryByteMask) |
        ((static_cast<uint64>(quality) & IrLibraryByteMask) << IrLibraryQualityShift) |
        (static_cast<uint64>(tiling ? 1 : 0) << IrLibraryVectorNoiseTilingShift) |
        ((static_cast<uint64>(tileSize) & IrLibraryWordMask) << IrLibraryHighWordShift);
    const uint64 parameters[] = {packed};
    const IrValueId arguments[] = {position.Value()};
    auto noise =
        context.Builder().Library(static_cast<uint32>(IrLibraryFunction::VectorNoise), parameters,
                                  IrFloat(VectorNoiseResultComponents), arguments);
    if (!noise) {
        return Result<void>::Failure(noise.Error());
    }

    const bool full = function == static_cast<int64>(VectorNoiseFunction::GradientALU) ||
                      function == static_cast<int64>(VectorNoiseFunction::VoronoiALU);
    if (full) {
        return context.Output("Result", noise.Value());
    }

    auto color = context.Prefix(noise.Value(), VectorNoiseColorComponents);
    if (!color) {
        return Result<void>::Failure(color.Error());
    }

    return context.Output("Result", color.Value());
}

constexpr LoweringEntry NoiseEntries[] = {
    {"Material.Noise", &LowerNoise},
    {"Material.VectorNoise", &LowerVectorNoise},
};

} // namespace

Span<const LoweringEntry> NoiseLowerings() noexcept {
    return NoiseEntries;
}

} // namespace rocks::material::detail
