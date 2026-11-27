#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <RenderData/MaterialEnums.h>

#include "CustomOutputTable.h"
#include "LoweringContext.h"
#include "Translator.h"

// custom 출력 번역이다(Docs/Design/MaterialSystem 6.11절 MS2d3). 출력 노드 번역 뒤에 custom 출력
// 노드마다 그 입력을 빈도의 문맥에서 번역하고, 노드가 없는 종류는 뿌리 표면이 준다.
namespace rocks::material::detail {

namespace {

constexpr StringView CustomOutputCacheType = "Material.MaterialCacheOutput";
constexpr StringView CustomOutputVirtualTexture = "VirtualTextureOutput";
constexpr StringView CustomOutputNeuralInput = "NeuralInput";
// UE ENeuralIndexType::NIT_TextureIndex의 batch 번호(−1)와 BufferIndex(0)다.
constexpr float CustomOutputTextureBatch = -1.0f;
constexpr int64 CustomOutputTextureIndexType = 0;
constexpr uint8 CustomOutputWidth = 4;

// kind의 결과 진입점이다.
IrEntry CustomOutputEntry(const CustomOutputKind& kind, EvaluationContext context) noexcept {
    if (kind.frequency == CustomOutputFrequency::Pixel) {
        return IrEntry::CustomOutput;
    }

    return context == EvaluationContext::VertexPrevious ? IrEntry::CustomOutputVertexPrevious
                                                        : IrEntry::CustomOutputVertex;
}

// UE NeuralNetworkInput 출력 0(MaterialExpressions.cpp:22466–22485)이다.
Result<IrValueId> NeuralCoordinates(LoweringContext& context, bool connected) noexcept {
    int64 type = CustomOutputTextureIndexType;
    auto read = context.ReadInteger("neuralIndexType", type, type);
    if (!read || !read.Value()) {
        return read ? Result<IrValueId>::Success(NoIrValue)
                    : Result<IrValueId>::Failure(read.Error());
    }

    const bool texture = type == CustomOutputTextureIndexType;
    if (connected) {
        auto input = context.Input("Coordinates");
        if (!input || input.Value() == NoIrValue) {
            return input;
        }

        auto coordinates = context.CoerceToWidth(input.Value(), 4, "Coordinates");
        if (!coordinates || coordinates.Value() == NoIrValue || !texture) {
            return coordinates;
        }

        // TextureIndex는 batch를 −1로 바꾼다(AppendVector(−1, yzw)).
        const uint8 tail[] = {1, 2, 3};
        auto rest = context.Swizzle(coordinates.Value(), tail);
        auto batch = context.FloatScalar(CustomOutputTextureBatch);
        if (!rest || !batch) {
            return !rest ? Move(rest) : Move(batch);
        }

        return context.Append(batch.Value(), rest.Value());
    }

    // (batch, 0, ViewportUv)다.
    const float head[] = {texture ? CustomOutputTextureBatch : 0.0f, 0.0f};
    auto prefix = context.FloatConstant(head, StringView());
    auto viewport = context.ContextValue("ViewportUv");
    if (!prefix || !viewport) {
        return !prefix ? Move(prefix) : Move(viewport);
    }

    return context.Append(prefix.Value(), viewport.Value());
}

} // namespace

Result<void> Translator::TranslateCustomOutput(const nodegraph::GraphNode& node,
                                               const CustomOutputKind& kind) noexcept {
    const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
    if (definition == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const EvaluationContext pixel[] = {EvaluationContext::Pixel};
    const EvaluationContext vertex[] = {EvaluationContext::Vertex,
                                        EvaluationContext::VertexPrevious};
    const Span<const EvaluationContext> contexts = kind.frequency == CustomOutputFrequency::Pixel
                                                       ? Span<const EvaluationContext>(pixel)
                                                       : Span<const EvaluationContext>(vertex);
    SystemAllocator scratch;
    Array<usize> starts(scratch);
    Array<usize> order(scratch);
    for (EvaluationContext evaluation : contexts) {
        context_ = evaluation;
        // RVT 출력 입력은 따로 캐시한 범위에서 번역한다(UE PushRuntimeVirtualTextureOutput).
        virtualTextureScope_ = kind.name == CustomOutputVirtualTexture;
        starts.Clear();
        for (const nodegraph::GraphEdge& edge : graph_->edges) {
            if (!(edge.toNode == node.id)) {
                continue;
            }

            if (const usize* source = nodes_.Find(edge.fromNode)) {
                if (auto added = starts.TryPushBack(*source); !added) {
                    return added;
                }
            }
        }

        order.Clear();
        if (auto planned = PlanOrder(starts.View(), order); !planned) {
            return planned;
        }

        if (auto lowered = LowerPlanned(order.View()); !lowered) {
            return lowered;
        }

        if (auto marked = States().InsertOrAssign(node.id, NodeState::Visiting); !marked) {
            return Result<void>::Failure(marked.Error());
        }

        LoweringContext context(*this, node, *definition);
        for (usize index = 0; index < kind.inputs.Size(); ++index) {
            const CustomOutputInput& input = kind.inputs[index];
            auto connected = context.IsConnected(input.key);
            if (!connected) {
                return Result<void>::Failure(connected.Error());
            }

            IrValueId value = NoIrValue;
            if (kind.name == CustomOutputNeuralInput && index == 0) {
                auto coordinates = NeuralCoordinates(context, connected.Value());
                if (!coordinates) {
                    return Result<void>::Failure(coordinates.Error());
                }

                value = coordinates.Value();
            } else if (connected.Value()) {
                auto read = context.Input(input.key);
                if (!read) {
                    return Result<void>::Failure(read.Error());
                }

                if (read.Value() != NoIrValue) {
                    auto coerced = context.CoerceToWidth(read.Value(), input.width, input.key);
                    if (!coerced) {
                        return Result<void>::Failure(coerced.Error());
                    }

                    value = coerced.Value();
                }
            } else if (input.hasDefault) {
                auto constant = context.FloatConstant(
                    Span<const float>(input.fallback, input.width), input.key);
                if (!constant) {
                    return Result<void>::Failure(constant.Error());
                }

                value = constant.Value();
            } else if (input.required) {
                // UE MaterialExpressions.cpp:20262–20344.
                if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, input.key,
                                                  "Input missing");
                    !reported) {
                    return reported;
                }
            }

            if (value == NoIrValue) {
                continue;
            }

            // 결과는 float4다(짧으면 0을 붙인다 — 7.3절).
            const uint8 width = context.TypeOf(value).components;
            if (width < CustomOutputWidth) {
                const float zeros[] = {0.0f, 0.0f, 0.0f};
                auto padding = context.FloatConstant(
                    Span<const float>(zeros, CustomOutputWidth - width), StringView());
                if (!padding) {
                    return Result<void>::Failure(padding.Error());
                }

                auto padded = context.Append(value, padding.Value());
                if (!padded) {
                    return Result<void>::Failure(padded.Error());
                }

                value = padded.Value();
            }

            if (auto added = results_.TryPushBack(
                    IrResult{CustomOutputEntry(kind, evaluation), input.slot, value});
                !added) {
                return added;
            }
        }

        if (auto marked = States().InsertOrAssign(node.id, NodeState::Done); !marked) {
            return Result<void>::Failure(marked.Error());
        }
    }

    virtualTextureScope_ = false;
    context_ = EvaluationContext::Pixel;
    return Result<void>::Success();
}

Result<void> Translator::CustomOutputDefines(const nodegraph::GraphNode& node,
                                             const CustomOutputKind& kind) noexcept {
    if (kind.name == StringView("VolumetricCloudEmptySpaceSkippingOutput")) {
        return defines_.TryPushBack(
            IrDefine{"MATERIAL_VOLUMETRIC_CLOUD_EMPTY_SPACE_SKIPPING_OUTPUT", 1});
    }

    if (kind.name != StringView("VolumetricAdvancedMaterialOutput")) {
        return Result<void>::Success();
    }

    // UE HT:2542–2561, 14965–14985.
    const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
    if (definition == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    LoweringContext context(*this, node, *definition);
    struct Flag {
        StringView field;
        bool fallback;
        bool value;
    };

    Flag flags[] = {{"perSamplePhaseEvaluation", false, false},
                    {"groundContribution", false, false},
                    {"grayScaleMaterial", false, false},
                    {"rayMarchVolumeShadow", true, true},
                    {"clampMultiScatteringContribution", true, true}};
    for (Flag& flag : flags) {
        auto read = context.ReadBool(flag.field, flag.fallback, flag.value);
        if (!read || !read.Value()) {
            return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
        }
    }

    int64 octaves = 0;
    auto octaveRead = context.ReadInteger("multiScatteringApproximationOctaveCount", 0, octaves);
    if (!octaveRead || !octaveRead.Value()) {
        return octaveRead ? Result<void>::Success() : Result<void>::Failure(octaveRead.Error());
    }

    bool phaseConnected = false;
    const StringView phases[] = {"PhaseG", "PhaseG2", "PhaseBlend"};
    for (StringView key : phases) {
        auto connected = context.IsConnected(key);
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        phaseConnected = phaseConnected || connected.Value();
    }

    auto density = context.IsConnected("ConservativeDensity");
    if (!density) {
        return Result<void>::Failure(density.Error());
    }

    // 출력 노드 AO 연결 또는 표면 AO(VolumetricFogCloud)다.
    bool occlusion = exportedSurface_.Field(SurfaceField::AmbientOcclusion) != NoIrValue &&
                     exportedSurface_.kind == SurfaceKind::VolumetricFogCloud;
    auto aoPin = input_.names.TryFind("AmbientOcclusion");
    if (aoPin && outputNode_ != nullptr) {
        for (const nodegraph::GraphEdge& edge : graph_->edges) {
            occlusion =
                occlusion || (edge.toNode == outputNode_->id && edge.toPin == aoPin.Value());
        }
    }

    const bool perSample = flags[0].value && phaseConnected;
    const IrDefine defines[] = {
        {"MATERIAL_VOLUMETRIC_ADVANCED", 1},
        {perSample ? StringView("MATERIAL_VOLUMETRIC_ADVANCED_PHASE_PERSAMPLE")
                   : StringView("MATERIAL_VOLUMETRIC_ADVANCED_PHASE_PERPIXEL"),
         1},
        {"MATERIAL_VOLUMETRIC_ADVANCED_GRAYSCALE_MATERIAL", flags[2].value ? 1u : 0u},
        {"MATERIAL_VOLUMETRIC_ADVANCED_RAYMARCH_VOLUME_SHADOW", flags[3].value ? 1u : 0u},
        {"MATERIAL_VOLUMETRIC_ADVANCED_CLAMP_MULTISCATTERING_CONTRIBUTION",
         flags[4].value ? 1u : 0u},
        {"MATERIAL_VOLUMETRIC_ADVANCED_MULTISCATTERING_OCTAVE_COUNT", static_cast<uint32>(octaves)},
        {"MATERIAL_VOLUMETRIC_ADVANCED_CONSERVATIVE_DENSITY", density.Value() ? 1u : 0u},
        {"MATERIAL_VOLUMETRIC_ADVANCED_OVERRIDE_AMBIENT_OCCLUSION", occlusion ? 1u : 0u},
        {"MATERIAL_VOLUMETRIC_ADVANCED_GROUND_CONTRIBUTION", flags[1].value ? 1u : 0u},
    };
    for (const IrDefine& define : defines) {
        if (auto added = defines_.TryPushBack(define); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::SurfaceCustomOutputs(Span<const bool> present) noexcept {
    const Span<const CustomOutputKind> kinds = CustomOutputKinds();
    const SurfaceRecord& surface = exportedSurface_;
    const auto shadingModel = [this](ShadingModel model) noexcept {
        return (shadingModels_ & (1u << static_cast<uint32>(model))) != 0;
    };

    for (usize index = 0; index < kinds.Size(); ++index) {
        if (present[index]) {
            continue;
        }

        const CustomOutputKind& kind = kinds[index];
        IrValueId values[4] = {NoIrValue, NoIrValue, NoIrValue, NoIrValue};
        if (kind.name == StringView("ClearCoatBottomNormal")) {
            values[0] = surface.secondNormal;
        } else if (kind.name == StringView("TangentOutput")) {
            values[0] = surface.customTangent;
        } else if (kind.name == StringView("ThinTranslucentMaterialOutput") &&
                   shadingModel(ShadingModel::ThinTranslucent)) {
            values[0] = surface.thinTransmittance;
            values[1] = surface.thinCoverage;
        } else if (kind.name == StringView("SingleLayerWaterMaterialOutput") &&
                   shadingModel(ShadingModel::SingleLayerWater)) {
            for (usize item = 0; item < 4; ++item) {
                values[item] = surface.water[item];
            }
        }

        for (usize item = 0; item < kind.inputs.Size() && item < 4; ++item) {
            IrValueId value = values[item];
            if (value == NoIrValue) {
                continue;
            }

            const uint8 width = builder_.Value(value).type.components;
            if (width < CustomOutputWidth) {
                const float zeros[] = {0.0f, 0.0f, 0.0f};
                auto padding =
                    builder_.FloatConstant(Span<const float>(zeros, CustomOutputWidth - width));
                if (!padding) {
                    return Result<void>::Failure(padding.Error());
                }

                auto padded = builder_.Append(value, padding.Value());
                if (!padded) {
                    return Result<void>::Failure(padded.Error());
                }

                value = padded.Value();
            }

            if (auto added = results_.TryPushBack(
                    IrResult{IrEntry::CustomOutput, kind.inputs[item].slot, value});
                !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> Translator::TranslateCustomOutputs() noexcept {
    const Span<const CustomOutputKind> kinds = CustomOutputKinds();
    SystemAllocator scratch;
    Array<bool> present(scratch);
    Array<Name> types(scratch);
    if (auto sized = present.TryResize(kinds.Size()); !sized) {
        return sized;
    }

    for (usize index = 0; index < kinds.Size(); ++index) {
        present[index] = false;
        auto type = input_.names.TryFind(kinds[index].type);
        if (auto added = types.TryPushBack(type ? type.Value() : Name()); !added) {
            return added;
        }
    }

    auto cacheType = input_.names.TryFind(CustomOutputCacheType);
    for (const nodegraph::GraphNode& node : graph_->nodes) {
        if (node.enableState == nodegraph::NodeEnableState::Disabled) {
            continue;
        }

        // TODO(MPQ-VirtualTexture): 머티리얼 캐시 출력은 가상 텍스처 경로가 생기면 번역한다(뿌리
        // 노드라 Pending 자리 값이 결과에 닿지 않으므로 여기서 진단한다).
        if (cacheType && node.type == cacheType.Value()) {
            String message(scratch);
            const StringView parts[] = {
                "Node is not translated until prerequisite ",
                MaterialPrerequisiteId(MaterialPrerequisite::VirtualTexture), " exists"};
            for (StringView part : parts) {
                if (auto appended = message.TryAppend(part); !appended) {
                    return appended;
                }
            }

            if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                       MaterialDiagnosticCode::PrerequisiteMissing, node.id, Name(),
                                       message.View());
                !reported) {
                return reported;
            }

            continue;
        }

        usize kind = kinds.Size();
        for (usize index = 0; index < kinds.Size(); ++index) {
            if (types[index].IsValid() && node.type == types[index]) {
                kind = index;
            }
        }

        if (kind == kinds.Size()) {
            continue;
        }

        // 같은 종류는 하나만이다(UE HT:876–961 — MaterialCache만 여럿).
        if (present[kind]) {
            const MaterialNodeDefinition* definition = input_.catalog.Find(node.type);
            String message(scratch);
            const StringView parts[] = {
                "The material can contain only one ",
                definition != nullptr ? definition->menu.displayName : kinds[kind].name, " node"};
            for (StringView part : parts) {
                if (auto appended = message.TryAppend(part); !appended) {
                    return appended;
                }
            }

            if (auto reported = Report(nodegraph::GraphDiagnosticSeverity::Error,
                                       MaterialDiagnosticCode::UnsupportedCapability, node.id,
                                       Name(), message.View());
                !reported) {
                return reported;
            }

            continue;
        }

        present[kind] = true;
        if (auto translated = TranslateCustomOutput(node, kinds[kind]); !translated) {
            return translated;
        }

        if (auto defined = CustomOutputDefines(node, kinds[kind]); !defined) {
            return defined;
        }
    }

    return SurfaceCustomOutputs(present.View());
}

} // namespace rocks::material::detail
