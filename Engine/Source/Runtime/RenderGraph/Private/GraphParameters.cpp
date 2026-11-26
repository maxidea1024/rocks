#include "RenderGraphInternal.h"

#include <RenderBase/ResourceParameters/DeviceBufferParameter.h>
#include <RenderBase/ResourceParameters/DeviceTextureParameter.h>
#include <RenderGraph/GraphParameterCollector.h>
#include <RenderGraph/Parameters/GraphBufferAccessParameter.h>
#include <RenderGraph/Parameters/GraphBufferSrvParameter.h>
#include <RenderGraph/Parameters/GraphBufferUavParameter.h>
#include <RenderGraph/Parameters/GraphRenderTargetParameter.h>
#include <RenderGraph/Parameters/GraphTextureAccessParameter.h>
#include <RenderGraph/Parameters/GraphTextureParameter.h>
#include <RenderGraph/Parameters/GraphTextureSrvParameter.h>
#include <RenderGraph/Parameters/GraphTextureUavParameter.h>
#include <RenderGraph/RenderGraph.h>

// 렌더 그래프 파라미터 구조체 경로(Docs/Design/ShaderSystem 16.2절 SSD65)다: 수집기, 구조체 판
// AddPass의 검사, pass 문맥의 SetParameters. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace graph_parameters_detail {

using shader::ShaderParameterResource;
using shader::ShaderParameterResourceKind;

Result<void> Fail(const char** reason, const char* text) noexcept {
    if (reason != nullptr) {
        *reason = text;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 멤버 값을 읽는다(틀은 값 배치 구조체를 첫 기반으로 가진 표준 배치 타입이다 — SH3b 구현 메모 2).
template <class Value>
const Value& MemberAt(const void* parameters, uint32 offset) noexcept {
    return *reinterpret_cast<const Value*>(static_cast<const uint8*>(parameters) + offset);
}

// 셰이더가 그 멤버(오프셋)를 쓰는가. 지도가 없으면 쓴다고 본다.
bool IsUsed(const ShaderResourceMap* used, uint32 offset) noexcept {
    if (used == nullptr) {
        return true;
    }

    const Span<const ResourceParameter> table = used->Table();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].memberOffset == offset && !used->PlacementsOf(index).IsEmpty()) {
            return true;
        }
    }

    return false;
}

graphics::ResourceAccess SrvAccessOf(GraphPassKind kind) noexcept {
    return kind == GraphPassKind::Raster ? graphics::ResourceAccess::SrvGraphics
                                         : graphics::ResourceAccess::SrvCompute;
}

// trace 이름의 문자열이다(없으면 빈 문자열).
const char* PassName(TraceScopeName& name) noexcept {
    return name.Name() != nullptr ? name.Name() : "";
}

bool IsComputeKind(GraphPassKind kind) noexcept {
    return kind == GraphPassKind::Compute || kind == GraphPassKind::AsyncCompute;
}

// 셰이더 멤버 하나의 handle이 null인가.
bool IsNullMember(const ShaderParameterResource& resource, const void* parameters) noexcept {
    switch (resource.kind) {
    case ShaderParameterResourceKind::GraphTexture:
        return MemberAt<GraphTextureValue>(parameters, resource.offset).texture.IsNull();
    case ShaderParameterResourceKind::GraphTextureSrv:
        return MemberAt<GraphTextureSrvValue>(parameters, resource.offset).view.IsNull();
    case ShaderParameterResourceKind::GraphTextureUav:
        return MemberAt<GraphTextureUavValue>(parameters, resource.offset).view.IsNull();
    case ShaderParameterResourceKind::GraphBufferSrv:
        return MemberAt<GraphBufferSrvValue>(parameters, resource.offset).view.IsNull();
    case ShaderParameterResourceKind::GraphBufferUav:
        return MemberAt<GraphBufferUavValue>(parameters, resource.offset).view.IsNull();
    default:
        break;
    }

    return true;
}

// 셰이더에 묶이는 그래프 멤버 하나를 접근으로 넣는다.
Result<void> CollectShaderMember(GraphPassAccesses& out, GraphPassKind kind,
                                 const ShaderParameterResource& resource, const void* parameters,
                                 const ShaderResourceMap* used, const char** reason) noexcept {
    const bool null = IsNullMember(resource, parameters);
    if (!IsUsed(used, resource.offset)) {
        return Result<void>::Success();
    }

    if (null) {
        // 지도 없이 넘긴 null 멤버는 의존이 아니다(UE도 null 멤버는 건너뛴다). 셰이더가 읽는다고
        // 지도가 말하면 오류다.
        return used == nullptr ? Result<void>::Success()
                               : Fail(reason, "a graph resource member read by the shader is "
                                              "null");
    }

    if (kind == GraphPassKind::Copy) {
        return Fail(reason, "copy passes take no shader resource members");
    }

    const bool uav = resource.kind == ShaderParameterResourceKind::GraphTextureUav ||
                     resource.kind == ShaderParameterResourceKind::GraphBufferUav;
    if (uav && !IsComputeKind(kind)) {
        return Fail(reason, "only compute passes write UAV members (ShaderSystem 7.2 item 4)");
    }

    const graphics::ResourceAccess srv = SrvAccessOf(kind);
    const graphics::ResourceAccess write = graphics::ResourceAccess::UavCompute;
    switch (resource.kind) {
    case ShaderParameterResourceKind::GraphTexture:
        out.Texture(MemberAt<GraphTextureValue>(parameters, resource.offset).texture, srv);
        break;
    case ShaderParameterResourceKind::GraphTextureSrv:
        out.TextureView(MemberAt<GraphTextureSrvValue>(parameters, resource.offset).view, srv);
        break;
    case ShaderParameterResourceKind::GraphTextureUav:
        out.TextureView(MemberAt<GraphTextureUavValue>(parameters, resource.offset).view, write);
        break;
    case ShaderParameterResourceKind::GraphBufferSrv:
        out.BufferView(MemberAt<GraphBufferSrvValue>(parameters, resource.offset).view, srv);
        break;
    case ShaderParameterResourceKind::GraphBufferUav:
        out.BufferView(MemberAt<GraphBufferUavValue>(parameters, resource.offset).view, write);
        break;
    default:
        break;
    }

    return Result<void>::Success();
}

Result<void> CollectRenderTargets(GraphPassAccesses& out, GraphPassKind kind,
                                  const GraphRenderTargetParameter& targets,
                                  const char** reason) noexcept {
    if (targets.colorCount == 0 && !targets.hasDepth) {
        return Result<void>::Success();
    }

    if (kind != GraphPassKind::Raster) {
        return Fail(reason, "only raster passes take render target members");
    }

    if (targets.colorCount > graphics::MaxColorTargets) {
        return Fail(reason, "a render target member has more than 8 color targets");
    }

    for (uint32 index = 0; index < targets.colorCount; ++index) {
        out.ColorTarget(targets.colors[index]);
    }

    if (targets.hasDepth) {
        out.DepthTarget(targets.depth);
    }

    return Result<void>::Success();
}

} // namespace graph_parameters_detail

using namespace graph_parameters_detail;

Result<void> CollectGraphPassAccesses(GraphPassAccesses& out, GraphPassKind kind,
                                      const shader::ShaderParameterLayout& layout,
                                      const void* parameters, const ShaderResourceMap* used,
                                      const char** reason) noexcept {
    if (layout.structId == 0 || parameters == nullptr) {
        return Fail(reason, "the parameter collector needs a generated parameter struct layout "
                            "and its value");
    }

    if (used != nullptr &&
        (used->Layout() == nullptr || used->Layout()->structId != layout.structId)) {
        return Fail(reason, "the resource map is not built from this parameter struct");
    }

    for (const ShaderParameterResource& resource : layout.resources) {
        Result<void> collected = Result<void>::Success();
        switch (resource.kind) {
        case ShaderParameterResourceKind::DeviceTexture:
        case ShaderParameterResourceKind::DeviceBuffer:
            break;
        case ShaderParameterResourceKind::GraphTexture:
        case ShaderParameterResourceKind::GraphTextureSrv:
        case ShaderParameterResourceKind::GraphTextureUav:
        case ShaderParameterResourceKind::GraphBufferSrv:
        case ShaderParameterResourceKind::GraphBufferUav:
            collected = CollectShaderMember(out, kind, resource, parameters, used, reason);
            break;
        case ShaderParameterResourceKind::GraphTextureAccess: {
            const auto& access = MemberAt<GraphTextureAccessParameter>(parameters, resource.offset);
            if (!access.texture.IsNull()) {
                out.Texture(access.texture, access.access, access.range, access.flags);
            }

            break;
        }
        case ShaderParameterResourceKind::GraphBufferAccess: {
            const auto& access = MemberAt<GraphBufferAccessParameter>(parameters, resource.offset);
            if (!access.buffer.IsNull()) {
                out.Buffer(access.buffer, access.access, access.flags);
            }

            break;
        }
        case ShaderParameterResourceKind::RenderTargets:
            collected = CollectRenderTargets(
                out, kind, MemberAt<GraphRenderTargetParameter>(parameters, resource.offset),
                reason);
            break;
        }

        if (!collected) {
            return collected;
        }
    }

    if (out.Error() != ErrorCode::None) {
        return Result<void>::Failure(out.Error());
    }

    return Result<void>::Success();
}

Result<void> RenderGraph::CollectParameterAccesses(TraceScopeName& name, GraphPassKind kind,
                                                   const shader::ShaderParameterLayout& layout,
                                                   const void* parameters,
                                                   const ShaderResourceMap* used,
                                                   GraphPassAccesses& out) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    detail::GraphState& state = *state_;
    if (state.executed) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const char* reason = "";
    auto collected = CollectGraphPassAccesses(out, kind, layout, parameters, used, &reason);
    if (!collected) {
        if (collected.Error() != ErrorCode::InvalidArgument) {
            return collected;
        }

        return Result<void>::Failure(detail::RecordGraphError(state, ErrorCode::InvalidArgument,
                                                              true, "pass '{}' parameters: {}",
                                                              PassName(name), reason));
    }

    return Result<void>::Success();
}

Result<void> GraphPassContext::SetParameters(const ShaderResourceMap& map,
                                             const void* parameters) const noexcept {
    if (map.Layout() == nullptr || parameters == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Span<const ResourceParameter> table = map.Table();
    for (usize index = 0; index < table.Size(); ++index) {
        const ResourceParameter& entry = table[index];
        const Span<const ResourcePlacement> placements = map.PlacementsOf(index);
        // 공유 샘플러(멤버 없음)는 호출자가 SetSampler로 준다.
        if (!entry.HasMember() || placements.IsEmpty()) {
            continue;
        }

        graphics::TextureHandle texture;
        graphics::BufferViewHandle view;
        const SamplerOverride* sampler = nullptr;
        switch (entry.memberKind) {
        case ShaderParameterResourceKind::DeviceTexture: {
            const auto& value = MemberAt<DeviceTextureValue>(parameters, entry.memberOffset);
            texture = value.texture;
            sampler = &value.sampler;
            break;
        }
        case ShaderParameterResourceKind::DeviceBuffer:
            view = MemberAt<DeviceBufferValue>(parameters, entry.memberOffset).view;
            break;
        case ShaderParameterResourceKind::GraphTexture: {
            const auto& value = MemberAt<GraphTextureValue>(parameters, entry.memberOffset);
            texture = Texture(value.texture);
            sampler = &value.sampler;
            break;
        }
        case ShaderParameterResourceKind::GraphTextureSrv: {
            const auto& value = MemberAt<GraphTextureSrvValue>(parameters, entry.memberOffset);
            texture = View(value.view);
            sampler = &value.sampler;
            break;
        }
        case ShaderParameterResourceKind::GraphTextureUav:
            texture = View(MemberAt<GraphTextureUavValue>(parameters, entry.memberOffset).view);
            break;
        case ShaderParameterResourceKind::GraphBufferSrv:
            view = View(MemberAt<GraphBufferSrvValue>(parameters, entry.memberOffset).view);
            break;
        case ShaderParameterResourceKind::GraphBufferUav:
            view = View(MemberAt<GraphBufferUavValue>(parameters, entry.memberOffset).view);
            break;
        case ShaderParameterResourceKind::GraphTextureAccess:
        case ShaderParameterResourceKind::GraphBufferAccess:
        case ShaderParameterResourceKind::RenderTargets:
            continue;
        }

        for (const ResourcePlacement& placement : placements) {
            switch (entry.kind) {
            case ResourceParameterKind::Texture:
                commands_.SetTexture(placement.stage, placement.slot, texture);
                break;
            case ResourceParameterKind::Sampler: {
                const graphics::SamplerDesc* desc =
                    sampler != nullptr && sampler->given ? &sampler->desc : nullptr;
                if (auto set = commands_.SetSampler(placement.stage, placement.slot, desc); !set) {
                    return set;
                }

                break;
            }
            case ResourceParameterKind::BufferView:
                commands_.SetBufferView(placement.slot, view);
                break;
            case ResourceParameterKind::WritableTexture:
                commands_.SetWritableTexture(placement.slot, texture);
                break;
            case ResourceParameterKind::WritableBuffer:
                commands_.SetWritableBufferView(placement.slot, view);
                break;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::render
