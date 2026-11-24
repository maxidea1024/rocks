#include <ShaderCore/ShaderParameterLayout.h>

#include "ShaderCoreInternal.h"

#include <Base/Endian.h>
#include <Base/NumericLimits.h>

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
    const uint64 element = ShaderParameterTypeBytes(member.type);
    return member.arrayCount == 0 ? element : element * member.arrayCount;
}

Result<void> ValidateShaderParameterLayout(const ShaderParameterLayout& layout,
                                           const char** reason) noexcept {
    using namespace parameter_layout_detail;
    if (!IsName(layout.name)) {
        return Reject(reason, "shader parameter layout name must be an identifier of at most 64 "
                              "bytes");
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

        if (!IsKnownType(member.type)) {
            return Reject(reason, "shader parameter member type is outside the allowed types "
                                  "(4-byte scalars, their 2-4 component vectors, float4x4)");
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

        if (member.offset < previousEnd) {
            return Reject(reason, "shader parameter members must be in offset order without "
                                  "overlap");
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
        HashName(hasher, layout.name);
        HashU8(hasher, layout.rootConstants ? 1 : 0);
        HashU32(hasher, layout.totalBytes);
        HashU32(hasher, static_cast<uint32>(layout.members.Size()));
        for (const ShaderParameterMember& member : layout.members) {
            HashName(hasher, member.name);
            HashU8(hasher, static_cast<uint8>(member.type));
            HashU32(hasher, member.offset);
            HashU32(hasher, member.arrayCount);
        }
    }

    return hasher.Finish();
}

} // namespace rocks::shader
