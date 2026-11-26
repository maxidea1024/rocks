#include <Core/Memory/SystemAllocator.h>
#include <MaterialSchema/MaterialContextValue.h>

#include "LoweringContext.h"
#include "Translator.h"

// 출력 노드 Surface 핀의 접기·내보내기와 대표 노멀 순환 검사다(Docs/Design/MaterialSystem 6.11절
// MS2d1). 표면 기록 트리를 closure 하나로 접어 SurfaceAttributes 칸(진입점 Surface)과 대표 노멀
// (진입점 Normal)으로 낸다.
namespace rocks::material::detail {

namespace {

// 칸마다의 Surface 진입점 결과 slot이다(SurfaceField 순서 — 생성 원문이 칸 이름으로 바꾼다).
// opacity는 Opacity 진입점과 다른 slot이다(SSS 칸 — 6.11절).
constexpr StringView SurfaceSlotNames[SurfaceFieldCount] = {
    "BaseColor",   "Metallic",       "Specular",     "Roughness",       "Anisotropy",
    "Emissive",    "Normal",         "Tangent",      "SubsurfaceColor", "CustomData0",
    "CustomData1", "SurfaceOpacity", "ShadingModel", "AmbientOcclusion"};

// saturate하는 칸이다(UE MaterialTemplate 고전 속성 — MT:3778–4146).
bool SaturatedField(SurfaceField field) noexcept {
    switch (field) {
    case SurfaceField::BaseColor:
    case SurfaceField::Metallic:
    case SurfaceField::Specular:
    case SurfaceField::Roughness:
    case SurfaceField::SubsurfaceColor:
    case SurfaceField::Opacity:
    case SurfaceField::AmbientOcclusion:
        return true;
    default:
        return false;
    }
}

// 표면 쓰임이 영역에 맞는가(고전 preset은 모든 영역 — UE 고전 컴파일).
bool UsageFitsDomain(const SurfaceRecord& record, MaterialDomain domain) noexcept {
    if (record.kind == SurfaceKind::Classic) {
        return true;
    }

    switch (record.usage) {
    case SurfaceUsage::Surface:
        return domain == MaterialDomain::Surface || domain == MaterialDomain::DeferredDecal;
    case SurfaceUsage::LightFunction:
        return domain == MaterialDomain::LightFunction;
    case SurfaceUsage::PostProcess:
        return domain == MaterialDomain::PostProcess;
    case SurfaceUsage::UserInterface:
        return domain == MaterialDomain::UserInterface;
    case SurfaceUsage::Decal:
        return domain == MaterialDomain::DeferredDecal;
    case SurfaceUsage::Volume:
        return domain == MaterialDomain::Volume;
    }

    return false;
}

// Renderer 14.2절 번호표의 비트 전부다(번호가 정해지지 않은 shading model 값이 섞일 때).
constexpr uint32 AllShadingModelBits =
    (1u << static_cast<uint32>(rocks::ShadingModel::Unlit)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::DefaultLit)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::Subsurface)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::ClearCoat)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::SubsurfaceProfile)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::TwoSidedFoliage)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::Hair)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::Cloth)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::Eye)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::SingleLayerWater)) |
    (1u << static_cast<uint32>(rocks::ShadingModel::ThinTranslucent));

constexpr uint32 UnlitShadingModelBit = 1u << static_cast<uint32>(rocks::ShadingModel::Unlit);

bool IsSurfaceTranslucent(BlendMode mode) noexcept {
    return mode != BlendMode::Opaque && mode != BlendMode::Masked;
}

} // namespace

Result<IrValueId> Translator::AddSurface(const SurfaceRecord& record) noexcept {
    const uint32 index = static_cast<uint32>(surfaces_.Size());
    if (auto added = surfaces_.TryPushBack(record); !added) {
        return Result<IrValueId>::Failure(added.Error());
    }

    return builder_.SurfaceRef(index);
}

const SurfaceRecord* Translator::FindSurface(IrValueId value) const noexcept {
    if (value == NoIrValue || value >= builder_.Count()) {
        return nullptr;
    }

    const IrValue& reference = builder_.Value(value);
    if (reference.op != IrOp::SurfaceRef || reference.immediate[0] >= surfaces_.Size()) {
        return nullptr;
    }

    return &surfaces_[static_cast<usize>(reference.immediate[0])];
}

Result<void> Translator::TranslateSurface(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("Surface");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    SurfaceRecord exported;
    IrValueId surface = NoIrValue;
    if (connected.Value()) {
        auto input = context.Input("Surface");
        if (!input) {
            return Result<void>::Failure(input.Error());
        }

        if (input.Value() == NoIrValue) {
            // 위쪽 오류가 이미 진단했다.
            return Result<void>::Success();
        }

        surface = input.Value();
    }

    // 늦은 진단 자리 값(TODO 표면 노드)이면 그 자리를 baseColor로 내보낸다(결과에 닿으므로
    // CheckPending이 그 노드를 진단한다 — 6.10절 MS2c1).
    if (surface != NoIrValue && builder_.Value(surface).op == IrOp::Pending) {
        const IrValue pending = builder_.Value(surface);
        auto placeholder =
            builder_.Pending(IrFloat(3), static_cast<IrPendingKind>(pending.immediate[0]),
                             static_cast<uint8>(pending.immediate[1]),
                             Guid{pending.immediate[2], pending.immediate[3]});
        if (!placeholder) {
            return Result<void>::Failure(placeholder.Error());
        }

        exported.Field(SurfaceField::BaseColor) = placeholder.Value();
    } else if (surface != NoIrValue) {
        const SurfaceRecord* record = FindSurface(surface);
        if (record == nullptr) {
            return context.Error(MaterialDiagnosticCode::TypeMismatch, "Surface",
                                 "Surface input requires a surface value");
        }

        if (!UsageFitsDomain(*record, input_.traits.domain)) {
            return Report(nodegraph::GraphDiagnosticSeverity::Error,
                          MaterialDiagnosticCode::UnsupportedCapability, record->node, Name(),
                          "Surface node is not valid for the material domain");
        }

        exported = *record;
        if (record->kind == SurfaceKind::Decal) {
            // UE SubstrateWeightParameterBlending: coverage가 opacity에 곱해진다.
            const SurfaceRecord* inner = FindSurface(record->child);
            if (inner == nullptr) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            exported = *inner;
            auto coverage = context.Unary(IrOp::Saturate, record->coverage, NumberRule::FloatOnly);
            if (!coverage) {
                return Result<void>::Failure(coverage.Error());
            }

            IrValueId opacity = coverage.Value();
            if (inner->Field(SurfaceField::Opacity) != NoIrValue) {
                auto scaled = context.Binary(IrOp::Multiply, inner->Field(SurfaceField::Opacity),
                                             coverage.Value(), NumberRule::FloatOnly);
                if (!scaled) {
                    return Result<void>::Failure(scaled.Error());
                }

                opacity = scaled.Value();
            }

            exported.Field(SurfaceField::Opacity) = opacity;
        }
    }

    // 정해지지 않은 기저·shading model 칸을 채운다(연결이 없으면 정점 기저, DefaultLit).
    if (exported.Field(SurfaceField::Normal) == NoIrValue) {
        auto normal = context.ContextValue("VertexNormal");
        if (!normal) {
            return Result<void>::Failure(normal.Error());
        }

        exported.Field(SurfaceField::Normal) = normal.Value();
    }

    if (exported.Field(SurfaceField::Tangent) == NoIrValue) {
        auto tangent = context.ContextValue("VertexTangent");
        if (!tangent) {
            return Result<void>::Failure(tangent.Error());
        }

        exported.Field(SurfaceField::Tangent) = tangent.Value();
    }

    if (exported.Field(SurfaceField::ShadingModel) == NoIrValue) {
        const uint32 number = static_cast<uint32>(rocks::ShadingModel::DefaultLit);
        auto model = builder_.UIntConstant(Span<const uint32>(&number, 1));
        if (!model) {
            return Result<void>::Failure(model.Error());
        }

        exported.Field(SurfaceField::ShadingModel) = model.Value();
    }

    // 칸 값을 고전 범위로 맞춘다(UE MaterialTemplate).
    for (usize index = 0; index < SurfaceFieldCount; ++index) {
        const SurfaceField field = static_cast<SurfaceField>(index);
        IrValueId& value = exported.Field(field);
        if (value == NoIrValue) {
            continue;
        }

        if (SaturatedField(field)) {
            auto saturated = context.Unary(IrOp::Saturate, value, NumberRule::FloatOnly);
            if (!saturated) {
                return Result<void>::Failure(saturated.Error());
            }

            value = saturated.Value();
        } else if (field == SurfaceField::Anisotropy) {
            auto low = context.FloatScalar(-1.0f);
            auto high = context.FloatScalar(1.0f);
            if (!low || !high) {
                return Result<void>::Failure(!low ? low.Error() : high.Error());
            }

            auto clamped = context.Ternary(IrOp::Clamp, value, low.Value(), high.Value(),
                                           NumberRule::FloatOnly);
            if (!clamped) {
                return Result<void>::Failure(clamped.Error());
            }

            value = clamped.Value();
        }
    }

    // 표면 AO(VolumetricFogCloud)는 출력 노드 AO 결과를 대신한다.
    const bool surfaceOcclusion = exported.Field(SurfaceField::AmbientOcclusion) != NoIrValue;
    for (usize index = 0; index < results_.Size(); ++index) {
        if (surfaceOcclusion && results_[index].entry == IrEntry::Surface &&
            results_[index].slot == StringView("AmbientOcclusion")) {
            results_[index].value = exported.Field(SurfaceField::AmbientOcclusion);
            exported.Field(SurfaceField::AmbientOcclusion) = NoIrValue;
        }
    }

    for (usize index = 0; index < SurfaceFieldCount; ++index) {
        const IrValueId value = exported.fields[index];
        if (value == NoIrValue) {
            continue;
        }

        if (auto added =
                results_.TryPushBack(IrResult{IrEntry::Surface, SurfaceSlotNames[index], value});
            !added) {
            return added;
        }
    }

    if (auto added = results_.TryPushBack(
            IrResult{IrEntry::Normal, "Normal", exported.Field(SurfaceField::Normal)});
        !added) {
        return added;
    }

    // 반투명에서 Opacity 출력 핀이 쓰이지 않으면(AlphaComposite 밖) 표면 opacity가 Opacity
    // 진입점이다 (UE Substrate는 coverage — 6.11절).
    bool hasOpacity = false;
    for (const IrResult& result : results_) {
        hasOpacity = hasOpacity || result.entry == IrEntry::Opacity;
    }

    if (!hasOpacity && IsSurfaceTranslucent(input_.traits.blendMode) &&
        exported.Field(SurfaceField::Opacity) != NoIrValue) {
        if (auto added = results_.TryPushBack(
                IrResult{IrEntry::Opacity, "Opacity", exported.Field(SurfaceField::Opacity)});
            !added) {
            return added;
        }
    }

    // shading model 집합(UE ShadingModelsFromCompilation): static으로 닿는 상수의 합집합이다.
    shadingModels_ = 0;
    SystemAllocator scratch;
    Array<IrValueId> pending(scratch);
    if (auto pushed = pending.TryPushBack(exported.Field(SurfaceField::ShadingModel)); !pushed) {
        return pushed;
    }

    while (!pending.IsEmpty()) {
        const IrValueId id = pending[pending.Size() - 1];
        (void)pending.TryPopBack();
        const IrValue& value = builder_.Value(id);
        if (value.op == IrOp::Constant && value.immediate[0] < MaxShadingModelCount) {
            shadingModels_ |= 1u << static_cast<uint32>(value.immediate[0]);
        } else if (value.op == IrOp::Select || value.op == IrOp::Branch) {
            for (uint8 operand = 1; operand < value.operandCount; ++operand) {
                if (auto pushed = pending.TryPushBack(value.operands[operand]); !pushed) {
                    return pushed;
                }
            }
        } else {
            shadingModels_ |= AllShadingModelBits;
        }
    }

    // AO는 lit shading model만 쓴다(UE Material.cpp:7929–7930 — Unlit 하나면 비활성, V5).
    if (shadingModels_ == UnlitShadingModelBit) {
        for (usize index = 0; index < results_.Size(); ++index) {
            if (results_[index].entry != IrEntry::Surface ||
                results_[index].slot != StringView("AmbientOcclusion")) {
                continue;
            }

            auto aoConnected = context.IsConnected("AmbientOcclusion");
            if (!aoConnected) {
                return Result<void>::Failure(aoConnected.Error());
            }

            if (aoConnected.Value()) {
                if (auto reported = context.Warning(
                        MaterialDiagnosticCode::InactiveOutput, "AmbientOcclusion",
                        "Output pin is not used by this domain and blend mode; it is ignored");
                    !reported) {
                    return reported;
                }
            }

            results_[index] = results_[results_.Size() - 1];
            (void)results_.TryPopBack();
            break;
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::CheckNormalCycle() noexcept {
    // 대표 노멀(Normal 진입점)에서 PixelNormal·ReflectionVector(c.pixelNormal을 읽는 값)가 닿으면
    // 순환이다(UE HT:11784–11787 "Invalid node PixelNormalWS used for Normal input.").
    const Span<const IrValue> values = builder_.Values();
    const MaterialContextValue* pixelNormal = FindMaterialContextValue("PixelNormal");
    const MaterialContextValue* reflection = FindMaterialContextValue("ReflectionVector");
    const Span<const MaterialContextValue> table = MaterialContextValues();
    const uint64 pixelIndex =
        pixelNormal == nullptr ? table.Size() : static_cast<uint64>(pixelNormal - table.Data());
    const uint64 reflectionIndex =
        reflection == nullptr ? table.Size() : static_cast<uint64>(reflection - table.Data());
    SystemAllocator scratch;
    Array<bool> reached(scratch);
    if (auto sized = reached.TryResize(values.Size()); !sized) {
        return sized;
    }

    for (bool& item : reached) {
        item = false;
    }

    bool any = false;
    for (const IrResult& result : results_) {
        if (result.entry == IrEntry::Normal) {
            reached[result.value] = true;
            any = true;
        }
    }

    if (!any) {
        return Result<void>::Success();
    }

    for (usize index = values.Size(); index > 0; --index) {
        if (!reached[index - 1]) {
            continue;
        }

        const IrValue& value = values[index - 1];
        if (value.op == IrOp::Extern &&
            (value.immediate[0] == pixelIndex || value.immediate[0] == reflectionIndex)) {
            if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                       MaterialDiagnosticCode::Cycle, value.site.node, Name(),
                                       "Invalid node PixelNormalWS used for Normal input.");
                !reported) {
                return reported;
            }
        }

        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            reached[value.operands[operand]] = true;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material::detail
