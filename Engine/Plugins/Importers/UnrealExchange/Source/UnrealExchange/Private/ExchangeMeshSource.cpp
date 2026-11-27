#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Unicode.h>
#include <MeshSource/MeshSourceData.h>
#include <UnrealExchange/ExchangeMeshSource.h>

#include "ExchangeJsonRead.h"

// 교환 MeshSource 기록 적재다(Docs/Design/UnrealExchange 5.5절 1번).

namespace rocks {

namespace exchange_mesh_source {

using exchange_detail::Member;
using exchange_detail::ReadString;
using exchange_detail::ReadUInt;

// 위상 배열 수와 배열마다의 components다(MeshAnimationAssets 9.1절, UnrealExchange 5.4절 2번).
constexpr usize TopologyArrays = 7;
constexpr uint64 TopologyComponents[TopologyArrays] = {1, 2, 1, 1, 1, 3, 1};
// 원소 수를 counts로 정할 수 없는 위상 배열(둘레 길이 합)의 표지다.
constexpr uint64 TopologyCountUnchecked = NumericLimits<uint64>::Max();

// 기록 요소·값 타입 이름이다(번호 + 1 = MeshElement·MeshValueType 값 — 5.4절 2번).
constexpr StringView ElementNames[] = {"Vertex",  "Corner",   "Edge",
                                       "Polygon", "Triangle", "PolygonGroup"};
constexpr StringView CountKeys[] = {"vertices", "corners",   "edges",
                                    "polygons", "triangles", "polygonGroups"};
constexpr usize ElementKinds = sizeof(ElementNames) / sizeof(ElementNames[0]);

struct ValueTypeEntry {
    StringView name;
    ExchangeBlobLayout layout;
    // 채널 하나의 components다(가변 길이 배치는 1 — components = 채널).
    uint32 components;
};

constexpr ValueTypeEntry ValueTypes[] = {
    {"Float", ExchangeBlobLayout::F32, 1},
    {"Vec2", ExchangeBlobLayout::F32, 2},
    {"Vec3", ExchangeBlobLayout::F32, 3},
    {"Vec4", ExchangeBlobLayout::F32, 4},
    {"UInt32", ExchangeBlobLayout::U32, 1},
    {"Bool", ExchangeBlobLayout::U8, 1},
    {"Name", ExchangeBlobLayout::Names, 1},
    {"BoneInfluences", ExchangeBlobLayout::BoneInfluences, 1},
    {"SparseVec3", ExchangeBlobLayout::SparseVec3, 1},
};

constexpr usize ValueTypeCount = sizeof(ValueTypes) / sizeof(ValueTypes[0]);
// Name 값 길이 word의 byte 수다(MeshAnimationAssets 3.4.1절).
constexpr usize NameLengthBytes = sizeof(uint32);
constexpr uint32 ByteBits = 8;

Result<void> Corrupt(ExchangeMeshFailure& failure) noexcept {
    failure.code = AssetDiagnosticCode::CorruptBundle;
    return Result<void>::Failure(ErrorCode::CorruptData);
}

usize FindName(Span<const StringView> names, StringView name) noexcept {
    for (usize index = 0; index < names.Size(); ++index) {
        if (names[index] == name) {
            return index;
        }
    }

    return names.Size();
}

// 기록 안 blob 참조 객체({blob, bytes, layout, components, count})가 가리키는 asset.blobs 번호다.
// 없으면 blobs 수다.
usize FindBlob(const ExchangeAsset& asset, JsonValue reference) noexcept {
    StringView hex;
    StringView layout;
    uint64 count = 0;
    uint64 components = 0;
    if (!ReadString(Member(reference, "blob"), hex) ||
        !ReadString(Member(reference, "layout"), layout) ||
        !ReadUInt(Member(reference, "count"), count) ||
        !ReadUInt(Member(reference, "components"), components)) {
        return asset.blobs.Size();
    }

    auto digest = Sha256Digest::ParseHex(hex);
    if (!digest) {
        return asset.blobs.Size();
    }

    for (usize index = 0; index < asset.blobs.Size(); ++index) {
        const ExchangeBlobRef& blob = asset.blobs[index];
        if (blob.digest == digest.Value() && blob.count == count && blob.components == components &&
            StringView(ExchangeBlobLayoutName(blob.layout)) == layout) {
            return index;
        }
    }

    return asset.blobs.Size();
}

uint32 ReadWord(const uint8* bytes) noexcept {
    uint32 value = 0;
    for (usize index = 0; index < NameLengthBytes; ++index) {
        value |= static_cast<uint32>(bytes[index]) << (index * ByteBits);
    }

    return value;
}

Result<void> AppendWord(Array<uint8>& out, uint32 value) noexcept {
    for (usize index = 0; index < NameLengthBytes; ++index) {
        if (auto pushed = out.TryPushBack(static_cast<uint8>(value >> (index * ByteBits)));
            !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// text를 NFC로 out에 둔다. 바뀌었으면 changed를 늘린다. 잘못된 UTF-8은 CorruptData다.
Result<void> Normalize(StringView text, StringBuilderBase& out, Allocator& allocator,
                       uint64& changed) noexcept {
    out.Clear();
    auto normalized = IsNormalized(NormalizationForm::NFC, text, allocator);
    if (!normalized) {
        return Result<void>::Failure(normalized.Error() == ErrorCode::InvalidArgument
                                         ? ErrorCode::CorruptData
                                         : normalized.Error());
    }

    if (normalized.Value()) {
        return out.TryAppend(text);
    }

    ++changed;
    return TryNormalize(NormalizationForm::NFC, text, out);
}

// Name 값 bytes(길이 word + UTF-8, values개)를 NFC로 바꾼 사본을 out에 쓴다.
Result<void> NormalizeNames(Span<const uint8> bytes, uint64 values, Array<uint8>& out,
                            uint64& changed, ExchangeMeshFailure& failure) noexcept {
    Allocator& allocator = out.GetAllocator();
    StringBuilder<> text(allocator);
    usize offset = 0;
    for (uint64 value = 0; value < values; ++value) {
        if (bytes.Size() - offset < NameLengthBytes) {
            return Corrupt(failure);
        }

        const uint32 length = ReadWord(bytes.Data() + offset);
        offset += NameLengthBytes;
        if (bytes.Size() - offset < length) {
            return Corrupt(failure);
        }

        const StringView name(reinterpret_cast<const char*>(bytes.Data() + offset), length);
        offset += length;
        if (auto normalized = Normalize(name, text, allocator, changed); !normalized) {
            return normalized.Error() == ErrorCode::CorruptData
                       ? Corrupt(failure)
                       : Result<void>::Failure(normalized.Error());
        }

        if (auto appended = AppendWord(out, static_cast<uint32>(text.View().ByteCount()));
            !appended) {
            return appended;
        }

        const auto* data = reinterpret_cast<const uint8*>(text.View().Data());
        if (auto appended = out.TryAppend(Span<const uint8>(data, text.View().ByteCount()));
            !appended) {
            return appended;
        }
    }

    return offset == bytes.Size() ? Result<void>::Success() : Corrupt(failure);
}

// (요소, 이름 UTF-8 byte) 순서다(MeshAnimationAssets 9.2절).
bool Less(MeshElement leftElement, StringView left, MeshElement rightElement,
          StringView right) noexcept {
    if (leftElement != rightElement) {
        return static_cast<uint8>(leftElement) < static_cast<uint8>(rightElement);
    }

    const usize common =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < common; ++index) {
        const auto a = static_cast<uint8>(left.Data()[index]);
        const auto b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

// 속성 하나의 정규화한 이름과 레코드다.
struct PendingAttribute {
    explicit PendingAttribute(Allocator& allocator) noexcept : name(allocator) {
    }

    // 정규화한 이름과 기록 원문의 이름(기록 순서 검사 — 문서를 가리킨다)이다.
    String name;
    StringView original;
    MeshAttributeRecord record;
};

// 요소 종류의 수다(element는 1부터).
uint32 ElementCount(const MeshSourceData& data, MeshElement element) noexcept {
    switch (element) {
    case MeshElement::Vertex:
        return data.vertexCount;
    case MeshElement::Corner:
        return data.cornerCount;
    case MeshElement::Edge:
        return data.edgeCount;
    case MeshElement::Polygon:
        return data.polygonCount;
    case MeshElement::Triangle:
        return data.triangleCount;
    case MeshElement::PolygonGroup:
        return data.polygonGroupCount;
    default:
        return 0;
    }
}

Result<void> ReadCounts(JsonValue counts, MeshSourceData& data,
                        ExchangeMeshFailure& failure) noexcept {
    if (!counts.IsObject() || counts.Size() != ElementKinds) {
        return Corrupt(failure);
    }

    uint32* targets[ElementKinds] = {&data.vertexCount,   &data.cornerCount,
                                     &data.edgeCount,     &data.polygonCount,
                                     &data.triangleCount, &data.polygonGroupCount};
    for (usize index = 0; index < ElementKinds; ++index) {
        uint64 value = 0;
        if (!ReadUInt(Member(counts, CountKeys[index]), value) ||
            value > NumericLimits<uint32>::Max()) {
            return Corrupt(failure);
        }

        *targets[index] = static_cast<uint32>(value);
    }

    return Result<void>::Success();
}

Result<void> ReadTopology(const ExchangeAsset& asset, Span<const Span<const uint8>> blobs,
                          JsonValue topology, Allocator& allocator, MemoryBulkSource& bulk,
                          MeshSourceData& data, ExchangeMeshFailure& failure) noexcept {
    if (!topology.IsArray() || topology.Size() != TopologyArrays) {
        return Corrupt(failure);
    }

    // 배열마다의 원소 수다(9.1절 — 코너·변·다각형 + 1·둘레 길이 합·다각형·삼각형·삼각형). 둘레
    // 길이 합은 counts로 정해지지 않아(코너 공유) Load의 길이 판정에 맡긴다.
    const uint64 expected[TopologyArrays] = {
        data.cornerCount,       data.edgeCount,    static_cast<uint64>(data.polygonCount) + 1,
        TopologyCountUnchecked, data.polygonCount, data.triangleCount,
        data.triangleCount};
    Array<uint8> joined(allocator);
    for (usize index = 0; index < TopologyArrays; ++index) {
        const usize blob = FindBlob(asset, topology.At(index));
        if (blob == asset.blobs.Size() || asset.blobs[blob].layout != ExchangeBlobLayout::U32 ||
            asset.blobs[blob].components != TopologyComponents[index] ||
            (expected[index] != TopologyCountUnchecked &&
             asset.blobs[blob].count != expected[index])) {
            return Corrupt(failure);
        }

        if (auto appended = joined.TryAppend(blobs[blob]); !appended) {
            return appended;
        }
    }

    auto added = bulk.Add(rocks::Move(joined));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    data.topology = added.Value();
    return Result<void>::Success();
}

Result<void> ReadAttribute(const ExchangeAsset& asset, Span<const Span<const uint8>> blobs,
                           JsonValue item, const MeshSourceData& data, Allocator& allocator,
                           MemoryBulkSource& bulk, PendingAttribute& out, uint64& normalized,
                           ExchangeMeshFailure& failure) noexcept {
    StringView name;
    StringView element;
    StringView type;
    uint64 channels = 0;
    if (!ReadString(Member(item, "name"), name) || !ReadString(Member(item, "element"), element) ||
        !ReadString(Member(item, "type"), type) || !ReadUInt(Member(item, "channels"), channels) ||
        channels == 0 || channels > NumericLimits<uint8>::Max()) {
        return Corrupt(failure);
    }

    const usize elementIndex = FindName(Span<const StringView>(ElementNames), element);
    usize typeIndex = 0;
    while (typeIndex < ValueTypeCount && ValueTypes[typeIndex].name != type) {
        ++typeIndex;
    }

    if (elementIndex == ElementKinds || typeIndex == ValueTypeCount) {
        return Corrupt(failure);
    }

    const ValueTypeEntry& entry = ValueTypes[typeIndex];
    out.record.element = static_cast<MeshElement>(elementIndex + 1);
    out.record.type = static_cast<MeshValueType>(typeIndex + 1);
    out.record.channels = static_cast<uint8>(channels);
    const usize blob = FindBlob(asset, Member(item, "data"));
    if (blob == asset.blobs.Size()) {
        return Corrupt(failure);
    }

    const ExchangeBlobRef& reference = asset.blobs[blob];
    if (reference.layout != entry.layout ||
        reference.components != static_cast<uint64>(entry.components) * channels ||
        reference.count != ElementCount(data, out.record.element)) {
        return Corrupt(failure);
    }

    StringBuilder<> text(allocator);
    if (auto made = Normalize(name, text, allocator, normalized); !made) {
        return made.Error() == ErrorCode::CorruptData ? Corrupt(failure)
                                                      : Result<void>::Failure(made.Error());
    }

    out.original = name;
    if (auto assigned = out.name.TryAssign(text.View()); !assigned) {
        return assigned;
    }

    Array<uint8> copy(allocator);
    if (entry.layout == ExchangeBlobLayout::Names) {
        if (auto made = NormalizeNames(blobs[blob], reference.count * reference.components, copy,
                                       normalized, failure);
            !made) {
            return made;
        }
    } else if (auto copied = copy.TryAppend(blobs[blob]); !copied) {
        return copied;
    }

    auto added = bulk.Add(rocks::Move(copy));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    out.record.data = added.Value();
    return Result<void>::Success();
}

} // namespace exchange_mesh_source

Result<void> LoadExchangeMeshSource(const ExchangeAsset& asset, Span<const Span<const uint8>> blobs,
                                    JsonValue record, NamePool& names, MeshSource& out,
                                    uint64& normalized, ExchangeMeshFailure& failure) noexcept {
    using namespace exchange_mesh_source;
    failure = ExchangeMeshFailure{};
    normalized = 0;
    if (blobs.Size() != asset.blobs.Size()) {
        failure.code = AssetDiagnosticCode::InvalidSource;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const JsonValue attributes = Member(record, "attributes");
    if (!attributes.IsArray()) {
        return Corrupt(failure);
    }

    Allocator& allocator = out.GetAllocator();
    MemoryBulkSource bulk(allocator);
    MeshSourceData data(allocator);
    if (auto read = ReadCounts(Member(record, "counts"), data, failure); !read) {
        return read;
    }

    if (auto read =
            ReadTopology(asset, blobs, Member(record, "topology"), allocator, bulk, data, failure);
        !read) {
        return read;
    }

    Array<PendingAttribute> pending(allocator);
    for (usize index = 0; index < attributes.Size(); ++index) {
        if (auto pushed = pending.TryEmplaceBack(allocator); !pushed) {
            return Result<void>::Failure(pushed.Error());
        }

        if (auto read = ReadAttribute(asset, blobs, attributes.At(index), data, allocator, bulk,
                                      pending[index], normalized, failure);
            !read) {
            return read;
        }

        // 기록은 원문 이름으로 9.2절 순서의 엄격 증가다(writer가 정렬해 쓴다 — 아니면 형식 위반).
        if (index != 0 && !Less(pending[index - 1].record.element, pending[index - 1].original,
                                pending[index].record.element, pending[index].original)) {
            return Corrupt(failure);
        }
    }

    // 정규화가 이름을 바꾸면 9.2절 순서가 달라질 수 있다(바꾼 것이 있을 때만 삽입 정렬). 정규화 뒤
    // 같은 이름 둘은 Load가 순서 위반으로 거부한다.
    for (usize index = 1; normalized != 0 && index < pending.Size(); ++index) {
        for (usize at = index; at > 0; --at) {
            PendingAttribute& left = pending[at - 1];
            PendingAttribute& right = pending[at];
            if (!Less(right.record.element, right.name.View(), left.record.element,
                      left.name.View())) {
                break;
            }

            PendingAttribute moved = rocks::Move(left);
            left = rocks::Move(right);
            right = rocks::Move(moved);
        }
    }

    for (PendingAttribute& item : pending) {
        auto interned = names.TryIntern(item.name.View());
        if (!interned) {
            return Result<void>::Failure(interned.Error());
        }

        item.record.name = interned.Value();
        if (auto pushed = data.attributes.TryPushBack(item.record); !pushed) {
            return pushed;
        }
    }

    MeshSource loaded(allocator);
    if (auto made = loaded.Load(data, bulk, names, &failure.mesh); !made) {
        failure.code = failure.mesh.code == AssetDiagnosticCode::CorruptPackage
                           ? AssetDiagnosticCode::CorruptBundle
                           : failure.mesh.code;
        return made;
    }

    out = rocks::Move(loaded);
    return Result<void>::Success();
}

} // namespace rocks
