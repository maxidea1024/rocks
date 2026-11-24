#include <ShaderCore/ShaderBindingLayout.h>

#include "ShaderCoreInternal.h"

#include <Core/Algorithms.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 셰이더 바인딩 layout v1(Docs/Design/ShaderSystem 7.2절 3항, 12.2.1절 SSD26)이다. byte 형식과
// 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace binding_layout_detail {

constexpr const char* KindNames[ShaderResourceKindCount] = {
    "ConstantBuffer",         "SampledTexture", "StorageBufferRead",
    "StorageBufferReadWrite", "StorageImage",   "Sampler"};

constexpr uint32 AttributeBits = (1u << MaxShaderVertexAttributes) - 1;
constexpr uint32 RenderTargetBits = (1u << MaxShaderRenderTargets) - 1;
constexpr uint32 InterStageBits = (1u << MaxShaderInterStageVariables) - 1;

bool IsName(StringView name) noexcept {
    return core_detail::IsShaderIdentifier(name, MaxShaderBindingNameBytes);
}

// 읽기·쓰기 공통 규칙이다. 맞으면 null, 아니면 영어 이유다.
const char* CheckLayout(const ShaderBindingLayout& layout) noexcept {
    if (!core_detail::IsSingleShaderStage(layout.stage)) {
        return "binding layout stage must be exactly one stage bit";
    }

    if (layout.rootConstantBytes > MaxShaderRootConstantBytes ||
        layout.rootConstantBytes % ShaderConstantSlotBytes != 0) {
        return "binding layout root constants must be a multiple of 16 bytes of at most 128";
    }

    if ((layout.attributeMask & ~AttributeBits) != 0 ||
        (layout.attributeMask != 0 && layout.stage != ShaderStageVertex)) {
        return "binding layout attribute mask is only for vertex shaders (ATTRIBUTE0-15)";
    }

    if ((layout.renderTargetMask & ~RenderTargetBits) != 0 ||
        (layout.renderTargetMask != 0 && layout.stage != ShaderStagePixel)) {
        return "binding layout render target mask is only for pixel shaders (SV_Target0-7)";
    }

    if ((layout.interStageMask & ~InterStageBits) != 0 ||
        (layout.interStageMask != 0 && layout.stage == ShaderStageCompute)) {
        return "binding layout inter-stage mask is only for vertex and pixel shaders "
               "(TEXCOORD0-15)";
    }

    if (layout.resources.Size() > MaxShaderBindingResources) {
        return "binding layout has more than 1024 resources";
    }

    uint64 nextMember = 0;
    for (usize index = 0; index < layout.resources.Size(); ++index) {
        const ShaderBindingResource& resource = layout.resources[index];
        if (static_cast<uint32>(resource.kind) >= ShaderResourceKindCount) {
            return "binding layout resource kind is outside the vocabulary";
        }

        if (index > 0) {
            const ShaderBindingResource& previous = layout.resources[index - 1];
            const bool ordered = previous.kind < resource.kind ||
                                 (previous.kind == resource.kind && previous.slot < resource.slot);
            if (!ordered) {
                return "binding layout resources must be in (kind, slot) order without "
                       "duplicates";
            }
        }

        if (!IsName(resource.name)) {
            return "binding layout resource name must be an identifier of at most 64 bytes";
        }

        const bool constantBuffer = resource.kind == ShaderResourceKind::ConstantBuffer;
        if (constantBuffer) {
            if (resource.constantBufferBytes == 0 ||
                resource.constantBufferBytes % ShaderConstantSlotBytes != 0 ||
                resource.constantBufferBytes > MaxShaderConstantBufferBytes) {
                return "binding layout constant buffer size must be a non-zero multiple of 16 "
                       "bytes of at most 65536";
            }

            if (resource.memberCount > MaxShaderBindingMembers) {
                return "binding layout constant buffer has more than 256 members";
            }
        } else if (resource.constantBufferBytes != 0 || resource.memberCount != 0) {
            return "binding layout size and members are only for constant buffers";
        }

        if (static_cast<uint32>(resource.samplerSource) >= ShaderSamplerSourceCount ||
            (resource.samplerSource != ShaderSamplerSource::None &&
             resource.kind != ShaderResourceKind::SampledTexture)) {
            return "binding layout sampler source is only for sampled textures";
        }

        if (resource.sharedSampler && resource.kind != ShaderResourceKind::Sampler) {
            return "binding layout shared sampler flag is only for samplers";
        }

        if (resource.firstMember != nextMember) {
            return "binding layout members must follow resource order";
        }

        nextMember += resource.memberCount;
        if (nextMember > layout.members.Size()) {
            return "binding layout member range is outside the member table";
        }

        uint64 previousEnd = 0;
        for (uint32 member = 0; member < resource.memberCount; ++member) {
            const ShaderBindingMember& value = layout.members[resource.firstMember + member];
            if (!IsName(value.name)) {
                return "binding layout member name must be an identifier of at most 64 bytes";
            }

            if (value.bytes == 0 || value.offset < previousEnd) {
                return "binding layout members must be non-empty, in offset order and must not "
                       "overlap";
            }

            previousEnd = static_cast<uint64>(value.offset) + value.bytes;
            if (previousEnd > resource.constantBufferBytes) {
                return "binding layout member ends past its constant buffer";
            }
        }
    }

    if (nextMember != layout.members.Size()) {
        return "binding layout member table has members no resource owns";
    }

    return nullptr;
}

void WriteName(ByteWriter& writer, StringView name) noexcept {
    writer.WriteU8(static_cast<uint8>(name.ByteCount()));
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(name.Data()), name.ByteCount()));
}

StringView ReadName(ByteReader& reader) noexcept {
    const uint8 bytes = reader.ReadU8();
    const Span<const uint8> name = reader.Take(bytes);
    return StringView(reinterpret_cast<const char*>(name.Data()), name.Size());
}

} // namespace binding_layout_detail

Span<const ShaderBindingMember>
ShaderBindingLayout::MembersOf(const ShaderBindingResource& resource) const noexcept {
    const uint64 end = static_cast<uint64>(resource.firstMember) + resource.memberCount;
    if (end > members.Size()) {
        return {};
    }

    return members.View().Slice(resource.firstMember, resource.memberCount);
}

const ShaderBindingResource* ShaderBindingLayout::Find(ShaderResourceKind kind,
                                                       uint16 slot) const noexcept {
    struct Key {
        ShaderResourceKind kind;
        uint16 slot;
    };

    const auto less = [](const auto& a, const auto& b) noexcept {
        return a.kind < b.kind || (a.kind == b.kind && a.slot < b.slot);
    };
    const usize index = algo::LowerBound(resources.View(), Key{kind, slot}, less);
    if (index == resources.Size() || resources[index].kind != kind ||
        resources[index].slot != slot) {
        return nullptr;
    }

    return &resources[index];
}

Result<void> EncodeShaderBindingLayout(const ShaderBindingLayout& layout,
                                       Array<uint8>& out) noexcept {
    using namespace binding_layout_detail;
    if (CheckLayout(layout) != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize start = out.Size();
    ByteWriter writer(out);
    writer.WriteU32LE(ShaderBindingLayoutVersion);
    writer.WriteU32LE(layout.stage);
    writer.WriteU32LE(layout.rootConstantBytes);
    writer.WriteU32LE(layout.attributeMask);
    writer.WriteU32LE(layout.renderTargetMask);
    writer.WriteU32LE(layout.interStageMask);
    writer.WriteU32LE(static_cast<uint32>(layout.resources.Size()));
    for (const ShaderBindingResource& resource : layout.resources) {
        writer.WriteU8(static_cast<uint8>(resource.kind));
        writer.WriteU16LE(resource.slot);
        WriteName(writer, resource.name);
        writer.WriteU32LE(resource.constantBufferBytes);
        writer.WriteU8(static_cast<uint8>(resource.samplerSource));
        writer.WriteU8(resource.sharedSampler ? 1 : 0);
        writer.WriteU16LE(static_cast<uint16>(resource.memberCount));
        for (const ShaderBindingMember& member : layout.MembersOf(resource)) {
            WriteName(writer, member.name);
            writer.WriteU32LE(member.offset);
            writer.WriteU32LE(member.bytes);
        }
    }

    if (auto written = writer.Status(); !written) {
        (void)out.TryResize(start);
        return written;
    }

    return Result<void>::Success();
}

ShaderReadStatus DecodeShaderBindingLayout(Span<const uint8> bytes,
                                           ShaderBindingLayout& out) noexcept {
    using namespace binding_layout_detail;
    out.resources.Clear();
    out.members.Clear();
    ByteReader reader(bytes);
    const uint32 version = reader.ReadU32LE();
    if (reader.Failed()) {
        return ShaderReadStatus::Corrupt("binding layout is truncated");
    }

    if (version != ShaderBindingLayoutVersion) {
        return ShaderReadStatus::Unsupported("binding layout version is not 2");
    }

    out.stage = reader.ReadU32LE();
    out.rootConstantBytes = reader.ReadU32LE();
    out.attributeMask = reader.ReadU32LE();
    out.renderTargetMask = reader.ReadU32LE();
    out.interStageMask = reader.ReadU32LE();
    const uint32 resourceCount = reader.ReadU32LE();
    if (reader.Failed() || resourceCount > MaxShaderBindingResources) {
        return ShaderReadStatus::Corrupt("binding layout header is truncated or out of range");
    }

    if (!out.resources.TryReserve(resourceCount)) {
        return ShaderReadStatus::Corrupt("out of memory reading the binding layout");
    }

    for (uint32 index = 0; index < resourceCount; ++index) {
        ShaderBindingResource resource;
        resource.kind = static_cast<ShaderResourceKind>(reader.ReadU8());
        resource.slot = reader.ReadU16LE();
        resource.name = ReadName(reader);
        resource.constantBufferBytes = reader.ReadU32LE();
        const uint8 source = reader.ReadU8();
        const uint8 shared = reader.ReadU8();
        resource.memberCount = reader.ReadU16LE();
        if (reader.Failed() || shared > 1 || source >= ShaderSamplerSourceCount ||
            resource.memberCount > MaxShaderBindingMembers) {
            return ShaderReadStatus::Corrupt("binding layout resource is truncated or out of "
                                             "range");
        }

        resource.samplerSource = static_cast<ShaderSamplerSource>(source);
        resource.sharedSampler = shared != 0;
        resource.firstMember = static_cast<uint32>(out.members.Size());
        for (uint32 member = 0; member < resource.memberCount; ++member) {
            ShaderBindingMember value;
            value.name = ReadName(reader);
            value.offset = reader.ReadU32LE();
            value.bytes = reader.ReadU32LE();
            if (reader.Failed()) {
                return ShaderReadStatus::Corrupt("binding layout member is truncated");
            }

            if (!out.members.TryPushBack(value)) {
                return ShaderReadStatus::Corrupt("out of memory reading the binding layout");
            }
        }

        (void)out.resources.TryPushBack(resource);
    }

    if (!reader.AtEnd()) {
        return ShaderReadStatus::Corrupt("binding layout has trailing bytes");
    }

    if (const char* broken = CheckLayout(out); broken != nullptr) {
        return ShaderReadStatus::Corrupt(broken);
    }

    return ShaderReadStatus::Success();
}

bool CheckShaderStageLink(const ShaderBindingLayout& vertex, const ShaderBindingLayout& pixel,
                          const char** reason) noexcept {
    const auto fail = [&](const char* why) noexcept {
        if (reason != nullptr) {
            *reason = why;
        }

        return false;
    };
    if (vertex.stage != ShaderStageVertex || pixel.stage != ShaderStagePixel) {
        return fail("stage link needs a vertex and a pixel binding layout");
    }

    if (pixel.interStageMask == 0) {
        return true;
    }

    uint32 top = 0;
    for (uint32 index = 0; index < MaxShaderInterStageVariables; ++index) {
        top = (pixel.interStageMask & (1u << index)) != 0 ? index : top;
    }

    const uint32 prefix = (1u << (top + 1)) - 1;
    if ((vertex.interStageMask & prefix) != pixel.interStageMask) {
        return fail(
            "pixel shader inputs must be the leading TEXCOORDn outputs of the vertex shader "
            "(ShaderSystem 7.2 item 7)");
    }

    return true;
}

const char* ShaderResourceKindName(ShaderResourceKind kind) noexcept {
    using namespace binding_layout_detail;
    const uint32 index = static_cast<uint32>(kind);
    return index < ShaderResourceKindCount ? KindNames[index] : "unknown";
}

} // namespace rocks::shader
