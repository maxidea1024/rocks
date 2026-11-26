#include <Core/Move.h>
#include <RenderBase/ShaderParameters.h>
#include <RenderBase/ShaderResourceMap.h>

// 자원 지도(Docs/Design/RenderBase 9.13절 5항)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace shader_resource_map_detail {

using shader::ShaderResourceKind;
using shader::ShaderResourceShape;

Result<ShaderResourceMap> Reject(const char** reason, const char* text) noexcept {
    if (reason != nullptr) {
        *reason = text;
    }

    return Result<ShaderResourceMap>::Failure(ErrorCode::InvalidArgument);
}

bool IsTextureShape(ShaderResourceShape shape) noexcept {
    return shape >= ShaderResourceShape::Texture1D &&
           shape <= ShaderResourceShape::TextureCubeArray;
}

// 셰이더 자원이 자원 표의 어느 종류에 드는가. 지도 밖 자원(상수 버퍼·쓰기 자원)은 거짓이다.
bool KindOf(const shader::ShaderBindingResource& resource, ResourceParameterKind& out) noexcept {
    switch (resource.kind) {
    case ShaderResourceKind::SampledTexture:
        out = IsTextureShape(resource.shape) ? ResourceParameterKind::Texture
                                             : ResourceParameterKind::BufferView;
        return true;
    case ShaderResourceKind::StorageBufferRead:
        out = ResourceParameterKind::BufferView;
        return true;
    case ShaderResourceKind::Sampler:
        out = ResourceParameterKind::Sampler;
        return true;
    case ShaderResourceKind::ConstantBuffer:
    case ShaderResourceKind::StorageBufferReadWrite:
    case ShaderResourceKind::StorageImage:
        break;
    }

    return false;
}

bool IsKnownKind(ResourceParameterKind kind) noexcept {
    return kind == ResourceParameterKind::Texture || kind == ResourceParameterKind::Sampler ||
           kind == ResourceParameterKind::BufferView;
}

} // namespace shader_resource_map_detail

using namespace shader_resource_map_detail;

const char* ResourceParameterKindName(ResourceParameterKind kind) noexcept {
    switch (kind) {
    case ResourceParameterKind::Texture:
        return "Texture";
    case ResourceParameterKind::Sampler:
        return "Sampler";
    case ResourceParameterKind::BufferView:
        return "BufferView";
    }

    return "unknown";
}

Result<ShaderResourceMap>
ShaderResourceMap::Build(Allocator& allocator, Span<const ResourceParameter> table,
                         Span<const shader::ShaderBindingLayout* const> stages,
                         const graphics::PipelineBindingNames& names,
                         const char** reason) noexcept {
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name.IsEmpty() || !IsKnownKind(table[index].kind)) {
            return Reject(reason, "a resource table entry has an empty name or an unknown kind");
        }

        for (usize other = 0; other < index; ++other) {
            if (table[other].name == table[index].name) {
                return Reject(reason, "the resource table names one shader resource twice");
            }
        }
    }

    ShaderResourceMap map(allocator);
    map.table_ = table;
    if (!map.placements_.TryResize(table.Size() * MaxResourcePlacements) ||
        !map.counts_.TryResize(table.Size())) {
        return Result<ShaderResourceMap>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize index = 0; index < table.Size(); ++index) {
        map.counts_[index] = 0;
    }

    for (const shader::ShaderBindingLayout* layout : stages) {
        if (layout == nullptr) {
            continue;
        }

        const graphics::ShaderStage stage = ShaderStageOfBit(layout->stage);
        for (const shader::ShaderBindingResource& resource : layout->resources) {
            ResourceParameterKind kind = ResourceParameterKind::Texture;
            if (!KindOf(resource, kind)) {
                continue;
            }

            auto found = map.Find(resource.name);
            if (!found) {
                return Reject(reason, "a shader texture, sampler or buffer view is not in the "
                                      "resource table (layout and table differ)");
            }

            const usize entry = found.Value();
            if (table[entry].kind != kind) {
                return Reject(reason, "a resource table entry and the shader resource of the "
                                      "same name differ in kind");
            }

            ResourcePlacement placement;
            placement.stage = stage;
            placement.slot = resource.slot;
            placement.shape = resource.shape;
            placement.component = resource.component;
            placement.structureStride = resource.structureStride;
            placement.sharedSampler = resource.sharedSampler;
            if (kind == ResourceParameterKind::BufferView) {
                placement.slot = graphics::MaxPipelineNamedSlots;
                for (uint32 slot = 0; slot < graphics::MaxPipelineNamedSlots; ++slot) {
                    if (names.bufferViews[slot] == resource.name) {
                        placement.slot = slot;
                    }
                }

                if (placement.slot == graphics::MaxPipelineNamedSlots) {
                    return Reject(reason, "a buffer view of the resource table is not in the "
                                          "pipeline binding names");
                }
            }

            uint8& count = map.counts_[entry];
            if (count == MaxResourcePlacements) {
                return Reject(reason, "a resource table entry is read by more than two stages");
            }

            map.placements_[entry * MaxResourcePlacements + count] = placement;
            ++count;
        }
    }

    for (usize index = 0; index < table.Size(); ++index) {
        if (map.counts_[index] == 0) {
            return Reject(reason, "a resource table entry is used by no shader stage");
        }
    }

    return Result<ShaderResourceMap>::Success(Move(map));
}

Span<const ResourcePlacement> ShaderResourceMap::PlacementsOf(usize parameter) const noexcept {
    if (parameter >= counts_.Size()) {
        return {};
    }

    return Span<const ResourcePlacement>(placements_.Data() + parameter * MaxResourcePlacements,
                                         counts_[parameter]);
}

Result<usize> ShaderResourceMap::Find(StringView name) const noexcept {
    for (usize index = 0; index < table_.Size(); ++index) {
        if (table_[index].name == name) {
            return Result<usize>::Success(index);
        }
    }

    return Result<usize>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::render
