#include "MeshSourceInternal.h"

#include <Core/Assert.h>
#include <Object/NameField.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

// 엔진 Color의 새 요소 기본 성분이다(UE StaticMeshAttributes의 Color 기본값 흰색, 3.7절 8).
constexpr float MeshDefaultColorComponent = 1.0f;

// 속성 값 저장소의 offset·개수 칸(uint32)이 담을 수 있는 최대 원소 수다.
constexpr usize MeshMaxPoolElements = NumericLimits<uint32>::Max();

// element 요소 하나를 더할 때 channel 하나에 필요한 자리를 잡는다.
Result<void> ReserveChannelSlot(MeshSource::Channel& channel, MeshValueType type) noexcept {
    const uint32 components = rocks::mesh_source_detail::FloatComponents(type);
    if (components != 0) {
        return channel.floats.TryEnsureCapacity(channel.floats.Size() + components);
    }

    switch (type) {
    case MeshValueType::UInt32:
        return channel.words.TryEnsureCapacity(channel.words.Size() + 1);
    case MeshValueType::Bool:
        return channel.bytes.TryEnsureCapacity(channel.bytes.Size() + 1);
    case MeshValueType::Name:
    case MeshValueType::BoneInfluences:
        return channel.ranges.TryEnsureCapacity(channel.ranges.Size() + 1);
    default:
        // SparseVec3은 0 값을 담지 않아 자리가 필요 없다.
        return Result<void>::Success();
    }
}

// 잡아 둔 자리에 기본값 하나를 넣는다(용량 안이라 실패하지 않는다).
void AppendChannelDefault(MeshSource::Channel& channel, MeshValueType type,
                          bool engineColor) noexcept {
    const uint32 components = rocks::mesh_source_detail::FloatComponents(type);
    const float initial = engineColor ? MeshDefaultColorComponent : 0.0f;
    for (uint32 component = 0; component < components; ++component) {
        ROCKS_ASSERT(channel.floats.TryPushBack(initial).IsOk());
    }

    switch (type) {
    case MeshValueType::UInt32:
        ROCKS_ASSERT(channel.words.TryPushBack(0u).IsOk());
        break;
    case MeshValueType::Bool:
        ROCKS_ASSERT(channel.bytes.TryPushBack(static_cast<uint8>(0)).IsOk());
        break;
    case MeshValueType::Name:
    case MeshValueType::BoneInfluences:
        ROCKS_ASSERT(channel.ranges.TryPushBack(MeshSource::ValueRange{}).IsOk());
        break;
    default:
        break;
    }
}

// channel을 count개 요소의 기본값으로 채운다(새 속성).
Result<void> FillChannelDefaults(MeshSource::Channel& channel, MeshValueType type, bool engineColor,
                                 uint32 count) noexcept {
    const uint32 components = rocks::mesh_source_detail::FloatComponents(type);
    if (components != 0) {
        if (auto resized = channel.floats.TryResize(static_cast<usize>(count) * components);
            !resized) {
            return resized;
        }

        if (engineColor) {
            for (float& value : channel.floats) {
                value = MeshDefaultColorComponent;
            }
        }

        return Result<void>::Success();
    }

    switch (type) {
    case MeshValueType::UInt32:
        return channel.words.TryResize(count);
    case MeshValueType::Bool:
        return channel.bytes.TryResize(count);
    case MeshValueType::Name:
    case MeshValueType::BoneInfluences:
        return channel.ranges.TryResize(count);
    default:
        return Result<void>::Success();
    }
}

} // namespace

MeshSource::MeshSource(Allocator& allocator) noexcept
    : allocator_(&allocator), positions_(allocator), cornerVertices_(allocator), edges_(allocator),
      polygonEnds_(allocator), polygonCorners_(allocator), polygonGroups_(allocator),
      triangleCorners_(allocator), trianglePolygons_(allocator), attributes_(allocator) {
}

uint32 MeshSource::Count(MeshElement element) const noexcept {
    switch (element) {
    case MeshElement::Vertex:
        return vertexCount_;
    case MeshElement::Corner:
        return cornerCount_;
    case MeshElement::Edge:
        return edgeCount_;
    case MeshElement::Polygon:
        return polygonCount_;
    case MeshElement::Triangle:
        return triangleCount_;
    case MeshElement::PolygonGroup:
        return polygonGroupCount_;
    case MeshElement::Invalid:
        break;
    }

    return 0;
}

Result<void> MeshSource::ReserveAttributeSlot(MeshElement element) noexcept {
    for (Attribute& attribute : attributes_) {
        if (attribute.element != element) {
            continue;
        }

        for (Channel& channel : attribute.channels) {
            if (auto reserved = ReserveChannelSlot(channel, attribute.type); !reserved) {
                return reserved;
            }
        }
    }

    return Result<void>::Success();
}

void MeshSource::AppendAttributeDefaults(MeshElement element) noexcept {
    for (Attribute& attribute : attributes_) {
        if (attribute.element != element) {
            continue;
        }

        for (Channel& channel : attribute.channels) {
            AppendChannelDefault(channel, attribute.type, attribute.engineColor);
        }
    }
}

Result<uint32> MeshSource::AddVertex(const Vec3& position) noexcept {
    if (vertexCount_ >= MeshMaxElements) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto reserved = positions_.TryEnsureCapacity(positions_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = ReserveAttributeSlot(MeshElement::Vertex); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    ROCKS_ASSERT(positions_.TryPushBack(position).IsOk());
    AppendAttributeDefaults(MeshElement::Vertex);
    return Result<uint32>::Success(vertexCount_++);
}

Result<uint32> MeshSource::AddCorner(uint32 vertex) noexcept {
    if (cornerCount_ >= MeshMaxElements) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto reserved = cornerVertices_.TryEnsureCapacity(cornerVertices_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = ReserveAttributeSlot(MeshElement::Corner); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    ROCKS_ASSERT(cornerVertices_.TryPushBack(vertex).IsOk());
    AppendAttributeDefaults(MeshElement::Corner);
    return Result<uint32>::Success(cornerCount_++);
}

Result<uint32> MeshSource::AddEdge(uint32 vertexA, uint32 vertexB) noexcept {
    if (edgeCount_ >= MeshMaxElements) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto reserved = edges_.TryEnsureCapacity(edges_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = ReserveAttributeSlot(MeshElement::Edge); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    MeshEdgeVertices edge;
    edge.first = vertexA < vertexB ? vertexA : vertexB;
    edge.second = vertexA < vertexB ? vertexB : vertexA;
    ROCKS_ASSERT(edges_.TryPushBack(edge).IsOk());
    AppendAttributeDefaults(MeshElement::Edge);
    return Result<uint32>::Success(edgeCount_++);
}

Result<uint32> MeshSource::AddPolygonGroup() noexcept {
    if (polygonGroupCount_ >= MeshMaxPolygonGroups) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto reserved = ReserveAttributeSlot(MeshElement::PolygonGroup); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    AppendAttributeDefaults(MeshElement::PolygonGroup);
    return Result<uint32>::Success(polygonGroupCount_++);
}

Result<uint32> MeshSource::AddPolygon(Span<const uint32> corners, uint32 group) noexcept {
    if (polygonCount_ >= MeshMaxElements ||
        corners.Size() > static_cast<usize>(MeshMaxElements) - polygonCorners_.Size()) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto reserved = polygonEnds_.TryEnsureCapacity(polygonEnds_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = polygonGroups_.TryEnsureCapacity(polygonGroups_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = polygonCorners_.TryEnsureCapacity(polygonCorners_.Size() + corners.Size());
        !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = ReserveAttributeSlot(MeshElement::Polygon); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    ROCKS_ASSERT(polygonCorners_.TryAppend(corners).IsOk());
    ROCKS_ASSERT(polygonEnds_.TryPushBack(static_cast<uint32>(polygonCorners_.Size())).IsOk());
    ROCKS_ASSERT(polygonGroups_.TryPushBack(group).IsOk());
    AppendAttributeDefaults(MeshElement::Polygon);
    return Result<uint32>::Success(polygonCount_++);
}

Result<uint32> MeshSource::AddTriangle(uint32 corner0, uint32 corner1, uint32 corner2,
                                       uint32 polygon) noexcept {
    if (triangleCount_ >= MeshMaxElements) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto reserved = triangleCorners_.TryEnsureCapacity(triangleCorners_.Size() + 1);
        !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = trianglePolygons_.TryEnsureCapacity(trianglePolygons_.Size() + 1);
        !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = ReserveAttributeSlot(MeshElement::Triangle); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    MeshTriangleCorners triangle;
    triangle.corners[0] = corner0;
    triangle.corners[1] = corner1;
    triangle.corners[2] = corner2;
    ROCKS_ASSERT(triangleCorners_.TryPushBack(triangle).IsOk());
    ROCKS_ASSERT(trianglePolygons_.TryPushBack(polygon).IsOk());
    AppendAttributeDefaults(MeshElement::Triangle);
    return Result<uint32>::Success(triangleCount_++);
}

Span<const uint32> MeshSource::PolygonCorners(uint32 polygon) const noexcept {
    ROCKS_ASSERT(polygon < polygonCount_);
    const uint32 begin = polygon == 0 ? 0 : polygonEnds_[polygon - 1];
    const uint32 end = polygonEnds_[polygon];
    return Span<const uint32>(polygonCorners_.Data() + begin, end - begin);
}

MeshAttributeInfo MeshSource::AttributeInfo(uint32 attribute) const noexcept {
    ROCKS_ASSERT(attribute < attributes_.Size());
    const Attribute& entry = attributes_[attribute];
    MeshAttributeInfo info;
    info.element = entry.element;
    info.type = entry.type;
    info.channels = static_cast<uint8>(entry.channels.Size());
    info.name = entry.name.View();
    return info;
}

uint32 MeshSource::FindAttribute(MeshElement element, StringView name) const noexcept {
    for (usize index = 0; index < attributes_.Size(); ++index) {
        if (attributes_[index].element == element &&
            NameTextEquals(attributes_[index].name.View(), name)) {
            return static_cast<uint32>(index);
        }
    }

    return MeshNoAttribute;
}

Result<uint32> MeshSource::AddAttribute(MeshElement element, StringView name, MeshValueType type,
                                        uint8 channels) noexcept {
    if (!rocks::mesh_source_detail::IsValidElement(element) ||
        !rocks::mesh_source_detail::IsValidValueType(type) || channels == 0 ||
        (rocks::mesh_source_detail::IsSingleChannelType(type) && channels != 1)) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto valid = ValidateNameFieldText(name, *allocator_); !valid) {
        return Result<uint32>::Failure(valid.Error());
    }

    if (IsNoneNameText(name)) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 정점 위치는 속성 목록 밖에 있지만 같은 이름 공간을 쓴다(3.7절 1).
    const bool position =
        element == MeshElement::Vertex && rocks::mesh_source_detail::ClassifyAttributeName(name) ==
                                              rocks::mesh_source_detail::EngineName::Position;
    if (position || FindAttribute(element, name) != MeshNoAttribute) {
        return Result<uint32>::Failure(ErrorCode::AlreadyExists);
    }

    Attribute attribute(*allocator_);
    attribute.element = element;
    attribute.type = type;
    attribute.engineColor = element == MeshElement::Corner && type == MeshValueType::Vec4 &&
                            rocks::mesh_source_detail::ClassifyAttributeName(name) ==
                                rocks::mesh_source_detail::EngineName::Color;
    if (auto assigned = attribute.name.TryAssign(name); !assigned) {
        return Result<uint32>::Failure(assigned.Error());
    }

    if (auto reserved = attribute.channels.TryReserve(channels); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    const uint32 count = Count(element);
    for (uint32 index = 0; index < channels; ++index) {
        ROCKS_ASSERT(attribute.channels.TryEmplaceBack(*allocator_).IsOk());
        if (auto filled =
                FillChannelDefaults(attribute.channels[index], type, attribute.engineColor, count);
            !filled) {
            return Result<uint32>::Failure(filled.Error());
        }
    }

    if (auto pushed = attributes_.TryPushBack(Move(attribute)); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(attributes_.Size() - 1));
}

const MeshSource::Attribute& MeshSource::CheckedAttribute(uint32 attribute,
                                                          MeshValueType type) const noexcept {
    ROCKS_ASSERT(attribute < attributes_.Size());
    const Attribute& entry = attributes_[attribute];
    // Invalid는 "float 성분 타입 아무것이나"다(FloatValues).
    if (type == MeshValueType::Invalid) {
        ROCKS_ASSERT(rocks::mesh_source_detail::FloatComponents(entry.type) != 0);
    } else {
        ROCKS_ASSERT(entry.type == type);
    }

    return entry;
}

const MeshSource::Channel& MeshSource::CheckedChannel(uint32 attribute, uint32 channel,
                                                      MeshValueType type) const noexcept {
    const Attribute& entry = CheckedAttribute(attribute, type);
    ROCKS_ASSERT(channel < entry.channels.Size());
    return entry.channels[channel];
}

MeshSource::Channel& MeshSource::MutableChannel(uint32 attribute, uint32 channel,
                                                MeshValueType type) noexcept {
    return const_cast<Channel&>(CheckedChannel(attribute, channel, type));
}

Span<float> MeshSource::FloatValues(uint32 attribute, uint32 channel) noexcept {
    return MutableChannel(attribute, channel, MeshValueType::Invalid).floats.View();
}

Span<const float> MeshSource::FloatValues(uint32 attribute, uint32 channel) const noexcept {
    return CheckedChannel(attribute, channel, MeshValueType::Invalid).floats.View();
}

Span<uint32> MeshSource::UInt32Values(uint32 attribute, uint32 channel) noexcept {
    return MutableChannel(attribute, channel, MeshValueType::UInt32).words.View();
}

Span<const uint32> MeshSource::UInt32Values(uint32 attribute, uint32 channel) const noexcept {
    return CheckedChannel(attribute, channel, MeshValueType::UInt32).words.View();
}

Span<uint8> MeshSource::BoolValues(uint32 attribute, uint32 channel) noexcept {
    return MutableChannel(attribute, channel, MeshValueType::Bool).bytes.View();
}

Span<const uint8> MeshSource::BoolValues(uint32 attribute, uint32 channel) const noexcept {
    return CheckedChannel(attribute, channel, MeshValueType::Bool).bytes.View();
}

float MeshSource::GetFloat(uint32 attribute, uint32 channel, uint32 index) const noexcept {
    const Channel& values = CheckedChannel(attribute, channel, MeshValueType::Float);
    ROCKS_ASSERT(index < values.floats.Size());
    return values.floats[index];
}

void MeshSource::SetFloat(uint32 attribute, uint32 channel, uint32 index, float value) noexcept {
    Channel& values = MutableChannel(attribute, channel, MeshValueType::Float);
    ROCKS_ASSERT(index < values.floats.Size());
    values.floats[index] = value;
}

Vec2 MeshSource::GetVec2(uint32 attribute, uint32 channel, uint32 index) const noexcept {
    const Channel& values = CheckedChannel(attribute, channel, MeshValueType::Vec2);
    const usize at = static_cast<usize>(index) * 2;
    ROCKS_ASSERT(at < values.floats.Size());
    return Vec2(values.floats[at], values.floats[at + 1]);
}

void MeshSource::SetVec2(uint32 attribute, uint32 channel, uint32 index,
                         const Vec2& value) noexcept {
    Channel& values = MutableChannel(attribute, channel, MeshValueType::Vec2);
    const usize at = static_cast<usize>(index) * 2;
    ROCKS_ASSERT(at < values.floats.Size());
    values.floats[at] = value.x;
    values.floats[at + 1] = value.y;
}

Vec3 MeshSource::GetVec3(uint32 attribute, uint32 channel, uint32 index) const noexcept {
    const Channel& values = CheckedChannel(attribute, channel, MeshValueType::Vec3);
    const usize at = static_cast<usize>(index) * 3;
    ROCKS_ASSERT(at < values.floats.Size());
    return Vec3(values.floats[at], values.floats[at + 1], values.floats[at + 2]);
}

void MeshSource::SetVec3(uint32 attribute, uint32 channel, uint32 index,
                         const Vec3& value) noexcept {
    Channel& values = MutableChannel(attribute, channel, MeshValueType::Vec3);
    const usize at = static_cast<usize>(index) * 3;
    ROCKS_ASSERT(at < values.floats.Size());
    values.floats[at] = value.x;
    values.floats[at + 1] = value.y;
    values.floats[at + 2] = value.z;
}

Vec4 MeshSource::GetVec4(uint32 attribute, uint32 channel, uint32 index) const noexcept {
    const Channel& values = CheckedChannel(attribute, channel, MeshValueType::Vec4);
    const usize at = static_cast<usize>(index) * 4;
    ROCKS_ASSERT(at < values.floats.Size());
    return Vec4(values.floats[at], values.floats[at + 1], values.floats[at + 2],
                values.floats[at + 3]);
}

void MeshSource::SetVec4(uint32 attribute, uint32 channel, uint32 index,
                         const Vec4& value) noexcept {
    Channel& values = MutableChannel(attribute, channel, MeshValueType::Vec4);
    const usize at = static_cast<usize>(index) * 4;
    ROCKS_ASSERT(at < values.floats.Size());
    values.floats[at] = value.x;
    values.floats[at + 1] = value.y;
    values.floats[at + 2] = value.z;
    values.floats[at + 3] = value.w;
}

StringView MeshSource::NameValue(uint32 attribute, uint32 channel, uint32 index) const noexcept {
    const Channel& values = CheckedChannel(attribute, channel, MeshValueType::Name);
    ROCKS_ASSERT(index < values.ranges.Size());
    const ValueRange range = values.ranges[index];
    if (range.count == 0) {
        return StringView();
    }

    return StringView(reinterpret_cast<const char*>(values.bytes.Data() + range.offset),
                      range.count);
}

Result<void> MeshSource::SetNameValue(uint32 attribute, uint32 channel, uint32 index,
                                      StringView value) noexcept {
    Channel& values = MutableChannel(attribute, channel, MeshValueType::Name);
    ROCKS_ASSERT(index < values.ranges.Size());
    ValueRange& range = values.ranges[index];
    const usize bytes = value.ByteCount();
    if (bytes <= range.count) {
        // 제자리(값이 같은 저장소를 가리킬 수 있어 겹침을 허용하는 이동).
        if (bytes != 0) {
            platform::MemoryMove(values.bytes.Data() + range.offset, value.Data(), bytes);
        }

        range.count = static_cast<uint32>(bytes);
        return Result<void>::Success();
    }

    if (bytes > MeshMaxPoolElements - values.bytes.Size()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // value가 이 저장소를 가리키면 늘리는 재할당이 그 byte를 옮기므로 위치로 기억해 둔다.
    const uint8* source = reinterpret_cast<const uint8*>(value.Data());
    const uint8* poolBegin = values.bytes.Data();
    const bool aliased =
        rocks::mesh_source_detail::PointsInto(source, poolBegin, values.bytes.Size());
    const usize aliasOffset = aliased ? static_cast<usize>(source - poolBegin) : 0;
    const usize offset = values.bytes.Size();
    if (auto grown = values.bytes.TryAppendUninitialized(bytes); !grown) {
        return Result<void>::Failure(grown.Error());
    }

    const uint8* from = aliased ? values.bytes.Data() + aliasOffset : source;
    platform::MemoryCopy(values.bytes.Data() + offset, from, bytes);
    range.offset = static_cast<uint32>(offset);
    range.count = static_cast<uint32>(bytes);
    return Result<void>::Success();
}

Span<const BoneInfluence> MeshSource::Influences(uint32 attribute, uint32 vertex) const noexcept {
    const Channel& values = CheckedChannel(attribute, 0, MeshValueType::BoneInfluences);
    ROCKS_ASSERT(vertex < values.ranges.Size());
    const ValueRange range = values.ranges[vertex];
    return Span<const BoneInfluence>(values.influences.Data() + range.offset, range.count);
}

Result<void> MeshSource::SetInfluences(uint32 attribute, uint32 vertex,
                                       Span<const BoneInfluence> influences) noexcept {
    Channel& values = MutableChannel(attribute, 0, MeshValueType::BoneInfluences);
    ROCKS_ASSERT(vertex < values.ranges.Size());
    ValueRange& range = values.ranges[vertex];
    const usize count = influences.Size();
    if (count <= range.count) {
        if (count != 0) {
            platform::MemoryMove(values.influences.Data() + range.offset, influences.Data(),
                                 count * sizeof(BoneInfluence));
        }

        range.count = static_cast<uint32>(count);
        return Result<void>::Success();
    }

    if (count > MeshMaxPoolElements - values.influences.Size()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const BoneInfluence* source = influences.Data();
    const BoneInfluence* poolBegin = values.influences.Data();
    const bool aliased = rocks::mesh_source_detail::PointsInto(
        source, poolBegin, values.influences.Size() * sizeof(BoneInfluence));
    const usize aliasOffset = aliased ? static_cast<usize>(source - poolBegin) : 0;
    const usize offset = values.influences.Size();
    if (auto grown = values.influences.TryResize(offset + count); !grown) {
        return grown;
    }

    const BoneInfluence* from = aliased ? values.influences.Data() + aliasOffset : source;
    platform::MemoryCopy(values.influences.Data() + offset, from, count * sizeof(BoneInfluence));
    range.offset = static_cast<uint32>(offset);
    range.count = static_cast<uint32>(count);
    return Result<void>::Success();
}

Span<const MeshSparseVec3> MeshSource::SparseValues(uint32 attribute) const noexcept {
    return CheckedChannel(attribute, 0, MeshValueType::SparseVec3).sparse.View();
}

Vec3 MeshSource::SparseValue(uint32 attribute, uint32 index) const noexcept {
    const Channel& values = CheckedChannel(attribute, 0, MeshValueType::SparseVec3);
    const Span<const MeshSparseVec3> entries = values.sparse.View();
    const usize at =
        algo::LowerBound(entries, index, [](const MeshSparseVec3& entry, uint32 key) noexcept {
            return entry.index < key;
        });
    if (at < entries.Size() && entries[at].index == index) {
        return entries[at].value;
    }

    return Vec3();
}

Result<void> MeshSource::SetSparseValue(uint32 attribute, uint32 index,
                                        const Vec3& value) noexcept {
    Channel& values = MutableChannel(attribute, 0, MeshValueType::SparseVec3);
    ROCKS_ASSERT(index < Count(attributes_[attribute].element));
    const usize at = algo::LowerBound(
        values.sparse.View(), index,
        [](const MeshSparseVec3& entry, uint32 key) noexcept { return entry.index < key; });
    const bool present = at < values.sparse.Size() && values.sparse[at].index == index;
    const bool zero = rocks::mesh_source_detail::IsPositiveZero(value);
    if (present) {
        if (zero) {
            ROCKS_ASSERT(values.sparse.TryRemoveAt(at).IsOk());
        } else {
            values.sparse[at].value = value;
        }

        return Result<void>::Success();
    }

    if (zero) {
        return Result<void>::Success();
    }

    return values.sparse.TryInsert(MeshSparseVec3{index, value}, at);
}

} // namespace rocks
