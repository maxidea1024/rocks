#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <Meta/TypeSchema.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/GraphSemantic.h>
#include <Serialization/Record.h>

// 머티리얼 그래프 도메인 schema다(Docs/Design/MaterialSystem 5절).
namespace rocks::material {

namespace {

bool SameFieldName(Span<const char> left, StringView right) noexcept {
    if (left.Size() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right.Data()[index]) {
            return false;
        }
    }

    return true;
}

// 의미에서 빼는 필드인가(정의의 nonSemanticFields를 그 스키마의 필드 ID로 본다).
bool IsNonSemanticField(const MaterialNodeDefinition& definition, const TypeSchema& schema,
                        Guid fieldId) noexcept {
    for (StringView name : definition.nonSemanticFields) {
        for (const FieldSchema& field : schema.fields) {
            if (field.id == fieldId && SameFieldName(field.name, name)) {
                return true;
            }
        }
    }

    return false;
}

bool IsTextureType(nodegraph::PinTypeId type) noexcept {
    return type < MaterialPinTypeCount &&
           ClassifyMaterialPinType(static_cast<MaterialPinType>(type)) ==
               MaterialPinTypeClass::Texture;
}

} // namespace

bool CanConnectMaterialPinTypes(nodegraph::PinTypeId from, nodegraph::PinTypeId to) noexcept {
    if (from == nodegraph::UnresolvedPinType || to == nodegraph::UnresolvedPinType) {
        return true;
    }

    if (from >= MaterialPinTypeCount || to >= MaterialPinTypeCount) {
        return false;
    }

    const MaterialPinTypeClass fromClass =
        ClassifyMaterialPinType(static_cast<MaterialPinType>(from));
    const MaterialPinTypeClass toClass = ClassifyMaterialPinType(static_cast<MaterialPinType>(to));
    switch (toClass) {
    case MaterialPinTypeClass::Numeric:
    case MaterialPinTypeClass::Static:
        // static 입력에 비static 값을 잇는 것은 번역의 V7이 거부한다(5.4절). 연결은 받는다.
        return fromClass == MaterialPinTypeClass::Numeric ||
               fromClass == MaterialPinTypeClass::Static;
    case MaterialPinTypeClass::TextureAny:
        return IsTextureType(from);
    case MaterialPinTypeClass::Texture:
    case MaterialPinTypeClass::Surface:
    case MaterialPinTypeClass::Attributes:
    case MaterialPinTypeClass::ShadingModel:
    case MaterialPinTypeClass::Pending:
        return from == to;
    }

    return false;
}

MaterialGraphSchema::MaterialGraphSchema(MaterialGraphKind kind, const MaterialNodeCatalog& catalog,
                                         NamePool& names, Allocator& scratch) noexcept
    : kind_(kind), catalog_(catalog), scratch_(scratch) {
    const StringView domainName =
        kind == MaterialGraphKind::Material ? MaterialDomainName : MaterialFunctionDomainName;
    auto domain = names.TryIntern(domainName);
    auto output = names.TryIntern(MaterialOutputNodeTypeName);
    if (domain && output) {
        domain_ = domain.Value();
        outputType_ = output.Value();
        ready_ = kind == MaterialGraphKind::Material || kind == MaterialGraphKind::Function;
    }
}

const nodegraph::PinTypeTable& MaterialGraphSchema::PinTypes() const noexcept {
    return MaterialPinTypes();
}

Result<void>
MaterialGraphSchema::NormalizeSemanticNode(const nodegraph::GraphNode& node,
                                           nodegraph::SemanticNodeWriter& writer) const noexcept {
    const MaterialNodeDefinition* definition = catalog_.Find(node.type);
    if (definition == nullptr || definition->nonSemanticFields.IsEmpty() ||
        definition->propertiesSchema == nullptr || node.properties.bytes.IsEmpty()) {
        // 모르는 노드·걸러 낼 필드가 없는 노드는 전체를 쓴다(모르는 노드는 바이트 보존 — GD14).
        return writer.WriteNode(node);
    }

    const Span<const uint8> bytes = node.properties.bytes.View();
    auto header = ReadRecordHeader(bytes);
    if (!header) {
        // 손상된 속성은 그대로 쓴다. 문서 검증(GI)이 따로 보고한다.
        return writer.WriteNode(node);
    }

    auto reader = RecordReader::Open(scratch_, bytes, header.Value().schema,
                                     header.Value().schemaVersion, header.Value().schemaVersion);
    if (!reader) {
        return writer.WriteNode(node);
    }

    const TypeSchema& schema = definition->propertiesSchema();
    RecordBuilder builder(scratch_, header.Value().schema, header.Value().schemaVersion);
    for (;;) {
        auto field = reader.Value().Next();
        if (!field) {
            if (field.Error() == ErrorCode::NotFound) {
                break;
            }

            return Result<void>::Failure(field.Error());
        }

        if (IsNonSemanticField(*definition, schema, field.Value().id)) {
            continue;
        }

        if (auto added =
                builder.AddField(field.Value().id, field.Value().wireType, field.Value().payload);
            !added) {
            return added;
        }
    }

    auto stripped = builder.Build();
    if (!stripped) {
        return Result<void>::Failure(stripped.Error());
    }

    return writer.WriteNode(node, stripped.Value().View());
}

nodegraph::ConnectVerdict
MaterialGraphSchema::CanConnect(const nodegraph::GraphReadView& view,
                                const nodegraph::ConnectQuery& query) const noexcept {
    nodegraph::ConnectVerdict verdict;
    if (query.graph == nullptr || query.fromPin == nullptr || query.toPin == nullptr) {
        verdict.reasonCode = static_cast<uint32>(MaterialConnectReason::TypeMismatch);
        return verdict;
    }

    if (query.fromPin->kind != query.toPin->kind) {
        verdict.reasonCode = static_cast<uint32>(MaterialConnectReason::KindMismatch);
        return verdict;
    }

    if (!CanConnectMaterialPinTypes(query.fromPin->type, query.toPin->type)) {
        verdict.reasonCode = static_cast<uint32>(MaterialConnectReason::TypeMismatch);
        return verdict;
    }

    auto cycle = nodegraph::WouldCreateCycle(*query.graph, query.from, query.to,
                                             view.Registry().GetAllocator());
    if (!cycle || cycle.Value()) {
        // 판정할 수 없으면(할당 실패) 잇지 않는다.
        verdict.reasonCode = static_cast<uint32>(MaterialConnectReason::Cycle);
        return verdict;
    }

    verdict.action = query.toEdgeCount >= query.toPin->maxEdges
                         ? nodegraph::ConnectAction::MakeReplacingInput
                         : nodegraph::ConnectAction::Make;
    return verdict;
}

Result<void>
MaterialGraphSchema::CreateDefaultNodes(nodegraph::GraphRole role, Guid graph,
                                        nodegraph::GraphIdSource& ids,
                                        nodegraph::GraphCommandList& out) const noexcept {
    if (kind_ != MaterialGraphKind::Material || role != nodegraph::GraphRole::Main) {
        return Result<void>::Success();
    }

    const MaterialNodeDefinition* output = catalog_.Find(outputType_);
    if (output == nullptr) {
        // 출력 노드 정의가 등록되지 않았다(엔진 노드 묶음 등록 전).
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto id = ids.Next();
    if (!id) {
        return Result<void>::Failure(id.Error());
    }

    nodegraph::GraphNode node(scratch_);
    node.id = id.Value();
    node.type = outputType_;
    node.typeSchemaVersion = output->schemaVersion;
    nodegraph::NodeLayout layout(scratch_);
    layout.node = node.id;
    return out.AddNode(graph, node, layout);
}

} // namespace rocks::material
