#include <Core/Text/String.h>

#include "CoordinateValues.h"
#include "LoweringContext.h"
#include "LoweringTable.h"
#include "PowerValue.h"
#include "SurfaceMath.h"

// 고급·보조 노드 lowering이다(Docs/Design/MaterialSystem 6.13절 MS2f1·MS2f2). UE 셰이더 보조 함수를
// 부르지 않고 같은 식을 IR 기본 연산으로 펼친다(결정 MD2). UE가 C++에서 미리 계산하는 상수는 같은
// float 식으로 번역 때 계산한다.
namespace rocks::material::detail {

namespace {

// 2π다(UE 2.0f * (float)UE_PI — RotateAboutAxis).
constexpr double AdvancedPi = 3.14159265358979323846;
// SphereMask 나눗셈 하한·퍼센트 단위다(UE 0.00001f, HardnessPercent * 0.01f).
constexpr float SphereMaskEpsilon = 0.00001f;
constexpr float SphereMaskPercent = 0.01f;
// Fresnel 밑 하한이다(UE UE_KINDA_SMALL_NUMBER).
constexpr float FresnelBaseFloor = 1.e-4f;
// RgbToHsv 0 나눗셈 방지 값이다(UE 1e-10f).
constexpr float HsvEpsilon = 1e-10f;
// HairColor 기본 입력과 흡수 계수다(UE MaterialExpressions.cpp:21224–21234,
// HairShadingCommon.ush:58–97).
constexpr float HairDefaultMelanin = 0.5f;
constexpr float HairDefaultRedness = 0.0f;
constexpr float HairMelaninFloor = 0.0001f;
constexpr float HairAbsorptionB = 0.3f;
constexpr float HairEumelanin[3] = {0.506f, 0.841f, 1.653f};
constexpr float HairPheomelanin[3] = {0.343f, 0.733f, 1.924f};
// BlackBody pow 상수다(UE pow(0.0004 * Temp, 4)).
constexpr float BlackBodyScale = 0.0004f;
constexpr float BlackBodyExponent = 4.0f;
// XYZ → 선형 sRGB 행렬이다(UE ColorSpace.ush:40–45 XYZ_2_Linear_sRGB_MAT — 행 순서).
constexpr float BlackBodyXyzToRgb[3][3] = {{3.2409699419f, -1.5373831776f, -0.4986107603f},
                                           {-0.9692436363f, 1.8759675015f, 0.0415550574f},
                                           {0.0556300797f, -0.2039769589f, 1.0569715142f}};
// Sobol 표다(UE FSobol::ComputeGPUSpatialSeed — Core Sobol.cpp:241–275, 셀 하위·상위 4 bit의 x·y
// bit 0–3마다 XOR하는 값, SobolIndex 표 — SobolRandom.ush:33–51).
constexpr uint32 SobolBitsPerNibble = 4;
constexpr uint32 SobolNibbleMask = 0xFu;
constexpr uint32 SobolLowX[SobolBitsPerNibble] = {0xf68eu, 0x8e56u, 0x1135u, 0x220au};
constexpr uint32 SobolLowY[SobolBitsPerNibble] = {0x94c4u, 0x4ac2u, 0xfb57u, 0x0454u};
constexpr uint32 SobolHighX[SobolBitsPerNibble] = {0x4414u, 0x8828u, 0xe69eu, 0xae76u};
constexpr uint32 SobolHighY[SobolBitsPerNibble] = {0xa28au, 0x265eu, 0xe69eu, 0xae76u};
constexpr uint32 SobolIndexBits = 10;
constexpr uint32 SobolNumbers[SobolIndexBits][2] = {
    {0x8680u, 0x4c80u}, {0xf240u, 0x9240u}, {0x8220u, 0x0e20u}, {0x4110u, 0x1610u},
    {0xa608u, 0x7608u}, {0x8a02u, 0x280au}, {0xe204u, 0x9e04u}, {0xa400u, 0x4682u},
    {0xe300u, 0xa74du}, {0xb700u, 0x9817u}};
// SobolPixel 결과 mask·shift, 고정소수 단위(0x10000)와 seed mask다.
constexpr uint32 SobolPixelShift = 8;
constexpr uint32 SobolPixelMask = 0xff00u;
constexpr float SobolScale = 65536.0f;
constexpr uint32 SobolSeedMask = 0xffffu;
// TemporalSobol 프레임 묶음이다(UE StateFrameIndexMod8 + 8 * Index).
constexpr float SobolFramesPerIndex = 8.0f;
// 벡터 폭이다.
constexpr uint8 AdvancedVector2 = 2;
constexpr uint8 AdvancedVector3 = 3;
constexpr uint8 AdvancedVector4 = 4;

// pin이 연결되지 않으면 message(UE 문구)로 MissingInput을 진단하고 false다.
Result<bool> AdvancedRequire(LoweringContext& context, StringView pin,
                             StringView message) noexcept {
    auto connected = context.IsConnected(pin);
    if (!connected) {
        return Result<bool>::Failure(connected.Error());
    }

    if (connected.Value()) {
        return Result<bool>::Success(true);
    }

    if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, StringView(), message);
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// UE ForceCast 맞춤이다(6.13절): Float로 바꾸고, 폭이 같으면 그대로, 넓으면 앞 성분, 좁은 벡터는
// 0을 붙인다. 스칼라는 replicate면 복제(ExactMatch|ReplicateValue), 아니면 그대로 둔다(플래그 없음
// — UE MCT_Float 마스크). 수가 아니면 V1이다.
Result<IrValueId> AdvancedCast(LoweringContext& context, IrValueId value, uint8 width,
                               bool replicate, StringView pin) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    IrType type = context.TypeOf(value);
    if (type.scalar == IrScalar::Bool || type.IsMatrix() || type.IsOpaque()) {
        if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, pin,
                                          "Value cannot be cast to a float vector");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    IrValueId current = value;
    if (type.scalar != IrScalar::Float) {
        auto converted = context.Convert(current, IrScalar::Float);
        if (!converted) {
            return converted;
        }

        current = converted.Value();
    }

    if (type.components == width) {
        return Result<IrValueId>::Success(current);
    }

    if (type.components == 1) {
        return replicate ? context.Splat(current, width) : Result<IrValueId>::Success(current);
    }

    if (type.components > width) {
        return context.Prefix(current, width);
    }

    auto zeros = context.Zero(IrFloat(static_cast<uint8>(width - type.components)));
    if (!zeros) {
        return zeros;
    }

    return context.Append(current, zeros.Value());
}

// UE Dot 맞춤이다(HT:9760–9777): 스칼라는 다른 쪽 폭으로 복제, 폭이 다른 두 벡터는 넓은 쪽을 좁은
// 쪽 폭으로 자른다.
IrValueId AdvancedDot(SurfaceMath& m, IrValueId left, IrValueId right) noexcept {
    if (left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    LoweringContext& context = m.Context();
    const uint8 a = context.TypeOf(left).components;
    const uint8 b = context.TypeOf(right).components;
    if (a == b) {
        return m.Dot(left, right);
    }

    if (a == 1 || b == 1) {
        return a == 1 ? m.Dot(m.Track(context.Splat(left, b)), right)
                      : m.Dot(left, m.Track(context.Splat(right, a)));
    }

    return a > b ? m.Dot(m.Track(context.Prefix(left, b)), right)
                 : m.Dot(left, m.Track(context.Prefix(right, a)));
}

// 입력 key의 값이다(연결·사용자 값·기본값 — 실패는 m에 남는다).
IrValueId AdvancedInput(SurfaceMath& m, StringView key) noexcept {
    return m.Track(m.Context().Input(key));
}

// 성분 네 개를 이은 float4다.
IrValueId AdvancedVector(SurfaceMath& m, IrValueId x, IrValueId y, IrValueId z,
                         IrValueId w) noexcept {
    LoweringContext& context = m.Context();
    const IrValueId xy =
        x == NoIrValue || y == NoIrValue ? NoIrValue : m.Track(context.Append(x, y));
    const IrValueId xyz =
        xy == NoIrValue || z == NoIrValue ? NoIrValue : m.Track(context.Append(xy, z));
    return xyz == NoIrValue || w == NoIrValue ? NoIrValue : m.Track(context.Append(xyz, w));
}

// m의 실패면 그 오류, 아니면 key 출력이다.
Result<void> AdvancedEmit(LoweringContext& context, SurfaceMath& m, StringView key,
                          IrValueId value) noexcept {
    if (auto status = m.Status(); !status) {
        return status;
    }

    return context.Output(key, value);
}

// RotateAboutAxis다(UE ME:18479–18508, HT:11721–11742, MaterialTemplate.ush:2031–2075 — 결과는 위치
// 오프셋).
Result<void> LowerRotateAboutAxis(LoweringContext& context) noexcept {
    const StringView pins[] = {"NormalizedRotationAxis", "RotationAngle", "PivotPoint", "Position"};
    const StringView messages[] = {"Missing RotateAboutAxis input NormalizedRotationAxis",
                                   "Missing RotateAboutAxis input RotationAngle",
                                   "Missing RotateAboutAxis input PivotPoint",
                                   "Missing RotateAboutAxis input Position"};
    for (usize at = 0; at < sizeof(pins) / sizeof(pins[0]); ++at) {
        auto present = AdvancedRequire(context, pins[at], messages[at]);
        if (!present || !present.Value()) {
            return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
        }
    }

    float period = 1.0f;
    if (auto read = context.ReadFloat("period", 1.0f, period); !read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceMath m(context);
    const IrValueId axisIn = AdvancedInput(m, pins[0]);
    if (axisIn != NoIrValue && context.TypeOf(axisIn).components == 1 &&
        !context.TypeOf(axisIn).IsMatrix()) {
        // UE: 스칼라(MCT_Float)는 ForceCast가 그대로 두고 AppendVector 뒤 CoerceParameter(Float4)가
        // 실패한다(HT:4791·4915 — 6.13절 MS2f1).
        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                             "Cannot cast from smaller type float2 to larger type float4.");
    }

    const IrValueId axis = m.Track(AdvancedCast(context, axisIn, AdvancedVector3, true, pins[0]));
    // 2π / Period는 UE처럼 번역 때 float로 계산한다.
    const float scale = 2.0f * static_cast<float>(AdvancedPi) / period;
    const IrValueId angle = m.Track(AdvancedCast(
        context, m.Mul(AdvancedInput(m, pins[1]), m.Scalar(scale)), 1, false, pins[1]));
    const IrValueId pivotIn = AdvancedInput(m, pins[2]);
    const IrValueId positionIn = AdvancedInput(m, pins[3]);
    if (axis == NoIrValue || angle == NoIrValue || pivotIn == NoIrValue ||
        positionIn == NoIrValue) {
        return m.Status();
    }

    const bool wide = context.TypeOf(pivotIn).scalar == IrScalar::Double ||
                      context.TypeOf(positionIn).scalar == IrScalar::Double;
    if (!wide) {
        const IrValueId pivot =
            m.Track(AdvancedCast(context, pivotIn, AdvancedVector3, true, pins[2]));
        const IrValueId position =
            m.Track(AdvancedCast(context, positionIn, AdvancedVector3, true, pins[3]));
        const IrValueId closest = m.Add(pivot, m.Mul(axis, m.Dot(axis, m.Sub(position, pivot))));
        const IrValueId u = m.Sub(position, closest);
        const IrValueId v = m.Cross(axis, u);
        const IrValueId r = m.Add(m.Mul(u, m.Cos(angle)), m.Mul(v, m.Sin(angle)));
        return AdvancedEmit(context, m, "Result", m.Sub(m.Add(closest, r), position));
    }

    // LWC 오버로드다(UE MaterialTemplate.ush:2053–2075 — 오프셋은 float, R − UAxis).
    const auto toDouble = [&](IrValueId value) noexcept {
        if (value == NoIrValue) {
            return NoIrValue;
        }

        IrValueId current = value;
        if (context.TypeOf(current).scalar != IrScalar::Double) {
            current = m.Track(context.Convert(current, IrScalar::Double));
        }

        const uint8 components = current == NoIrValue ? 0 : context.TypeOf(current).components;
        if (components == 1) {
            return m.Track(context.Splat(current, AdvancedVector3));
        }

        return components > AdvancedVector3 ? m.Track(context.Prefix(current, AdvancedVector3))
                                            : current;
    };

    const IrValueId pivot = toDouble(pivotIn);
    const IrValueId position = toDouble(positionIn);
    if (pivot == NoIrValue || position == NoIrValue) {
        return m.Status();
    }

    const auto arithmetic = [&](IrOp op, IrValueId left, IrValueId right) noexcept {
        return left == NoIrValue || right == NoIrValue
                   ? NoIrValue
                   : m.Track(context.Binary(op, left, right, NumberRule::Arithmetic));
    };

    const IrValueId offset =
        m.Track(context.Convert(arithmetic(IrOp::Subtract, position, pivot), IrScalar::Float));
    const IrValueId closest = arithmetic(IrOp::Add, pivot, m.Mul(axis, m.Dot(axis, offset)));
    const IrValueId u =
        m.Track(context.Convert(arithmetic(IrOp::Subtract, position, closest), IrScalar::Float));
    const IrValueId v = m.Cross(axis, u);
    const IrValueId r = m.Add(m.Mul(u, m.Cos(angle)), m.Mul(v, m.Sin(angle)));
    return AdvancedEmit(context, m, "Result", m.Sub(r, u));
}

// SphereMask다(UE ME:18521–18594 — Hardness는 연결되면 비율, 아니면 퍼센트 상수).
Result<void> LowerSphereMask(LoweringContext& context) noexcept {
    auto hasA = AdvancedRequire(context, "A", "Missing input A");
    if (!hasA || !hasA.Value()) {
        return hasA ? Result<void>::Success() : Result<void>::Failure(hasA.Error());
    }

    auto hasB = AdvancedRequire(context, "B", "Missing input B");
    if (!hasB || !hasB.Value()) {
        return hasB ? Result<void>::Success() : Result<void>::Failure(hasB.Error());
    }

    SurfaceMath m(context);
    const IrValueId a = AdvancedInput(m, "A");
    const IrValueId b = AdvancedInput(m, "B");
    if (a == NoIrValue || b == NoIrValue) {
        return m.Status();
    }

    // 둘 다 스칼라면 abs, 아니면 length다(UE CompileHelperLength).
    const bool scalars = context.TypeOf(a).components == 1 && context.TypeOf(b).components == 1;
    const IrValueId delta = m.Sub(a, b);
    const IrValueId distance = scalars ? m.Abs(delta) : m.Length(delta);
    const IrValueId inverseRadius =
        m.Div(m.Scalar(1.0f), m.Max(m.Scalar(SphereMaskEpsilon), AdvancedInput(m, "Radius")));
    auto hardnessConnected = context.IsConnected("Hardness");
    if (!hardnessConnected) {
        return Result<void>::Failure(hardnessConnected.Error());
    }

    IrValueId inverseHardness = NoIrValue;
    const IrValueId hardness = AdvancedInput(m, "Hardness");
    float percent = 0.0f;
    if (!hardnessConnected.Value() && context.ConstantFloat(hardness, percent)) {
        // 퍼센트 상수다(UE 1.0f / FMath::Max(1.0f - HardnessPercent * 0.01f, 0.00001f)).
        float softness = 1.0f - percent * SphereMaskPercent;
        softness = softness > SphereMaskEpsilon ? softness : SphereMaskEpsilon;
        inverseHardness = m.Scalar(1.0f / softness);
    } else if (!hardnessConnected.Value()) {
        inverseHardness =
            m.Div(m.Scalar(1.0f), m.Max(m.OneMinus(m.Mul(hardness, m.Scalar(SphereMaskPercent))),
                                        m.Scalar(SphereMaskEpsilon)));
    } else {
        inverseHardness =
            m.Div(m.Scalar(1.0f), m.Max(m.OneMinus(hardness), m.Scalar(SphereMaskEpsilon)));
    }

    const IrValueId normalized = m.Mul(distance, inverseRadius);
    return AdvancedEmit(context, m, "Result",
                        m.Saturate(m.Mul(m.OneMinus(normalized), inverseHardness)));
}

// DeriveNormalZ다(UE ME:12690–12706 — 스칼라 입력은 (x, z, 0)).
Result<void> LowerDeriveNormalZ(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "InXY",
                                   "Missing input normal xy vector whose z should be derived.");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    SurfaceMath m(context);
    const IrValueId xy =
        m.Track(AdvancedCast(context, AdvancedInput(m, "InXY"), AdvancedVector2, false, "InXY"));
    if (xy == NoIrValue) {
        return m.Status();
    }

    const IrValueId z = m.Sqrt(m.Saturate(m.OneMinus(m.Dot(xy, xy))));
    IrValueId result = m.Track(context.Append(xy, z));
    if (context.TypeOf(xy).components == 1) {
        result = m.Track(context.Append(result, m.Scalar(0.0f)));
    }

    return AdvancedEmit(context, m, "Result", result);
}

// Fresnel이다(UE ME:11936–11953).
Result<void> LowerFresnel(LoweringContext& context) noexcept {
    auto normalConnected = context.IsConnected("Normal");
    if (!normalConnected) {
        return Result<void>::Failure(normalConnected.Error());
    }

    SurfaceMath m(context);
    const IrValueId normal = normalConnected.Value() ? AdvancedInput(m, "Normal")
                                                     : m.Track(context.ContextValue("PixelNormal"));
    const IrValueId camera = m.Track(context.ContextValue("CameraVector"));
    const IrValueId facing = m.OneMinus(m.Max(m.Scalar(0.0f), AdvancedDot(m, normal, camera)));
    const IrValueId base = m.Max(m.Abs(facing), m.Scalar(FresnelBaseFloor));
    const IrValueId exponent = AdvancedInput(m, "ExponentIn");
    const IrValueId reflect = AdvancedInput(m, "BaseReflectFractionIn");
    if (base == NoIrValue || exponent == NoIrValue || reflect == NoIrValue) {
        return m.Status();
    }

    const IrValueId power = m.Track(PowerValue(context, base, exponent, "ExponentIn"));
    return AdvancedEmit(context, m, "Result", m.Add(m.Mul(power, m.OneMinus(reflect)), reflect));
}

// BumpOffset이다(UE ME:5987–6007).
Result<void> LowerBumpOffset(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "Height", "Missing Height input");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    float referencePlane = 0.5f;
    if (auto read = context.ReadFloat("referencePlane", 0.5f, referencePlane);
        !read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceMath m(context);
    const IrValueId camera = m.Track(context.ContextValue("CameraVector"));
    const IrValueId tangent =
        camera == NoIrValue ? NoIrValue
                            : m.Track(context.ContextValue("TransformWorldVectorToTangent",
                                                           Span<const IrValueId>(&camera, 1)));
    const uint8 xy[] = {0, 1};
    const IrValueId eye = tangent == NoIrValue ? NoIrValue : m.Track(context.Swizzle(tangent, xy));
    const IrValueId ratio = m.Track(
        AdvancedCast(context, AdvancedInput(m, "HeightRatioInput"), 1, false, "HeightRatioInput"));
    const IrValueId height =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Height"), 1, false, "Height"));
    const IrValueId ratioScalar =
        ratio == NoIrValue ? NoIrValue : m.Track(context.Prefix(ratio, 1));
    const IrValueId heightScalar =
        height == NoIrValue ? NoIrValue : m.Track(context.Prefix(height, 1));
    const IrValueId offset =
        m.Add(m.Mul(ratioScalar, heightScalar), m.Mul(m.Scalar(-referencePlane), ratioScalar));
    const IrValueId coordinate = m.Track(CoordinateOrTexCoord(context));
    return AdvancedEmit(context, m, "Result", m.Add(m.Mul(eye, offset), coordinate));
}

// Desaturation이다(UE ME:7939–7951 — Fraction이 없으면 스칼라 Grey).
Result<void> LowerDesaturation(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "Input", "Missing Desaturation input");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    float factors[MaxIrComponents] = {};
    if (auto read = context.ReadColor("luminanceFactors", factors); !read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceMath m(context);
    const IrValueId color =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Input"), AdvancedVector3, true, "Input"));
    const IrValueId grey = m.Dot(color, m.Vector3(factors[0], factors[1], factors[2]));
    auto fractionConnected = context.IsConnected("Fraction");
    if (!fractionConnected) {
        return Result<void>::Failure(fractionConnected.Error());
    }

    if (!fractionConnected.Value()) {
        return AdvancedEmit(context, m, "Result", grey);
    }

    IrValueId fraction = AdvancedInput(m, "Fraction");
    if (fraction != NoIrValue && context.TypeOf(fraction).components == 1) {
        fraction = m.Track(context.Splat(fraction, AdvancedVector3));
    }

    return AdvancedEmit(context, m, "Result", m.Lerp(color, m.Splat3(grey), fraction));
}

// 입력이 정확히 Float3·Float4인가(UE HsvToRgb·RgbToHsv — 아니면 입력을 그대로 낸다).
bool AdvancedColorVector(LoweringContext& context, IrValueId value) noexcept {
    const IrType type = context.TypeOf(value);
    return type.scalar == IrScalar::Float && !type.IsMatrix() &&
           (type.components == AdvancedVector3 || type.components == AdvancedVector4);
}

// HsvToRgb다(UE ME:10909–10926, HT:10964–10997).
Result<void> LowerHsvToRgb(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "Input", "Missing HSVToRGB input");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    SurfaceMath m(context);
    const IrValueId hsv = AdvancedInput(m, "Input");
    if (hsv == NoIrValue || !AdvancedColorVector(context, hsv)) {
        return AdvancedEmit(context, m, "Result", hsv);
    }

    const IrValueId h6 = m.Mul(m.Component(hsv, 0), m.Scalar(6.0f));
    const IrValueId r = m.Sub(m.Abs(m.Sub(h6, m.Scalar(3.0f))), m.Scalar(1.0f));
    const IrValueId g = m.Sub(m.Scalar(2.0f), m.Abs(m.Sub(h6, m.Scalar(2.0f))));
    const IrValueId b = m.Sub(m.Scalar(2.0f), m.Abs(m.Sub(h6, m.Scalar(4.0f))));
    const IrValueId rg =
        r == NoIrValue || g == NoIrValue ? NoIrValue : m.Track(context.Append(r, g));
    const IrValueId rgb =
        m.Saturate(rg == NoIrValue || b == NoIrValue ? NoIrValue : m.Track(context.Append(rg, b)));
    IrValueId result =
        m.Mul(m.Add(m.Mul(m.Sub(rgb, m.Scalar(1.0f)), m.Component(hsv, 1)), m.Scalar(1.0f)),
              m.Component(hsv, 2));
    if (context.TypeOf(hsv).components == AdvancedVector4 && result != NoIrValue) {
        result = m.Track(context.Append(result, m.Component(hsv, 3)));
    }

    return AdvancedEmit(context, m, "Result", result);
}

// RgbToHsv다(UE ME:10940–10957, HT:10999–11036).
Result<void> LowerRgbToHsv(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "Input", "Missing RGBToHSV input");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    SurfaceMath m(context);
    const IrValueId rgb = AdvancedInput(m, "Input");
    if (rgb == NoIrValue || !AdvancedColorVector(context, rgb)) {
        return AdvancedEmit(context, m, "Result", rgb);
    }

    const IrValueId r = m.Component(rgb, 0);
    const IrValueId g = m.Component(rgb, 1);
    const IrValueId b = m.Component(rgb, 2);
    const IrValueId p =
        m.Select(m.Less(g, b), AdvancedVector(m, b, g, m.Scalar(-1.0f), m.Scalar(2.0f / 3.0f)),
                 AdvancedVector(m, g, b, m.Scalar(0.0f), m.Scalar(-1.0f / 3.0f)));
    const IrValueId px = m.Component(p, 0);
    const IrValueId q =
        m.Select(m.Less(r, px), AdvancedVector(m, px, m.Component(p, 1), m.Component(p, 3), r),
                 AdvancedVector(m, r, m.Component(p, 1), m.Component(p, 2), px));
    const IrValueId qx = m.Component(q, 0);
    const IrValueId qy = m.Component(q, 1);
    const IrValueId qw = m.Component(q, 3);
    const IrValueId chroma = m.Sub(qx, m.Min(qw, qy));
    const IrValueId hue = m.Abs(
        m.Add(m.Div(m.Sub(qw, qy), m.Add(m.Mul(m.Scalar(6.0f), chroma), m.Scalar(HsvEpsilon))),
              m.Component(q, 2)));
    const IrValueId saturation = m.Div(chroma, m.Add(qx, m.Scalar(HsvEpsilon)));
    const IrValueId hs = hue == NoIrValue || saturation == NoIrValue
                             ? NoIrValue
                             : m.Track(context.Append(hue, saturation));
    IrValueId result =
        hs == NoIrValue || qx == NoIrValue ? NoIrValue : m.Track(context.Append(hs, qx));
    if (context.TypeOf(rgb).components == AdvancedVector4 && result != NoIrValue) {
        result = m.Track(context.Append(result, m.Component(rgb, 3)));
    }

    return AdvancedEmit(context, m, "Result", result);
}

// BlackBody다(UE ME:18990–19006, MaterialTemplate.ush:2112–2131 — Temp가 없으면 Rocks는 진단한다).
Result<void> LowerBlackBody(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "Temp", "Missing BlackBody Temp input");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    SurfaceMath m(context);
    const IrValueId cast =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Temp"), 1, false, "Temp"));
    const IrValueId t = cast == NoIrValue ? NoIrValue : m.Track(context.Prefix(cast, 1));
    // UE 식의 연산 순서 그대로다(c·T·T = (c·T)·T).
    const auto term = [&](float c) noexcept { return m.Mul(m.Scalar(c), t); };
    const auto square = [&](float c) noexcept { return m.Mul(m.Mul(m.Scalar(c), t), t); };
    const IrValueId u =
        m.Div(m.Add(m.Add(m.Scalar(0.860117757f), term(1.54118254e-4f)), square(1.28641212e-7f)),
              m.Add(m.Add(m.Scalar(1.0f), term(8.42420235e-4f)), square(7.08145163e-7f)));
    const IrValueId v =
        m.Div(m.Add(m.Add(m.Scalar(0.317398726f), term(4.22806245e-5f)), square(4.20481691e-8f)),
              m.Add(m.Sub(m.Scalar(1.0f), term(2.89741816e-5f)), square(1.61456053e-7f)));
    const IrValueId denominator =
        m.Add(m.Sub(m.Mul(m.Scalar(2.0f), u), m.Mul(m.Scalar(8.0f), v)), m.Scalar(4.0f));
    const IrValueId x = m.Div(m.Mul(m.Scalar(3.0f), u), denominator);
    const IrValueId y = m.Div(m.Mul(m.Scalar(2.0f), v), denominator);
    const IrValueId z = m.Sub(m.Sub(m.Scalar(1.0f), x), y);
    const IrValueId bigY = m.Scalar(1.0f);
    const IrValueId bigX = m.Mul(m.Div(bigY, y), x);
    const IrValueId bigZ = m.Mul(m.Div(bigY, y), z);
    const IrValueId xyz = bigX == NoIrValue || bigZ == NoIrValue
                              ? NoIrValue
                              : m.Track(context.Append(m.Track(context.Append(bigX, bigY)), bigZ));
    IrValueId rows[AdvancedVector3] = {};
    for (usize row = 0; row < AdvancedVector3; ++row) {
        rows[row] = m.Dot(m.Vector3(BlackBodyXyzToRgb[row][0], BlackBodyXyzToRgb[row][1],
                                    BlackBodyXyzToRgb[row][2]),
                          xyz);
    }

    const IrValueId rg = rows[0] == NoIrValue || rows[1] == NoIrValue
                             ? NoIrValue
                             : m.Track(context.Append(rows[0], rows[1]));
    const IrValueId linear =
        rg == NoIrValue || rows[2] == NoIrValue ? NoIrValue : m.Track(context.Append(rg, rows[2]));
    const IrValueId radiance =
        m.Pow(m.Mul(m.Scalar(BlackBodyScale), t), m.Scalar(BlackBodyExponent));
    return AdvancedEmit(context, m, "Result", m.Mul(m.Max(m.Scalar(0.0f), linear), radiance));
}

// SrgbToWorkingColorSpace다(UE ME:10492–10520 — 작업 색공간 sRGB 고정이라 그대로).
Result<void> LowerSrgbToWorkingColorSpace(LoweringContext& context) noexcept {
    auto present = AdvancedRequire(context, "Input", "Missing sRGBColorToWorkingColorSpace input");
    if (!present || !present.Value()) {
        return present ? Result<void>::Success() : Result<void>::Failure(present.Error());
    }

    auto value = context.Input("Input");
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

// HairColor다(UE ME:21213–21234, HairShadingCommon.ush:58–97).
Result<void> LowerHairColor(LoweringContext& context) noexcept {
    SurfaceMath m(context);
    const auto optional = [&](StringView pin, uint8 width, IrValueId fallback) noexcept {
        auto connected = context.IsConnected(pin);
        if (!connected) {
            return m.Track(Result<IrValueId>::Failure(connected.Error()));
        }

        if (!connected.Value()) {
            return fallback;
        }

        const IrValueId cast =
            m.Track(AdvancedCast(context, AdvancedInput(m, pin), width, width != 1, pin));
        return cast == NoIrValue || width != 1 ? cast : m.Track(context.Prefix(cast, 1));
    };

    const IrValueId melaninIn = optional("Melanin", 1, m.Scalar(HairDefaultMelanin));
    const IrValueId rednessIn = optional("Redness", 1, m.Scalar(HairDefaultRedness));
    const IrValueId dye = optional("DyeColor", AdvancedVector3, m.Vector3(1.0f, 1.0f, 1.0f));
    // D(B)는 UE와 같은 float 식으로 번역 때 계산한다.
    const float b = HairAbsorptionB;
    const float b2 = b * b;
    const float b3 = b * b2;
    const float b4 = b2 * b2;
    const float b5 = b * b4;
    const float d = (5.969f - 0.215f * b + 2.532f * b2 - 10.73f * b3 + 5.574f * b4 + 0.245f * b5);
    const IrValueId melaninRaw =
        m.Log(m.Max(m.OneMinus(m.Saturate(melaninIn)), m.Scalar(HairMelaninFloor)));
    const IrValueId melanin =
        melaninRaw == NoIrValue
            ? NoIrValue
            : m.Track(context.Unary(IrOp::Negate, melaninRaw, NumberRule::FloatOnly));
    const IrValueId redness = m.Saturate(rednessIn);
    const IrValueId eumelanin = m.Mul(melanin, m.OneMinus(redness));
    const IrValueId pheomelanin = m.Mul(melanin, redness);
    const IrValueId dyeRatio = m.Div(m.Log(m.Saturate(dye)), m.Scalar(d));
    const IrValueId dyeAbsorption = m.Mul(dyeRatio, dyeRatio);
    const IrValueId absorption = m.Add(
        m.Mul(eumelanin, m.Vector3(HairEumelanin[0], HairEumelanin[1], HairEumelanin[2])),
        m.Mul(pheomelanin, m.Vector3(HairPheomelanin[0], HairPheomelanin[1], HairPheomelanin[2])));
    const IrValueId root = m.Sqrt(m.Add(absorption, dyeAbsorption));
    const IrValueId negated =
        root == NoIrValue ? NoIrValue
                          : m.Track(context.Unary(IrOp::Negate, root, NumberRule::FloatOnly));
    return AdvancedEmit(context, m, "Color", m.Exp(m.Mul(negated, m.Scalar(d))));
}

// UInt 상수다(성분 하나 또는 둘).
IrValueId SobolUInt(SurfaceMath& m, uint32 x) noexcept {
    return m.Track(m.Context().Builder().UIntConstant(Span<const uint32>(&x, 1)));
}

IrValueId SobolUInt2(SurfaceMath& m, uint32 x, uint32 y) noexcept {
    const uint32 values[] = {x, y};
    return m.Track(m.Context().Builder().UIntConstant(values));
}

// 정수 연산이다.
IrValueId SobolBinary(SurfaceMath& m, IrOp op, IrValueId left, IrValueId right) noexcept {
    if (left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return m.Track(m.Context().Binary(op, left, right, NumberRule::IntegerOnly));
}

// value(UInt 스칼라)의 bit가 있으면 whenSet, 아니면 zero다(UE (x & bit) ? c : 0).
IrValueId SobolPick(SurfaceMath& m, IrValueId value, uint32 bit, IrValueId whenSet,
                    IrValueId zero) noexcept {
    const IrValueId masked = SobolBinary(m, IrOp::BitwiseAnd, value, SobolUInt(m, bit));
    if (masked == NoIrValue || whenSet == NoIrValue || zero == NoIrValue) {
        return NoIrValue;
    }

    const IrValueId set =
        m.Track(m.Context().Binary(IrOp::NotEquals, masked, SobolUInt(m, 0), NumberRule::Equality));
    return set == NoIrValue ? NoIrValue : m.Track(m.Context().Select(set, whenSet, zero));
}

// UE SobolPixel이다(SobolRandom.ush:21–31 — LUT 두 칸의 XOR을 표 상수로 계산한다).
IrValueId SobolPixelValue(SurfaceMath& m, IrValueId pixel) noexcept {
    const IrValueId nibble = SobolUInt2(m, SobolNibbleMask, SobolNibbleMask);
    const IrValueId low = SobolBinary(m, IrOp::BitwiseAnd, pixel, nibble);
    const IrValueId high =
        SobolBinary(m, IrOp::BitwiseAnd,
                    SobolBinary(m, IrOp::BitShiftRight, pixel,
                                SobolUInt2(m, SobolBitsPerNibble, SobolBitsPerNibble)),
                    nibble);
    const IrValueId zero = SobolUInt(m, 0);
    IrValueId packed = zero;
    const IrValueId lanes[] = {m.Component(low, 0), m.Component(low, 1), m.Component(high, 0),
                               m.Component(high, 1)};
    const uint32* tables[] = {SobolLowX, SobolLowY, SobolHighX, SobolHighY};
    for (usize lane = 0; lane < sizeof(lanes) / sizeof(lanes[0]); ++lane) {
        for (uint32 bit = 0; bit < SobolBitsPerNibble; ++bit) {
            packed = SobolBinary(
                m, IrOp::BitwiseXor, packed,
                SobolPick(m, lanes[lane], 1u << bit, SobolUInt(m, tables[lane][bit]), zero));
        }
    }

    const IrValueId shifted =
        SobolBinary(m, IrOp::BitShiftLeft, packed, SobolUInt(m, SobolPixelShift));
    const IrValueId pair = packed == NoIrValue || shifted == NoIrValue
                               ? NoIrValue
                               : m.Track(m.Context().Append(packed, shifted));
    return SobolBinary(m, IrOp::BitwiseAnd, pair, SobolUInt2(m, SobolPixelMask, SobolPixelMask));
}

// UE SobolIndex다(SobolRandom.ush:33–51 — Bits 10).
IrValueId SobolIndexValue(SurfaceMath& m, IrValueId base, IrValueId index) noexcept {
    const IrValueId zero = SobolUInt2(m, 0, 0);
    IrValueId result = base;
    for (uint32 bit = 0; bit < SobolIndexBits; ++bit) {
        result =
            SobolBinary(m, IrOp::BitwiseXor, result,
                        SobolPick(m, index, 1u << bit,
                                  SobolUInt2(m, SobolNumbers[bit][0], SobolNumbers[bit][1]), zero));
    }

    return result;
}

// float2(sobol ^ (uint2(Seed · 0x10000) & 0xffff)) / 0x10000이다.
IrValueId SobolFinish(SurfaceMath& m, IrValueId sobol, IrValueId seed) noexcept {
    LoweringContext& context = m.Context();
    const IrValueId scaled = m.Mul(seed, m.Scalar(SobolScale));
    const IrValueId seedBits = SobolBinary(
        m, IrOp::BitwiseAnd,
        scaled == NoIrValue ? NoIrValue : m.Track(context.Convert(scaled, IrScalar::UInt)),
        SobolUInt2(m, SobolSeedMask, SobolSeedMask));
    const IrValueId mixed = SobolBinary(m, IrOp::BitwiseXor, sobol, seedBits);
    return m.Div(mixed == NoIrValue ? NoIrValue : m.Track(context.Convert(mixed, IrScalar::Float)),
                 m.Scalar(SobolScale));
}

// Sobol이다(UE ME:18601–18607, HT:11946–11956).
Result<void> LowerSobol(LoweringContext& context) noexcept {
    auto cellConnected = context.IsConnected("Cell");
    if (!cellConnected) {
        return Result<void>::Failure(cellConnected.Error());
    }

    SurfaceMath m(context);
    const IrValueId cell = cellConnected.Value()
                               ? m.Track(AdvancedCast(context, AdvancedInput(m, "Cell"),
                                                      AdvancedVector2, true, "Cell"))
                               : m.Track(context.Zero(IrFloat(AdvancedVector2)));
    const IrValueId index =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Index"), 1, false, "Index"));
    const IrValueId seed =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Seed"), AdvancedVector2, true, "Seed"));
    if (cell == NoIrValue || index == NoIrValue || seed == NoIrValue) {
        return m.Status();
    }

    const IrValueId first = m.Track(context.Prefix(index, 1));
    const IrValueId pixel = m.Track(context.Convert(cell, IrScalar::UInt));
    const IrValueId bits =
        first == NoIrValue ? NoIrValue : m.Track(context.Convert(first, IrScalar::UInt));
    const IrValueId sobol = SobolIndexValue(m, SobolPixelValue(m, pixel), bits);
    const IrValueId floor = m.Track(context.Unary(IrOp::Floor, cell, NumberRule::FloatOnly));
    return AdvancedEmit(context, m, "Result", m.Add(floor, SobolFinish(m, sobol, seed)));
}

// TemporalSobol이다(UE ME:18618–18623, HT:11958–11966 — 픽셀 위치·프레임 번호 mod 8).
Result<void> LowerTemporalSobol(LoweringContext& context) noexcept {
    SurfaceMath m(context);
    const IrValueId index =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Index"), 1, false, "Index"));
    const IrValueId seed =
        m.Track(AdvancedCast(context, AdvancedInput(m, "Seed"), AdvancedVector2, true, "Seed"));
    const IrValueId position = m.Track(context.ContextValue("SvPosition"));
    const IrValueId frame = m.Track(context.ContextValue("StateFrameIndexMod8"));
    if (index == NoIrValue || seed == NoIrValue || position == NoIrValue || frame == NoIrValue) {
        return m.Status();
    }

    const uint8 xy[] = {0, 1};
    const IrValueId pixel =
        m.Track(context.Convert(m.Track(context.Swizzle(position, xy)), IrScalar::UInt));
    const IrValueId frameIndex =
        m.Add(m.Track(context.Convert(frame, IrScalar::Float)),
              m.Mul(m.Scalar(SobolFramesPerIndex), m.Track(context.Prefix(index, 1))));
    const IrValueId bits =
        frameIndex == NoIrValue ? NoIrValue : m.Track(context.Convert(frameIndex, IrScalar::UInt));
    const IrValueId sobol = SobolIndexValue(m, SobolPixelValue(m, pixel), bits);
    return AdvancedEmit(context, m, "Result", SobolFinish(m, sobol, seed));
}

constexpr LoweringEntry AdvancedEntries[] = {
    {"Material.RotateAboutAxis", &LowerRotateAboutAxis},
    {"Material.SphereMask", &LowerSphereMask},
    {"Material.DeriveNormalZ", &LowerDeriveNormalZ},
    {"Material.Fresnel", &LowerFresnel},
    {"Material.BumpOffset", &LowerBumpOffset},
    {"Material.Desaturation", &LowerDesaturation},
    {"Material.HsvToRgb", &LowerHsvToRgb},
    {"Material.RgbToHsv", &LowerRgbToHsv},
    {"Material.BlackBody", &LowerBlackBody},
    {"Material.SrgbToWorkingColorSpace", &LowerSrgbToWorkingColorSpace},
    {"Material.HairColor", &LowerHairColor},
    {"Material.Sobol", &LowerSobol},
    {"Material.TemporalSobol", &LowerTemporalSobol},
};

} // namespace

Span<const LoweringEntry> AdvancedLowerings() noexcept {
    return AdvancedEntries;
}

} // namespace rocks::material::detail
