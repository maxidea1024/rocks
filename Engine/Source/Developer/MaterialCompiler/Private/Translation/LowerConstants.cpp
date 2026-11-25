#include "LoweringContext.h"
#include "LoweringTable.h"

// 상수 노드 lowering이다(Docs/Design/MaterialSystem 6.8절 lowering 표 "Constant…"). UE
// MaterialExpressions.cpp Compile(Constant·Constant2·3·4)과 MaterialExpressionsToMIR.cpp
// Build(성분 출력은 Subscript)를 따른다.
namespace rocks::material::detail {

namespace {

// 성분 출력 키다(Constant2·3은 Result 뒤 R·G·B, Constant4는 RGBA 뒤 R·G·B·A).
constexpr StringView ConstantComponentKeys[MaxIrComponents] = {"R", "G", "B", "A"};

// value 벡터와 성분 출력(앞 count개)을 낸다.
Result<void> OutputVectorAndComponents(LoweringContext& context, StringView vectorKey,
                                       IrValueId value, uint8 count) noexcept {
    if (auto written = context.Output(vectorKey, value); !written) {
        return written;
    }

    for (uint8 index = 0; index < count; ++index) {
        const uint8 component[] = {index};
        auto picked = context.Swizzle(value, Span<const uint8>(component, 1));
        if (!picked) {
            return Result<void>::Failure(picked.Error());
        }

        if (auto written = context.Output(ConstantComponentKeys[index], picked.Value()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

Result<void> LowerConstant(LoweringContext& context) noexcept {
    float r = 0.0f;
    auto read = context.ReadFloat("r", 0.0f, r);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto value = context.FloatConstant(Span<const float>(&r, 1), StringView());
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

Result<void> LowerConstant2(LoweringContext& context) noexcept {
    float components[2] = {0.0f, 0.0f};
    auto r = context.ReadFloat("r", 0.0f, components[0]);
    auto g = context.ReadFloat("g", 0.0f, components[1]);
    if (!r || !g) {
        return Result<void>::Failure(!r ? r.Error() : g.Error());
    }

    if (!r.Value() || !g.Value()) {
        return Result<void>::Success();
    }

    auto value = context.FloatConstant(Span<const float>(components, 2), StringView());
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return OutputVectorAndComponents(context, "Result", value.Value(), 2);
}

// Constant3·4의 color 칸(UE FLinearColor Constant)이다.
Result<void> LowerColorConstant(LoweringContext& context, uint8 count,
                                StringView vectorKey) noexcept {
    float color[MaxIrComponents];
    auto read = context.ReadColor("constant", color);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto value = context.FloatConstant(Span<const float>(color, count), StringView());
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return OutputVectorAndComponents(context, vectorKey, value.Value(), count);
}

Result<void> LowerConstant3(LoweringContext& context) noexcept {
    return LowerColorConstant(context, 3, "Result");
}

Result<void> LowerConstant4(LoweringContext& context) noexcept {
    constexpr uint8 ColorComponents = 4;
    constexpr uint8 RgbComponents = 3;
    if (auto lowered = LowerColorConstant(context, ColorComponents, "RGBA"); !lowered) {
        return lowered;
    }

    // UE Constant4Vector 출력 RGB(마스크 1,1,1,0)다.
    float color[MaxIrComponents];
    auto read = context.ReadColor("constant", color);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto rgb = context.FloatConstant(Span<const float>(color, RgbComponents), StringView());
    if (!rgb) {
        return Result<void>::Failure(rgb.Error());
    }

    return context.Output("RGB", rgb.Value());
}

Result<void> LowerConstantDouble(LoweringContext& context) noexcept {
    double value = 0.0;
    auto read = context.ReadDouble("value", 0.0, value);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto constant = context.DoubleConstant(value, StringView());
    if (!constant) {
        return Result<void>::Failure(constant.Error());
    }

    return context.Output("Result", constant.Value());
}

constexpr LoweringEntry ConstantEntries[] = {
    {"Material.Constant", &LowerConstant},
    {"Material.Constant2", &LowerConstant2},
    {"Material.Constant3", &LowerConstant3},
    {"Material.Constant4", &LowerConstant4},
    {"Material.ConstantDouble", &LowerConstantDouble},
};

} // namespace

Span<const LoweringEntry> ConstantLowerings() noexcept {
    return ConstantEntries;
}

} // namespace rocks::material::detail
