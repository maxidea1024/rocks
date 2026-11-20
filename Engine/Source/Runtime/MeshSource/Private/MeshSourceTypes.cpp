#include "MeshSourceInternal.h"

#include <Platform/Memory.h>

namespace rocks::mesh_source_detail {

namespace {

// ASCII 대소문자를 무시하고 text가 prefix로 시작하는지다(비ASCII byte는 그대로 비교 — Name 규칙과
// 같다).
bool StartsWithAsciiCaseless(StringView text, StringView prefix) noexcept {
    if (text.ByteCount() < prefix.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < prefix.ByteCount(); ++index) {
        char left = text.Data()[index];
        char right = prefix.Data()[index];
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left - 'A' + 'a');
        }

        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right - 'A' + 'a');
        }

        if (left != right) {
            return false;
        }
    }

    return true;
}

// 접두 이름(prefix + 이름)이면 kind, 뒤가 비면 Malformed, 접두가 아니면 None이다.
EngineName ClassifyPrefixed(StringView name, StringView prefix, EngineName kind) noexcept {
    if (!StartsWithAsciiCaseless(name, prefix)) {
        return EngineName::None;
    }

    return name.ByteCount() == prefix.ByteCount() ? EngineName::Malformed : kind;
}

} // namespace

EngineName ClassifyAttributeName(StringView name) noexcept {
    struct Exact {
        StringView text;
        EngineName kind;
    };

    const Exact exact[] = {
        {MeshAttributeName::Position, EngineName::Position},
        {MeshAttributeName::Normal, EngineName::Normal},
        {MeshAttributeName::Tangent, EngineName::Tangent},
        {MeshAttributeName::BinormalSign, EngineName::BinormalSign},
        {MeshAttributeName::Color, EngineName::Color},
        {MeshAttributeName::TexCoord, EngineName::TexCoord},
        {MeshAttributeName::IsHard, EngineName::IsHard},
        {MeshAttributeName::PartName, EngineName::PartName},
        {MeshAttributeName::MaterialSlotName, EngineName::MaterialSlotName},
        {MeshAttributeName::SkinWeights, EngineName::SkinWeights},
    };
    for (const Exact& entry : exact) {
        if (NameTextEquals(name, entry.text)) {
            return entry.kind;
        }
    }

    if (EngineName kind =
            ClassifyPrefixed(name, MeshAttributeName::SkinWeightsPrefix, EngineName::SkinWeights);
        kind != EngineName::None) {
        return kind;
    }

    if (EngineName kind =
            ClassifyPrefixed(name, MeshAttributeName::MorphNormalPrefix, EngineName::MorphNormal);
        kind != EngineName::None) {
        return kind;
    }

    return ClassifyPrefixed(name, MeshAttributeName::MorphPrefix, EngineName::Morph);
}

EngineRule EngineRuleFor(EngineName kind) noexcept {
    switch (kind) {
    case EngineName::Position:
        return {MeshElement::Vertex, MeshValueType::Vec3, 1};
    case EngineName::Normal:
    case EngineName::Tangent:
        return {MeshElement::Corner, MeshValueType::Vec3, 1};
    case EngineName::BinormalSign:
        return {MeshElement::Corner, MeshValueType::Float, 1};
    case EngineName::Color:
        return {MeshElement::Corner, MeshValueType::Vec4, 1};
    case EngineName::TexCoord:
        return {MeshElement::Corner, MeshValueType::Vec2, static_cast<uint8>(MeshMaxTexCoords)};
    case EngineName::IsHard:
        return {MeshElement::Edge, MeshValueType::Bool, 1};
    case EngineName::PartName:
        return {MeshElement::Polygon, MeshValueType::Name, 1};
    case EngineName::MaterialSlotName:
        return {MeshElement::PolygonGroup, MeshValueType::Name, 1};
    case EngineName::SkinWeights:
        return {MeshElement::Vertex, MeshValueType::BoneInfluences, 1};
    case EngineName::Morph:
        return {MeshElement::Vertex, MeshValueType::SparseVec3, 1};
    case EngineName::MorphNormal:
        return {MeshElement::Corner, MeshValueType::SparseVec3, 1};
    case EngineName::None:
    case EngineName::Malformed:
        break;
    }

    return {};
}

bool IsValidElement(MeshElement element) noexcept {
    const uint32 value = static_cast<uint32>(element);
    return value >= static_cast<uint32>(MeshElement::Vertex) &&
           value <= static_cast<uint32>(MeshElement::PolygonGroup);
}

bool IsValidValueType(MeshValueType type) noexcept {
    const uint32 value = static_cast<uint32>(type);
    return value >= static_cast<uint32>(MeshValueType::Float) &&
           value <= static_cast<uint32>(MeshValueType::SparseVec3);
}

uint32 FloatComponents(MeshValueType type) noexcept {
    switch (type) {
    case MeshValueType::Float:
        return 1;
    case MeshValueType::Vec2:
        return 2;
    case MeshValueType::Vec3:
        return 3;
    case MeshValueType::Vec4:
        return 4;
    default:
        return 0;
    }
}

bool IsSingleChannelType(MeshValueType type) noexcept {
    return type == MeshValueType::BoneInfluences || type == MeshValueType::SparseVec3;
}

bool IsPositiveZero(const Vec3& value) noexcept {
    return platform::BitCast<uint32>(value.x) == 0 && platform::BitCast<uint32>(value.y) == 0 &&
           platform::BitCast<uint32>(value.z) == 0;
}

Result<void> Fail(MeshDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                  MeshIssue issue, MeshElement element, uint32 attribute, uint32 index) noexcept {
    if (failure != nullptr) {
        failure->code = code;
        failure->error = error;
        failure->issue = issue;
        failure->element = element;
        failure->attribute = attribute;
        failure->index = index;
        failure->count = 0;
    }

    return Result<void>::Failure(error);
}

} // namespace rocks::mesh_source_detail

namespace rocks {

const char* MeshIssueName(MeshIssue issue) noexcept {
    switch (issue) {
    case MeshIssue::None:
        return "None";
    case MeshIssue::FormatVersion:
        return "FormatVersion";
    case MeshIssue::ElementCountTooLarge:
        return "ElementCountTooLarge";
    case MeshIssue::BulkUnreadable:
        return "BulkUnreadable";
    case MeshIssue::TopologyLength:
        return "TopologyLength";
    case MeshIssue::PolygonStarts:
        return "PolygonStarts";
    case MeshIssue::AttributeRecordKind:
        return "AttributeRecordKind";
    case MeshIssue::AttributeRecordName:
        return "AttributeRecordName";
    case MeshIssue::AttributeOrder:
        return "AttributeOrder";
    case MeshIssue::MissingPosition:
        return "MissingPosition";
    case MeshIssue::AttributeLength:
        return "AttributeLength";
    case MeshIssue::NameValueLength:
        return "NameValueLength";
    case MeshIssue::InfluenceStarts:
        return "InfluenceStarts";
    case MeshIssue::SparseIndexOrder:
        return "SparseIndexOrder";
    case MeshIssue::SparseIndexRange:
        return "SparseIndexRange";
    case MeshIssue::SparseZeroEntry:
        return "SparseZeroEntry";
    case MeshIssue::NonFinitePosition:
        return "NonFinitePosition";
    case MeshIssue::CornerVertexRange:
        return "CornerVertexRange";
    case MeshIssue::EdgeVertexRange:
        return "EdgeVertexRange";
    case MeshIssue::EdgeSameVertex:
        return "EdgeSameVertex";
    case MeshIssue::EdgeOrder:
        return "EdgeOrder";
    case MeshIssue::DuplicateEdge:
        return "DuplicateEdge";
    case MeshIssue::PolygonTooFewCorners:
        return "PolygonTooFewCorners";
    case MeshIssue::PolygonCornerRange:
        return "PolygonCornerRange";
    case MeshIssue::PolygonGroupRange:
        return "PolygonGroupRange";
    case MeshIssue::PerimeterEdgeMissing:
        return "PerimeterEdgeMissing";
    case MeshIssue::TriangleCornerRange:
        return "TriangleCornerRange";
    case MeshIssue::TrianglePolygonRange:
        return "TrianglePolygonRange";
    case MeshIssue::TriangleCountMismatch:
        return "TriangleCountMismatch";
    case MeshIssue::TriangleCornerNotOnPerimeter:
        return "TriangleCornerNotOnPerimeter";
    case MeshIssue::DuplicateAttributeName:
        return "DuplicateAttributeName";
    case MeshIssue::EngineAttributeMismatch:
        return "EngineAttributeMismatch";
    case MeshIssue::BoolValue:
        return "BoolValue";
    case MeshIssue::NameValue:
        return "NameValue";
    case MeshIssue::TooManyInfluences:
        return "TooManyInfluences";
    case MeshIssue::DuplicateInfluenceBone:
        return "DuplicateInfluenceBone";
    case MeshIssue::BoneIndexRange:
        return "BoneIndexRange";
    case MeshIssue::ZeroWeightSum:
        return "ZeroWeightSum";
    case MeshIssue::NonFiniteTexCoord:
        return "NonFiniteTexCoord";
    case MeshIssue::NonFiniteColor:
        return "NonFiniteColor";
    case MeshIssue::UvOutOfRange:
        return "UvOutOfRange";
    case MeshIssue::NonNormalizedWeights:
        return "NonNormalizedWeights";
    }

    return "Unknown";
}

AssetDiagnostic ToAssetDiagnostic(const MeshDiagnostic& diagnostic, AssetId asset) noexcept {
    AssetDiagnostic out;
    out.code = diagnostic.code;
    out.error = diagnostic.error;
    out.asset = asset;
    out.section = 0;
    out.detail = diagnostic.index != MeshNoIndex ? diagnostic.index : diagnostic.count;
    return out;
}

} // namespace rocks
