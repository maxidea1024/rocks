#include <AssetNugget/NuggetReflected.h>
#include <Core/Memory/SystemAllocator.h>
#include <Material/MaterialGraphReferences.h>
#include <MaterialCompiler/MaterialConstants.h>
#include <MaterialCook/MaterialTranslationKey.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/MaterialShaderTraits.h>
#include <MaterialSchema/MaterialTraitsDesc.generated.h>
#include <NodeGraph/GraphSemantic.h>

// 머티리얼 번역 너겟 키다(Docs/Design/MaterialSystem 12.2절 표·12.8절 MS3c).
namespace rocks {

namespace {

using material::MaterialAssetReferenceRole;

// byte 사전순 비교다.
bool TextLess(StringView left, StringView right) noexcept {
    return StringView::CompareBytes(left, right) < 0;
}

// 의존 역할 문자열이다(키에 든다 — 12.2절 표).
StringView RoleName(MaterialAssetReferenceRole role) noexcept {
    switch (role) {
    case MaterialAssetReferenceRole::Texture:
        return "Texture";
    case MaterialAssetReferenceRole::Function:
        return "Function";
    case MaterialAssetReferenceRole::Material:
        return "Material";
    case MaterialAssetReferenceRole::ColorCurve:
        return "ColorCurve";
    case MaterialAssetReferenceRole::CurveAtlas:
        return "CurveAtlas";
    case MaterialAssetReferenceRole::Collection:
        return "Collection";
    }

    return "Unknown";
}

void WriteTarget(NuggetKeyWriter& writer,
                 const material::MaterialTranslationTarget& target) noexcept {
    writer.WriteU32(static_cast<uint32>(target.quality));
    writer.WriteBool(target.isMobile);
    writer.WriteBool(target.isPc);
    writer.WriteBool(target.isConsole);
    writer.WriteU32(target.maxSamplers);
    writer.WriteU32(static_cast<uint32>(target.bindless));
    writer.WriteBool(target.distanceFields);
    writer.WriteBool(target.virtualTexturing);
    writer.WriteBool(target.rayTracing);
    writer.WriteBool(target.virtualizedGeometry);
    writer.WriteBool(target.proxyBake);
    writer.WriteBool(target.lightBake);
    writer.WriteBool(target.simulateMobileForLowQuality);
}

void WriteFacts(NuggetKeyWriter& writer, const material::MaterialTextureFacts& facts) noexcept {
    writer.WriteU32(static_cast<uint32>(facts.kind));
    writer.WriteU32(static_cast<uint32>(facts.compression));
    writer.WriteU32(static_cast<uint32>(facts.lodGroup));
    writer.WriteBool(facts.srgb);
    writer.WriteBool(facts.virtualTexture);
    for (TextureAddress address : facts.address) {
        writer.WriteU32(static_cast<uint32>(address));
    }
}

void WriteCurve(NuggetKeyWriter& writer,
                const material::MaterialColorCurveSnapshot& curve) noexcept {
    const TangentCurve* channels[] = {curve.r, curve.g, curve.b, curve.a};
    for (const TangentCurve* channel : channels) {
        if (channel == nullptr) {
            writer.WriteNone();
            continue;
        }

        writer.WriteList(channel->KeyCount());
        for (const TangentCurveKey& key : channel->Keys()) {
            writer.WriteF32(key.time);
            writer.WriteF32(key.value);
            writer.WriteU32(static_cast<uint32>(key.interp));
            writer.WriteU32(static_cast<uint32>(key.tangentMode));
            writer.WriteU32(static_cast<uint32>(key.tangentWeight));
            writer.WriteF32(key.arriveTangent);
            writer.WriteF32(key.arriveTangentWeight);
            writer.WriteF32(key.leaveTangent);
            writer.WriteF32(key.leaveTangentWeight);
        }
    }

    const ColorCurveAdjustments& adjust = curve.adjustments;
    const float values[] = {
        adjust.hue,      adjust.saturation, adjust.brightness, adjust.brightnessCurve,
        adjust.vibrance, adjust.minAlpha,   adjust.maxAlpha};
    for (float value : values) {
        writer.WriteF32(value);
    }
}

// 그래프의 semantic hash다(파라미터 기본값·화면 데이터 제외 — 5.5절).
Result<void> WriteGraph(NuggetKeyWriter& writer, const MaterialCookCatalog& catalog,
                        const nodegraph::GraphDocument& document) noexcept {
    SystemAllocator scratch;
    auto hash = nodegraph::ComputeSemanticHash(document, catalog.Registry(), scratch);
    if (!hash) {
        return Result<void>::Failure(hash.Error());
    }

    writer.WriteDigest(hash.Value());
    return Result<void>::Success();
}

// 문서에 쓰인 노드 종류 Name을 out에 더한다(중복 없음).
Result<void> CollectUsedTypes(const nodegraph::GraphDocument& document, Array<Name>& out) noexcept {
    for (const nodegraph::Graph& graph : document.graphs) {
        for (const nodegraph::GraphNode& node : graph.nodes) {
            bool seen = false;
            for (Name existing : out) {
                seen = seen || existing == node.type;
            }

            if (!seen) {
                if (auto pushed = out.TryPushBack(node.type); !pushed) {
                    return pushed;
                }
            }
        }
    }

    return Result<void>::Success();
}

// 노드 종류 목록이다(이름 순 — 엔진 노드 정의, 루트와 모은 그래프 전부에 쓰인 것). 쓰인 Name 집합을
// 한 번 모으고 정의를 한 번 훑어 고른 뒤 이름 순으로 정렬한다.
Result<void> WriteNodeTypes(NuggetKeyWriter& writer,
                            const MaterialTranslationKeyDesc& desc) noexcept {
    NamePool& names = desc.catalog->Names();
    SystemAllocator scratch;
    Array<Name> used(scratch);
    if (auto collected = CollectUsedTypes(*desc.root, used); !collected) {
        return collected;
    }

    for (usize index = 0; index < desc.sources->GraphCount(); ++index) {
        const material::MaterialGraphSnapshot& snapshot = desc.sources->Graph(index);
        if (snapshot.document != nullptr) {
            if (auto collected = CollectUsedTypes(*snapshot.document, used); !collected) {
                return collected;
            }
        }
    }

    Array<const material::MaterialNodeDefinition*> picked(scratch);
    for (const Span<const material::MaterialNodeDefinition> group :
         material::BuiltinMaterialNodeGroups()) {
        for (const material::MaterialNodeDefinition& definition : group) {
            auto type = names.TryFind(definition.type);
            if (!type) {
                continue;
            }

            bool inUse = false;
            for (Name name : used) {
                inUse = inUse || name == type.Value();
            }

            if (inUse) {
                if (auto pushed = picked.TryPushBack(&definition); !pushed) {
                    return pushed;
                }
            }
        }
    }

    for (usize index = 1; index < picked.Size(); ++index) {
        for (usize at = index; at > 0 && TextLess(picked[at]->type, picked[at - 1]->type); --at) {
            const material::MaterialNodeDefinition* swap = picked[at];
            picked[at] = picked[at - 1];
            picked[at - 1] = swap;
        }
    }

    writer.WriteList(picked.Size());
    for (const material::MaterialNodeDefinition* definition : picked) {
        writer.WriteString(definition->type);
        writer.WriteU32(definition->schemaVersion);
        writer.WriteU32(definition->loweringVersion);
    }

    return Result<void>::Success();
}

// custom 속성 표 서명이다(null이면 "builtin", 아니면 등록 정의를 id 순으로 — 12.8절 키 세부).
Result<void> WriteAttributes(NuggetKeyWriter& writer,
                             const material::MaterialAttributeTable* table) noexcept {
    if (table == nullptr) {
        writer.WriteString("builtin");
        return Result<void>::Success();
    }

    SystemAllocator scratch;
    Array<const material::MaterialAttributeDefinition*> order(scratch);
    for (usize index = 0; index < table->Size(); ++index) {
        if (auto pushed = order.TryPushBack(&table->At(index)); !pushed) {
            return pushed;
        }
    }

    for (usize index = 1; index < order.Size(); ++index) {
        for (usize at = index; at > 0 && GuidLess(order[at]->id, order[at - 1]->id); --at) {
            const material::MaterialAttributeDefinition* swap = order[at];
            order[at] = order[at - 1];
            order[at - 1] = swap;
        }
    }

    writer.WriteString("registered");
    writer.WriteList(order.Size());
    for (const material::MaterialAttributeDefinition* definition : order) {
        writer.WriteGuid(definition->id);
        writer.WriteString(definition->name);
        writer.WriteU32(static_cast<uint32>(definition->type));
        writer.WriteU32(definition->defaultValue.componentCount);
        for (float value : definition->defaultValue.values) {
            writer.WriteF32(value);
        }

        writer.WriteU32(static_cast<uint32>(definition->frequency));
        writer.WriteBool(definition->hidden);
        writer.WriteString(definition->customOutput);
        writer.WriteU32(definition->customOutputIndex);
    }

    return Result<void>::Success();
}

// 텍스처 참조 위치 하나다(그래프 asset·노드 id·필드 순으로 정렬해 쓴다 — 저장 순서와 무관).
struct TextureSite {
    Guid graph;
    Guid node;
    StringView field;
    Guid texture;
};

bool SiteLess(const TextureSite& left, const TextureSite& right) noexcept {
    if (!(left.graph == right.graph)) {
        return GuidLess(left.graph, right.graph);
    }

    if (!(left.node == right.node)) {
        return GuidLess(left.node, right.node);
    }

    return TextLess(left.field, right.field);
}

Result<void> CollectTextureSites(const MaterialTranslationKeyDesc& desc, Guid graphAsset,
                                 const nodegraph::GraphDocument& document,
                                 Array<TextureSite>& out) noexcept {
    SystemAllocator scratch;
    Array<MaterialGraphReference> references(scratch);
    if (auto collected =
            CollectMaterialGraphReferences(desc.catalog->Names(), document, references);
        !collected) {
        return collected;
    }

    for (const MaterialGraphReference& reference : references) {
        if (reference.role != MaterialAssetReferenceRole::Texture &&
            reference.role != MaterialAssetReferenceRole::CurveAtlas) {
            continue;
        }

        if (auto pushed = out.TryPushBack(
                TextureSite{graphAsset, reference.node, reference.field, reference.asset.value});
            !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 텍스처 참조 위치 목록이다(AssetId 없이 — 사실만, 루트 그래프는 asset null).
Result<void> WriteTextureSites(NuggetKeyWriter& writer,
                               const MaterialTranslationKeyDesc& desc) noexcept {
    SystemAllocator scratch;
    Array<TextureSite> sites(scratch);
    if (auto collected = CollectTextureSites(desc, Guid{}, *desc.root, sites); !collected) {
        return collected;
    }

    for (usize index = 0; index < desc.sources->GraphCount(); ++index) {
        const material::MaterialGraphSnapshot& snapshot = desc.sources->Graph(index);
        if (snapshot.document == nullptr) {
            continue;
        }

        if (auto collected = CollectTextureSites(desc, snapshot.asset, *snapshot.document, sites);
            !collected) {
            return collected;
        }
    }

    for (usize index = 1; index < sites.Size(); ++index) {
        for (usize at = index; at > 0 && SiteLess(sites[at], sites[at - 1]); --at) {
            const TextureSite swap = sites[at];
            sites[at] = sites[at - 1];
            sites[at - 1] = swap;
        }
    }

    writer.WriteList(sites.Size());
    for (const TextureSite& site : sites) {
        writer.WriteGuid(site.graph);
        writer.WriteGuid(site.node);
        writer.WriteString(site.field);
        if (const material::MaterialTextureFacts* facts = desc.sources->FindTexture(site.texture)) {
            WriteFacts(writer, *facts);
        } else {
            writer.WriteNone();
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<Sha256Digest>
ComputeMaterialTranslationKey(const MaterialTranslationKeyDesc& desc) noexcept {
    // 키·모으기·hook은 같은 NamePool을 쓴다(다르면 노드 종류·참조 위치가 조용히 빈다).
    if (desc.catalog == nullptr || desc.registry == nullptr || desc.root == nullptr ||
        desc.traits == nullptr || desc.sources == nullptr ||
        desc.registry->Names() != &desc.catalog->Names()) {
        return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
    }

    NuggetKeyWriter writer;
    WriteNuggetKeyHeader(writer, MaterialTranslationNuggetId, desc.projectGeneration);
    writer.WriteU32(MaterialTranslationKeyLayoutVersion);
    writer.WriteGuid(desc.assetType);
    if (auto graph = WriteGraph(writer, *desc.catalog, *desc.root); !graph) {
        return Result<Sha256Digest>::Failure(graph.Error());
    }

    // 기본 속성은 리플렉션 정규 인코딩이다(필드가 늘어도 키가 따라간다).
    {
        auto lease = desc.registry->Types().AcquireValueType(
            GeneratedType<material::MaterialTraitsDesc>::Id(), TypeKind::Struct);
        if (!lease) {
            return Result<Sha256Digest>::Failure(lease.Error());
        }

        SystemAllocator scratch;
        // 쓰기 문맥은 바꿀 수 있는 값을 받는다(읽기만 한다 — 사본).
        material::MaterialTraitsDesc traits = *desc.traits;
        const reflection_text::AssignmentTarget target = reflection_text::AssignmentTarget::Value(
            *desc.registry, desc.access, lease.Value(), &traits, scratch);
        if (auto written = WriteReflected(writer, target); !written) {
            return Result<Sha256Digest>::Failure(written.Error());
        }
    }

    // static 값(parameterId 순).
    {
        SystemAllocator scratch;
        Array<const material::MaterialStaticParameterValue*> order(scratch);
        for (const material::MaterialStaticParameterValue& value : desc.statics) {
            if (auto pushed = order.TryPushBack(&value); !pushed) {
                return Result<Sha256Digest>::Failure(pushed.Error());
            }
        }

        for (usize index = 1; index < order.Size(); ++index) {
            for (usize at = index;
                 at > 0 && GuidLess(order[at]->parameterId, order[at - 1]->parameterId); --at) {
                const material::MaterialStaticParameterValue* swap = order[at];
                order[at] = order[at - 1];
                order[at - 1] = swap;
            }
        }

        // 같은 parameterId가 둘이면 입력 오류다(합친 사슬은 하나여야 한다).
        for (usize index = 1; index < order.Size(); ++index) {
            if (order[index]->parameterId == order[index - 1]->parameterId) {
                return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // 정규형: Bool은 값만, ComponentMask는 mask만 쓴다.
        writer.WriteList(order.Size());
        for (const material::MaterialStaticParameterValue* value : order) {
            writer.WriteGuid(value->parameterId);
            writer.WriteU32(static_cast<uint32>(value->kind));
            if (value->kind == material::MaterialStaticParameterKind::Bool) {
                writer.WriteBool(value->value);
            } else {
                for (bool channel : value->mask) {
                    writer.WriteBool(channel);
                }
            }
        }
    }

    WriteTarget(writer, desc.target);
    if (auto attributes = WriteAttributes(writer, desc.attributes); !attributes) {
        return Result<Sha256Digest>::Failure(attributes.Error());
    }

    if (auto types = WriteNodeTypes(writer, desc); !types) {
        return Result<Sha256Digest>::Failure(types.Error());
    }

    writer.WriteU32(MaterialCommonContractVersion);
    writer.WriteU32(material::MaterialConstantProgramVersion);
    writer.WriteU32(material::MaterialShaderTraitsVersion);

    // 의존 내용 키다(텍스처 역할은 위치 목록이 대신한다).
    for (const MaterialCookReference& reference : desc.sources->References()) {
        if (reference.role == MaterialAssetReferenceRole::Texture) {
            continue;
        }

        writer.WriteString(RoleName(reference.role));
        writer.WriteGuid(reference.asset.value);
        const Guid asset = reference.asset.value;
        switch (reference.role) {
        case MaterialAssetReferenceRole::Function: {
            const material::MaterialGraphSnapshot* snapshot = desc.sources->FindFunction(asset);
            if (snapshot == nullptr || snapshot->document == nullptr) {
                writer.WriteNone();
                break;
            }

            if (auto graph = WriteGraph(writer, *desc.catalog, *snapshot->document); !graph) {
                return Result<Sha256Digest>::Failure(graph.Error());
            }

            writer.WriteU32(static_cast<uint32>(snapshot->usage));
            writer.WriteString(snapshot->name);
            break;
        }
        case MaterialAssetReferenceRole::Material: {
            const material::MaterialGraphSnapshot* snapshot = desc.sources->FindMaterial(asset);
            if (snapshot == nullptr || snapshot->document == nullptr) {
                writer.WriteNone();
                break;
            }

            if (auto graph = WriteGraph(writer, *desc.catalog, *snapshot->document); !graph) {
                return Result<Sha256Digest>::Failure(graph.Error());
            }

            writer.WriteString(snapshot->name);
            break;
        }
        case MaterialAssetReferenceRole::ColorCurve: {
            const material::MaterialColorCurveSnapshot* curve = desc.sources->FindColorCurve(asset);
            if (curve == nullptr) {
                writer.WriteNone();
                break;
            }

            WriteCurve(writer, *curve);
            break;
        }
        case MaterialAssetReferenceRole::CurveAtlas: {
            const material::MaterialCurveAtlasSnapshot* atlas = desc.sources->FindCurveAtlas(asset);
            if (atlas == nullptr) {
                writer.WriteNone();
                break;
            }

            writer.WriteList(atlas->curves.Size());
            for (Guid row : atlas->curves) {
                writer.WriteGuid(row);
            }

            break;
        }
        case MaterialAssetReferenceRole::Collection: {
            const material::MaterialCollectionLayout* layout = desc.sources->FindCollection(asset);
            if (layout == nullptr) {
                writer.WriteNone();
                break;
            }

            writer.WriteU32(layout->vectors);
            writer.WriteList(layout->entries.Size());
            for (const material::MaterialCollectionEntry& entry : layout->entries) {
                writer.WriteGuid(entry.parameterId);
                writer.WriteU32(entry.vector);
                writer.WriteU32(entry.component);
            }

            break;
        }
        case MaterialAssetReferenceRole::Texture:
            break;
        }
    }

    writer.WriteNone();

    // 텍스처 참조 위치다.
    if (auto sites = WriteTextureSites(writer, desc); !sites) {
        return Result<Sha256Digest>::Failure(sites.Error());
    }

    return writer.Finish();
}

} // namespace rocks
