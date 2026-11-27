#include <Core/Text/String.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 표면 연산 lowering이다(Docs/Design/MaterialSystem 6.11절 MS2d2b). 연산 기록은 자식 표면과 연산
// 매개변수이고, 접기(parameter blending)는 출력 노드가 한다. 허용 표는 UE
// SubstrateTranslatorCommon.cpp:388–394, 문구는 :464–517과 같다.
namespace rocks::material::detail {

namespace {

// 연산 하나의 모양이다.
struct SurfaceOperatorShape {
    SurfaceOperator op;
    // UE 문구의 연산 이름이다.
    StringView name;
    // 자식 입력(Weight는 하나)이다.
    StringView first;
    StringView second;
    // 매개변수 입력과 기본값이다.
    StringView operandKey;
    float operandDefault;
    // 허용되는 흐르는 종류(SurfaceFlow 비트)다.
    uint16 allowed;
};

constexpr uint16 OperatorSlabOnly = SurfaceFlowBit(SurfaceFlow::Slab);
// UE MTV_WeightOp_AllowedList·MTV_SelectOp_AllowedList다.
constexpr uint16 OperatorWeightAllowed = SurfaceFlowBit(SurfaceFlow::Unlit) |
                                         SurfaceFlowBit(SurfaceFlow::Slab) |
                                         SurfaceFlowBit(SurfaceFlow::Toon);
constexpr uint16 OperatorSelectAllowed =
    SurfaceFlowBit(SurfaceFlow::Slab) | SurfaceFlowBit(SurfaceFlow::Hair) |
    SurfaceFlowBit(SurfaceFlow::Eye) | SurfaceFlowBit(SurfaceFlow::Toon);
// 동적 고전 preset의 전용 BSDF 후보다(연산 아래에서 막는다 — 6.11절 MS2d2).
constexpr uint16 OperatorDedicatedFlows = SurfaceFlowBit(SurfaceFlow::Hair) |
                                          SurfaceFlowBit(SurfaceFlow::Eye) |
                                          SurfaceFlowBit(SurfaceFlow::SingleLayerWater);
// UE 기본값이다(MES:2643 Mix 0.5, Weight 1, SelectValue 0, Top Thickness는 두께 stack 기본
// SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM).
constexpr float OperatorDefaultMix = 0.5f;
constexpr float OperatorDefaultThicknessCm = 0.01f;
constexpr float OperatorDefaultThreshold = 0.5f;

// UE 문구의 종류 이름이다(SurfaceFlow 비트 순서 — STC:470–508).
constexpr StringView OperatorFlowNames[] = {"Unlit ", "Slab ",        "Hair ",         "Eye ",
                                            "SLW ",   "Toon ",        "VFogCloud ",    "UI ",
                                            "Decal ", "PostProcess ", "LightFunction "};

Result<void> OperatorReport(LoweringContext& context, StringView message) noexcept {
    return context.Error(MaterialDiagnosticCode::UnsupportedCapability, StringView(), message);
}

// 허용되지 않는 종류의 UE 문구다.
Result<void> OperatorNotAllowed(LoweringContext& context, StringView name, uint16 flows) noexcept {
    SystemAllocator scratch;
    String message(scratch);
    if (auto appended = message.TryAppend("Substrate error: Operator "); !appended) {
        return appended;
    }

    const StringView middle[] = {name, " cannot be used with the following data flowing through {"};
    for (StringView part : middle) {
        if (auto appended = message.TryAppend(part); !appended) {
            return appended;
        }
    }

    constexpr usize FlowCount = sizeof(OperatorFlowNames) / sizeof(OperatorFlowNames[0]);
    for (usize index = 0; index < FlowCount; ++index) {
        if ((flows & (1u << index)) == 0) {
            continue;
        }

        if (auto appended = message.TryAppend(OperatorFlowNames[index]); !appended) {
            return appended;
        }
    }

    if (auto appended = message.TryAppend("}."); !appended) {
        return appended;
    }

    return OperatorReport(context, message.View());
}

// 매개변수 입력이다(연결이 없으면 기본값 상수).
Result<IrValueId> OperatorOperand(LoweringContext& context, StringView key,
                                  float fallback) noexcept {
    if (key.IsEmpty()) {
        return context.FloatScalar(fallback);
    }

    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.FloatScalar(fallback);
    }

    auto input = context.Input(key);
    if (!input || input.Value() == NoIrValue) {
        return input;
    }

    return context.CoerceToWidth(input.Value(), 1, key);
}

Result<void> LowerSurfaceOperator(LoweringContext& context,
                                  const SurfaceOperatorShape& shape) noexcept {
    SurfaceRecord record;
    record.kind = SurfaceKind::Operator;
    record.op = shape.op;
    const StringView keys[] = {shape.first, shape.second};
    uint16 flow = 0;
    uint16 dedicated = 0;
    for (usize index = 0; index < 2; ++index) {
        if (keys[index].IsEmpty()) {
            continue;
        }

        auto child = context.Input(keys[index]);
        if (!child || child.Value() == NoIrValue) {
            // 필수 입력 오류나 위쪽 오류가 이미 진단했다.
            return child ? Result<void>::Success() : Result<void>::Failure(child.Error());
        }

        // 선행 없는 표면(늦은 진단 자리 값)은 그대로 지나간다(결과에 닿으면 그 노드가 진단된다).
        if (context.Builder().Value(child.Value()).op == IrOp::Pending) {
            return context.Output("Result", child.Value());
        }

        const SurfaceRecord* inner = context.SurfaceOf(child.Value());
        if (inner == nullptr) {
            return context.Error(MaterialDiagnosticCode::TypeMismatch, keys[index],
                                 "Surface input requires a surface value");
        }

        flow |= inner->flow;
        const IrValueId model = inner->Field(SurfaceField::ShadingModel);
        if (inner->kind == SurfaceKind::Classic && model != NoIrValue &&
            context.Builder().Value(model).op != IrOp::Constant) {
            dedicated |= inner->flow & OperatorDedicatedFlows;
        }

        record.children[index] = child.Value();
    }

    // Decal은 뿌리여야 하고(MES:1931–1950), SingleLayerWater는 섞지 못한다(MES:2368–2375).
    if ((flow & SurfaceFlowBit(SurfaceFlow::Decal)) != 0) {
        return OperatorReport(context,
                              "Substrate Convert To Decal node must be the root of a parameter "
                              "blending sub tree: no more Substrate operations can be applied a "
                              "over its output.");
    }

    if ((flow & SurfaceFlowBit(SurfaceFlow::SingleLayerWater)) != 0) {
        return OperatorReport(
            context,
            "Substrate SingleLayerWater BSDF node cannot be used with parameter blending.");
    }

    const uint16 refused = static_cast<uint16>((flow & ~shape.allowed) | dedicated);
    if (refused != 0) {
        return OperatorNotAllowed(context, shape.name, refused);
    }

    auto operand = OperatorOperand(context, shape.operandKey, shape.operandDefault);
    if (!operand || operand.Value() == NoIrValue) {
        return operand ? Result<void>::Success() : Result<void>::Failure(operand.Error());
    }

    record.operand = operand.Value();
    if (shape.op == SurfaceOperator::Select) {
        // Threshold는 0..1이다(UE ClampMin·ClampMax).
        float threshold = OperatorDefaultThreshold;
        auto read = context.ReadFloat("threshold", threshold, threshold);
        if (!read || !read.Value()) {
            return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
        }

        record.threshold = threshold < 0.0f ? 0.0f : (threshold > 1.0f ? 1.0f : threshold);
    }

    record.flow = flow;
    auto surface = context.MakeSurface(record);
    if (!surface) {
        return Result<void>::Failure(surface.Error());
    }

    return context.Output("Result", surface.Value());
}

// UE SubstrateHorizontalMixing(MES:2609–2787).
Result<void> LowerSurfaceHorizontalMix(LoweringContext& context) noexcept {
    const SurfaceOperatorShape shape = {SurfaceOperator::HorizontalMix,
                                        "Horizontal",
                                        "Background",
                                        "Foreground",
                                        "Mix",
                                        OperatorDefaultMix,
                                        OperatorSlabOnly};
    return LowerSurfaceOperator(context, shape);
}

// UE SubstrateVerticalLayering(MES:2791–2993).
Result<void> LowerSurfaceVerticalLayer(LoweringContext& context) noexcept {
    const SurfaceOperatorShape shape = {
        SurfaceOperator::VerticalLayer, "Vertical",      "Top", "Base", "Thickness",
        OperatorDefaultThicknessCm,     OperatorSlabOnly};
    return LowerSurfaceOperator(context, shape);
}

// UE SubstrateAdd(MES:2997–3168) — 매개변수 입력이 없다(operand는 쓰지 않는 상수).
Result<void> LowerSurfaceAdd(LoweringContext& context) noexcept {
    const SurfaceOperatorShape shape = {SurfaceOperator::Add, "Add", "A", "B", StringView(), 1.0f,
                                        OperatorSlabOnly};
    return LowerSurfaceOperator(context, shape);
}

// UE SubstrateWeight(MES:3172–3297).
Result<void> LowerSurfaceWeight(LoweringContext& context) noexcept {
    const SurfaceOperatorShape shape = {
        SurfaceOperator::Weight, "Weight", "A", StringView(), "Weight", 1.0f,
        OperatorWeightAllowed};
    return LowerSurfaceOperator(context, shape);
}

// UE SubstrateSelect(MES:3301–3488).
Result<void> LowerSurfaceSelect(LoweringContext& context) noexcept {
    const SurfaceOperatorShape shape = {
        SurfaceOperator::Select, "Select", "A", "B", "SelectValue", 0.0f, OperatorSelectAllowed};
    return LowerSurfaceOperator(context, shape);
}

constexpr LoweringEntry SurfaceOperatorEntries[] = {
    {"Material.SurfaceHorizontalMix", &LowerSurfaceHorizontalMix},
    {"Material.SurfaceVerticalLayer", &LowerSurfaceVerticalLayer},
    {"Material.SurfaceAdd", &LowerSurfaceAdd},
    {"Material.SurfaceWeight", &LowerSurfaceWeight},
    {"Material.SurfaceSelect", &LowerSurfaceSelect},
};

} // namespace

Span<const LoweringEntry> SurfaceOperatorLowerings() noexcept {
    return SurfaceOperatorEntries;
}

} // namespace rocks::material::detail
