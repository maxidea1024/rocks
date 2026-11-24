#include <Graphics/ShaderBindings.h>

#include <ShaderCore/ShaderFormatTypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// backend 공용 쿠킹 셰이더 처리(Docs/Design/ShaderSystem 16.1절 SH2e2 메모)다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::graphics {

namespace shader_bindings_detail {

using shader::ShaderComponentType;
using shader::ShaderResourceKind;
using shader::ShaderResourceShape;

constexpr char SamplerSuffix[] = "Sampler";

uint32 StageBit(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return shader::ShaderStageVertex;
    case ShaderStage::Pixel:
        return shader::ShaderStagePixel;
    case ShaderStage::Compute:
        return shader::ShaderStageCompute;
    }

    return 0;
}

Result<void> Reject(char (&error)[PipelineSlotErrorBytes], const char* format, ...) noexcept
    __attribute__((format(printf, 2, 3)));

Result<void> Reject(char (&error)[PipelineSlotErrorBytes], const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, sizeof(error), format, arguments);
    va_end(arguments);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> Fail(const char** reason, const char* text) noexcept {
    if (reason != nullptr) {
        *reason = text;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsTextureShape(ShaderResourceShape shape) noexcept {
    return shape >= ShaderResourceShape::Texture1D &&
           shape <= ShaderResourceShape::TextureCubeArray;
}

PixelShaderType PixelTypeOf(ShaderComponentType component) noexcept {
    switch (component) {
    case ShaderComponentType::Float:
        return PixelShaderType::Float;
    case ShaderComponentType::Int:
        return PixelShaderType::Int;
    case ShaderComponentType::Uint:
        return PixelShaderType::Uint;
    case ShaderComponentType::None:
        break;
    }

    return PixelShaderType::None;
}

// 이름이 slot 표 names[0..count)의 몇 번째인가(없으면 -1).
int32 FindName(const StringView (&names)[MaxPipelineNamedSlots], StringView name) noexcept {
    for (uint32 slot = 0; slot < MaxPipelineNamedSlots; ++slot) {
        if (!names[slot].IsEmpty() && names[slot] == name) {
            return static_cast<int32>(slot);
        }
    }

    return -1;
}

// 이름이 <texture>Sampler인가.
bool IsTextureSampler(StringView texture, StringView name) noexcept {
    const usize suffix = sizeof(SamplerSuffix) - 1;
    return !texture.IsEmpty() && name.ByteCount() == texture.ByteCount() + suffix &&
           memcmp(name.Data(), texture.Data(), texture.ByteCount()) == 0 &&
           memcmp(name.Data() + texture.ByteCount(), SamplerSuffix, suffix) == 0;
}

int NameLength(StringView name) noexcept {
    return static_cast<int>(name.ByteCount());
}

} // namespace shader_bindings_detail

using namespace shader_bindings_detail;

const char* ShaderStageName(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Pixel:
        return "pixel";
    case ShaderStage::Compute:
        return "compute";
    }

    return "unknown";
}

DeviceShaderCode::DeviceShaderCode(Allocator& allocator) noexcept
    : code_(allocator), layoutBytes_(allocator), headerBytes_(allocator), layout_(allocator),
      header_(allocator) {
}

Result<void> DeviceShaderCode::Load(const ShaderCodeDesc& desc, ShaderCodeKind expected,
                                    const char** reason) noexcept {
    if (desc.kind != expected) {
        return Fail(reason, "shader code kind is not the one this device takes");
    }

    if (desc.code.IsEmpty() || (desc.kind == ShaderCodeKind::Spirv && desc.code.Size() % 4 != 0)) {
        return Fail(reason, "shader code is empty (or SPIR-V is not whole 32-bit words)");
    }

    const bool essl = desc.kind == ShaderCodeKind::Essl;
    if (essl == desc.glesBindingHeader.IsEmpty()) {
        return Fail(reason, "the GLES binding header is required for ESSL and only for ESSL");
    }

    if (!code_.TryAppend(desc.code) || !layoutBytes_.TryAppend(desc.bindingLayout) ||
        !headerBytes_.TryAppend(desc.glesBindingHeader)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    stage_ = desc.stage;
    const uint32 stageBit = StageBit(desc.stage);
    const shader::ShaderReadStatus layout =
        shader::DecodeShaderBindingLayout(layoutBytes_.View(), layout_);
    if (!layout) {
        return Fail(reason, layout.reason);
    }

    if (layout_.stage != stageBit) {
        return Fail(reason, "shader binding layout stage differs from the shader stage");
    }

    if (essl) {
        const shader::ShaderReadStatus header =
            shader::DecodeShaderGlesBindingHeader(headerBytes_.View(), header_);
        if (!header) {
            return Fail(reason, header.reason);
        }

        if (header_.stage != stageBit) {
            return Fail(reason, "GLES binding header stage differs from the shader stage");
        }
    }

    return Result<void>::Success();
}

Result<ShaderHandle> DeviceShaderTable::Create(const ShaderCodeDesc& desc, ShaderCodeKind expected,
                                               const char** reason) noexcept {
    DeviceShaderCode code(allocator_);
    if (auto loaded = code.Load(desc, expected, reason); !loaded) {
        return Result<ShaderHandle>::Failure(loaded.Error());
    }

    auto added = shaders_.TryAdd(Move(code));
    if (!added) {
        return Result<ShaderHandle>::Failure(added.Error());
    }

    return Result<ShaderHandle>::Success(ShaderHandle{added.Value()});
}

void DeviceShaderTable::Clear() noexcept {
    shaders_.Clear();
}

Result<void> ResolvePipelineSlots(Span<const DeviceShaderCode* const> stages,
                                  const PipelineBindingNames& names, uint32 drawConstantBytes,
                                  PipelineSlots& out,
                                  char (&error)[PipelineSlotErrorBytes]) noexcept {
    out = PipelineSlots{};
    if (stages.Size() > 2) {
        return Reject(error, "a pipeline has at most two shader stages");
    }

    for (usize index = 0; index < stages.Size(); ++index) {
        const DeviceShaderCode& code = *stages[index];
        const ShaderStage stage = code.Stage();
        const char* stageName = ShaderStageName(stage);
        const shader::ShaderBindingLayout& layout = code.Layout();
        out.rootConstantBytes[index] = layout.rootConstantBytes;
        if (layout.rootConstantBytes > drawConstantBytes) {
            return Reject(error,
                          "%s shader reads %u bytes of draw constants but the pipeline declares "
                          "drawConstantBytes %u",
                          stageName, layout.rootConstantBytes, drawConstantBytes);
        }

        for (const shader::ShaderBindingResource& resource : layout.resources) {
            const StringView name = resource.name;
            PipelineSlotBinding binding;
            binding.stage = stage;
            binding.kind = resource.kind;
            binding.shape = resource.shape;
            binding.location = resource.slot;
            const bool compute = stage == ShaderStage::Compute;
            bool fits = false;
            const char* why = "is not in the pipeline binding names";
            switch (resource.kind) {
            case ShaderResourceKind::ConstantBuffer:
                why = "is a constant buffer (pipelines bind only draw constants until parameter "
                      "batches, RenderBase GX6)";
                break;
            case ShaderResourceKind::SampledTexture: {
                if (!names.texture.IsEmpty() && name == names.texture) {
                    binding.role = PipelineSlotRole::Texture;
                    fits = stage != ShaderStage::Vertex && IsTextureShape(resource.shape);
                    why = stage == ShaderStage::Vertex ? "is a texture read by the vertex shader "
                                                         "(the texture slot is pixel or compute)"
                                                       : "is named as the texture but is a typed "
                                                         "buffer";
                    break;
                }

                const int32 slot = FindName(names.bufferViews, name);
                if (slot < 0) {
                    break;
                }

                binding.role = PipelineSlotRole::BufferView;
                binding.slot = static_cast<uint8>(slot);
                fits = resource.shape == ShaderResourceShape::TypedBuffer &&
                       AddDrawBufferSlot(out.bufferSlots, static_cast<uint32>(slot),
                                         BufferViewKind::Typed, PixelTypeOf(resource.component));
                why = "is named as a buffer view but is not a typed buffer, or another stage reads "
                      "that slot as a different kind";
                break;
            }
            case ShaderResourceKind::Sampler:
                binding.role = PipelineSlotRole::Sampler;
                fits = !resource.sharedSampler && stage != ShaderStage::Vertex &&
                       IsTextureSampler(names.texture, name);
                why = "is not the sampler of the pipeline texture (<texture>Sampler; shared "
                      "samplers need parameter batches)";
                break;
            case ShaderResourceKind::StorageBufferRead: {
                const int32 slot = FindName(names.bufferViews, name);
                if (slot < 0) {
                    break;
                }

                const bool structured = resource.shape == ShaderResourceShape::StructuredBuffer;
                binding.role = PipelineSlotRole::BufferView;
                binding.slot = static_cast<uint8>(slot);
                fits = AddDrawBufferSlot(
                    out.bufferSlots, static_cast<uint32>(slot),
                    structured ? BufferViewKind::Structured : BufferViewKind::Raw,
                    PixelShaderType::None, structured ? resource.structureStride : 0u);
                why = "is read as a different buffer kind by another stage";
                break;
            }
            case ShaderResourceKind::StorageBufferReadWrite:
            case ShaderResourceKind::StorageImage: {
                if (!compute) {
                    why = "is a UAV in a graphics stage (UAVs are compute only)";
                    break;
                }

                const bool texture = IsTextureShape(resource.shape);
                const int32 slot = texture ? FindName(names.writableTextures, name)
                                           : FindName(names.writableBuffers, name);
                if (slot < 0) {
                    break;
                }

                const uint32 bit = 1u << slot;
                binding.slot = static_cast<uint8>(slot);
                if (texture) {
                    binding.role = PipelineSlotRole::WritableTexture;
                    fits = (out.writeSlots.usedMask & bit) == 0 && (out.writeTextures & bit) == 0;
                    out.writeTextures |= bit;
                    why = "shares a writable slot with another resource";
                    break;
                }

                binding.role = PipelineSlotRole::WritableBuffer;
                BufferViewKind kind = BufferViewKind::Typed;
                PixelShaderType type = PixelTypeOf(resource.component);
                uint32 stride = 0;
                if (resource.shape == ShaderResourceShape::StructuredBuffer) {
                    kind = BufferViewKind::Structured;
                    type = PixelShaderType::None;
                    stride = resource.structureStride;
                } else if (resource.shape == ShaderResourceShape::RawBuffer) {
                    kind = BufferViewKind::Raw;
                    type = PixelShaderType::None;
                }

                fits = (out.writeTextures & bit) == 0 &&
                       AddDrawBufferSlot(out.writeSlots, static_cast<uint32>(slot), kind, type,
                                         stride);
                why = "shares a writable slot with another resource";
                break;
            }
            }

            if (fits && out.count == MaxPipelineSlotBindings) {
                fits = false;
                why = "exceeds the pipeline binding count";
            }

            if (!fits) {
                return Reject(error, "%s shader resource %.*s (%s) %s", stageName, NameLength(name),
                              name.Data(), shader::ShaderResourceKindName(resource.kind), why);
            }

            out.bindings[out.count++] = binding;
        }
    }

    return Result<void>::Success();
}

Result<void> MatchShaderVertexInputs(const shader::ShaderBindingLayout& vertex,
                                     const VertexLayout& layout,
                                     char (&error)[PipelineSlotErrorBytes]) noexcept {
    for (uint32 location = 0; location < shader::MaxShaderVertexAttributes; ++location) {
        const ShaderComponentType component = shader::ShaderAttributeComponent(vertex, location);
        if (component == ShaderComponentType::None) {
            continue;
        }

        const VertexAttribute* attribute = nullptr;
        for (const VertexAttribute& candidate : layout.attributes) {
            if (candidate.location == location) {
                attribute = &candidate;
            }
        }

        if (attribute == nullptr) {
            return Reject(error,
                          "vertex shader reads location %u which the vertex layout does not "
                          "provide",
                          location);
        }

        const VertexShaderType wanted =
            component == ShaderComponentType::Int
                ? VertexShaderType::Int
                : (component == ShaderComponentType::Uint ? VertexShaderType::Uint
                                                          : VertexShaderType::Float);
        const VertexShaderType fed = VertexFormatShaderType(attribute->format);
        if (fed != wanted) {
            return Reject(error,
                          "vertex input location %u: format %s feeds %s but the shader reads %s",
                          location, VertexFormatName(attribute->format), VertexShaderTypeName(fed),
                          VertexShaderTypeName(wanted));
        }
    }

    return Result<void>::Success();
}

Result<void> FindPipelineShaders(const DeviceShaderTable& table, const PipelineDesc& desc,
                                 const DeviceShaderCode*& vertex, const DeviceShaderCode*& pixel,
                                 char (&error)[PipelineSlotErrorBytes]) noexcept {
    vertex = table.Find(desc.vertexShader);
    pixel = table.Find(desc.pixelShader);
    if (vertex == nullptr || pixel == nullptr) {
        return Reject(error, "pipeline shader handle is null or stale");
    }

    if (vertex->Stage() != ShaderStage::Vertex || pixel->Stage() != ShaderStage::Pixel) {
        return Reject(error, "pipeline shaders must be a vertex and a pixel shader");
    }

    const char* reason = "";
    if (!shader::CheckShaderStageLink(vertex->Layout(), pixel->Layout(), &reason)) {
        return Reject(error, "%s", reason);
    }

    return Result<void>::Success();
}

Result<void> FindComputeShader(const DeviceShaderTable& table, const ComputePipelineDesc& desc,
                               const DeviceShaderCode*& compute,
                               char (&error)[PipelineSlotErrorBytes]) noexcept {
    compute = table.Find(desc.computeShader);
    if (compute == nullptr) {
        return Reject(error, "compute shader handle is null or stale");
    }

    if (compute->Stage() != ShaderStage::Compute) {
        return Reject(error, "compute pipeline shader must be a compute shader");
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics
