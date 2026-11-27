#include <Core/Memory/SystemAllocator.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <MaterialCompiler/MaterialIrModule.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 머티리얼 IR 핵심 시험이다(Docs/Design/MaterialSystem 6.5·6.6·6.8절, 단계 MS2a1): hash-consing,
// 정확히 반올림되는 연산만의 상수 접기와 접지 않는 경우(0 나누기·NaN·Inf·초월·범위 밖 변환), 대수
// 단순화의 IEEE 경계(−0·x+0), 타입 규칙, DCE와 후위 번호, 결과 정렬, IR hash 결정성.
using namespace rocks;
using namespace rocks::material;
namespace material_ir_tests_file {

namespace {

float FloatAt(const IrBuilder& builder, IrValueId id, usize component) noexcept {
    const uint32 bits = static_cast<uint32>(builder.Value(id).immediate[component]);
    float value = 0.0f;
    platform::MemoryCopy(&value, &bits, sizeof(value));
    return value;
}

uint32 BitsAt(const IrBuilder& builder, IrValueId id, usize component) noexcept {
    return static_cast<uint32>(builder.Value(id).immediate[component]);
}

IrValueId Float(IrBuilder& builder, float value) noexcept {
    const float components[] = {value};
    auto made = builder.FloatConstant(components);
    return made ? made.Value() : NoIrValue;
}

IrValueId Int(IrBuilder& builder, int32 value) noexcept {
    const int32 components[] = {value};
    auto made = builder.IntConstant(components);
    return made ? made.Value() : NoIrValue;
}

// 상수가 아닌 float 스칼라다(미분 — 접히지 않는 값).
IrValueId Varying(IrBuilder& builder, float seed) noexcept {
    auto derivative = builder.Unary(IrOp::Ddx, Float(builder, seed));
    return derivative ? derivative.Value() : NoIrValue;
}

} // namespace

// hash-consing: 같은 값은 같은 번호이고 위치(site)는 키가 아니다. 미분은 Pixel 빈도다.
TEST(MaterialIr_HashConsing) {
    SystemAllocator allocator;
    IrBuilder builder(allocator);
    builder.SetSite(IrSite{Guid{1, 1}, "A"});
    const IrValueId x = Varying(builder, 2.0f);
    builder.SetSite(IrSite{Guid{2, 2}, "B"});
    const IrValueId again = Varying(builder, 2.0f);
    CHECK(x == again);
    CHECK(builder.Value(x).site.node == (Guid{1, 1}));
    CHECK(builder.Value(x).frequency == IrFrequency::Pixel);
    const usize before = builder.Count();
    auto sum = builder.Binary(IrOp::Add, x, x);
    auto sumAgain = builder.Binary(IrOp::Add, x, x);
    REQUIRE(sum && sumAgain);
    CHECK(sum.Value() == sumAgain.Value());
    CHECK(builder.Count() == before + 1);
    CHECK(builder.Value(sum.Value()).frequency == IrFrequency::Pixel);
}

// 상수 접기: 사칙·비교·정수 wrap·shift·변환·floor 계열은 접고, 0 나누기·NaN·Inf 결과·초월 함수·
// 범위 밖 변환은 남긴다.
TEST(MaterialIr_ConstantFolding) {
    SystemAllocator allocator;
    IrBuilder builder(allocator);
    auto sum = builder.Binary(IrOp::Add, Float(builder, 1.5f), Float(builder, 2.25f));
    REQUIRE(sum);
    CHECK(builder.Value(sum.Value()).op == IrOp::Constant);
    CHECK(FloatAt(builder, sum.Value(), 0) == 3.75f);

    auto quotient = builder.Binary(IrOp::Divide, Float(builder, 1.0f), Float(builder, 0.0f));
    REQUIRE(quotient);
    CHECK(builder.Value(quotient.Value()).op == IrOp::Divide);

    // 큰 수의 곱이 Inf가 되면 접지 않는다.
    auto overflow = builder.Binary(IrOp::Multiply, Float(builder, 3.0e38f), Float(builder, 10.0f));
    REQUIRE(overflow);
    CHECK(builder.Value(overflow.Value()).op == IrOp::Multiply);

    auto sine = builder.Unary(IrOp::Sin, Float(builder, 1.0f));
    REQUIRE(sine);
    CHECK(builder.Value(sine.Value()).op == IrOp::Sin);

    auto compare = builder.Binary(IrOp::LessThan, Float(builder, 1.0f), Float(builder, 2.0f));
    REQUIRE(compare);
    CHECK(builder.Value(compare.Value()).type == IrBool(1));
    CHECK(builder.Value(compare.Value()).immediate[0] == 1);

    // 정수: wrap, 0 나누기·최소값 / −1은 남김, shift는 하위 5 bit.
    auto wrapped = builder.Binary(IrOp::Add, Int(builder, 2147483647), Int(builder, 1));
    REQUIRE(wrapped);
    CHECK(BitsAt(builder, wrapped.Value(), 0) == 0x80000000u);
    auto byZero = builder.Binary(IrOp::Divide, Int(builder, 7), Int(builder, 0));
    REQUIRE(byZero);
    CHECK(builder.Value(byZero.Value()).op == IrOp::Divide);
    auto minimum = builder.Binary(IrOp::Divide, Int(builder, -2147483647 - 1), Int(builder, -1));
    REQUIRE(minimum);
    CHECK(builder.Value(minimum.Value()).op == IrOp::Divide);
    auto shifted = builder.Binary(IrOp::BitShiftLeft, Int(builder, 1), Int(builder, 33));
    REQUIRE(shifted);
    CHECK(BitsAt(builder, shifted.Value(), 0) == 2u);
    auto negativeModulo = builder.Binary(IrOp::Modulo, Int(builder, -7), Int(builder, 3));
    REQUIRE(negativeModulo);
    CHECK(static_cast<int32>(BitsAt(builder, negativeModulo.Value(), 0)) == -1);

    // floor 계열: 짝수 반올림, 음수 frac, abs(−0) = +0, ceil(−0.5) = −0.
    struct Unary {
        IrOp op;
        float input;
        uint32 expected;
    };

    const Unary unary[] = {
        {IrOp::Round, 2.5f, 0x40000000u},      {IrOp::Round, -2.5f, 0xC0000000u},
        {IrOp::Frac, -0.25f, 0x3F400000u},     {IrOp::Abs, -0.0f, 0x00000000u},
        {IrOp::Ceil, -0.5f, 0x80000000u},      {IrOp::Floor, -0.5f, 0xBF800000u},
        {IrOp::Truncate, -1.75f, 0xBF800000u}, {IrOp::Saturate, 1.5f, 0x3F800000u},
        {IrOp::Sign, -3.0f, 0xBF800000u},
    };
    for (const Unary& testCase : unary) {
        auto folded = builder.Unary(testCase.op, Float(builder, testCase.input));
        REQUIRE(folded);
        CHECK(builder.Value(folded.Value()).op == IrOp::Constant);
        CHECK(BitsAt(builder, folded.Value(), 0) == testCase.expected);
    }

    // 변환: float → int는 0 쪽 자름, 범위 밖·음수 uint는 남김.
    auto truncated = builder.Convert(Float(builder, -3.75f), IrScalar::Int);
    REQUIRE(truncated);
    CHECK(static_cast<int32>(BitsAt(builder, truncated.Value(), 0)) == -3);
    auto outOfRange = builder.Convert(Float(builder, 3.0e9f), IrScalar::Int);
    REQUIRE(outOfRange);
    CHECK(builder.Value(outOfRange.Value()).op == IrOp::Convert);
    auto negativeUnsigned = builder.Convert(Float(builder, -1.0f), IrScalar::UInt);
    REQUIRE(negativeUnsigned);
    CHECK(builder.Value(negativeUnsigned.Value()).op == IrOp::Convert);

    // clamp, swizzle·append·splat 상수.
    auto clamped = builder.Ternary(IrOp::Clamp, Float(builder, 5.0f), Float(builder, 0.0f),
                                   Float(builder, 1.0f));
    REQUIRE(clamped);
    CHECK(FloatAt(builder, clamped.Value(), 0) == 1.0f);
    const float vector[] = {1.0f, 2.0f, 3.0f};
    auto three = builder.FloatConstant(vector);
    REQUIRE(three);
    const uint8 zx[] = {2, 0};
    auto picked = builder.Swizzle(three.Value(), zx);
    REQUIRE(picked);
    CHECK(builder.Value(picked.Value()).op == IrOp::Constant);
    CHECK(FloatAt(builder, picked.Value(), 0) == 3.0f &&
          FloatAt(builder, picked.Value(), 1) == 1.0f);
}

// 대수 단순화: x*1·1*x·x/1·x−(+0)·정수 x+0은 x이고, x−(−0)·float x+0·x*0은 남긴다.
TEST(MaterialIr_AlgebraicSimplification) {
    SystemAllocator allocator;
    IrBuilder builder(allocator);
    const IrValueId x = Varying(builder, 1.0f);
    auto times = builder.Binary(IrOp::Multiply, x, Float(builder, 1.0f));
    auto timesLeft = builder.Binary(IrOp::Multiply, Float(builder, 1.0f), x);
    auto divided = builder.Binary(IrOp::Divide, x, Float(builder, 1.0f));
    auto minusZero = builder.Binary(IrOp::Subtract, x, Float(builder, 0.0f));
    REQUIRE(times && timesLeft && divided && minusZero);
    CHECK(times.Value() == x && timesLeft.Value() == x && divided.Value() == x);
    CHECK(minusZero.Value() == x);

    auto minusNegativeZero = builder.Binary(IrOp::Subtract, x, Float(builder, -0.0f));
    auto plusZero = builder.Binary(IrOp::Add, x, Float(builder, 0.0f));
    auto timesZero = builder.Binary(IrOp::Multiply, x, Float(builder, 0.0f));
    REQUIRE(minusNegativeZero && plusZero && timesZero);
    CHECK(minusNegativeZero.Value() != x);
    CHECK(plusZero.Value() != x);
    CHECK(builder.Value(timesZero.Value()).op == IrOp::Multiply);

    auto integer = builder.Convert(x, IrScalar::Int);
    REQUIRE(integer);
    auto integerPlusZero = builder.Binary(IrOp::Add, integer.Value(), Int(builder, 0));
    REQUIRE(integerPlusZero);
    CHECK(integerPlusZero.Value() == integer.Value());

    // swizzle: 같은 성분 그대로는 원래 값, 겹친 swizzle은 하나로 합친다.
    const float components[] = {1.0f, 2.0f, 3.0f};
    auto three = builder.FloatConstant(components);
    REQUIRE(three);
    auto varying = builder.Unary(IrOp::Ddx, three.Value());
    REQUIRE(varying);
    const uint8 identity[] = {0, 1, 2};
    auto same = builder.Swizzle(varying.Value(), identity);
    REQUIRE(same);
    CHECK(same.Value() == varying.Value());
    const uint8 zyx[] = {2, 1, 0};
    auto reversed = builder.Swizzle(varying.Value(), zyx);
    REQUIRE(reversed);
    auto back = builder.Swizzle(reversed.Value(), zyx);
    REQUIRE(back);
    CHECK(back.Value() == varying.Value());
    const uint8 single[] = {0};
    auto last = builder.Swizzle(reversed.Value(), single);
    REQUIRE(last);
    CHECK(builder.Value(last.Value()).operands[0] == varying.Value());
    CHECK((builder.Value(last.Value()).immediate[0] & 3u) == 2u);

    // 같은 두 가지의 선택은 그 값이고, splat 1은 그대로다.
    auto condition = builder.Binary(IrOp::LessThan, x, Float(builder, 0.5f));
    REQUIRE(condition);
    auto chosen = builder.Select(condition.Value(), x, x);
    REQUIRE(chosen);
    CHECK(chosen.Value() == x);
    auto splat = builder.Splat(x, 1);
    REQUIRE(splat);
    CHECK(splat.Value() == x);
}

// 타입 규칙: 폭이 다른 산술·Bool 산술·float3 아닌 cross는 거부하고, dot·distance·length는 스칼라,
// 비교는 Bool 성분, 행 벡터 × 행렬은 열 수 벡터다.
TEST(MaterialIr_TypeRules) {
    SystemAllocator allocator;
    IrBuilder builder(allocator);
    const float two[] = {1.0f, 2.0f};
    const float three[] = {1.0f, 2.0f, 3.0f};
    auto a = builder.FloatConstant(two);
    auto b = builder.FloatConstant(three);
    REQUIRE(a && b);
    auto x = builder.Unary(IrOp::Ddx, a.Value());
    auto y = builder.Unary(IrOp::Ddx, b.Value());
    REQUIRE(x && y);
    CHECK(builder.Binary(IrOp::Add, x.Value(), y.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(builder.Binary(IrOp::Cross, x.Value(), x.Value()).Error() == ErrorCode::InvalidArgument);
    auto cross = builder.Binary(IrOp::Cross, y.Value(), y.Value());
    REQUIRE(cross);
    CHECK(builder.Value(cross.Value()).type == IrFloat(3));
    auto dot = builder.Binary(IrOp::Dot, y.Value(), y.Value());
    REQUIRE(dot);
    CHECK(builder.Value(dot.Value()).type == IrFloat(1));
    auto length = builder.Unary(IrOp::Length, y.Value());
    REQUIRE(length);
    CHECK(builder.Value(length.Value()).type == IrFloat(1));
    auto equal = builder.Binary(IrOp::Equals, y.Value(), y.Value());
    REQUIRE(equal);
    CHECK(builder.Value(equal.Value()).type == IrBool(3));
    CHECK(builder.Binary(IrOp::Add, equal.Value(), equal.Value()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(builder.Unary(IrOp::Sin, equal.Value()).Error() == ErrorCode::InvalidArgument);
    const uint8 bad[] = {3};
    CHECK(builder.Swizzle(y.Value(), bad).Error() == ErrorCode::InvalidArgument);
    CHECK(builder.Append(y.Value(), x.Value()).Error() == ErrorCode::InvalidArgument);
    auto appended = builder.Append(x.Value(), x.Value());
    REQUIRE(appended);
    CHECK(builder.Value(appended.Value()).type == IrFloat(4));
    CHECK(builder.Splat(y.Value(), 3).Error() == ErrorCode::InvalidArgument);
    CHECK(builder.Branch(equal.Value(), y.Value(), y.Value()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(!IsValidIrType(IrFloatMatrix(2, 2)) && IsValidIrType(IrFloatMatrix(4, 4)));
    CHECK(!IsValidIrType(IrFloat(5)));
}

// 모듈: 닿지 않는 값은 빠지고, 번호는 결과 순서의 후위 순(피연산자가 먼저)이며, 결과는 진입점
// 순서로 정렬된다. 만든 순서가 달라도 같은 모듈·같은 hash이고, 값이 바뀌면 hash가 바뀐다.
TEST(MaterialIr_ModuleDeadValuesAndHash) {
    SystemAllocator allocator;
    const auto build = [&](bool reversed, float scale, IrModule& module) noexcept {
        IrBuilder builder(allocator);
        IrValueId x = NoIrValue;
        IrValueId y = NoIrValue;
        if (reversed) {
            y = Varying(builder, 3.0f);
            x = Varying(builder, 2.0f);
        } else {
            x = Varying(builder, 2.0f);
            y = Varying(builder, 3.0f);
        }

        (void)builder.Unary(IrOp::Sin, x); // 닿지 않는 값
        auto product = builder.Binary(IrOp::Multiply, x, Float(builder, scale));
        auto sum = builder.Binary(IrOp::Add, product.Value(), y);
        const IrResult results[] = {
            {IrEntry::OpacityMask, "OpacityMask", sum.Value()},
            {IrEntry::Opacity, "Opacity", x},
        };
        return BuildIrModule(builder, results, module);
    };

    IrModule first(allocator);
    IrModule second(allocator);
    IrModule changed(allocator);
    REQUIRE(build(false, 0.5f, first));
    REQUIRE(build(true, 0.5f, second));
    REQUIRE(build(false, 0.25f, changed));
    // Opacity(x)가 먼저: 상수 2 → ddx → (OpacityMask) 상수 0.5 → 곱 → 상수 3 → ddx → 합.
    REQUIRE(first.Results().Size() == 2);
    CHECK(first.Results()[0].entry == IrEntry::Opacity);
    CHECK(first.Results()[1].entry == IrEntry::OpacityMask);
    REQUIRE(first.Values().Size() == 7);
    for (usize index = 0; index < first.Values().Size(); ++index) {
        const IrValue& value = first.Values()[index];
        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            CHECK(value.operands[operand] < index);
        }

        CHECK(value.op != IrOp::Sin);
    }

    CHECK(first.Results()[0].value == 1);
    CHECK(first.Results()[1].value == 6);
    CHECK(first.Hash() == second.Hash());
    CHECK(!(first.Hash() == changed.Hash()));

    // 겹치는 결과 칸·빌더 밖 번호는 거부한다.
    IrBuilder builder(allocator);
    const IrValueId x = Varying(builder, 1.0f);
    const IrResult duplicate[] = {{IrEntry::Opacity, "Opacity", x},
                                  {IrEntry::Opacity, "Opacity", x}};
    IrModule module(allocator);
    CHECK(BuildIrModule(builder, duplicate, module).Error() == ErrorCode::InvalidArgument);
    const IrResult outside[] = {{IrEntry::Opacity, "Opacity", 999}};
    CHECK(BuildIrModule(builder, outside, module).Error() == ErrorCode::InvalidArgument);
}

} // namespace material_ir_tests_file
