#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <Platform/Memory.h>

#include "LoweringContext.h"
#include "LoweringTable.h"

// 공간 변환 노드 lowering이다(Docs/Design/MaterialSystem 6.9절 MS2b2b). UE 기본 번역기
// TransformBase(HLSLMaterialTranslator.cpp:11093–11507)와 같은 직접 쌍·경유 공간·입력 캐스트·결과
// 타입이다. 행렬 곱 쌍은 pass가 공급하는 문맥 함수
// 값(Transform<원본><Position|Vector>To<대상>)이고, TranslatedWorld ↔
// World·PeriodicWorld·FirstPerson은 IR 연산이다.
namespace rocks::material::detail {

namespace {

// UE EMaterialCommonBasis다(MaterialShared.h:310–323 — 이 파일 안 번호).
enum class Basis : uint8 {
    Tangent = 0,
    Local = 1,
    World = 2,
    View = 3,
    Camera = 4,
    MeshParticle = 5,
    Instance = 6,
    TranslatedWorld = 7,
    PeriodicWorld = 8,
    FirstPerson = 9,
    Invalid = 10
};

// 벡터 변환 enum(UE EMaterialVectorCoordTransform·Source — 같은 번호)의 공간이다.
Basis VectorBasis(int64 value) noexcept {
    switch (value) {
    case 0:
        return Basis::Tangent;
    case 1:
        return Basis::Local;
    case 2:
        return Basis::World;
    case 3:
        return Basis::View;
    case 4:
        return Basis::Camera;
    case 5:
        return Basis::MeshParticle;
    case 6:
        return Basis::Instance;
    default:
        return Basis::Invalid;
    }
}

// 위치 변환 enum(UE EMaterialPositionTransformSource)의 공간이다.
Basis PositionBasis(int64 value) noexcept {
    switch (value) {
    case 0:
        return Basis::Local;
    case 1:
        return Basis::World;
    case 2:
        return Basis::PeriodicWorld;
    case 3:
        return Basis::TranslatedWorld;
    case 4:
        return Basis::FirstPerson;
    case 5:
        return Basis::View;
    case 6:
        return Basis::Camera;
    case 7:
        return Basis::MeshParticle;
    case 8:
        return Basis::Instance;
    default:
        return Basis::Invalid;
    }
}

// 직접 쌍의 계산 방식이다.
enum class DirectKind : uint8 {
    None = 0,
    // 문맥 함수 값 하나.
    Function = 1,
    // 입력 그대로.
    Identity = 2,
    // Double(A) − PreViewTranslation(UE WSSubtract).
    SubtractPreView = 3,
    // Float(A + PreViewTranslation)(UE WSAddDemote).
    AddPreViewDemote = 4,
    // A − PeriodicWorldOrigin.
    SubtractPeriodic = 5,
    // A + PeriodicWorldOrigin.
    AddPeriodic = 6,
    // TransformToFirstPerson(A, saturate(alpha)).
    FirstPerson = 7
};

struct Direct {
    DirectKind kind = DirectKind::None;
    StringView function;
};

constexpr Direct Function(StringView name) noexcept {
    return Direct{DirectKind::Function, name};
}

// UE TransformBase의 직접 쌍이다(위치·벡터). 없으면 None(경유).
Direct DirectTransform(Basis from, Basis to, bool position) noexcept {
    switch (from) {
    case Basis::Tangent:
        return to == Basis::World && !position ? Function("TransformTangentVectorToWorld")
                                               : Direct{};
    case Basis::Local:
        if (to == Basis::World) {
            return position ? Function("TransformLocalPositionToWorld")
                            : Function("TransformLocalVectorToWorld");
        }

        return to == Basis::TranslatedWorld && position
                   ? Function("TransformLocalPositionToTranslatedWorld")
                   : Direct{};
    case Basis::TranslatedWorld:
        switch (to) {
        case Basis::World:
            return position ? Direct{DirectKind::SubtractPreView, {}}
                            : Direct{DirectKind::Identity, {}};
        case Basis::Camera:
            return position ? Function("TransformTranslatedWorldPositionToCamera")
                            : Function("TransformTranslatedWorldVectorToCamera");
        case Basis::View:
            return position ? Function("TransformTranslatedWorldPositionToView")
                            : Function("TransformTranslatedWorldVectorToView");
        case Basis::Tangent:
            return Function("TransformWorldVectorToTangent");
        case Basis::Local:
            return position ? Function("TransformTranslatedWorldPositionToLocal")
                            : Function("TransformTranslatedWorldVectorToLocal");
        case Basis::MeshParticle:
            return position ? Function("TransformTranslatedWorldPositionToParticle")
                            : Function("TransformTranslatedWorldVectorToParticle");
        case Basis::Instance:
            return position ? Function("TransformTranslatedWorldPositionToInstance")
                            : Function("TransformTranslatedWorldVectorToInstance");
        case Basis::PeriodicWorld:
            return Direct{DirectKind::SubtractPeriodic, {}};
        case Basis::FirstPerson:
            return Direct{DirectKind::FirstPerson, {}};
        default:
            return Direct{};
        }
    case Basis::World:
        switch (to) {
        case Basis::Tangent:
            return Function("TransformWorldVectorToTangent");
        case Basis::Local:
            return position ? Function("TransformWorldPositionToLocal")
                            : Function("TransformWorldVectorToLocal");
        case Basis::TranslatedWorld:
            return position ? Direct{DirectKind::AddPreViewDemote, {}}
                            : Direct{DirectKind::Identity, {}};
        case Basis::MeshParticle:
            return position ? Function("TransformWorldPositionToParticle")
                            : Function("TransformWorldVectorToParticle");
        case Basis::Instance:
            return position ? Function("TransformWorldPositionToInstance")
                            : Function("TransformWorldVectorToInstance");
        default:
            return Direct{};
        }
    case Basis::Camera:
        if (to != Basis::TranslatedWorld) {
            return Direct{};
        }

        return position ? Function("TransformCameraPositionToTranslatedWorld")
                        : Function("TransformCameraVectorToTranslatedWorld");
    case Basis::View:
        if (to != Basis::TranslatedWorld) {
            return Direct{};
        }

        return position ? Function("TransformViewPositionToTranslatedWorld")
                        : Function("TransformViewVectorToTranslatedWorld");
    case Basis::MeshParticle:
        if (to == Basis::World) {
            return position ? Function("TransformParticlePositionToWorld")
                            : Function("TransformParticleVectorToWorld");
        }

        if (to == Basis::TranslatedWorld) {
            return position ? Function("TransformParticlePositionToTranslatedWorld")
                            : Function("TransformParticleVectorToTranslatedWorld");
        }

        return Direct{};
    case Basis::Instance:
        if (to == Basis::World) {
            return position ? Function("TransformInstancePositionToWorld")
                            : Function("TransformInstanceVectorToWorld");
        }

        if (to == Basis::TranslatedWorld) {
            return position ? Function("TransformInstancePositionToTranslatedWorld")
                            : Function("TransformInstanceVectorToTranslatedWorld");
        }

        return Direct{};
    case Basis::PeriodicWorld:
        return to == Basis::TranslatedWorld ? Direct{DirectKind::AddPeriodic, {}} : Direct{};
    default:
        return Direct{};
    }
}

// 직접 쌍이 없을 때의 경유 공간이다(UE: 기본 World, 원본 World·Camera·View·PeriodicWorld와
// Local·Particle·Instance → PeriodicWorld·FirstPerson은 TranslatedWorld).
Basis Intermediary(Basis from, Basis to) noexcept {
    if (from == Basis::World || from == Basis::Camera || from == Basis::View ||
        from == Basis::PeriodicWorld) {
        return Basis::TranslatedWorld;
    }

    if ((from == Basis::Local || from == Basis::MeshParticle || from == Basis::Instance) &&
        (to == Basis::PeriodicWorld || to == Basis::FirstPerson)) {
        return Basis::TranslatedWorld;
    }

    return Basis::World;
}

// 변환 하나의 부가 입력이다(그 공간을 쓸 때만 번역한다 — MaterialExpressions.cpp:11275–11282).
struct TransformExtras {
    IrValueId tileSize = NoIrValue;
    bool tileIsPowerOfTwo = false;
    IrValueId alpha = NoIrValue;
};

// value를 scalar 종류의 3 성분으로 맞춘다(UE ValidCast — Float4는 앞 셋).
Result<IrValueId> CastToThree(LoweringContext& context, IrValueId value, IrScalar scalar) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    auto three = context.Prefix(value, 3);
    if (!three) {
        return three;
    }

    return context.Convert(three.Value(), scalar);
}

// 경유 깊이 상한이다(UE 경유는 많아야 두 번 — 표가 잘못되어도 무한 재귀하지 않게).
constexpr uint32 MaxTransformDepth = 4;

Result<IrValueId> TransformBase(LoweringContext& context, Basis from, Basis to, IrValueId value,
                                bool position, const TransformExtras& extras,
                                uint32 depth) noexcept {
    if (value == NoIrValue || from == to) {
        return Result<IrValueId>::Success(value);
    }

    const Direct direct = DirectTransform(from, to, position);
    if (direct.kind == DirectKind::None) {
        const Basis middle = Intermediary(from, to);
        if (depth >= MaxTransformDepth || middle == from || middle == to) {
            return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
        }

        auto halfway = TransformBase(context, from, middle, value, position, extras, depth + 1);
        if (!halfway) {
            return halfway;
        }

        return TransformBase(context, middle, to, halfway.Value(), position, extras, depth + 1);
    }

    const IrScalar inputScalar =
        from == Basis::World && position ? IrScalar::Double : IrScalar::Float;
    auto cast = CastToThree(context, value, inputScalar);
    if (!cast || cast.Value() == NoIrValue) {
        return cast;
    }

    switch (direct.kind) {
    case DirectKind::Function: {
        const IrValueId arguments[] = {cast.Value()};
        return context.ContextValue(direct.function, arguments);
    }
    case DirectKind::Identity:
        return cast;
    case DirectKind::SubtractPreView:
    case DirectKind::AddPreViewDemote: {
        // 자르지 않는 Double 연산이다(UE WSSubtract·WSAddDemote 직접 코드 — Sub의 자름 없음).
        auto preView = context.ContextValue("PreViewTranslation");
        auto wide = context.Convert(cast.Value(), IrScalar::Double);
        if (!preView || !wide) {
            return !preView ? Move(preView) : Move(wide);
        }

        if (preView.Value() == NoIrValue) {
            return preView;
        }

        const IrOp op = direct.kind == DirectKind::SubtractPreView ? IrOp::Subtract : IrOp::Add;
        auto combined = context.Builder().Binary(op, wide.Value(), preView.Value());
        if (!combined || direct.kind == DirectKind::SubtractPreView) {
            return combined;
        }

        return context.Convert(combined.Value(), IrScalar::Float);
    }
    case DirectKind::SubtractPeriodic:
    case DirectKind::AddPeriodic: {
        if (extras.tileSize == NoIrValue) {
            if (auto reported =
                    context.Error(MaterialDiagnosticCode::MissingInput, "PeriodicWorldTileSize",
                                  "Missing periodic world tile size");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }

        const IrValueId arguments[] = {extras.tileSize};
        auto origin =
            context.ContextValue(extras.tileIsPowerOfTwo ? StringView("PeriodicWorldOriginPow2")
                                                         : StringView("PeriodicWorldOrigin"),
                                 arguments);
        if (!origin) {
            return origin;
        }

        return context.Binary(direct.kind == DirectKind::SubtractPeriodic ? IrOp::Subtract
                                                                          : IrOp::Add,
                              cast.Value(), origin.Value(), NumberRule::Arithmetic);
    }
    case DirectKind::FirstPerson: {
        if (extras.alpha == NoIrValue) {
            if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput,
                                              "FirstPersonInterpolationAlpha",
                                              "Missing first person interpolation alpha");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }

        auto clamped = context.Unary(IrOp::Saturate, extras.alpha, NumberRule::FloatOnly);
        if (!clamped || clamped.Value() == NoIrValue) {
            return clamped;
        }

        const IrValueId arguments[] = {cast.Value(), clamped.Value()};
        return context.ContextValue("TransformToFirstPerson", arguments);
    }
    case DirectKind::None:
        break;
    }

    return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
}

// 두 노드 공통의 검증·전처리다(UE TransformBase 앞부분 — HLSLMaterialTranslator.cpp:11106–11139).
// 오류를 진단하면 value를 NoIrValue로 돌려준다.
Result<IrValueId> PrepareTransformInput(LoweringContext& context, Basis from, Basis to,
                                        IrValueId value, bool position) noexcept {
    if (position && (from == Basis::Tangent || to == Basis::Tangent)) {
        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          "Tangent basis not available for position "
                                          "transformations");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    if (from == Basis::FirstPerson && to != Basis::FirstPerson) {
        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          "Transforming from First Person Space is not supported");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrType type = context.TypeOf(value);
    if (type.components == 1 && !type.IsMatrix() && from == Basis::Tangent) {
        // 스칼라 Tangent 입력은 (0, 0, x)다.
        const float zero[] = {0.0f, 0.0f};
        auto prefix = context.FloatConstant(zero, StringView());
        auto scalar = context.Convert(value, IrScalar::Float);
        if (!prefix || !scalar) {
            return !prefix ? Move(prefix) : Move(scalar);
        }

        return context.Append(prefix.Value(), scalar.Value());
    }

    if (type.components < 3 || type.IsMatrix()) {
        if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                                          "input must be a 3-component vector or a scalar (if "
                                          "source is Tangent)");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return Result<IrValueId>::Success(value);
}

// UE Transform(MaterialExpressions.cpp:11216–11233).
Result<void> LowerTransformVector(LoweringContext& context) noexcept {
    int64 source = 0;
    int64 destination = 2;
    auto sourceRead = context.ReadInteger("transformSourceType", 0, source);
    auto destinationRead = context.ReadInteger("transformType", 2, destination);
    if (!sourceRead || !destinationRead) {
        return Result<void>::Failure(!sourceRead ? sourceRead.Error() : destinationRead.Error());
    }

    if (!sourceRead.Value() || !destinationRead.Value()) {
        return Result<void>::Success();
    }

    const Basis from = VectorBasis(source);
    const Basis to = VectorBasis(destination);
    if (from == Basis::Invalid || to == Basis::Invalid) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Encountered unknown transform space");
    }

    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input",
                             "Missing Transform input vector");
    }

    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    auto prepared = PrepareTransformInput(context, from, to, input.Value(), false);
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }

    auto result = TransformBase(context, from, to, prepared.Value(), false, TransformExtras{}, 0);
    if (!result) {
        return Result<void>::Failure(result.Error());
    }

    return context.Output("Result", result.Value());
}

// UE TransformPosition(MaterialExpressions.cpp:11260–11285).
Result<void> LowerTransformPosition(LoweringContext& context) noexcept {
    int64 source = 0;
    int64 destination = 0;
    float tile = 32.0f;
    float alpha = 1.0f;
    const Result<bool> reads[] = {
        context.ReadInteger("transformSourceType", 0, source),
        context.ReadInteger("transformType", 0, destination),
        context.ReadFloat("constPeriodicWorldTileSize", 32.0f, tile),
        context.ReadFloat("constFirstPersonInterpolationAlpha", 1.0f, alpha)};
    for (const Result<bool>& read : reads) {
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<void>::Success();
        }
    }

    const Basis from = PositionBasis(source);
    const Basis to = PositionBasis(destination);
    if (from == Basis::Invalid || to == Basis::Invalid) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Encountered unknown transform space");
    }

    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input",
                             "Missing Transform Position input vector");
    }

    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    TransformExtras extras;
    if (from == Basis::PeriodicWorld || to == Basis::PeriodicWorld) {
        // 동적 입력이다(MaterialDynamicPins TransformPositionDynamicPins). 연결이 없으면 속성
        // 상수다.
        auto size = context.NamedDynamicInput("PeriodicWorldTileSize", Optional<float>(tile));
        if (!size) {
            return Result<void>::Failure(size.Error());
        }

        float constant = 0.0f;
        if (context.ConstantFloat(size.Value(), constant) && constant > 0.0f) {
            // 2의 거듭제곱 상수다(UE IsConstFloatOfPow2Expression — 가수 비트가 모두 0).
            constexpr uint32 FloatMantissaBits = 23;
            constexpr uint32 MantissaMask = (1u << FloatMantissaBits) - 1;
            uint32 bits = 0;
            platform::MemoryCopy(&bits, &constant, sizeof(bits));
            extras.tileIsPowerOfTwo = (bits & MantissaMask) == 0;
        }

        auto three = size.Value() == NoIrValue
                         ? Result<IrValueId>::Success(NoIrValue)
                         : context.CoerceTo(size.Value(), IrFloat(3), "PeriodicWorldTileSize");
        if (!three) {
            return Result<void>::Failure(three.Error());
        }

        extras.tileSize = three.Value();
    }

    if (from == Basis::FirstPerson || to == Basis::FirstPerson) {
        auto weight =
            context.NamedDynamicInput("FirstPersonInterpolationAlpha", Optional<float>(alpha));
        auto single =
            weight && weight.Value() != NoIrValue
                ? context.CoerceTo(weight.Value(), IrFloat(1), "FirstPersonInterpolationAlpha")
                : Move(weight);
        if (!single) {
            return Result<void>::Failure(single.Error());
        }

        extras.alpha = single.Value();
    }

    auto prepared = PrepareTransformInput(context, from, to, input.Value(), true);
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }

    auto result = TransformBase(context, from, to, prepared.Value(), true, extras, 0);
    if (!result) {
        return Result<void>::Failure(result.Error());
    }

    return context.Output("Result", result.Value());
}

constexpr LoweringEntry TransformEntries[] = {
    {"Material.TransformVector", &LowerTransformVector},
    {"Material.TransformPosition", &LowerTransformPosition},
};

} // namespace

Span<const LoweringEntry> TransformLowerings() noexcept {
    return TransformEntries;
}

} // namespace rocks::material::detail
