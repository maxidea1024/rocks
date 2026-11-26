#include <ShaderCore/ShaderParameterLayout.h>

#include "ShaderCoreInternal.h"

#include <Base/Endian.h>
#include <Base/NumericLimits.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>

// 손으로 쓴 파라미터 레이아웃 표(Docs/Design/ShaderSystem 7.2절 5항, 12.2.1절 SSD27)다. 규칙은 공개
// 헤더 주석을 따른다.
namespace rocks::shader {

namespace parameter_layout_detail {

struct TypeInfo {
    uint32 bytes;
    uint32 alignment;
    ShaderParameterBaseType base;
    const char* name;
};

// ShaderParameterType 값 순서다.
constexpr TypeInfo Types[] = {{4, 4, ShaderParameterBaseType::Float, "float"},
                              {8, 8, ShaderParameterBaseType::Float, "float2"},
                              {12, 16, ShaderParameterBaseType::Float, "float3"},
                              {16, 16, ShaderParameterBaseType::Float, "float4"},
                              {4, 4, ShaderParameterBaseType::Int, "int"},
                              {8, 8, ShaderParameterBaseType::Int, "int2"},
                              {12, 16, ShaderParameterBaseType::Int, "int3"},
                              {16, 16, ShaderParameterBaseType::Int, "int4"},
                              {4, 4, ShaderParameterBaseType::Uint, "uint"},
                              {8, 8, ShaderParameterBaseType::Uint, "uint2"},
                              {12, 16, ShaderParameterBaseType::Uint, "uint3"},
                              {16, 16, ShaderParameterBaseType::Uint, "uint4"},
                              {64, 16, ShaderParameterBaseType::Float, "float4x4"}};

constexpr usize TypeCount = sizeof(Types) / sizeof(Types[0]);

static_assert(TypeCount == static_cast<usize>(ShaderParameterType::Float4x4) + 1,
              "every shader parameter type needs a TypeInfo row");

bool IsKnownType(ShaderParameterType type) noexcept {
    return static_cast<usize>(type) < TypeCount;
}

Result<void> Reject(const char** reason, const char* why) noexcept {
    if (reason != nullptr) {
        *reason = why;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsName(StringView name) noexcept {
    return core_detail::IsShaderIdentifier(name, MaxShaderParameterNameBytes);
}

// 정규 byte를 hasher에 더한다(공개 헤더의 서명 byte 순서).
void HashU8(Sha256& hasher, uint8 value) noexcept {
    hasher.Update(Span<const uint8>(&value, 1));
}

void HashU32(Sha256& hasher, uint32 value) noexcept {
    uint8 bytes[sizeof(uint32)];
    StoreLittleEndian<uint32>(bytes, value);
    hasher.Update(Span<const uint8>(bytes, sizeof(bytes)));
}

void HashName(Sha256& hasher, StringView name) noexcept {
    HashU8(hasher, static_cast<uint8>(name.ByteCount()));
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(name.Data()), name.ByteCount()));
}

// C++ 정규 이름(::로 이은 식별자)인가.
bool IsStructName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > MaxShaderParameterStructNameBytes) {
        return false;
    }

    const char* text = name.Data();
    usize start = 0;
    usize index = 0;
    while (true) {
        while (index < name.ByteCount() && text[index] != ':') {
            ++index;
        }

        if (!core_detail::IsShaderIdentifier(StringView(text + start, index - start),
                                             MaxShaderParameterStructNameBytes)) {
            return false;
        }

        if (index == name.ByteCount()) {
            return true;
        }

        if (index + 1 >= name.ByteCount() || text[index + 1] != ':') {
            return false;
        }

        index += 2;
        start = index;
    }
}

// 생성 원문 경로(/<루트…>/Generated/<…>.rsh)인가.
bool IsGeneratedSourcePath(StringView path) noexcept {
    const StringView directory("/Generated/");
    const StringView extension(".rsh");
    if (path.IsEmpty() || path.ByteCount() > MaxShaderParameterSourcePathBytes ||
        path.Data()[0] != '/' || !path.EndsWith(extension, StringComparison::CaseSensitive)) {
        return false;
    }

    for (usize index = 0; index + directory.ByteCount() <= path.ByteCount(); ++index) {
        if (StringView(path.Data() + index, directory.ByteCount()) == directory) {
            return true;
        }
    }

    return false;
}

// 자원 멤버 종류 이름이다(ShaderParameterResourceKind 값 순서).
constexpr const char* ResourceKindNames[ShaderParameterResourceKindCount] = {
    "DeviceTexture",     "DeviceBuffer",   "GraphTexture",   "GraphTextureSrv",
    "GraphTextureUav",   "GraphBufferSrv", "GraphBufferUav", "GraphTextureAccess",
    "GraphBufferAccess", "RenderTargets"};

// 짝 샘플러 이름의 접미다(7.2절 9항).
constexpr StringView SamplerSuffix("Sampler");

// 생성 HLSL이 쓸 수 없는 예약 전역 이름이다(공유 샘플러와 그 짝 이름, PS 진단 버퍼 — 7.2절 9·12항).
constexpr StringView ReservedNames[] = {"SharedWrap", "SharedClamp", "SharedWrapSampler",
                                        "SharedClampSampler", "RocksDebugBuffer"};

bool IsReservedName(StringView name) noexcept {
    for (const StringView reserved : ReservedNames) {
        if (name == reserved) {
            return true;
        }
    }

    return false;
}

void HashResource(Sha256& hasher, const ShaderParameterResource& resource) noexcept {
    HashName(hasher, resource.name);
    HashName(hasher, resource.samplerName);
    HashU8(hasher, static_cast<uint8>(resource.kind));
    HashU32(hasher, resource.offset);
    HashU8(hasher, static_cast<uint8>(resource.shape));
    HashU8(hasher, static_cast<uint8>(resource.component));
    HashU32(hasher, resource.structureStride);
    HashU8(hasher, static_cast<uint8>(resource.sampler));
}

// 표 하나의 정규 byte(공개 헤더의 "표 byte")를 hasher에 더한다.
void HashLayout(Sha256& hasher, const ShaderParameterLayout& layout) noexcept {
    HashName(hasher, layout.name);
    HashName(hasher, layout.structName);
    HashU8(hasher, layout.rootConstants ? 1 : 0);
    HashU32(hasher, layout.totalBytes);
    HashU32(hasher, static_cast<uint32>(layout.members.Size()));
    for (const ShaderParameterMember& member : layout.members) {
        HashName(hasher, member.name);
        HashU8(hasher, static_cast<uint8>(member.type));
        HashU32(hasher, member.offset);
        HashU32(hasher, member.arrayCount);
        if (member.type == ShaderParameterType::Struct && member.structLayout != nullptr) {
            HashLayout(hasher, *member.structLayout);
        }
    }

    HashU32(hasher, static_cast<uint32>(layout.resources.Size()));
    for (const ShaderParameterResource& resource : layout.resources) {
        HashResource(hasher, resource);
    }
}

void WriteName(ByteWriter& writer, StringView name) noexcept {
    writer.WriteU8(static_cast<uint8>(name.ByteCount()));
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(name.Data()), name.ByteCount()));
}

void WriteLayout(ByteWriter& writer, const ShaderParameterLayout& layout) noexcept {
    WriteName(writer, layout.name);
    WriteName(writer, layout.structName);
    writer.WriteU8(layout.rootConstants ? 1 : 0);
    writer.WriteU32LE(layout.totalBytes);
    writer.WriteU32LE(static_cast<uint32>(layout.members.Size()));
    for (const ShaderParameterMember& member : layout.members) {
        WriteName(writer, member.name);
        writer.WriteU8(static_cast<uint8>(member.type));
        writer.WriteU32LE(member.offset);
        writer.WriteU32LE(member.arrayCount);
        if (member.type == ShaderParameterType::Struct && member.structLayout != nullptr) {
            WriteLayout(writer, *member.structLayout);
        }
    }

    writer.WriteU32LE(static_cast<uint32>(layout.resources.Size()));
    for (const ShaderParameterResource& resource : layout.resources) {
        WriteName(writer, resource.name);
        WriteName(writer, resource.samplerName);
        writer.WriteU8(static_cast<uint8>(resource.kind));
        writer.WriteU32LE(resource.offset);
        writer.WriteU8(static_cast<uint8>(resource.shape));
        writer.WriteU8(static_cast<uint8>(resource.component));
        writer.WriteU32LE(resource.structureStride);
        writer.WriteU8(static_cast<uint8>(resource.sampler));
    }
}

} // namespace parameter_layout_detail

uint32 ShaderParameterTypeBytes(ShaderParameterType type) noexcept {
    using namespace parameter_layout_detail;
    return IsKnownType(type) ? Types[static_cast<usize>(type)].bytes : 0;
}

uint32 ShaderParameterTypeAlignment(ShaderParameterType type) noexcept {
    using namespace parameter_layout_detail;
    return IsKnownType(type) ? Types[static_cast<usize>(type)].alignment : 0;
}

ShaderParameterBaseType ShaderParameterTypeBase(ShaderParameterType type) noexcept {
    using namespace parameter_layout_detail;
    return IsKnownType(type) ? Types[static_cast<usize>(type)].base
                             : ShaderParameterBaseType::Float;
}

const char* ShaderParameterTypeName(ShaderParameterType type) noexcept {
    using namespace parameter_layout_detail;
    return IsKnownType(type) ? Types[static_cast<usize>(type)].name : "unknown";
}

uint64 ShaderParameterMemberBytes(const ShaderParameterMember& member) noexcept {
    if (member.type == ShaderParameterType::Struct) {
        return member.structLayout == nullptr ? 0 : member.structLayout->totalBytes;
    }

    const uint64 element = ShaderParameterTypeBytes(member.type);
    return member.arrayCount == 0 ? element : element * member.arrayCount;
}

const char* ShaderParameterResourceKindName(ShaderParameterResourceKind kind) noexcept {
    using namespace parameter_layout_detail;
    const usize index = static_cast<usize>(kind);
    return index < ShaderParameterResourceKindCount ? ResourceKindNames[index] : "unknown";
}

namespace parameter_layout_detail {

bool IsTextureShape(ShaderResourceShape shape) noexcept {
    return shape >= ShaderResourceShape::Texture1D &&
           shape <= ShaderResourceShape::TextureCubeArray;
}

bool IsMultisampleShape(ShaderResourceShape shape) noexcept {
    return shape == ShaderResourceShape::Texture2DMultisample ||
           shape == ShaderResourceShape::Texture2DMultisampleArray;
}

// 버퍼 모양(typed·구조화·raw)과 원소·stride가 맞는가.
bool IsBufferShape(const ShaderParameterResource& resource) noexcept {
    switch (resource.shape) {
    case ShaderResourceShape::TypedBuffer:
        return resource.component != ShaderComponentType::None && resource.structureStride == 0;
    case ShaderResourceShape::StructuredBuffer:
        return resource.component == ShaderComponentType::None && resource.structureStride != 0 &&
               resource.structureStride % sizeof(uint32) == 0;
    case ShaderResourceShape::RawBuffer:
        return resource.component == ShaderComponentType::None && resource.structureStride == 0;
    default:
        break;
    }

    return false;
}

// 짝 샘플러 이름이 <name>Sampler인가.
bool IsPairName(const ShaderParameterResource& resource) noexcept {
    const StringView pair = resource.samplerName;
    return pair.ByteCount() == resource.name.ByteCount() + SamplerSuffix.ByteCount() &&
           StringView(pair.Data(), resource.name.ByteCount()) == resource.name &&
           pair.EndsWith(SamplerSuffix, StringComparison::CaseSensitive);
}

// 종류에 맞는 모양·원소·stride·짝인가(16.2절 SSD59·SSD62). 맞으면 null, 아니면 영어 이유다.
const char* CheckResourceShape(const ShaderParameterResource& resource) noexcept {
    if (static_cast<uint32>(resource.kind) >= ShaderParameterResourceKindCount ||
        static_cast<uint32>(resource.shape) >= ShaderResourceShapeCount ||
        static_cast<uint32>(resource.component) >= ShaderComponentTypeCount ||
        static_cast<uint32>(resource.sampler) >= ShaderParameterSamplerCount) {
        return "shader parameter resource kind, shape, component or sampler is outside the "
               "vocabulary";
    }

    const bool pairs = resource.sampler != ShaderParameterSampler::None;
    if (pairs != !resource.samplerName.IsEmpty() || (pairs && !IsPairName(resource))) {
        return "a shader parameter resource sampler pair is named <name>Sampler and only a "
               "paired resource has one";
    }

    bool fits = false;
    switch (resource.kind) {
    case ShaderParameterResourceKind::DeviceTexture:
    case ShaderParameterResourceKind::GraphTexture:
    case ShaderParameterResourceKind::GraphTextureSrv: {
        const bool texture = IsTextureShape(resource.shape);
        const bool filtered = texture && !IsMultisampleShape(resource.shape) &&
                              resource.component == ShaderComponentType::Float;
        fits = texture && resource.component != ShaderComponentType::None &&
               resource.structureStride == 0 && pairs == filtered;
        break;
    }
    case ShaderParameterResourceKind::DeviceBuffer:
    case ShaderParameterResourceKind::GraphBufferSrv:
    case ShaderParameterResourceKind::GraphBufferUav:
        fits = !pairs && IsBufferShape(resource);
        break;
    case ShaderParameterResourceKind::GraphTextureUav:
        fits = !pairs &&
               (resource.shape == ShaderResourceShape::Texture2D ||
                resource.shape == ShaderResourceShape::Texture2DArray ||
                resource.shape == ShaderResourceShape::Texture3D) &&
               resource.component != ShaderComponentType::None && resource.structureStride == 0;
        break;
    case ShaderParameterResourceKind::GraphTextureAccess:
    case ShaderParameterResourceKind::GraphBufferAccess:
    case ShaderParameterResourceKind::RenderTargets:
        fits = !pairs && resource.shape == ShaderResourceShape::None &&
               resource.component == ShaderComponentType::None && resource.structureStride == 0;
        break;
    }

    return fits ? nullptr
                : "shader parameter resource shape, element type, structure stride or sampler "
                  "pair does not fit its kind";
}

// 이름이 상수 멤버·앞 자원·짝 이름과 겹치는가.
bool NameTaken(const ShaderParameterLayout& layout, usize before, StringView name) noexcept {
    for (const ShaderParameterMember& member : layout.members) {
        if (member.name == name) {
            return true;
        }
    }

    for (usize index = 0; index < before; ++index) {
        const ShaderParameterResource& other = layout.resources[index];
        if (other.name == name || (!other.samplerName.IsEmpty() && other.samplerName == name)) {
            return true;
        }
    }

    return false;
}

Result<void> ValidateResources(const ShaderParameterLayout& layout, bool nested,
                               const char** reason) noexcept {
    if (layout.resources.IsEmpty()) {
        return Result<void>::Success();
    }

    if (nested || layout.rootConstants || layout.structId == 0) {
        return Reject(reason, "only generated, non-nested constant buffer layouts have resource "
                              "members");
    }

    if (layout.resources.Size() > MaxShaderParameterResources) {
        return Reject(reason, "shader parameter layout has more than 256 resource members");
    }

    uint64 previous = 0;
    usize renderTargets = 0;
    for (usize index = 0; index < layout.resources.Size(); ++index) {
        const ShaderParameterResource& resource = layout.resources[index];
        if (!IsName(resource.name) ||
            (!resource.samplerName.IsEmpty() && !IsName(resource.samplerName))) {
            return Reject(reason, "shader parameter resource names must be identifiers of at "
                                  "most 64 bytes");
        }

        if (IsReservedName(resource.name) || IsReservedName(resource.samplerName) ||
            NameTaken(layout, index, resource.name) ||
            (!resource.samplerName.IsEmpty() && NameTaken(layout, index, resource.samplerName))) {
            return Reject(reason, "shader parameter resource names must not repeat a member, "
                                  "resource, sampler or reserved name");
        }

        if (resource.offset < layout.totalBytes || resource.offset % ShaderConstantSlotBytes != 0 ||
            (index > 0 && resource.offset <= previous)) {
            return Reject(reason, "shader parameter resource offsets are 16-byte multiples after "
                                  "the constant part, in increasing order");
        }

        if (const char* shape = CheckResourceShape(resource); shape != nullptr) {
            return Reject(reason, shape);
        }

        renderTargets += resource.kind == ShaderParameterResourceKind::RenderTargets ? 1 : 0;
        previous = resource.offset;
    }

    if (renderTargets > 1) {
        return Reject(reason, "shader parameter layout has more than one render target member");
    }

    return Result<void>::Success();
}

Result<void> ValidateLayout(const ShaderParameterLayout& layout, bool nested,
                            const char** reason) noexcept {
    if (!IsName(layout.name)) {
        return Reject(reason, "shader parameter layout name must be an identifier of at most 64 "
                              "bytes");
    }

    if (!layout.structName.IsEmpty() && !IsStructName(layout.structName)) {
        return Reject(reason, "shader parameter struct name must be a qualified C++ name of at "
                              "most 128 bytes");
    }

    if (layout.structId != ShaderParameterStructId(layout.structName)) {
        return Reject(reason, "shader parameter struct identity must be the hash of the struct "
                              "name (ShaderParameterStructId)");
    }

    if (layout.source.ByteCount() > MaxShaderParameterSourceBytes ||
        (!layout.sourcePath.IsEmpty() && !IsGeneratedSourcePath(layout.sourcePath)) ||
        layout.sourcePath.IsEmpty() != layout.source.IsEmpty()) {
        return Reject(reason, "generated shader parameter source needs a /<root>/Generated/... "
                              ".rsh path and text of at most 64 KiB together");
    }

    if (auto resources = ValidateResources(layout, nested, reason); !resources) {
        return resources;
    }

    // 자원 구조체(상수 멤버 없음 — 16.2절 SSD60)는 상수 부분이 비어 있다.
    if (layout.members.IsEmpty() && layout.totalBytes == 0 && !layout.resources.IsEmpty()) {
        return Result<void>::Success();
    }

    if (layout.members.IsEmpty() || layout.members.Size() > MaxShaderParameterMembers) {
        return Reject(reason, "shader parameter layout must have 1 to 256 members");
    }

    if (layout.totalBytes == 0 || layout.totalBytes % ShaderConstantSlotBytes != 0 ||
        layout.totalBytes > MaxShaderConstantBufferBytes) {
        return Reject(reason, "shader parameter layout size must be a non-zero multiple of 16 "
                              "bytes of at most 65536");
    }

    if (layout.rootConstants && layout.totalBytes > MaxShaderRootConstantBytes) {
        return Reject(reason, "root constants must be at most 128 bytes");
    }

    if (nested && layout.rootConstants) {
        return Reject(reason, "a nested shader parameter struct cannot be root constants");
    }

    uint64 previousEnd = 0;
    // 앞 멤버가 끝난 16 byte 슬롯과 그 기본 타입이다(슬롯 하나에 기본 타입을 섞지 않는다).
    uint64 previousSlot = NumericLimits<uint64>::Max();
    ShaderParameterBaseType previousBase = ShaderParameterBaseType::Float;
    for (usize index = 0; index < layout.members.Size(); ++index) {
        const ShaderParameterMember& member = layout.members[index];
        if (!IsName(member.name)) {
            return Reject(reason, "shader parameter member name must be an identifier of at most "
                                  "64 bytes");
        }

        for (usize other = 0; other < index; ++other) {
            if (layout.members[other].name == member.name) {
                return Reject(reason, "shader parameter member names must be unique");
            }
        }

        if (member.offset < previousEnd) {
            return Reject(reason, "shader parameter members must be in offset order without "
                                  "overlap");
        }

        if (member.type == ShaderParameterType::Struct) {
            if (nested) {
                return Reject(reason, "shader parameter structs nest only one level");
            }

            if (member.structLayout == nullptr) {
                return Reject(reason, "a struct member needs its nested layout");
            }

            if (member.arrayCount != 0) {
                return Reject(reason, "arrays of structs are not allowed in shader parameters");
            }

            if (member.offset % ShaderConstantSlotBytes != 0) {
                return Reject(reason, "a nested struct must start on a 16-byte boundary");
            }

            if (auto valid = ValidateLayout(*member.structLayout, true, reason); !valid) {
                return valid;
            }

            const uint64 end = member.offset + uint64{member.structLayout->totalBytes};
            if (end > layout.totalBytes) {
                return Reject(reason, "shader parameter member ends past the layout size");
            }

            previousEnd = end;
            previousSlot = NumericLimits<uint64>::Max();
            continue;
        }

        if (!IsKnownType(member.type) || member.structLayout != nullptr) {
            return Reject(reason, "shader parameter member type is outside the allowed types "
                                  "(4-byte scalars, their 2-4 component vectors, float4x4, "
                                  "nested structs)");
        }

        const TypeInfo& info = Types[static_cast<usize>(member.type)];
        if (member.offset % info.alignment != 0) {
            return Reject(reason, "shader parameter member offset breaks the alignment rule "
                                  "(scalar 4, 2 components 8, 3-4 components and matrices 16)");
        }

        if (member.arrayCount != 0 && info.bytes % ShaderConstantSlotBytes != 0) {
            return Reject(reason, "shader parameter arrays need an element size that is a "
                                  "multiple of 16 bytes (pack scalars into float4 arrays)");
        }

        const uint64 end = member.offset + ShaderParameterMemberBytes(member);
        if (end > layout.totalBytes) {
            return Reject(reason, "shader parameter member ends past the layout size");
        }

        const uint64 firstSlot = member.offset / ShaderConstantSlotBytes;
        if (firstSlot == previousSlot && info.base != previousBase) {
            return Reject(reason, "a 16-byte slot must not mix float, int and uint members");
        }

        previousEnd = end;
        previousSlot = (end - 1) / ShaderConstantSlotBytes;
        previousBase = info.base;
    }

    return Result<void>::Success();
}

} // namespace parameter_layout_detail

Result<void> ValidateShaderParameterLayout(const ShaderParameterLayout& layout,
                                           const char** reason) noexcept {
    return parameter_layout_detail::ValidateLayout(layout, false, reason);
}

Result<void> ValidateShaderParameterLayouts(Span<const ShaderParameterLayout> layouts,
                                            const char** reason) noexcept {
    using namespace parameter_layout_detail;
    if (layouts.Size() > MaxShaderParameterLayouts) {
        return Reject(reason, "a shader type has at most 16 parameter layouts");
    }

    usize rootConstants = 0;
    for (usize index = 0; index < layouts.Size(); ++index) {
        if (auto valid = ValidateShaderParameterLayout(layouts[index], reason); !valid) {
            return valid;
        }

        for (usize other = 0; other < index; ++other) {
            if (layouts[other].name == layouts[index].name) {
                return Reject(reason, "shader parameter layout names must be unique");
            }

            if (!layouts[other].sourcePath.IsEmpty() &&
                layouts[other].sourcePath == layouts[index].sourcePath) {
                return Reject(reason, "generated shader parameter source paths must be unique");
            }

            // 자원·짝 샘플러는 셰이더 전역 이름이라 표 사이에서도 겹칠 수 없다.
            for (const ShaderParameterResource& resource : layouts[index].resources) {
                if (!IsShaderBoundResource(resource.kind)) {
                    continue;
                }

                for (const ShaderParameterResource& seen : layouts[other].resources) {
                    if (!IsShaderBoundResource(seen.kind)) {
                        continue;
                    }

                    const bool clash =
                        seen.name == resource.name ||
                        (!resource.samplerName.IsEmpty() &&
                         (seen.name == resource.samplerName ||
                          seen.samplerName == resource.samplerName)) ||
                        (!seen.samplerName.IsEmpty() && seen.samplerName == resource.name);
                    if (clash) {
                        return Reject(reason, "shader parameter layouts of one shader type must "
                                              "not share resource or sampler names");
                    }
                }
            }
        }

        rootConstants += layouts[index].rootConstants ? 1 : 0;
    }

    if (rootConstants > 1) {
        return Reject(reason, "a shader type has at most one root constants layout");
    }

    return Result<void>::Success();
}

Sha256Digest ShaderParameterLayoutSignature(Span<const ShaderParameterLayout> layouts) noexcept {
    using namespace parameter_layout_detail;
    if (layouts.IsEmpty()) {
        return Sha256Digest{};
    }

    Sha256 hasher;
    HashU32(hasher, static_cast<uint32>(layouts.Size()));
    for (const ShaderParameterLayout& layout : layouts) {
        HashLayout(hasher, layout);
    }

    return hasher.Finish();
}

Result<void> EncodeShaderParameterLayouts(Span<const ShaderParameterLayout> layouts,
                                          Array<uint8>& out) noexcept {
    using namespace parameter_layout_detail;
    if (auto valid = ValidateShaderParameterLayouts(layouts); !valid) {
        return valid;
    }

    const usize start = out.Size();
    ByteWriter writer(out);
    writer.WriteU32LE(static_cast<uint32>(layouts.Size()));
    for (const ShaderParameterLayout& layout : layouts) {
        WriteLayout(writer, layout);
    }

    if (auto written = writer.Status(); !written) {
        (void)out.TryResize(start);
        return written;
    }

    return Result<void>::Success();
}

const ShaderParameterLayout* ShaderParameterLayoutSet::Find(StringView name) const noexcept {
    for (const ShaderParameterLayout& layout : layouts) {
        if (layout.name == name) {
            return &layout;
        }
    }

    return nullptr;
}

namespace parameter_layout_detail {

// 읽은 표 하나의 멤버 범위(out.members[first, first + count))다.
struct LayoutRange {
    usize first = 0;
    usize count = 0;
};

// Struct 멤버 하나의 연결 대기다: out.members[member]가 out.nested[nested]를 가리켜야 한다. 배열이
// 다 자란 뒤 포인터로 잇는다.
struct NestedLink {
    usize member = 0;
    usize nested = 0;
};

// 디코딩 중 상태다.
struct DecodeState {
    explicit DecodeState(Allocator& allocator) noexcept
        : nestedRanges(allocator), links(allocator) {
    }

    Array<LayoutRange> nestedRanges;
    Array<NestedLink> links;
};

// 표 byte 끝의 자원 목록을 out.resources 끝에 읽고 그 범위를 resourceRange에 둔다. 실패하면 영어
// 이유다.
const char* ReadResources(ByteReader& reader, ShaderParameterLayoutSet& out,
                          LayoutRange& resourceRange) noexcept {
    const auto readName = [&]() noexcept {
        const uint8 length = reader.ReadU8();
        const Span<const uint8> name = reader.Take(length);
        return StringView(reinterpret_cast<const char*>(name.Data()), name.Size());
    };
    const uint32 count = reader.ReadU32LE();
    if (reader.Failed() || count > MaxShaderParameterResources) {
        return "parameter layout resource list is truncated or out of range";
    }

    resourceRange.first = out.resources.Size();
    resourceRange.count = count;
    for (uint32 index = 0; index < count; ++index) {
        ShaderParameterResource resource;
        resource.name = readName();
        resource.samplerName = readName();
        const uint8 kind = reader.ReadU8();
        resource.offset = reader.ReadU32LE();
        const uint8 shape = reader.ReadU8();
        const uint8 component = reader.ReadU8();
        resource.structureStride = reader.ReadU32LE();
        const uint8 sampler = reader.ReadU8();
        if (reader.Failed()) {
            return "parameter layout resource is truncated";
        }

        if (kind >= ShaderParameterResourceKindCount || shape >= ShaderResourceShapeCount ||
            component >= ShaderComponentTypeCount || sampler >= ShaderParameterSamplerCount) {
            return "parameter layout resource is out of range";
        }

        resource.kind = static_cast<ShaderParameterResourceKind>(kind);
        resource.shape = static_cast<ShaderResourceShape>(shape);
        resource.component = static_cast<ShaderComponentType>(component);
        resource.sampler = static_cast<ShaderParameterSampler>(sampler);
        if (!out.resources.TryPushBack(resource)) {
            return "out of memory reading parameter layouts";
        }
    }

    return nullptr;
}

// 표 byte 하나를 읽는다. nested가 참이면 중첩 표(Struct 멤버를 받지 않는다)다. 실패하면 영어
// 이유다.
const char* ReadLayout(ByteReader& reader, bool nested, ShaderParameterLayoutSet& out,
                       DecodeState& state, ShaderParameterLayout& layout, LayoutRange& range,
                       LayoutRange& resourceRange) noexcept {
    const auto readName = [&]() noexcept {
        const uint8 length = reader.ReadU8();
        const Span<const uint8> name = reader.Take(length);
        return StringView(reinterpret_cast<const char*>(name.Data()), name.Size());
    };
    layout.name = readName();
    layout.structName = readName();
    layout.structId = ShaderParameterStructId(layout.structName);
    const uint8 root = reader.ReadU8();
    layout.totalBytes = reader.ReadU32LE();
    const uint32 memberCount = reader.ReadU32LE();
    if (reader.Failed() || root > 1 || memberCount > MaxShaderParameterMembers) {
        return "parameter layout is truncated or out of range";
    }

    layout.rootConstants = root != 0;
    // 중첩 표의 멤버가 같은 배열에 끼어들므로 이 표의 멤버를 먼저 모아 끝에 붙인다.
    ShaderParameterMember own[MaxShaderParameterMembers];
    usize ownNested[MaxShaderParameterMembers];
    for (uint32 member = 0; member < memberCount; ++member) {
        ShaderParameterMember value;
        value.name = readName();
        const uint8 type = reader.ReadU8();
        value.offset = reader.ReadU32LE();
        value.arrayCount = reader.ReadU32LE();
        if (reader.Failed()) {
            return "parameter layout member is truncated";
        }

        value.type = static_cast<ShaderParameterType>(type);
        ownNested[member] = NumericLimits<usize>::Max();
        if (value.type == ShaderParameterType::Struct) {
            if (nested) {
                return "parameter layout nests structs more than one level";
            }

            ShaderParameterLayout inner;
            LayoutRange innerRange;
            LayoutRange innerResources;
            if (const char* failed =
                    ReadLayout(reader, true, out, state, inner, innerRange, innerResources);
                failed != nullptr) {
                return failed;
            }

            // 중첩 표는 자원을 갖지 않는다(검사가 거부한다 — 여기서는 범위를 버린다).
            if (innerResources.count != 0) {
                return "parameter layout nested struct has resource members";
            }

            ownNested[member] = out.nested.Size();
            if (!out.nested.TryPushBack(inner) || !state.nestedRanges.TryPushBack(innerRange)) {
                return "out of memory reading parameter layouts";
            }
        } else if (ShaderParameterTypeBytes(value.type) == 0) {
            return "parameter layout member has an unknown type";
        }

        own[member] = value;
    }

    range.first = out.members.Size();
    range.count = memberCount;
    for (uint32 member = 0; member < memberCount; ++member) {
        if (ownNested[member] != NumericLimits<usize>::Max() &&
            !state.links.TryPushBack(NestedLink{out.members.Size(), ownNested[member]})) {
            return "out of memory reading parameter layouts";
        }

        if (!out.members.TryPushBack(own[member])) {
            return "out of memory reading parameter layouts";
        }
    }

    return ReadResources(reader, out, resourceRange);
}

} // namespace parameter_layout_detail

ShaderReadStatus DecodeShaderParameterLayouts(Span<const uint8> bytes,
                                              ShaderParameterLayoutSet& out) noexcept {
    using namespace parameter_layout_detail;
    out.layouts.Clear();
    out.members.Clear();
    out.nested.Clear();
    out.resources.Clear();
    if (bytes.IsEmpty()) {
        return ShaderReadStatus::Success();
    }

    ByteReader reader(bytes);
    LayoutRange ranges[MaxShaderParameterLayouts] = {};
    LayoutRange resourceRanges[MaxShaderParameterLayouts] = {};
    DecodeState state(out.members.GetAllocator());
    const uint32 count = reader.ReadU32LE();
    if (reader.Failed() || count == 0 || count > MaxShaderParameterLayouts) {
        return ShaderReadStatus::Corrupt("parameter layout list is truncated or has a bad count");
    }

    for (uint32 index = 0; index < count; ++index) {
        ShaderParameterLayout layout;
        if (const char* failed =
                ReadLayout(reader, false, out, state, layout, ranges[index], resourceRanges[index]);
            failed != nullptr) {
            return ShaderReadStatus::Corrupt(failed);
        }

        if (!out.layouts.TryPushBack(layout)) {
            return ShaderReadStatus::Corrupt("out of memory reading parameter layouts");
        }
    }

    if (!reader.AtEnd()) {
        return ShaderReadStatus::Corrupt("parameter layout list has trailing bytes");
    }

    // 배열이 다 자랐으므로 멤버 Span과 중첩 포인터를 잇는다.
    for (usize index = 0; index < out.nested.Size(); ++index) {
        out.nested[index].members = out.members.View().Slice(state.nestedRanges[index].first,
                                                             state.nestedRanges[index].count);
    }

    for (uint32 index = 0; index < count; ++index) {
        out.layouts[index].members =
            out.members.View().Slice(ranges[index].first, ranges[index].count);
        out.layouts[index].resources =
            out.resources.View().Slice(resourceRanges[index].first, resourceRanges[index].count);
    }

    for (const NestedLink& link : state.links) {
        out.members[link.member].structLayout = &out.nested[link.nested];
    }

    const char* reason = nullptr;
    if (!ValidateShaderParameterLayouts(out.View(), &reason)) {
        return ShaderReadStatus::Corrupt(reason);
    }

    return ShaderReadStatus::Success();
}

} // namespace rocks::shader
