#include <Core/Move.h>
#include <RenderBase/ShaderParameters.h>
#include <RenderBase/ShaderResourceMap.h>

// 자원 지도(Docs/Design/RenderBase 9.13절 5항, ShaderSystem 16.2절 SSD64)다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::render {

namespace shader_resource_map_detail {

using shader::ShaderParameterResourceKind;
using shader::ShaderResourceKind;
using shader::ShaderResourceShape;

// 공유 샘플러 예약 이름이다(ShaderSystem 7.2절 9항). 생성 표 지도의 공유 샘플러 항목 이름은 이
// 정적 문자열을 가리킨다(단계 layout의 이름은 셰이더 집합 byte를 가리키므로 쓰지 않는다).
constexpr StringView SharedSamplerNames[] = {"SharedWrapSampler", "SharedClampSampler"};

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

// 셰이더 자원이 자원 표의 어느 종류에 드는가. 지도 밖 자원(상수 버퍼, writes가 거짓이면 쓰기
// 자원)은 거짓이다.
bool KindOf(const shader::ShaderBindingResource& resource, bool writes,
            ResourceParameterKind& out) noexcept {
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
    case ShaderResourceKind::StorageImage:
        out = resource.shape == ShaderResourceShape::TypedBuffer
                  ? ResourceParameterKind::WritableBuffer
                  : ResourceParameterKind::WritableTexture;
        return writes;
    case ShaderResourceKind::StorageBufferReadWrite:
        out = ResourceParameterKind::WritableBuffer;
        return writes;
    case ShaderResourceKind::ConstantBuffer:
        break;
    }

    return false;
}

bool IsKnownKind(ResourceParameterKind kind) noexcept {
    return kind == ResourceParameterKind::Texture || kind == ResourceParameterKind::Sampler ||
           kind == ResourceParameterKind::BufferView;
}

// 자원 멤버 종류의 지도 항목 종류다(셰이더에 묶이는 멤버만).
ResourceParameterKind EntryKindOf(const shader::ShaderParameterResource& resource) noexcept {
    switch (resource.kind) {
    case ShaderParameterResourceKind::DeviceBuffer:
    case ShaderParameterResourceKind::GraphBufferSrv:
        return ResourceParameterKind::BufferView;
    case ShaderParameterResourceKind::GraphTextureUav:
        return ResourceParameterKind::WritableTexture;
    case ShaderParameterResourceKind::GraphBufferUav:
        return ResourceParameterKind::WritableBuffer;
    default:
        break;
    }

    return ResourceParameterKind::Texture;
}

// 이름 표 slots에서 name의 slot이다. 없으면 MaxPipelineNamedSlots다.
uint32 NamedSlot(const StringView (&slots)[graphics::MaxPipelineNamedSlots],
                 StringView name) noexcept {
    for (uint32 slot = 0; slot < graphics::MaxPipelineNamedSlots; ++slot) {
        if (slots[slot] == name) {
            return slot;
        }
    }

    return graphics::MaxPipelineNamedSlots;
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
    case ResourceParameterKind::WritableTexture:
        return "WritableTexture";
    case ResourceParameterKind::WritableBuffer:
        return "WritableBuffer";
    }

    return "unknown";
}

const char* ShaderResourceMap::Place(usize entry, graphics::ShaderStage stage,
                                     const shader::ShaderBindingResource& resource,
                                     const graphics::PipelineBindingNames& names) noexcept {
    ResourcePlacement placement;
    placement.stage = stage;
    placement.slot = resource.slot;
    placement.shape = resource.shape;
    placement.component = resource.component;
    placement.structureStride = resource.structureStride;
    placement.sharedSampler = resource.sharedSampler;
    // 텍스처·샘플러 slot은 값 1의 backend 위치 그대로다(Vulkan binding은 종류를 이어 매겨 이름 표
    // 상한보다 클 수 있다). 버퍼 view·쓰기 자원만 이름 표 slot이다.
    bool named = true;
    switch (table_[entry].kind) {
    case ResourceParameterKind::BufferView:
        placement.slot = NamedSlot(names.bufferViews, resource.name);
        break;
    case ResourceParameterKind::WritableTexture:
        placement.slot = NamedSlot(names.writableTextures, resource.name);
        break;
    case ResourceParameterKind::WritableBuffer:
        placement.slot = NamedSlot(names.writableBuffers, resource.name);
        break;
    case ResourceParameterKind::Texture:
    case ResourceParameterKind::Sampler:
        named = false;
        break;
    }

    if (named && placement.slot == graphics::MaxPipelineNamedSlots) {
        return "a buffer view or writable resource of the resource table is not in the pipeline "
               "binding names";
    }

    uint8& count = counts_[entry];
    if (count == MaxResourcePlacements) {
        return "a resource table entry is read by more than two stages";
    }

    placements_[entry * MaxResourcePlacements + count] = placement;
    ++count;
    return nullptr;
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
            if (!KindOf(resource, false, kind)) {
                continue;
            }

            auto found = map.Find(resource.name);
            if (!found) {
                return Reject(reason, "a shader texture, sampler or buffer view is not in the "
                                      "resource table (layout and table differ)");
            }

            if (table[found.Value()].kind != kind) {
                return Reject(reason, "a resource table entry and the shader resource of the "
                                      "same name differ in kind");
            }

            if (const char* failed = map.Place(found.Value(), stage, resource, names);
                failed != nullptr) {
                return Reject(reason, failed);
            }
        }
    }

    for (usize index = 0; index < table.Size(); ++index) {
        if (map.counts_[index] == 0) {
            return Reject(reason, "a resource table entry is used by no shader stage");
        }
    }

    return Result<ShaderResourceMap>::Success(Move(map));
}

Result<ShaderResourceMap>
ShaderResourceMap::Build(Allocator& allocator, const shader::ShaderParameterLayout& layout,
                         Span<const shader::ShaderBindingLayout* const> stages,
                         const graphics::PipelineBindingNames& names,
                         const char** reason) noexcept {
    const char* invalid = nullptr;
    if (layout.structId == 0 || !shader::ValidateShaderParameterLayout(layout, &invalid)) {
        return Reject(reason, invalid != nullptr ? invalid
                                                 : "the resource map needs a generated parameter "
                                                   "struct layout (ShaderParameterLayoutOf<T>())");
    }

    ShaderResourceMap map(allocator);
    map.layout_ = &layout;
    // 표 항목: 셰이더에 묶이는 자원 멤버와 그 짝 샘플러.
    for (const shader::ShaderParameterResource& resource : layout.resources) {
        if (!shader::IsShaderBoundResource(resource.kind)) {
            continue;
        }

        ResourceParameter entry;
        entry.name = resource.name;
        entry.kind = EntryKindOf(resource);
        entry.memberOffset = resource.offset;
        entry.memberKind = resource.kind;
        if (!map.entries_.TryPushBack(entry)) {
            return Result<ShaderResourceMap>::Failure(ErrorCode::OutOfMemory);
        }

        if (!resource.samplerName.IsEmpty()) {
            entry.name = resource.samplerName;
            entry.kind = ResourceParameterKind::Sampler;
            if (!map.entries_.TryPushBack(entry)) {
                return Result<ShaderResourceMap>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    // 단계가 읽는 공유 샘플러(멤버 없음)다.
    for (const StringView shared : SharedSamplerNames) {
        bool read = false;
        for (const shader::ShaderBindingLayout* stage : stages) {
            for (usize index = 0; stage != nullptr && index < stage->resources.Size(); ++index) {
                const shader::ShaderBindingResource& resource = stage->resources[index];
                read = read || (resource.kind == ShaderResourceKind::Sampler &&
                                resource.sharedSampler && resource.name == shared);
            }
        }

        ResourceParameter entry;
        entry.name = shared;
        entry.kind = ResourceParameterKind::Sampler;
        if (read && !map.entries_.TryPushBack(entry)) {
            return Result<ShaderResourceMap>::Failure(ErrorCode::OutOfMemory);
        }
    }

    map.table_ = map.entries_.View();
    const usize count = map.entries_.Size();
    if (!map.placements_.TryResize(count * MaxResourcePlacements) ||
        !map.counts_.TryResize(count)) {
        return Result<ShaderResourceMap>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize index = 0; index < count; ++index) {
        map.counts_[index] = 0;
    }

    for (const shader::ShaderBindingLayout* stageLayout : stages) {
        if (stageLayout == nullptr) {
            continue;
        }

        const graphics::ShaderStage stage = ShaderStageOfBit(stageLayout->stage);
        for (const shader::ShaderBindingResource& resource : stageLayout->resources) {
            ResourceParameterKind kind = ResourceParameterKind::Texture;
            if (!KindOf(resource, true, kind)) {
                continue;
            }

            const bool member = resource.structId == layout.structId &&
                                resource.memberOffset != shader::NoShaderParameterMember;
            const bool write = kind == ResourceParameterKind::WritableTexture ||
                               kind == ResourceParameterKind::WritableBuffer;
            if (!member && write) {
                // 멤버가 아닌 쓰기 자원(PS 진단 버퍼 등)은 지도 밖이다.
                continue;
            }

            usize found = count;
            usize sameOffset = count;
            for (usize index = 0; index < count; ++index) {
                const ResourceParameter& entry = map.entries_[index];
                if (resource.sharedSampler) {
                    found = !entry.HasMember() && entry.name == resource.name ? index : found;
                    continue;
                }

                if (member && entry.memberOffset == resource.memberOffset) {
                    sameOffset = index;
                    found = entry.kind == kind ? index : found;
                }
            }

            if (found == count && sameOffset != count) {
                return Reject(reason, "a resource member and the shader resource at its offset "
                                      "differ in kind");
            }

            if (found == count) {
                return Reject(reason, "a shader texture, sampler or buffer view is not a member of "
                                      "the parameter struct");
            }

            if (const char* failed = map.Place(found, stage, resource, names); failed != nullptr) {
                return Reject(reason, failed);
            }
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
